// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Utils/VmhJson.h"

#include "JsonObjectConverter.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "UObject/Class.h"
#include "UObject/EnumProperty.h"
#include "UObject/Package.h"
#include "UObject/PropertyPortFlags.h"
#include "UObject/UnrealType.h"
#include "UObject/UObjectIterator.h"
#include "UObject/UObjectGlobals.h"

namespace VmhJson
{
	TSharedPtr<FJsonObject> Obj()
	{
		return MakeShared<FJsonObject>();
	}

	TArray<TSharedPtr<FJsonValue>> StringArray(const TArray<FString>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FString& V : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(V));
		}
		return Out;
	}

	TArray<TSharedPtr<FJsonValue>> NameArray(const TArray<FName>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		Out.Reserve(Values.Num());
		for (const FName& V : Values)
		{
			Out.Add(MakeShared<FJsonValueString>(V.ToString()));
		}
		return Out;
	}

	FString Stringify(const TSharedPtr<FJsonObject>& Object, bool bPretty)
	{
		if (!Object.IsValid())
		{
			return TEXT("{}");
		}
		FString Out;
		if (bPretty)
		{
			TSharedRef<TJsonWriter<>> Writer = TJsonWriterFactory<>::Create(&Out);
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		}
		else
		{
			TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
				TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
			FJsonSerializer::Serialize(Object.ToSharedRef(), Writer);
		}
		return Out;
	}

	FString Ok(const TSharedPtr<FJsonObject>& Data)
	{
		TSharedPtr<FJsonObject> Out = Obj();
		Out->SetBoolField(TEXT("success"), true);
		if (Data.IsValid())
		{
			for (const auto& PairRaw : Data->Values)
			{
				// UE 5.8: FJsonObject keys are TSharedString, not FString
				const TPair<FString, TSharedPtr<FJsonValue>> Pair(FString(*PairRaw.Key), PairRaw.Value);
				Out->SetField(Pair.Key, Pair.Value);
			}
		}
		return Stringify(Out);
	}

	FString Error(const FString& Code, const FString& Message, const TSharedPtr<FJsonObject>& Extra)
	{
		TSharedPtr<FJsonObject> Out = Obj();
		Out->SetBoolField(TEXT("success"), false);
		Out->SetStringField(TEXT("error_code"), Code);
		Out->SetStringField(TEXT("error"), Message);
		if (Extra.IsValid())
		{
			for (const auto& PairRaw : Extra->Values)
			{
				// UE 5.8: FJsonObject keys are TSharedString, not FString
				const TPair<FString, TSharedPtr<FJsonValue>> Pair(FString(*PairRaw.Key), PairRaw.Value);
				Out->SetField(Pair.Key, Pair.Value);
			}
		}
		return Stringify(Out);
	}

	TSharedPtr<FJsonObject> Parse(const FString& Json, FString& OutError)
	{
		const FString Trimmed = Json.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return Obj();
		}
		TSharedPtr<FJsonObject> Out;
		TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Trimmed);
		if (!FJsonSerializer::Deserialize(Reader, Out) || !Out.IsValid())
		{
			OutError = FString::Printf(TEXT("Invalid JSON object: %s"), *Reader->GetErrorMessage());
			return nullptr;
		}
		return Out;
	}

	TSharedPtr<FJsonObject> StructToJson(const UStruct* Type, const void* Value)
	{
		TSharedRef<FJsonObject> Out = MakeShared<FJsonObject>();
		if (Type && Value)
		{
			FJsonObjectConverter::UStructToJsonObject(Type, Value, Out, 0, 0);
		}
		return Out;
	}

	bool JsonToStruct(const TSharedPtr<FJsonObject>& Json, const UStruct* Type, void* Value, FString& OutError)
	{
		if (!Json.IsValid() || !Type || !Value)
		{
			OutError = TEXT("JsonToStruct: invalid arguments");
			return false;
		}
		FText FailReason;
		if (!FJsonObjectConverter::JsonObjectToUStruct(Json.ToSharedRef(), Type, Value, 0, 0, false, &FailReason))
		{
			OutError = FString::Printf(TEXT("Could not apply JSON to %s: %s"), *Type->GetName(), *FailReason.ToString());
			return false;
		}
		return true;
	}

	FProperty* FindPropertyLoose(const UStruct* Type, const FString& Name)
	{
		if (!Type || Name.IsEmpty())
		{
			return nullptr;
		}
		if (FProperty* Exact = Type->FindPropertyByName(FName(*Name)))
		{
			return Exact;
		}
		for (TFieldIterator<FProperty> It(Type); It; ++It)
		{
			const FString PropName = It->GetName();
			if (PropName.Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
			// bool properties: accept "Foo" for "bFoo" and vice versa
			if (PropName.StartsWith(TEXT("b")) && PropName.Mid(1).Equals(Name, ESearchCase::IgnoreCase))
			{
				return *It;
			}
			if (Name.StartsWith(TEXT("b")) && Name.Mid(1).Equals(PropName, ESearchCase::IgnoreCase))
			{
				return *It;
			}
		}
		return nullptr;
	}

	TSharedPtr<FJsonObject> ObjectToJson(const UObject* Object, const TArray<FString>& PropertyFilter)
	{
		TSharedPtr<FJsonObject> Out = Obj();
		if (!Object)
		{
			return Out;
		}
		if (PropertyFilter.Num() == 0)
		{
			TSharedRef<FJsonObject> Ref = MakeShared<FJsonObject>();
			FJsonObjectConverter::UStructToJsonObject(Object->GetClass(), Object, Ref, 0, 0);
			return Ref;
		}
		for (const FString& Name : PropertyFilter)
		{
			FProperty* Prop = FindPropertyLoose(Object->GetClass(), Name);
			if (!Prop)
			{
				Out->SetField(Name, MakeShared<FJsonValueNull>());
				continue;
			}
			const void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
			TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(Prop, ValuePtr, 0, 0);
			Out->SetField(Prop->GetName(), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
		}
		return Out;
	}

	bool PatchObject(UObject* Object, const TSharedPtr<FJsonObject>& Patch, TArray<FString>& OutChanged, FString& OutError)
	{
		if (!Object)
		{
			OutError = TEXT("PatchObject: null object");
			return false;
		}
		if (!Patch.IsValid())
		{
			return true;
		}
		bool bModified = false;
		for (const auto& PairRaw : Patch->Values)
		{
			// UE 5.8: FJsonObject keys are TSharedString, not FString
			const TPair<FString, TSharedPtr<FJsonValue>> Pair(FString(*PairRaw.Key), PairRaw.Value);
			FProperty* Prop = FindPropertyLoose(Object->GetClass(), Pair.Key);
			if (!Prop)
			{
				OutError = FString::Printf(TEXT("%s has no property named '%s'"), *Object->GetClass()->GetName(), *Pair.Key);
				return false;
			}
			if (!bModified)
			{
				Object->Modify();
				bModified = true;
			}
#if WITH_EDITOR
			Object->PreEditChange(Prop);
#endif
			void* ValuePtr = Prop->ContainerPtrToValuePtr<void>(Object);
			FText FailReason;
			if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Prop, ValuePtr, 0, 0, false, &FailReason))
			{
				OutError = FString::Printf(TEXT("Could not set '%s' on %s: %s"), *Prop->GetName(), *Object->GetName(), *FailReason.ToString());
				return false;
			}
#if WITH_EDITOR
			FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
			Object->PostEditChangeProperty(Event);
#endif
			OutChanged.Add(Prop->GetName());
		}
		return true;
	}

	UClass* FindClass(const FString& PathOrName)
	{
		const FString Trimmed = PathOrName.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return nullptr;
		}
		if (Trimmed.StartsWith(TEXT("/")))
		{
			if (UClass* Found = FindObject<UClass>(nullptr, *Trimmed))
			{
				return Found;
			}
			return LoadObject<UClass>(nullptr, *Trimmed);
		}
		if (Trimmed.Contains(TEXT(".")))
		{
			const FString Scripted = FString::Printf(TEXT("/Script/%s"), *Trimmed);
			if (UClass* Found = FindObject<UClass>(nullptr, *Scripted))
			{
				return Found;
			}
		}
		FString Bare = Trimmed;
		if (Bare.StartsWith(TEXT("U")) && Bare.Len() > 1 && FChar::IsUpper(Bare[1]))
		{
			// Accept the C++ spelling (UMetaHumanCharacter) as well as the reflected name.
			if (UClass* Found = FindFirstObject<UClass>(*Bare.Mid(1), EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
		}
		return FindFirstObject<UClass>(*Bare, EFindFirstObjectOptions::NativeFirst);
	}

	UScriptStruct* FindStruct(const FString& PathOrName)
	{
		const FString Trimmed = PathOrName.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return nullptr;
		}
		if (Trimmed.StartsWith(TEXT("/")))
		{
			return FindObject<UScriptStruct>(nullptr, *Trimmed);
		}
		FString Bare = Trimmed;
		if (Bare.StartsWith(TEXT("F")) && Bare.Len() > 1 && FChar::IsUpper(Bare[1]))
		{
			if (UScriptStruct* Found = FindFirstObject<UScriptStruct>(*Bare.Mid(1), EFindFirstObjectOptions::NativeFirst))
			{
				return Found;
			}
		}
		return FindFirstObject<UScriptStruct>(*Bare, EFindFirstObjectOptions::NativeFirst);
	}

	int64 ParseEnum(const UEnum* Enum, const FString& Name)
	{
		if (!Enum)
		{
			return INDEX_NONE;
		}
		const FString Trimmed = Name.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return INDEX_NONE;
		}
		if (Trimmed.IsNumeric())
		{
			const int64 Numeric = FCString::Atoi64(*Trimmed);
			return Enum->IsValidEnumValue(Numeric) ? Numeric : INDEX_NONE;
		}
		const int64 Value = Enum->GetValueByNameString(Trimmed, EGetByNameFlags::None);
		if (Value != INDEX_NONE)
		{
			return Value;
		}
		// Case-insensitive fallback on short names and display names.
		for (int32 i = 0; i < Enum->NumEnums(); ++i)
		{
			const FString Short = Enum->GetNameStringByIndex(i);
			const FString Display = Enum->GetDisplayNameTextByIndex(i).ToString();
			if (Short.Equals(Trimmed, ESearchCase::IgnoreCase) || Display.Equals(Trimmed, ESearchCase::IgnoreCase))
			{
				return Enum->GetValueByIndex(i);
			}
		}
		return INDEX_NONE;
	}

	FString EnumNames(const UEnum* Enum)
	{
		if (!Enum)
		{
			return FString();
		}
		TArray<FString> Names;
		for (int32 i = 0; i < Enum->NumEnums(); ++i)
		{
			if (Enum->HasMetaData(TEXT("Hidden"), i))
			{
				continue;
			}
			const FString Short = Enum->GetNameStringByIndex(i);
			if (Short.EndsWith(TEXT("_MAX")) || Short == TEXT("Count"))
			{
				continue;
			}
			Names.Add(Short);
		}
		return FString::Join(Names, TEXT(", "));
	}

	static bool CallFunctionInternal(UObject* Target, UFunction* Function, const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = Obj();
		const FString Label = FString::Printf(TEXT("%s::%s"), *Target->GetClass()->GetName(), *Function->GetName());

		// Input parameters in declaration order, for positional ("0", "1", ...) arguments.
		TArray<FProperty*> InputParms;
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			if (It->HasAnyPropertyFlags(CPF_ReturnParm))
			{
				continue;
			}
			const bool bPureOut = It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ReferenceParm | CPF_ConstParm);
			if (!bPureOut)
			{
				InputParms.Add(*It);
			}
		}

		// Parameter buffer: initialise every parm, fill inputs from JSON, call, read outputs.
		TArray<uint8> Buffer;
		Buffer.SetNumZeroed(FMath::Max<int32>(Function->ParmsSize, 1));
		uint8* Parms = Buffer.GetData();
		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->InitializeValue_InContainer(Parms);
		}

		bool bOk = true;
		if (Args.IsValid() && Args->Values.Num() > 0)
		{
			for (const auto& PairRaw : Args->Values)
			{
				// UE 5.8: FJsonObject keys are TSharedString, not FString
				const TPair<FString, TSharedPtr<FJsonValue>> Pair(FString(*PairRaw.Key), PairRaw.Value);
				FProperty* Prop = nullptr;
				if (Pair.Key.IsNumeric())
				{
					const int32 Index = FCString::Atoi(*Pair.Key);
					Prop = InputParms.IsValidIndex(Index) ? InputParms[Index] : nullptr;
				}
				else
				{
					Prop = FindPropertyLoose(Function, Pair.Key);
				}
				if (!Prop || !Prop->HasAnyPropertyFlags(CPF_Parm) || Prop->HasAnyPropertyFlags(CPF_ReturnParm))
				{
					OutError = FString::Printf(TEXT("%s has no input parameter '%s'"), *Label, *Pair.Key);
					bOk = false;
					break;
				}
				FText FailReason;
				if (!FJsonObjectConverter::JsonValueToUProperty(Pair.Value, Prop, Prop->ContainerPtrToValuePtr<void>(Parms), 0, 0, false, &FailReason))
				{
					OutError = FString::Printf(TEXT("%s: could not convert argument '%s': %s"), *Label, *Pair.Key, *FailReason.ToString());
					bOk = false;
					break;
				}
			}
		}

		if (bOk)
		{
			Target->ProcessEvent(Function, Parms);

			for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
			{
				const bool bIsOut = It->HasAnyPropertyFlags(CPF_OutParm) && !It->HasAnyPropertyFlags(CPF_ConstParm);
				const bool bIsReturn = It->HasAnyPropertyFlags(CPF_ReturnParm);
				if (!bIsOut && !bIsReturn)
				{
					continue;
				}
				TSharedPtr<FJsonValue> Value = FJsonObjectConverter::UPropertyToJsonValue(*It, It->ContainerPtrToValuePtr<void>(Parms), 0, 0);
				OutResult->SetField(bIsReturn ? TEXT("ReturnValue") : It->GetName(), Value.IsValid() ? Value : MakeShared<FJsonValueNull>());
			}
		}

		for (TFieldIterator<FProperty> It(Function); It && It->HasAnyPropertyFlags(CPF_Parm); ++It)
		{
			It->DestroyValue_InContainer(Parms);
		}
		return bOk;
	}

	bool CallStaticFunction(UClass* Class, FName FunctionName, const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = Obj();
		if (!Class)
		{
			OutError = TEXT("CallStaticFunction: null class");
			return false;
		}
		UFunction* Function = Class->FindFunctionByName(FunctionName);
		if (!Function)
		{
			OutError = FString::Printf(TEXT("%s has no function named '%s'"), *Class->GetName(), *FunctionName.ToString());
			return false;
		}
		if (!Function->HasAnyFunctionFlags(FUNC_Static))
		{
			OutError = FString::Printf(TEXT("%s::%s is not static; use call_object_function / set_properties on an instance"), *Class->GetName(), *FunctionName.ToString());
			return false;
		}
		return CallFunctionInternal(Class->GetDefaultObject(), Function, Args, OutResult, OutError);
	}

	bool CallObjectFunction(UObject* Target, FName FunctionName, const TSharedPtr<FJsonObject>& Args, TSharedPtr<FJsonObject>& OutResult, FString& OutError)
	{
		OutResult = Obj();
		if (!Target)
		{
			OutError = TEXT("CallObjectFunction: null target");
			return false;
		}
		UFunction* Function = Target->FindFunction(FunctionName);
		if (!Function)
		{
			OutError = FString::Printf(TEXT("%s has no function named '%s'"), *Target->GetClass()->GetName(), *FunctionName.ToString());
			return false;
		}
		return CallFunctionInternal(Target, Function, Args, OutResult, OutError);
	}
}
