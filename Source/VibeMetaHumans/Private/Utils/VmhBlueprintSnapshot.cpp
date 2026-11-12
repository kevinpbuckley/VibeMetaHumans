// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Utils/VmhBlueprintSnapshot.h"

#include "Components/ActorComponent.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "UObject/UnrealType.h"
#include "Utils/VmhJson.h"

namespace VmhBlueprintSnapshot
{
	namespace
	{
		bool IsRecordable(const FProperty* Prop)
		{
			if (Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_NonPIEDuplicateTransient | CPF_Deprecated))
			{
				return false;
			}
			if (Prop->IsA<FMulticastDelegateProperty>() || Prop->IsA<FDelegateProperty>())
			{
				return false;
			}
			return Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible);
		}

		FString KeyOf(const FProperty* Prop, int32 Index)
		{
			return Prop->ArrayDim > 1 ? FString::Printf(TEXT("%s[%d]"), *Prop->GetName(), Index) : Prop->GetName();
		}

		/**
		 * Every recordable property of Object that differs from Archetype (all of them when Archetype is null).
		 * bSkipComponentRefs (class defaults): the generated class's component variables are not values to restore -
		 * the components are, and a rebuilt class may not have the variable yet.
		 */
		TSharedPtr<FJsonObject> ExportDiffering(UObject* Object, const UObject* Archetype, bool bSkipComponentRefs = false)
		{
			TSharedPtr<FJsonObject> Props = VmhJson::Obj();
			if (!Object)
			{
				return Props;
			}
			for (TFieldIterator<FProperty> It(Object->GetClass()); It; ++It)
			{
				const FProperty* Prop = *It;
				if (!IsRecordable(Prop))
				{
					continue;
				}
				if (bSkipComponentRefs)
				{
					const FObjectPropertyBase* ObjectProp = CastField<FObjectPropertyBase>(Prop);
					if (ObjectProp && ObjectProp->PropertyClass && ObjectProp->PropertyClass->IsChildOf(UActorComponent::StaticClass()))
					{
						continue;
					}
				}
				for (int32 Index = 0; Index < Prop->ArrayDim; ++Index)
				{
					if (Archetype && Archetype->IsA(Prop->GetOwnerClass()) && Prop->Identical_InContainer(Object, Archetype, Index, PPF_None))
					{
						continue;
					}
					FString Text;
					Prop->ExportText_InContainer(Index, Text, Object, nullptr, Object, PPF_None);
					Props->SetStringField(KeyOf(Prop, Index), Text);
				}
			}
			return Props;
		}

		FString CurrentText(UObject* Object, const FProperty* Prop, int32 Index)
		{
			FString Text;
			Prop->ExportText_InContainer(Index, Text, Object, nullptr, Object, PPF_None);
			return Text;
		}

		bool Skipped(const FRestoreOptions& Options, const FString& Component, const FString& Property)
		{
			const FString Full = Component.IsEmpty() ? Property : Component + TEXT(".") + Property;
			for (const FString& Pattern : Options.SkipPatterns)
			{
				if (Full.MatchesWildcard(Pattern) || Property.MatchesWildcard(Pattern))
				{
					return true;
				}
			}
			return false;
		}

		/** Re-import Recorded onto Object where the current value differs. Appends to Applied / Failed / SkippedOut. */
		void ApplyProperties(UObject* Object, const FString& Component, const TSharedPtr<FJsonObject>& Recorded, const FRestoreOptions& Options,
			TArray<TSharedPtr<FJsonValue>>& Applied, TArray<TSharedPtr<FJsonValue>>& Failed, TArray<TSharedPtr<FJsonValue>>& SkippedOut)
		{
			if (!Object || !Recorded.IsValid())
			{
				return;
			}
			for (const TPair<FString, TSharedPtr<FJsonValue>>& Entry : Recorded->Values)
			{
				FString Name = Entry.Key;
				int32 Index = 0;
				int32 Bracket = INDEX_NONE;
				if (Name.FindChar(TEXT('['), Bracket))
				{
					Index = FCString::Atoi(*Name.Mid(Bracket + 1));
					Name = Name.Left(Bracket);
				}
				const FString Label = Component.IsEmpty() ? Entry.Key : Component + TEXT(".") + Entry.Key;
				if (Skipped(Options, Component, Entry.Key))
				{
					SkippedOut.Add(MakeShared<FJsonValueString>(Label));
					continue;
				}
				FProperty* Prop = FindFProperty<FProperty>(Object->GetClass(), FName(*Name));
				if (!Prop || Index >= Prop->ArrayDim)
				{
					Failed.Add(MakeShared<FJsonValueString>(Label + TEXT(": no such property on ") + Object->GetClass()->GetName()));
					continue;
				}
				const FString Wanted = Entry.Value->AsString();
				const FString Before = CurrentText(Object, Prop, Index);
				if (Before == Wanted)
				{
					continue;
				}
				TSharedPtr<FJsonObject> Change = VmhJson::Obj();
				Change->SetStringField(TEXT("property"), Label);
				Change->SetStringField(TEXT("before"), Before.Left(300));
				Change->SetStringField(TEXT("after"), Wanted.Left(300));
				if (!Options.bDryRun)
				{
					Object->Modify();
					Object->PreEditChange(Prop);
					const TCHAR* Result = Prop->ImportText_InContainer(*Wanted, Object, Object, PPF_None);
					FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
					Object->PostEditChangeProperty(Event);
					if (!Result)
					{
						Failed.Add(MakeShared<FJsonValueString>(Label + TEXT(": the recorded text did not import")));
						continue;
					}
				}
				Applied.Add(MakeShared<FJsonValueObject>(Change));
			}
		}
	}

	TSharedPtr<FJsonObject> Take(UBlueprint* Blueprint)
	{
		TSharedPtr<FJsonObject> Snap = VmhJson::Obj();
		if (!Blueprint)
		{
			return Snap;
		}
		Snap->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Snap->SetStringField(TEXT("taken"), FDateTime::Now().ToIso8601());
		TArray<TSharedPtr<FJsonValue>> Components;
		if (USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (!Node || !Node->ComponentTemplate)
				{
					continue;
				}
				TSharedPtr<FJsonObject> C = VmhJson::Obj();
				C->SetStringField(TEXT("name"), Node->GetVariableName().ToString());
				C->SetStringField(TEXT("class"), Node->ComponentClass ? Node->ComponentClass->GetPathName() : Node->ComponentTemplate->GetClass()->GetPathName());
				const USCS_Node* Parent = SCS->FindParentNode(Node);
				C->SetStringField(TEXT("parent"), Parent ? Parent->GetVariableName().ToString() : (Node->ParentComponentOrVariableName.IsNone() ? FString() : Node->ParentComponentOrVariableName.ToString()));
				C->SetStringField(TEXT("socket"), Node->AttachToName.IsNone() ? TEXT("") : Node->AttachToName.ToString());
				C->SetObjectField(TEXT("properties"), ExportDiffering(Node->ComponentTemplate, Node->ComponentTemplate->GetArchetype()));
				Components.Add(MakeShared<FJsonValueObject>(C));
			}
		}
		Snap->SetArrayField(TEXT("components"), Components);
		if (Blueprint->GeneratedClass)
		{
			UObject* CDO = Blueprint->GeneratedClass->GetDefaultObject();
			Snap->SetObjectField(TEXT("class_defaults"), ExportDiffering(CDO, CDO ? CDO->GetArchetype() : nullptr, true));
		}
		return Snap;
	}

	TSharedPtr<FJsonObject> Restore(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Snapshot, const FRestoreOptions& Options, FString& OutError)
	{
		if (!Blueprint || !Blueprint->SimpleConstructionScript)
		{
			OutError = TEXT("no Blueprint with a construction script");
			return nullptr;
		}
		const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
		if (!Snapshot.IsValid() || !Snapshot->TryGetArrayField(TEXT("components"), Components))
		{
			OutError = TEXT("the snapshot has no components array (take it with snapshot_blueprint_components)");
			return nullptr;
		}
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		TArray<TSharedPtr<FJsonValue>> Added, Applied, Failed, SkippedOut, Unplaced;
		bool bStructural = false;

		// pass 1: re-create missing components, parents first (repeat while a pass places something)
		TArray<TSharedPtr<FJsonObject>> Pending;
		for (const TSharedPtr<FJsonValue>& V : *Components)
		{
			const TSharedPtr<FJsonObject> C = V->AsObject();
			if (C.IsValid() && !SCS->FindSCSNode(FName(*C->GetStringField(TEXT("name")))))
			{
				Pending.Add(C);
			}
		}
		if (Options.bAddMissing)
		{
			TSet<FString> PlannedNames; // dry run: names that WOULD exist after this pass
			bool bProgress = true;
			while (Pending.Num() > 0 && bProgress)
			{
				bProgress = false;
				for (int32 I = Pending.Num() - 1; I >= 0; --I)
				{
					const TSharedPtr<FJsonObject> C = Pending[I];
					const FString Name = C->GetStringField(TEXT("name"));
					const FString ParentName = C->GetStringField(TEXT("parent"));
					USCS_Node* ParentNode = ParentName.IsEmpty() ? nullptr : SCS->FindSCSNode(FName(*ParentName));
					if (!ParentName.IsEmpty() && !ParentNode && !PlannedNames.Contains(ParentName))
					{
						continue; // parent not there yet
					}
					UClass* Class = LoadObject<UClass>(nullptr, *C->GetStringField(TEXT("class")));
					if (!Class || !Class->IsChildOf(UActorComponent::StaticClass()))
					{
						Failed.Add(MakeShared<FJsonValueString>(Name + TEXT(": component class not found: ") + C->GetStringField(TEXT("class"))));
						Pending.RemoveAt(I);
						continue;
					}
					TSharedPtr<FJsonObject> Rec = VmhJson::Obj();
					Rec->SetStringField(TEXT("name"), Name);
					Rec->SetStringField(TEXT("class"), Class->GetName());
					Rec->SetStringField(TEXT("parent"), ParentName);
					if (!Options.bDryRun)
					{
						USCS_Node* Node = SCS->CreateNode(Class, FName(*Name));
						const FString Socket = C->GetStringField(TEXT("socket"));
						Node->AttachToName = Socket.IsEmpty() ? NAME_None : FName(*Socket);
						if (ParentNode)
						{
							ParentNode->AddChildNode(Node);
						}
						else
						{
							SCS->AddNode(Node);
						}
						TArray<TSharedPtr<FJsonValue>> NodeApplied;
						ApplyProperties(Node->ComponentTemplate, Name, C->GetObjectField(TEXT("properties")), Options, NodeApplied, Failed, SkippedOut);
						Rec->SetNumberField(TEXT("properties_set"), NodeApplied.Num());
						bStructural = true;
					}
					PlannedNames.Add(Name);
					Added.Add(MakeShared<FJsonValueObject>(Rec));
					Pending.RemoveAt(I);
					bProgress = true;
				}
			}
			for (const TSharedPtr<FJsonObject>& C : Pending)
			{
				Unplaced.Add(MakeShared<FJsonValueString>(C->GetStringField(TEXT("name")) + TEXT(" (parent ") + C->GetStringField(TEXT("parent")) + TEXT(" missing)")));
			}
		}

		// pass 2: recorded values on the components the Blueprint still has
		if (Options.bApplyToExisting)
		{
			TSet<FString> JustAdded;
			for (const TSharedPtr<FJsonValue>& A : Added) { JustAdded.Add(A->AsObject()->GetStringField(TEXT("name"))); }
			for (const TSharedPtr<FJsonValue>& V : *Components)
			{
				const TSharedPtr<FJsonObject> C = V->AsObject();
				const FString Name = C.IsValid() ? C->GetStringField(TEXT("name")) : FString();
				USCS_Node* Node = Name.IsEmpty() || JustAdded.Contains(Name) ? nullptr : SCS->FindSCSNode(FName(*Name));
				if (Node && Node->ComponentTemplate)
				{
					ApplyProperties(Node->ComponentTemplate, Name, C->GetObjectField(TEXT("properties")), Options, Applied, Failed, SkippedOut);
				}
			}
		}
		const TSharedPtr<FJsonObject>* Defaults = nullptr;
		if (Options.bClassDefaults && Blueprint->GeneratedClass && Snapshot->TryGetObjectField(TEXT("class_defaults"), Defaults))
		{
			ApplyProperties(Blueprint->GeneratedClass->GetDefaultObject(), TEXT(""), *Defaults, Options, Applied, Failed, SkippedOut);
		}

		bool bCompiled = false;
		if (!Options.bDryRun && (Added.Num() > 0 || Applied.Num() > 0))
		{
			if (bStructural)
			{
				FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
			}
			else
			{
				FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
			}
			FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
			Blueprint->MarkPackageDirty();
			bCompiled = Blueprint->Status != BS_Error;
		}

		TSharedPtr<FJsonObject> Out = VmhJson::Obj();
		Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
		Out->SetBoolField(TEXT("dry_run"), Options.bDryRun);
		Out->SetArrayField(TEXT("added"), Added);
		Out->SetArrayField(TEXT("applied"), Applied);
		Out->SetArrayField(TEXT("failed"), Failed);
		Out->SetArrayField(TEXT("skipped"), SkippedOut);
		Out->SetArrayField(TEXT("unplaced"), Unplaced);
		Out->SetBoolField(TEXT("compiled"), bCompiled);
		return Out;
	}
}
