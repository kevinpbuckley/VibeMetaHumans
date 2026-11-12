// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanCharacterService.h"

#include "Animation/Skeleton.h"
#include "AssetExportTask.h"
#include "AssetImportTask.h"
#include "AssetToolsModule.h"
#include "Components/DynamicMeshComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/MeshNormals.h"
#include "Components/SkeletalMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Containers/Ticker.h"
#include "DNA.h"
#include "DNAAssetUserData.h"
#include "DNACommon.h"
#include "DNAUtils.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Exporters/Exporter.h"
#include "Exporters/FbxExportOption.h"
#include "Exporters/SkeletalMeshExporterFBX.h"
#include "Exporters/StaticMeshExporterFBX.h"
#include "Factories/FbxImportUI.h"
#include "Factories/FbxSkeletalMeshImportData.h"
#include "Factories/TextureFactory.h"
#include "GroomAsset.h"
#include "Engine/SkeletalMeshSocket.h"
#include "HairStrandsFactory.h"
#include "GroomBindingAsset.h"
#include "GroomComponent.h"
#include "HAL/FileManager.h"
#include "IAssetTools.h"
#include "ImageUtils.h"
#include "Misc/FileHelper.h"
#include "TextureCompiler.h"
#include "InterchangeDnaModule.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "MaterialDomain.h"
#include "MaterialEditingLibrary.h"
#include "Materials/Material.h"
#include "Materials/MaterialInstanceConstant.h"
#include "MetaHumanCharacterExportBlueprintLibrary.h"
#include "MetaHumanCharacterTargetKeyPoints.h"
#include "MetaHumanConformTargetParams.h"
#include "PreviewScene.h"
#include "Rendering/SkeletalMeshModel.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "StaticMeshResources.h"
#include "Editor.h"
#include "EditorViewportClient.h"
#include "GameFramework/Actor.h"
#include "MetaHumanBodyType.h"
#include "MetaHumanCharacter.h"
#include "MetaHumanCharacterAssemblySettings.h"
#include "MetaHumanCharacterBodyIdentity.h"
#include "MetaHumanCharacterEditorModule.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterIdentity.h"
#include "MetaHumanCollection.h"
#include "MetaHumanCollectionBlueprintLibrary.h"
#include "MetaHumanIdentity.h"
#include "MetaHumanInstance.h"
#include "MetaHumanPaletteItemKey.h"
#include "MetaHumanPipelineSlotSelection.h"
#include "MetaHumanPipelineSlotSelectionData.h"
#include "MetaHumanTypes.h"
#include "MetaHumanWardrobeItem.h"
#include "JsonObjectConverter.h"
#include "StructUtils/PropertyBag.h"
#include "MetaHumanCharacterPipelineSpecification.h"
#include "Misc/Paths.h"
#include "Subsystem/MetaHumanCharacterBuild.h"
#include "UObject/UnrealType.h"
#include "Utils/VmhAssets.h"
#include "Utils/VmhBlueprintSnapshot.h"
#include "Utils/VmhJson.h"
#include "Services/UMetaHumanWardrobeService.h"
#include "FileHelpers.h"
#include "Misc/PackageName.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "VibeMetaHumansModule.h"
#include "FrameTrackingContourData.h"
#include "MetaHumanFaceContourTrackerAsset.h"
#include "Nodes/HyprsenseNode.h"
#include "Nodes/ImageUtilNodes.h"
#include "Pipeline/Pipeline.h"

namespace
{
	UMetaHumanCharacterEditorSubsystem* GetSubsystem(FString& OutError)
	{
		UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
		if (!Subsystem)
		{
			OutError = TEXT("MetaHumanCharacterEditorSubsystem is not available (is the MetaHuman Creator plugin enabled?)");
		}
		return Subsystem;
	}

	UMetaHumanCharacter* LoadCharacter(const FString& Path, FString& OutError)
	{
		UObject* Object = VmhAssets::Load(Path);
		UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(Object);
		if (!Character)
		{
			OutError = Object
				? FString::Printf(TEXT("%s is a %s, not a MetaHumanCharacter"), *Path, *Object->GetClass()->GetName())
				: FString::Printf(TEXT("No MetaHuman Character asset at %s"), *Path);
		}
		return Character;
	}

	/** Resolve subsystem + character and make sure the character is open for editing. */
	struct FEditContext
	{
		UMetaHumanCharacterEditorSubsystem* Subsystem = nullptr;
		UMetaHumanCharacter* Character = nullptr;
		FString Error;
		FString ErrorCode;

		bool IsValid() const { return Subsystem && Character && Error.IsEmpty(); }
		FString ToError() const { return VmhJson::Error(ErrorCode.IsEmpty() ? TEXT("EDIT_FAILED") : ErrorCode, Error); }
	};

	FEditContext OpenForEdit(const FString& CharacterPath, bool bRequireEdit = true)
	{
		FEditContext Ctx;
		Ctx.Subsystem = GetSubsystem(Ctx.Error);
		if (!Ctx.Subsystem)
		{
			Ctx.ErrorCode = TEXT("NO_SUBSYSTEM");
			return Ctx;
		}
		Ctx.Character = LoadCharacter(CharacterPath, Ctx.Error);
		if (!Ctx.Character)
		{
			Ctx.ErrorCode = TEXT("CHARACTER_NOT_FOUND");
			return Ctx;
		}
		if (bRequireEdit && !Ctx.Subsystem->IsObjectAddedForEditing(Ctx.Character))
		{
			if (!Ctx.Subsystem->TryAddObjectToEdit(Ctx.Character))
			{
				Ctx.Error = FString::Printf(TEXT("TryAddObjectToEdit failed for %s (see Output Log; a missing optional-content model or an invalid character state is the usual cause)"), *Ctx.Character->GetPathName());
				Ctx.ErrorCode = TEXT("BEGIN_EDIT_FAILED");
			}
		}
		return Ctx;
	}

	/** The subsystem declares IsFixedBodyType but never defines it (unresolved at link time), so read the asset flag. */
	bool IsFixedBodyType(const UMetaHumanCharacter* Character)
	{
		if (!Character)
		{
			return false;
		}
		if (const FBoolProperty* Prop = CastField<FBoolProperty>(Character->GetClass()->FindPropertyByName(TEXT("bFixedBodyType"))))
		{
			return Prop->GetPropertyValue_InContainer(Character);
		}
		return false;
	}

	const TCHAR* RigStateName(EMetaHumanCharacterRigState State)
	{
		switch (State)
		{
		case EMetaHumanCharacterRigState::Unrigged: return TEXT("Unrigged");
		case EMetaHumanCharacterRigState::RigPending: return TEXT("RigPending");
		case EMetaHumanCharacterRigState::Rigged: return TEXT("Rigged");
		default: return TEXT("Unknown");
		}
	}

	FString ImportErrorName(EImportErrorCode Code)
	{
		if (const UEnum* Enum = StaticEnum<EImportErrorCode>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Code));
		}
		return FString::FromInt(static_cast<int32>(Code));
	}

	/** Copy of a struct UPROPERTY on an object, by name (so we never depend on member access). */
	template <typename TStruct>
	bool ReadStruct(UObject* Object, const TCHAR* PropertyName, TStruct& Out, FString& OutError)
	{
		FStructProperty* Prop = CastField<FStructProperty>(Object->GetClass()->FindPropertyByName(PropertyName));
		if (!Prop || Prop->Struct != TStruct::StaticStruct())
		{
			OutError = FString::Printf(TEXT("%s has no %s struct property"), *Object->GetClass()->GetName(), PropertyName);
			return false;
		}
		Out = *Prop->ContainerPtrToValuePtr<TStruct>(Object);
		return true;
	}

	bool WriteStruct(UObject* Object, const TCHAR* PropertyName, const UScriptStruct* Type, const void* Value, FString& OutError)
	{
		FStructProperty* Prop = CastField<FStructProperty>(Object->GetClass()->FindPropertyByName(PropertyName));
		if (!Prop || Prop->Struct != Type)
		{
			OutError = FString::Printf(TEXT("%s has no %s struct property"), *Object->GetClass()->GetName(), PropertyName);
			return false;
		}
		Object->Modify();
		Prop->CopyCompleteValue(Prop->ContainerPtrToValuePtr<void>(Object), Value);
		return true;
	}

	TSharedPtr<FJsonObject> ConstraintsToJson(const TArray<FMetaHumanCharacterBodyConstraint>& Constraints)
	{
		TSharedPtr<FJsonObject> Out = VmhJson::Obj();
		TArray<TSharedPtr<FJsonValue>> Items;
		for (const FMetaHumanCharacterBodyConstraint& C : Constraints)
		{
			TSharedPtr<FJsonObject> Item = VmhJson::Obj();
			Item->SetStringField(TEXT("name"), C.Name.ToString());
			Item->SetBoolField(TEXT("active"), C.bIsActive);
			Item->SetNumberField(TEXT("target"), C.TargetMeasurement);
			Item->SetNumberField(TEXT("min"), C.MinMeasurement);
			Item->SetNumberField(TEXT("max"), C.MaxMeasurement);
			const float Range = C.MaxMeasurement - C.MinMeasurement;
			Item->SetNumberField(TEXT("normalized"), Range > KINDA_SMALL_NUMBER ? (C.TargetMeasurement - C.MinMeasurement) / Range : 0.f);
			Items.Add(MakeShared<FJsonValueObject>(Item));
		}
		Out->SetArrayField(TEXT("constraints"), Items);
		return Out;
	}

	/** Re-align the neck after a body change (what Epic's MetaHumanGenerator does via ResetNeckToBody). */
	void ResetNeckToBody(UMetaHumanCharacterEditorSubsystem* Subsystem, UMetaHumanCharacter* Character)
	{
		TSharedRef<FMetaHumanCharacterIdentity::FState> NewState = Subsystem->CopyFaceState(Character);
		NewState->ResetNeckRegion();
		Subsystem->ApplyFaceState(Character, NewState);
	}

	TSharedPtr<FJsonObject> ItemKeyToJson(UMetaHumanCharacterPalette* Palette, const FMetaHumanPaletteItemKey& Key)
	{
		TSharedPtr<FJsonObject> Out = VmhJson::StructToJson(FMetaHumanPaletteItemKey::StaticStruct(), &Key);
		Out->SetStringField(TEXT("asset_name"), Key.ToAssetNameString());
		Out->SetStringField(TEXT("slot"), UMetaHumanCollectionBlueprintLibrary::GetItemSlotName(Palette, Key).ToString());
		Out->SetStringField(TEXT("display_name"), UMetaHumanCollectionBlueprintLibrary::GetItemDisplayName(Palette, Key).ToString());
		return Out;
	}

	FString CharacterResult(const FEditContext& Ctx, TSharedPtr<FJsonObject> Extra = nullptr)
	{
		TSharedPtr<FJsonObject> Out = Extra.IsValid() ? Extra : VmhJson::Obj();
		Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
		Out->SetStringField(TEXT("rig_state"), RigStateName(Ctx.Subsystem->GetRiggingState(Ctx.Character)));
		return VmhJson::Ok(Out);
	}

	/**
	 * The collection that holds the character's live wardrobe. While the character is open for editing (the
	 * Creator, or begin_edit), the subsystem works on a PREVIEW copy of the internal collection and copies that
	 * whole copy back over the asset after each Creator edit (OnEditPreviewCollection). A change made only to the
	 * internal collection is therefore lost on the next Creator edit: 2026-10-07, MissionCommander's built
	 * Blueprint wore the grooms add_wardrobe_item had picked while his MetaHuman went back to the Creator's older
	 * selection. Read and write wardrobe state here, and finish a write with CommitWardrobeEdit.
	 */
	UMetaHumanCollection* LiveCollection(UMetaHumanCharacterEditorSubsystem* Subsystem, UMetaHumanCharacter* Character)
	{
		if (Subsystem && Subsystem->IsObjectAddedForEditing(Character))
		{
			if (UMetaHumanCollection* Preview = Subsystem->GetPreviewCollection(Character))
			{
				return Preview;
			}
		}
		return Character->GetMutableInternalCollection();
	}

	/** Copy an edited preview collection back into the character asset (what the Creator's own panels do), then refresh the preview. */
	void CommitWardrobeEdit(const FEditContext& Ctx, UMetaHumanCollection* Edited)
	{
		if (Edited && Edited != Ctx.Character->GetInternalCollection())
		{
			Ctx.Subsystem->OnEditPreviewCollection(Ctx.Character);
		}
		Ctx.Character->MarkPackageDirty();
		Ctx.Subsystem->RunCharacterEditorPipelineForPreview(Ctx.Character);
		// the Creator's viewport does not redraw on its own after a scripted edit (2026-10-07: the owner had to
		// refresh); invalidate every editor viewport, the Creator's included
		if (GEditor)
		{
			for (FEditorViewportClient* Client : GEditor->GetAllViewportClients())
			{
				if (Client)
				{
					Client->Invalidate();
				}
			}
		}
	}

	/** The selection in SlotName (case-insensitive); ItemName (asset name, e.g. WI_Hair_S_Clean) picks one when a slot holds several. */
	bool FindSlotSelection(UMetaHumanInstance* Instance, const FString& SlotName, const FString& ItemName, FMetaHumanPipelineSlotSelection& OutSelection, FString& OutError)
	{
		TArray<FString> InSlot;
		for (const FMetaHumanPipelineSlotSelectionData& Data : Instance->GetSlotSelectionData())
		{
			if (!Data.Selection.SlotName.ToString().Equals(SlotName, ESearchCase::IgnoreCase) || Data.Selection.SelectedItem.IsNull())
			{
				continue;
			}
			const FString Name = Data.Selection.SelectedItem.ToAssetNameString();
			if (ItemName.IsEmpty() || Name.Equals(ItemName, ESearchCase::IgnoreCase))
			{
				OutSelection = Data.Selection;
				return true;
			}
			InSlot.Add(Name);
		}
		OutError = InSlot.Num() > 0
			? FString::Printf(TEXT("Slot '%s' has no selected item '%s' (selected: %s)"), *SlotName, *ItemName, *FString::Join(InSlot, TEXT(", ")))
			: FString::Printf(TEXT("Nothing is selected in slot '%s' (select_wardrobe_item first)"), *SlotName);
		return false;
	}

	/** Instance parameters need the collection's built data and an assembly: the Creator's preview has both, run it when missing. */
	void EnsureAssembled(const FEditContext& Ctx, UMetaHumanInstance* Instance)
	{
		if (!Instance->IsAssembled())
		{
			Ctx.Subsystem->RunCharacterEditorPipelineForPreview(Ctx.Character);
		}
	}

	/** A property bag's values as JSON plus each parameter's type. */
	TSharedPtr<FJsonObject> BagToJson(const FInstancedPropertyBag& Bag, TSharedPtr<FJsonObject>* OutTypes = nullptr)
	{
		const UPropertyBag* Struct = Bag.GetPropertyBagStruct();
		if (!Struct)
		{
			return VmhJson::Obj();
		}
		if (OutTypes)
		{
			*OutTypes = VmhJson::Obj();
			for (const FPropertyBagPropertyDesc& Desc : Struct->GetPropertyDescs())
			{
				FString Type = StaticEnum<EPropertyBagPropertyType>()->GetNameStringByValue(static_cast<int64>(Desc.ValueType));
				if (Desc.ValueTypeObject)
				{
					Type += TEXT(":") + Desc.ValueTypeObject->GetName();
				}
				(*OutTypes)->SetStringField(Desc.Name.ToString(), Type);
			}
		}
		return VmhJson::StructToJson(Struct, Bag.GetValue().GetMemory());
	}

	TArray<FString> BagNames(const FInstancedPropertyBag& Bag)
	{
		TArray<FString> Names;
		if (const UPropertyBag* Struct = Bag.GetPropertyBagStruct())
		{
			for (const FPropertyBagPropertyDesc& Desc : Struct->GetPropertyDescs())
			{
				Names.Add(Desc.Name.ToString());
			}
		}
		return Names;
	}

	TArray<TSharedPtr<FJsonValue>> IntArray(const TArray<int32>& Values)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 V : Values) { Out.Add(MakeShared<FJsonValueNumber>(V)); }
		return Out;
	}

	// =====================================================================
	// Deferred save / build (2026-10-07): a Creator save, or a build, made in the same call that queued the async
	// work it waits on deadlocked the shared editor (UPackage::Save / BuildMetaHuman sleep-polling for work only the
	// game thread advances - Master Smith's minidumps, out/_MetaHumanBatch/hang/). Both are deferred to a LATER
	// editor tick with FTSTicker so the RPC call itself returns at once; the heavy work still runs on the game
	// thread once it fires, so poll the matching get_*_status rather than waiting on these calls.
	// =====================================================================

	struct FSaveDeferredState
	{
		FString Status = TEXT("pending"); // pending | skipped | saved | failed
		FString Error;
		FString RequestedAt;
		FString At;
		double StartTime = 0.0;
		double Seconds = 0.0;
		int32 Ticks = 0;
		int32 MinFrames = 45;
		float DelaySeconds = 15.f;
		bool bActive = true;
		uint64 Generation = 0; // which save_deferred call owns the state: an older ticker sees a newer generation and drops out
	};
	TMap<FString, FSaveDeferredState> GPendingSaves;
	uint64 GSaveGeneration = 0;

	struct FBuildAsyncState
	{
		FString Status = TEXT("queued"); // queued | running | done | failed
		FString Error;
		double Seconds = 0.0;
		TArray<FString> Blueprints;
		FString OutputFolder;
	};
	TMap<FString, FBuildAsyncState> GPendingBuilds;

	FString NowString()
	{
		return FDateTime::Now().ToString(TEXT("%H:%M:%S"));
	}

	/** The groom slots (Hair, Eyebrows, ...) a character's wardrobe selections fill right now. */
	TArray<FString> WornGroomSlots(UMetaHumanCharacter* Character)
	{
		static const TArray<FString> GroomSlots = { TEXT("Hair"), TEXT("Eyebrows"), TEXT("Eyelashes"), TEXT("Beard"), TEXT("Mustache"), TEXT("Peachfuzz") };
		TArray<FString> Worn;
		FString Unused;
		if (UMetaHumanCollection* Collection = LiveCollection(GetSubsystem(Unused), Character))
		{
			for (const FMetaHumanPipelineSlotSelectionData& Data : Collection->GetMutableDefaultInstance()->GetSlotSelectionData())
			{
				const FString Slot = Data.Selection.SlotName.ToString();
				if (!Data.Selection.SelectedItem.IsNull() && GroomSlots.Contains(Slot))
				{
					Worn.Add(Slot);
				}
			}
		}
		return Worn;
	}

	/**
	 * Which Creator sculpt landmarks look like ears, neck or skull-top FROM THEIR POSITIONS (the landmark set
	 * differs per preset - Lorenzo 79, Orlando 125 - so this is never a fixed index list; Master Smith's
	 * face_fit.py auto_exclude, 2026-10-06). Y grows toward the face (the nose tip is the max); Z is up.
	 */
	void ClassifyLandmarks(const TArray<FVector>& L, TArray<int32>& OutEars, TArray<int32>& OutNeck, TArray<int32>& OutSkull)
	{
		OutEars.Reset(); OutNeck.Reset(); OutSkull.Reset();
		if (L.Num() == 0) { return; }
		float YMax = -UE_BIG_NUMBER;
		for (const FVector& P : L) { YMax = FMath::Max(YMax, static_cast<float>(P.Y)); }
		TArray<int32> Behind, Front;
		for (int32 i = 0; i < L.Num(); ++i)
		{
			if (L[i].Y < YMax - 13.8) { Behind.Add(i); }
			if (L[i].Y > YMax - 6.0) { Front.Add(i); }
		}
		float ChinZ = UE_BIG_NUMBER, TopZ = -UE_BIG_NUMBER;
		for (int32 i : Front) { ChinZ = FMath::Min(ChinZ, static_cast<float>(L[i].Z)); }
		for (int32 i = 0; i < L.Num(); ++i)
		{
			if (L[i].Y > YMax - 5.0) { TopZ = FMath::Max(TopZ, static_cast<float>(L[i].Z)); }
		}
		float MaxAbsXBehind = 0.f;
		for (int32 i : Behind) { MaxAbsXBehind = FMath::Max(MaxAbsXBehind, FMath::Abs(static_cast<float>(L[i].X))); }
		for (int32 i = 0; i < L.Num(); ++i)
		{
			const FVector& P = L[i];
			if (P.Z < ChinZ - 1.0) { OutNeck.Add(i); }
			else if (P.Z > TopZ + 1.0) { OutSkull.Add(i); }
			else if (Behind.Contains(i) && MaxAbsXBehind > KINDA_SMALL_NUMBER && FMath::Abs(static_cast<float>(P.X)) > 0.5f * MaxAbsXBehind)
			{
				OutEars.Add(i);
			}
		}
	}
}

// =====================================================================
// Lifecycle
// =====================================================================

FString UMetaHumanCharacterService::CreateCharacter(const FString& PackagePath, const FString& AssetName, const FString& PresetCharacterPath)
{
	FString Error;
	UMetaHumanCharacterEditorSubsystem* Subsystem = GetSubsystem(Error);
	if (!Subsystem)
	{
		return VmhJson::Error(TEXT("NO_SUBSYSTEM"), Error);
	}

	UMetaHumanCharacter* Preset = nullptr;
	if (!PresetCharacterPath.IsEmpty())
	{
		Preset = LoadCharacter(PresetCharacterPath, Error);
		if (!Preset)
		{
			return VmhJson::Error(TEXT("PRESET_NOT_FOUND"), Error);
		}
	}

	UObject* Created = VmhAssets::Create(PackagePath, AssetName, UMetaHumanCharacter::StaticClass(),
		TEXT("/Script/MetaHumanCharacterEditor.MetaHumanCharacterFactoryNew"), Error);
	UMetaHumanCharacter* Character = Cast<UMetaHumanCharacter>(Created);
	if (!Character)
	{
		return VmhJson::Error(TEXT("CREATE_FAILED"), Error.IsEmpty() ? TEXT("Factory returned no MetaHumanCharacter") : Error);
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	if (Preset)
	{
		if (!Subsystem->IsObjectAddedForEditing(Character) && !Subsystem->TryAddObjectToEdit(Character))
		{
			return VmhJson::Error(TEXT("BEGIN_EDIT_FAILED"), TEXT("Created the asset but could not open it for editing to apply the preset"));
		}
		Subsystem->InitializeFromPreset(Character, Preset);
		Out->SetStringField(TEXT("preset"), Preset->GetPathName());
	}

	if (!VmhAssets::Save(Character, Error))
	{
		UE_LOG(LogVibeMetaHumans, Warning, TEXT("%s"), *Error);
	}

	Out->SetStringField(TEXT("character"), Character->GetPathName());
	Out->SetBoolField(TEXT("optional_content_installed"), FMetaHumanCharacterEditorModule::IsOptionalMetaHumanContentInstalled());
	Out->SetStringField(TEXT("next"), TEXT("set_body_shape / set_skin_tone / set_settings, then request_auto_rig and build"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::BeginEdit(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("editing"), true);
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::EndEdit(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, false);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	const bool bWasEditing = Ctx.Subsystem->IsObjectAddedForEditing(Ctx.Character);
	if (bWasEditing)
	{
		Ctx.Subsystem->RemoveObjectToEdit(Ctx.Character);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("was_editing"), bWasEditing);
	Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::GetSummary(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, false);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCharacter* Character = Ctx.Character;
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Character->GetPathName());
	Out->SetStringField(TEXT("name"), Character->GetName());
	Out->SetBoolField(TEXT("is_editing"), Ctx.Subsystem->IsObjectAddedForEditing(Character));
	Out->SetStringField(TEXT("rig_state"), RigStateName(Ctx.Subsystem->GetRiggingState(Character)));
	Out->SetBoolField(TEXT("has_face_dna"), Character->HasFaceDNA());
	Out->SetBoolField(TEXT("has_body_dna"), Character->HasBodyDNA());
	Out->SetBoolField(TEXT("has_synthesized_textures"), Character->HasSynthesizedTextures());
	Out->SetBoolField(TEXT("has_high_resolution_textures"), Character->HasHighResolutionTextures());
	Out->SetBoolField(TEXT("fixed_body_type"), IsFixedBodyType(Character));
	Out->SetBoolField(TEXT("auto_rigging_in_progress"), Ctx.Subsystem->IsAutoRiggingFace(Character));
	Out->SetBoolField(TEXT("requesting_high_res_textures"), Ctx.Subsystem->IsRequestingHighResolutionTextures(Character));

	FString Err;
	FMetaHumanCharacterSkinSettings Skin;
	if (ReadStruct(Character, TEXT("SkinSettings"), Skin, Err))
	{
		TSharedPtr<FJsonObject> SkinJson = VmhJson::Obj();
		SkinJson->SetNumberField(TEXT("u"), Skin.Skin.U);
		SkinJson->SetNumberField(TEXT("v"), Skin.Skin.V);
		SkinJson->SetNumberField(TEXT("roughness"), Skin.Skin.Roughness);
		SkinJson->SetNumberField(TEXT("face_texture_index"), Skin.Skin.FaceTextureIndex);
		SkinJson->SetNumberField(TEXT("body_texture_index"), Skin.Skin.BodyTextureIndex);
		Out->SetObjectField(TEXT("skin"), SkinJson);
	}
	FMetaHumanCharacterEyesSettings Eyes;
	if (ReadStruct(Character, TEXT("EyesSettings"), Eyes, Err))
	{
		TSharedPtr<FJsonObject> EyesJson = VmhJson::Obj();
		EyesJson->SetNumberField(TEXT("left_u"), Eyes.EyeLeft.Iris.PrimaryColorU);
		EyesJson->SetNumberField(TEXT("left_v"), Eyes.EyeLeft.Iris.PrimaryColorV);
		EyesJson->SetNumberField(TEXT("right_u"), Eyes.EyeRight.Iris.PrimaryColorU);
		EyesJson->SetNumberField(TEXT("right_v"), Eyes.EyeRight.Iris.PrimaryColorV);
		Out->SetObjectField(TEXT("eyes"), EyesJson);
	}
	FMetaHumanCharacterFaceEvaluationSettings FaceEval;
	if (ReadStruct(Character, TEXT("FaceEvaluationSettings"), FaceEval, Err))
	{
		Out->SetObjectField(TEXT("face_evaluation"), VmhJson::StructToJson(FMetaHumanCharacterFaceEvaluationSettings::StaticStruct(), &FaceEval));
	}

	if (Ctx.Subsystem->IsObjectAddedForEditing(Character))
	{
		TArray<TSharedPtr<FJsonValue>> Active;
		for (const FMetaHumanCharacterBodyConstraint& C : Ctx.Subsystem->GetBodyConstraints(Character))
		{
			if (!C.bIsActive)
			{
				continue;
			}
			TSharedPtr<FJsonObject> Item = VmhJson::Obj();
			Item->SetStringField(TEXT("name"), C.Name.ToString());
			Item->SetNumberField(TEXT("target"), C.TargetMeasurement);
			Active.Add(MakeShared<FJsonValueObject>(Item));
		}
		Out->SetArrayField(TEXT("active_body_constraints"), Active);
	}

	if (UMetaHumanCollection* Collection = LiveCollection(Ctx.Subsystem, Character))
	{
		TArray<TSharedPtr<FJsonValue>> Selections;
		for (const FMetaHumanPipelineSlotSelectionData& Data : Collection->GetMutableDefaultInstance()->GetSlotSelectionData())
		{
			TSharedPtr<FJsonObject> Item = VmhJson::Obj();
			Item->SetStringField(TEXT("slot"), Data.Selection.SlotName.ToString());
			Item->SetStringField(TEXT("item"), Data.Selection.SelectedItem.ToAssetNameString());
			Selections.Add(MakeShared<FJsonValueObject>(Item));
		}
		Out->SetArrayField(TEXT("wardrobe_selections"), Selections);
	}
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::Save(const FString& CharacterPath)
{
	FString Error;
	UMetaHumanCharacter* Character = LoadCharacter(CharacterPath, Error);
	if (!Character)
	{
		return VmhJson::Error(TEXT("CHARACTER_NOT_FOUND"), Error);
	}
	UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
	if (Subsystem && Subsystem->IsObjectAddedForEditing(Character))
	{
		// a save made in the same call that changed a Creator character can deadlock the editor (sleep-polling
		// UPackage::Save for async texture synthesis only the game thread advances, 2026-10-06/07); defer it instead.
		FString ParseErr;
		TSharedPtr<FJsonObject> Out = VmhJson::Parse(SaveDeferred(CharacterPath, 15.f, 45), ParseErr);
		if (!Out.IsValid())
		{
			Out = VmhJson::Obj();
		}
		Out->SetStringField(TEXT("routed"), TEXT("save_deferred (the character is in an edit session)"));
		return VmhJson::Stringify(Out);
	}
	if (!VmhAssets::Save(Character, Error))
	{
		return VmhJson::Error(TEXT("SAVE_FAILED"), Error);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Character->GetPathName());
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::SaveDeferred(const FString& CharacterPath, float DelaySeconds, int32 MinFrames)
{
	FString Error;
	UMetaHumanCharacter* Character = LoadCharacter(CharacterPath, Error);
	if (!Character)
	{
		return VmhJson::Error(TEXT("CHARACTER_NOT_FOUND"), Error);
	}
	const FString Path = Character->GetPathName();
	UMetaHumanCharacterEditorSubsystem* Subsystem = GetSubsystem(Error);
	const bool bWasEditing = Subsystem && Subsystem->IsObjectAddedForEditing(Character);
	if (bWasEditing)
	{
		Subsystem->RemoveObjectToEdit(Character);
	}

	// a newer request replaces the state in place (TMap::Add on an existing key reuses its slot, so a flag set on the old
	// state would be overwritten): each request takes a new generation and an older ticker drops out when it sees it
	FSaveDeferredState& State = GPendingSaves.Add(Path);
	State = FSaveDeferredState();
	State.Generation = ++GSaveGeneration;
	State.Status = TEXT("pending");
	State.RequestedAt = NowString();
	State.StartTime = FPlatformTime::Seconds();
	State.MinFrames = FMath::Max(1, MinFrames);
	State.DelaySeconds = FMath::Max(0.f, DelaySeconds);
	State.bActive = true;
	const uint64 Generation = State.Generation;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda([Path, Generation](float) -> bool
	{
		FSaveDeferredState* S = GPendingSaves.Find(Path);
		if (!S || !S->bActive || S->Generation != Generation)
		{
			return false; // replaced by a newer save_deferred, or never existed
		}
		++S->Ticks;
		if (S->Ticks < S->MinFrames || (FPlatformTime::Seconds() - S->StartTime) < S->DelaySeconds)
		{
			return true; // keep waiting
		}
		FString LoadErr;
		UMetaHumanCharacter* Char = LoadCharacter(Path, LoadErr);
		UMetaHumanCharacterEditorSubsystem* Sub = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
		if (Char && Sub && Sub->IsObjectAddedForEditing(Char))
		{
			S->Status = TEXT("skipped");
			S->Error = TEXT("back in an edit session by the time the deferred save ran; call save_deferred again");
		}
		else if (Char)
		{
			const double Start = FPlatformTime::Seconds();
			FString SaveErr;
			const bool bOk = VmhAssets::Save(Char, SaveErr);
			S->Seconds = FPlatformTime::Seconds() - Start;
			S->Status = bOk ? TEXT("saved") : TEXT("failed");
			if (!bOk) { S->Error = SaveErr; }
		}
		else
		{
			S->Status = TEXT("failed");
			S->Error = LoadErr;
		}
		S->At = NowString();
		S->bActive = false;
		return false; // run once
	}), 0.f);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Path);
	Out->SetBoolField(TEXT("was_editing"), bWasEditing);
	Out->SetStringField(TEXT("status"), TEXT("pending"));
	Out->SetNumberField(TEXT("after_seconds"), State.DelaySeconds);
	Out->SetNumberField(TEXT("after_frames"), State.MinFrames);
	Out->SetStringField(TEXT("hint"), TEXT("poll get_save_status(character_path)"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::GetSaveStatus(const FString& CharacterPath)
{
	FString Error;
	UMetaHumanCharacter* Character = LoadCharacter(CharacterPath, Error);
	const FString Path = Character ? Character->GetPathName() : CharacterPath;
	const FSaveDeferredState* State = GPendingSaves.Find(Path);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Path);
	if (!State)
	{
		Out->SetStringField(TEXT("status"), TEXT("never_requested"));
		Out->SetBoolField(TEXT("pending"), false);
		return VmhJson::Ok(Out);
	}
	Out->SetStringField(TEXT("status"), State->Status);
	Out->SetBoolField(TEXT("pending"), State->bActive);
	Out->SetStringField(TEXT("requested_at"), State->RequestedAt);
	if (!State->At.IsEmpty()) { Out->SetStringField(TEXT("at"), State->At); }
	Out->SetNumberField(TEXT("seconds"), State->Seconds);
	if (!State->Error.IsEmpty()) { Out->SetStringField(TEXT("error"), State->Error); }
	return VmhJson::Ok(Out);
}

// =====================================================================
// Look
// =====================================================================

namespace
{
	enum class ESection { Skin, Eyes, Makeup, Head, FaceEvaluation, Viewport, Unknown };

	ESection ParseSection(const FString& Section)
	{
		const FString S = Section.TrimStartAndEnd().ToLower();
		if (S == TEXT("skin")) return ESection::Skin;
		if (S == TEXT("eyes")) return ESection::Eyes;
		if (S == TEXT("makeup")) return ESection::Makeup;
		if (S == TEXT("head") || S == TEXT("head_model") || S == TEXT("teeth") || S == TEXT("eyelashes")) return ESection::Head;
		if (S == TEXT("face_evaluation") || S == TEXT("face")) return ESection::FaceEvaluation;
		if (S == TEXT("viewport")) return ESection::Viewport;
		return ESection::Unknown;
	}

	const TCHAR* SectionProperty(ESection Section)
	{
		switch (Section)
		{
		case ESection::Skin: return TEXT("SkinSettings");
		case ESection::Eyes: return TEXT("EyesSettings");
		case ESection::Makeup: return TEXT("MakeupSettings");
		case ESection::Head: return TEXT("HeadModelSettings");
		case ESection::FaceEvaluation: return TEXT("FaceEvaluationSettings");
		case ESection::Viewport: return TEXT("ViewportSettings");
		default: return TEXT("");
		}
	}

	const FString SectionError = TEXT("Section must be one of: skin, eyes, makeup, head, face_evaluation, viewport");
}

FString UMetaHumanCharacterService::GetSettings(const FString& CharacterPath, const FString& Section)
{
	const ESection S = ParseSection(Section);
	if (S == ESection::Unknown)
	{
		return VmhJson::Error(TEXT("UNKNOWN_SECTION"), SectionError);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, false);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FStructProperty* Prop = CastField<FStructProperty>(Ctx.Character->GetClass()->FindPropertyByName(SectionProperty(S)));
	if (!Prop)
	{
		return VmhJson::Error(TEXT("NO_PROPERTY"), FString::Printf(TEXT("Character has no %s"), SectionProperty(S)));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
	Out->SetStringField(TEXT("section"), Section);
	Out->SetStringField(TEXT("struct"), Prop->Struct->GetName());
	Out->SetObjectField(TEXT("settings"), VmhJson::StructToJson(Prop->Struct, Prop->ContainerPtrToValuePtr<void>(Ctx.Character)));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::SetSettings(const FString& CharacterPath, const FString& Section, const FString& JsonPatch)
{
	const ESection S = ParseSection(Section);
	if (S == ESection::Unknown)
	{
		return VmhJson::Error(TEXT("UNKNOWN_SECTION"), SectionError);
	}
	FString ParseError;
	TSharedPtr<FJsonObject> Patch = VmhJson::Parse(JsonPatch, ParseError);
	if (!Patch.IsValid())
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCharacter* Character = Ctx.Character;
	FString Err;

	switch (S)
	{
	case ESection::Skin:
	{
		FMetaHumanCharacterSkinSettings Value;
		if (!ReadStruct(Character, TEXT("SkinSettings"), Value, Err) || !VmhJson::JsonToStruct(Patch, FMetaHumanCharacterSkinSettings::StaticStruct(), &Value, Err))
		{
			return VmhJson::Error(TEXT("PATCH_FAILED"), Err);
		}
		Ctx.Subsystem->CommitSkinSettings(Character, Value);
		break;
	}
	case ESection::Eyes:
	{
		FMetaHumanCharacterEyesSettings Value;
		if (!ReadStruct(Character, TEXT("EyesSettings"), Value, Err) || !VmhJson::JsonToStruct(Patch, FMetaHumanCharacterEyesSettings::StaticStruct(), &Value, Err))
		{
			return VmhJson::Error(TEXT("PATCH_FAILED"), Err);
		}
		Ctx.Subsystem->CommitEyesSettings(Character, Value);
		break;
	}
	case ESection::Makeup:
	{
		FMetaHumanCharacterMakeupSettings Value;
		if (!ReadStruct(Character, TEXT("MakeupSettings"), Value, Err) || !VmhJson::JsonToStruct(Patch, FMetaHumanCharacterMakeupSettings::StaticStruct(), &Value, Err))
		{
			return VmhJson::Error(TEXT("PATCH_FAILED"), Err);
		}
		Ctx.Subsystem->CommitMakeupSettings(Character, Value);
		break;
	}
	case ESection::Head:
	{
		FMetaHumanCharacterHeadModelSettings Value;
		if (!ReadStruct(Character, TEXT("HeadModelSettings"), Value, Err) || !VmhJson::JsonToStruct(Patch, FMetaHumanCharacterHeadModelSettings::StaticStruct(), &Value, Err))
		{
			return VmhJson::Error(TEXT("PATCH_FAILED"), Err);
		}
		Ctx.Subsystem->CommitHeadModelSettings(Character, Value);
		break;
	}
	case ESection::FaceEvaluation:
	{
		FMetaHumanCharacterFaceEvaluationSettings Value;
		if (!ReadStruct(Character, TEXT("FaceEvaluationSettings"), Value, Err) || !VmhJson::JsonToStruct(Patch, FMetaHumanCharacterFaceEvaluationSettings::StaticStruct(), &Value, Err))
		{
			return VmhJson::Error(TEXT("PATCH_FAILED"), Err);
		}
		Ctx.Subsystem->CommitFaceEvaluationSettings(Character, Value);
		break;
	}
	case ESection::Viewport:
	{
		FStructProperty* Prop = CastField<FStructProperty>(Character->GetClass()->FindPropertyByName(TEXT("ViewportSettings")));
		if (!Prop)
		{
			return VmhJson::Error(TEXT("NO_PROPERTY"), TEXT("Character has no ViewportSettings"));
		}
		Character->Modify();
		if (!VmhJson::JsonToStruct(Patch, Prop->Struct, Prop->ContainerPtrToValuePtr<void>(Character), Err))
		{
			return VmhJson::Error(TEXT("PATCH_FAILED"), Err);
		}
		FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
		Character->PostEditChangeProperty(Event);
		break;
	}
	default:
		break;
	}

	return GetSettings(CharacterPath, Section);
}

FString UMetaHumanCharacterService::SetSkinTone(const FString& CharacterPath, float U, float V)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FString Err;
	FMetaHumanCharacterSkinSettings Skin;
	if (!ReadStruct(Ctx.Character, TEXT("SkinSettings"), Skin, Err))
	{
		return VmhJson::Error(TEXT("NO_PROPERTY"), Err);
	}
	Skin.Skin.U = FMath::Clamp(U, 0.f, 1.f);
	Skin.Skin.V = FMath::Clamp(V, 0.f, 1.f);
	Ctx.Subsystem->CommitSkinSettings(Ctx.Character, Skin);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetNumberField(TEXT("u"), Skin.Skin.U);
	Out->SetNumberField(TEXT("v"), Skin.Skin.V);
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SetEyeColor(const FString& CharacterPath, float U, float V, const FString& Eye)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FString Err;
	FMetaHumanCharacterEyesSettings Eyes;
	if (!ReadStruct(Ctx.Character, TEXT("EyesSettings"), Eyes, Err))
	{
		return VmhJson::Error(TEXT("NO_PROPERTY"), Err);
	}
	const FString Which = Eye.TrimStartAndEnd().ToLower();
	const bool bLeft = Which.IsEmpty() || Which == TEXT("both") || Which == TEXT("left");
	const bool bRight = Which.IsEmpty() || Which == TEXT("both") || Which == TEXT("right");
	if (!bLeft && !bRight)
	{
		return VmhJson::Error(TEXT("BAD_EYE"), TEXT("Eye must be both, left or right"));
	}
	const float CU = FMath::Clamp(U, 0.f, 1.f);
	const float CV = FMath::Clamp(V, 0.f, 1.f);
	auto Apply = [CU, CV](FMetaHumanCharacterEyeProperties& P)
	{
		P.Iris.PrimaryColorU = CU;
		P.Iris.PrimaryColorV = CV;
		P.Iris.SecondaryColorU = CU;
		P.Iris.SecondaryColorV = CV;
	};
	if (bLeft) Apply(Eyes.EyeLeft);
	if (bRight) Apply(Eyes.EyeRight);
	Ctx.Subsystem->CommitEyesSettings(Ctx.Character, Eyes);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetNumberField(TEXT("u"), CU);
	Out->SetNumberField(TEXT("v"), CV);
	Out->SetStringField(TEXT("eye"), Which.IsEmpty() ? TEXT("both") : Which);
	return CharacterResult(Ctx, Out);
}

// =====================================================================
// Body
// =====================================================================

FString UMetaHumanCharacterService::GetBodyConstraints(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	TSharedPtr<FJsonObject> Out = ConstraintsToJson(Ctx.Subsystem->GetBodyConstraints(Ctx.Character, false));
	Out->SetBoolField(TEXT("fixed_body_type"), IsFixedBodyType(Ctx.Character));
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SetBodyConstraints(const FString& CharacterPath, const FString& ValuesJson, bool bNormalized, bool bOnlyNamed)
{
	FString ParseError;
	TSharedPtr<FJsonObject> Values = VmhJson::Parse(ValuesJson, ParseError);
	if (!Values.IsValid())
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	if (IsFixedBodyType(Ctx.Character))
	{
		return VmhJson::Error(TEXT("FIXED_BODY"), TEXT("This character uses a fixed body type; call set_body_type(..., \"BlendableBody\") first to use parametric constraints"));
	}

	TArray<FMetaHumanCharacterBodyConstraint> Constraints = Ctx.Subsystem->GetBodyConstraints(Ctx.Character, false);
	TMap<FString, float> Before;
	for (const FMetaHumanCharacterBodyConstraint& C : Constraints) { Before.Add(C.Name.ToString(), C.TargetMeasurement); }

	TArray<FString> Applied;
	TArray<FString> Unknown;
	TSet<FString> Named; // the measurements this call names, plus Height: only these end up active
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Values->Values)
	{
		FMetaHumanCharacterBodyConstraint* Found = Constraints.FindByPredicate([&Pair](const FMetaHumanCharacterBodyConstraint& C)
		{
			return C.Name.ToString().Equals(Pair.Key, ESearchCase::IgnoreCase);
		});
		if (!Found)
		{
			Unknown.Add(Pair.Key);
			continue;
		}
		double Value = 0.0;
		if (!Pair.Value->TryGetNumber(Value))
		{
			Unknown.Add(Pair.Key + TEXT(" (not a number)"));
			continue;
		}
		Named.Add(Found->Name.ToString());
		const bool bIsHeight = Found->Name.ToString().Equals(TEXT("Height"), ESearchCase::IgnoreCase);
		if (bIsHeight || !bNormalized)
		{
			Found->TargetMeasurement = FMath::Clamp(static_cast<float>(Value), Found->MinMeasurement, Found->MaxMeasurement);
		}
		else
		{
			Found->TargetMeasurement = FMath::Lerp(Found->MinMeasurement, Found->MaxMeasurement, FMath::Clamp(static_cast<float>(Value), 0.f, 1.f));
		}
		Applied.Add(Found->Name.ToString());
	}
	if (Applied.Num() == 0)
	{
		TArray<FString> Names;
		for (const FMetaHumanCharacterBodyConstraint& C : Constraints) Names.Add(C.Name.ToString());
		return VmhJson::Error(TEXT("NO_MATCHING_CONSTRAINT"), FString::Printf(TEXT("None of the keys matched a body constraint. Available: %s"), *FString::Join(Names, TEXT(", "))));
	}

	// the named measurements become active; the others keep their flag (a call that only sets Height or the body shape
	// must not switch off a Neck an earlier call set). bOnlyNamed leaves only the named ones plus Height active:
	// pinning every constraint active blocked a neck change, Height + the named ones worked (MissionCommander r4/r5).
	if (bOnlyNamed)
	{
		Named.Add(TEXT("Height"));
	}
	for (FMetaHumanCharacterBodyConstraint& C : Constraints)
	{
		if (Named.Contains(C.Name.ToString()))
		{
			C.bIsActive = true;
		}
		else if (bOnlyNamed)
		{
			C.bIsActive = false;
		}
	}

	Ctx.Subsystem->SetBodyConstraints(Ctx.Character, Constraints);
	Ctx.Subsystem->CommitBodyState(Ctx.Character);
	ResetNeckToBody(Ctx.Subsystem, Ctx.Character);

	// before/after/delta for EVERY measurement, read back from the committed state (a body change often moves more
	// than the named ones as a side effect, e.g. Shoulder Height +1.4 cm from a neck-only edit, 2026-10-06).
	const TArray<FMetaHumanCharacterBodyConstraint> After = Ctx.Subsystem->GetBodyConstraints(Ctx.Character, false);
	TArray<TSharedPtr<FJsonValue>> MeasurementsJson;
	for (const FMetaHumanCharacterBodyConstraint& C : After)
	{
		const FString Name = C.Name.ToString();
		const float B = Before.Contains(Name) ? Before[Name] : C.TargetMeasurement;
		TSharedPtr<FJsonObject> M = VmhJson::Obj();
		M->SetStringField(TEXT("name"), Name);
		M->SetBoolField(TEXT("active"), C.bIsActive);
		M->SetNumberField(TEXT("before"), B);
		M->SetNumberField(TEXT("after"), C.TargetMeasurement);
		M->SetNumberField(TEXT("delta"), C.TargetMeasurement - B);
		MeasurementsJson.Add(MakeShared<FJsonValueObject>(M));
	}
	TSharedPtr<FJsonObject> Out = ConstraintsToJson(After);
	Out->SetArrayField(TEXT("measurements"), MeasurementsJson);
	Out->SetArrayField(TEXT("applied"), VmhJson::StringArray(Applied));
	Out->SetArrayField(TEXT("active_now"), VmhJson::StringArray(Named.Array()));
	if (Unknown.Num() > 0)
	{
		Out->SetArrayField(TEXT("ignored"), VmhJson::StringArray(Unknown));
	}
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::GetFaceLandmarks(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	TArray<FVector> Landmarks;
	Ctx.Subsystem->GetFaceLandmarks(Ctx.Character, Landmarks);
	if (Landmarks.Num() == 0)
	{
		return VmhJson::Error(TEXT("NO_LANDMARKS"), TEXT("get_face_landmarks returned no points"));
	}
	TArray<int32> Ears, Neck, Skull;
	ClassifyLandmarks(Landmarks, Ears, Neck, Skull);

	TSharedPtr<FJsonObject> LandmarksJson = VmhJson::Obj();
	for (int32 i = 0; i < Landmarks.Num(); ++i)
	{
		TArray<TSharedPtr<FJsonValue>> V;
		V.Add(MakeShared<FJsonValueNumber>(Landmarks[i].X));
		V.Add(MakeShared<FJsonValueNumber>(Landmarks[i].Y));
		V.Add(MakeShared<FJsonValueNumber>(Landmarks[i].Z));
		LandmarksJson->SetArrayField(FString::FromInt(i), V);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetNumberField(TEXT("count"), Landmarks.Num());
	Out->SetObjectField(TEXT("landmarks"), LandmarksJson);
	Out->SetStringField(TEXT("frame"), TEXT("edit frame, centimetres, +Y toward the face, +Z up"));
	Out->SetArrayField(TEXT("likely_ears"), IntArray(Ears));
	Out->SetArrayField(TEXT("likely_neck"), IntArray(Neck));
	Out->SetArrayField(TEXT("likely_skull"), IntArray(Skull));
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::MoveFaceLandmarks(const FString& CharacterPath, const FString& TargetsJson, const FString& HoldJson, float ToleranceCm, int32 MaxIterations, float Gain, bool bAllowEars, bool bAbsolute)
{
	FString ParseError;
	TSharedPtr<FJsonObject> Targets = VmhJson::Parse(TargetsJson, ParseError);
	if (!Targets.IsValid() || Targets->Values.Num() == 0)
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError.IsEmpty() ? TEXT("TargetsJson must be a non-empty {\"index\":[dx,dy,dz]} object") : ParseError);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	const TArray<FString> Worn = WornGroomSlots(Ctx.Character);
	if (Worn.Num() > 0)
	{
		return VmhJson::Error(TEXT("GROOMS_WORN"), FString::Printf(TEXT("%s wears grooms (%s); spawning the Creator preview with grooms on hung the shared editor for minutes. Clear the groom slots, fit the face, then add grooms back"), *CharacterPath, *FString::Join(Worn, TEXT(", "))));
	}
	if (Ctx.Subsystem->GetRiggingState(Ctx.Character) == EMetaHumanCharacterRigState::Rigged)
	{
		return VmhJson::Error(TEXT("FACE_RIGGED"), TEXT("the face rig is still on; call remove_face_rig first"));
	}

	TArray<FVector> L0;
	Ctx.Subsystem->GetFaceLandmarks(Ctx.Character, L0);
	if (L0.Num() == 0)
	{
		return VmhJson::Error(TEXT("NO_LANDMARKS"), TEXT("get_face_landmarks returned no points"));
	}
	TArray<int32> EarIdx, NeckIdx, SkullIdx;
	ClassifyLandmarks(L0, EarIdx, NeckIdx, SkullIdx);
	const TSet<int32> Ears(EarIdx);

	TMap<int32, FVector> TargetAbs;
	TArray<FString> BadIndex, EarRefused;
	for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Targets->Values)
	{
		const int32 Idx = FCString::Atoi(*Pair.Key);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!L0.IsValidIndex(Idx))
		{
			BadIndex.Add(Pair.Key + TEXT(" (out of range)"));
			continue;
		}
		if (!Pair.Value->TryGetArray(Arr) || Arr->Num() != 3)
		{
			BadIndex.Add(Pair.Key + TEXT(" (expected [dx,dy,dz])"));
			continue;
		}
		if (Ears.Contains(Idx) && !bAllowEars)
		{
			EarRefused.Add(Pair.Key);
			continue;
		}
		const FVector V((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		TargetAbs.Add(Idx, bAbsolute ? V : L0[Idx] + V);
	}
	if (EarRefused.Num() > 0)
	{
		return VmhJson::Error(TEXT("EAR_LANDMARKS_REFUSED"), FString::Printf(TEXT("indices %s look like ear landmarks (moving them crumpled MissionCommander's ears, 2026-10-07); pass bAllowEars=true to move them anyway"), *FString::Join(EarRefused, TEXT(", "))));
	}
	if (TargetAbs.Num() == 0)
	{
		return VmhJson::Error(TEXT("NO_VALID_TARGETS"), FString::Printf(TEXT("no usable targets (bad: %s)"), *FString::Join(BadIndex, TEXT(", "))));
	}
	const int32 NumMoveTargets = TargetAbs.Num();

	// holds: every other landmark pinned at its current position ("auto", the default); an explicit index list; or
	// none held ("none") - only the named targets are driven and nothing else is corrected back.
	const FString HoldMode = HoldJson.TrimStartAndEnd();
	TArray<int32> HoldIdx;
	if (HoldMode.IsEmpty() || HoldMode.Equals(TEXT("auto"), ESearchCase::IgnoreCase))
	{
		for (int32 i = 0; i < L0.Num(); ++i)
		{
			if (!TargetAbs.Contains(i)) { HoldIdx.Add(i); }
		}
	}
	else if (!HoldMode.Equals(TEXT("none"), ESearchCase::IgnoreCase))
	{
		FString WrapErr;
		TSharedPtr<FJsonObject> Wrapped = VmhJson::Parse(FString::Printf(TEXT("{\"h\":%s}"), *HoldMode), WrapErr);
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Wrapped.IsValid() || !Wrapped->TryGetArrayField(TEXT("h"), Arr))
		{
			return VmhJson::Error(TEXT("BAD_HOLD"), TEXT("HoldJson must be \"auto\", \"none\" or a JSON array of indices"));
		}
		for (const TSharedPtr<FJsonValue>& V : *Arr) { HoldIdx.Add(static_cast<int32>(V->AsNumber())); }
	}
	int32 NumHeld = 0;
	for (int32 i : HoldIdx)
	{
		if (L0.IsValidIndex(i) && !TargetAbs.Contains(i))
		{
			TargetAbs.Add(i, L0[i]);
			++NumHeld;
		}
	}

	MaxIterations = FMath::Clamp(MaxIterations, 1, 64);
	ToleranceCm = FMath::Max(0.001f, ToleranceCm);
	const float Cap = 1.0f; // cm per iteration step (Master Smith's ed_p4_a2_ears_neck.py CAP)
	TArray<TSharedPtr<FJsonValue>> IterationsLog;
	float Worst = 0.f;
	int32 WorstIndex = INDEX_NONE;
	int32 IterationsRun = 0;
	for (int32 It = 0; It < MaxIterations; ++It)
	{
		TArray<FVector> L;
		Ctx.Subsystem->GetFaceLandmarks(Ctx.Character, L);
		Worst = 0.f; WorstIndex = INDEX_NONE;
		TArray<int32> MoveIdx;
		TArray<FVector> MoveDelta;
		for (const TPair<int32, FVector>& Pair : TargetAbs)
		{
			const FVector Residual = Pair.Value - L[Pair.Key];
			const float Mag = Residual.GetAbsMax();
			if (Mag > Worst) { Worst = Mag; WorstIndex = Pair.Key; }
			if (Mag >= ToleranceCm * 0.5f)
			{
				const float G = Ears.Contains(Pair.Key) ? Gain : 1.f;
				FVector Ask = Residual * G;
				Ask.X = FMath::Clamp(Ask.X, -Cap, Cap);
				Ask.Y = FMath::Clamp(Ask.Y, -Cap, Cap);
				Ask.Z = FMath::Clamp(Ask.Z, -Cap, Cap);
				MoveIdx.Add(Pair.Key);
				MoveDelta.Add(Ask);
			}
		}
		TSharedPtr<FJsonObject> IterRec = VmhJson::Obj();
		IterRec->SetNumberField(TEXT("worst_cm"), Worst);
		IterRec->SetNumberField(TEXT("worst_index"), WorstIndex);
		IterRec->SetNumberField(TEXT("moved"), MoveIdx.Num());
		IterationsLog.Add(MakeShared<FJsonValueObject>(IterRec));
		IterationsRun = It + 1;
		if (Worst < ToleranceCm || MoveIdx.Num() == 0)
		{
			break;
		}
		Ctx.Subsystem->TranslateFaceLandmarks(Ctx.Character, MoveIdx, MoveDelta);
		Ctx.Subsystem->CommitFaceState(Ctx.Character);
	}

	TArray<FVector> LFinal;
	Ctx.Subsystem->GetFaceLandmarks(Ctx.Character, LFinal);
	TSharedPtr<FJsonObject> ResidualsFinal = VmhJson::Obj();
	for (const TPair<int32, FVector>& Pair : TargetAbs)
	{
		ResidualsFinal->SetNumberField(FString::FromInt(Pair.Key), (Pair.Value - LFinal[Pair.Key]).GetAbsMax());
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetNumberField(TEXT("targets"), NumMoveTargets);
	Out->SetNumberField(TEXT("held"), NumHeld);
	Out->SetNumberField(TEXT("iterations_run"), IterationsRun);
	Out->SetBoolField(TEXT("converged"), Worst < ToleranceCm);
	Out->SetNumberField(TEXT("worst_residual_cm"), Worst);
	Out->SetArrayField(TEXT("iterations"), IterationsLog);
	Out->SetObjectField(TEXT("final_residuals_cm"), ResidualsFinal);
	if (BadIndex.Num() > 0)
	{
		Out->SetArrayField(TEXT("ignored"), VmhJson::StringArray(BadIndex));
	}
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SetBodyShape(const FString& CharacterPath, float MasculineFeminine, float Fat, float Muscularity, float HeightCm)
{
	TSharedPtr<FJsonObject> Values = VmhJson::Obj();
	if (MasculineFeminine >= 0.f) Values->SetNumberField(TEXT("Masculine/Feminine"), MasculineFeminine);
	if (Fat >= 0.f) Values->SetNumberField(TEXT("Fat"), Fat);
	if (Muscularity >= 0.f) Values->SetNumberField(TEXT("Muscularity"), Muscularity);
	if (HeightCm >= 0.f) Values->SetNumberField(TEXT("Height"), HeightCm);
	if (Values->Values.Num() == 0)
	{
		return VmhJson::Error(TEXT("NO_VALUES"), TEXT("Pass at least one non-negative value"));
	}
	return SetBodyConstraints(CharacterPath, VmhJson::Stringify(Values), true);
}

FString UMetaHumanCharacterService::SetBodyType(const FString& CharacterPath, const FString& BodyType)
{
	const UEnum* Enum = StaticEnum<EMetaHumanBodyType>();
	const int64 Value = VmhJson::ParseEnum(Enum, BodyType);
	if (Value == INDEX_NONE || Value >= static_cast<int64>(EMetaHumanBodyType::Count))
	{
		return VmhJson::Error(TEXT("BAD_BODY_TYPE"), FString::Printf(TEXT("Unknown body type '%s'. Options: %s"), *BodyType, *VmhJson::EnumNames(Enum)));
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	Ctx.Subsystem->SetMetaHumanBodyType(Ctx.Character, static_cast<EMetaHumanBodyType>(Value), UMetaHumanCharacterEditorSubsystem::EBodyMeshUpdateMode::Full);
	Ctx.Subsystem->CommitBodyState(Ctx.Character);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("body_type"), Enum->GetNameStringByValue(Value));
	Out->SetBoolField(TEXT("fixed_body_type"), IsFixedBodyType(Ctx.Character));
	return CharacterResult(Ctx, Out);
}

// =====================================================================
// Face
// =====================================================================

FString UMetaHumanCharacterService::GetFaceCoefficients(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	TArray<float> Coefficients;
	Ctx.Subsystem->GetFaceModelCoefficients(Ctx.Character, Coefficients);
	TArray<TSharedPtr<FJsonValue>> Values;
	for (float C : Coefficients)
	{
		Values.Add(MakeShared<FJsonValueNumber>(C));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetNumberField(TEXT("count"), Coefficients.Num());
	Out->SetArrayField(TEXT("coefficients"), Values);
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SetFaceCoefficients(const FString& CharacterPath, const TArray<float>& Coefficients)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	TArray<float> Current;
	Ctx.Subsystem->GetFaceModelCoefficients(Ctx.Character, Current);
	if (Current.Num() != Coefficients.Num())
	{
		return VmhJson::Error(TEXT("BAD_LENGTH"), FString::Printf(TEXT("Expected %d coefficients, got %d"), Current.Num(), Coefficients.Num()));
	}
	Ctx.Subsystem->SetFaceModelCoefficients(Ctx.Character, Coefficients);
	Ctx.Subsystem->CommitFaceState(Ctx.Character);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetNumberField(TEXT("count"), Coefficients.Num());
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::ResetFace(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	Ctx.Subsystem->ResetCharacterFace(Ctx.Character);
	Ctx.Subsystem->CommitFaceState(Ctx.Character);
	return CharacterResult(Ctx);
}

// =====================================================================
// Import
// =====================================================================

FString UMetaHumanCharacterService::ImportFromIdentity(const FString& CharacterPath, const FString& IdentityPath, bool bUseEyeMeshes, bool bUseTeethMesh, bool bUseMetricScale)
{
	UMetaHumanIdentity* Identity = VmhAssets::Load<UMetaHumanIdentity>(IdentityPath);
	if (!Identity)
	{
		return VmhJson::Error(TEXT("IDENTITY_NOT_FOUND"), FString::Printf(TEXT("No MetaHuman Identity at %s"), *IdentityPath));
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FImportFromIdentityParams Params;
	Params.bUseEyeMeshes = bUseEyeMeshes;
	Params.bUseTeethMesh = bUseTeethMesh;
	Params.bUseMetricScale = bUseMetricScale;
	const EImportErrorCode Code = Ctx.Subsystem->ImportFromIdentity(Ctx.Character, Identity, Params);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("identity"), Identity->GetPathName());
	Out->SetStringField(TEXT("import_result"), ImportErrorName(Code));
	if (Code != EImportErrorCode::Success)
	{
		Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
		return VmhJson::Error(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("ImportFromIdentity returned %s (IdentityNotConformed means run the Identity's conform/solve first)"), *ImportErrorName(Code)), Out);
	}
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::ImportFromFaceDna(const FString& CharacterPath, const FString& DnaFilePath, bool bImportWholeRig, bool bIsolateHeadFromBody)
{
	if (!FPaths::FileExists(DnaFilePath))
	{
		return VmhJson::Error(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("No file at %s"), *DnaFilePath));
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FImportFromDNAParams Params;
	Params.bImportWholeRig = bImportWholeRig;
	Params.bIsolateHeadFromBody = bIsolateHeadFromBody;
	const EImportErrorCode Code = Ctx.Subsystem->ImportFromFaceDna(Ctx.Character, DnaFilePath, Params);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("dna_file"), DnaFilePath);
	Out->SetStringField(TEXT("import_result"), ImportErrorName(Code));
	if (Code != EImportErrorCode::Success)
	{
		Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
		return VmhJson::Error(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("ImportFromFaceDna returned %s"), *ImportErrorName(Code)), Out);
	}
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::ImportFromTemplate(const FString& CharacterPath, const FString& HeadMeshPath, const FString& LeftEyeMeshPath, const FString& RightEyeMeshPath, const FString& TeethMeshPath)
{
	UObject* Head = VmhAssets::Load(HeadMeshPath);
	if (!Head)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No head mesh at %s"), *HeadMeshPath));
	}
	UObject* LeftEye = LeftEyeMeshPath.IsEmpty() ? nullptr : VmhAssets::Load(LeftEyeMeshPath);
	UObject* RightEye = RightEyeMeshPath.IsEmpty() ? nullptr : VmhAssets::Load(RightEyeMeshPath);
	UObject* Teeth = TeethMeshPath.IsEmpty() ? nullptr : VmhAssets::Load(TeethMeshPath);

	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FImportFromTemplateParams Params;
	Params.bUseEyeMeshes = LeftEye && RightEye;
	Params.bUseTeethMesh = Teeth != nullptr;
	const EImportErrorCode Code = Ctx.Subsystem->ImportFromTemplate(Ctx.Character, Head, LeftEye, RightEye, Teeth, Params);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("import_result"), ImportErrorName(Code));
	if (Code != EImportErrorCode::Success)
	{
		Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
		return VmhJson::Error(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("ImportFromTemplate returned %s"), *ImportErrorName(Code)), Out);
	}
	return CharacterResult(Ctx, Out);
}

// =====================================================================
// Cloud
// =====================================================================

FString UMetaHumanCharacterService::RequestAutoRig(const FString& CharacterPath, const FString& RigType, bool bBlocking)
{
	const UEnum* Enum = StaticEnum<EMetaHumanRigType>();
	int64 Value = RigType.IsEmpty() ? static_cast<int64>(EMetaHumanRigType::JointsOnly) : VmhJson::ParseEnum(Enum, RigType);
	if (Value == INDEX_NONE)
	{
		return VmhJson::Error(TEXT("BAD_RIG_TYPE"), FString::Printf(TEXT("RigType must be one of: %s"), *VmhJson::EnumNames(Enum)));
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FMetaHumanCharacterAutoRiggingRequestParams Params;
	Params.RigType = static_cast<EMetaHumanRigType>(Value);
	Params.bBlocking = bBlocking;
	Params.bReportProgress = !bBlocking;
	Ctx.Subsystem->RequestAutoRigging(Ctx.Character, Params);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("rig_type"), Enum->GetNameStringByValue(Value));
	Out->SetBoolField(TEXT("blocking"), bBlocking);
	Out->SetBoolField(TEXT("auto_rigging_in_progress"), Ctx.Subsystem->IsAutoRiggingFace(Ctx.Character));
	Out->SetBoolField(TEXT("has_face_dna"), Ctx.Character->HasFaceDNA());
	if (Ctx.Subsystem->GetRiggingState(Ctx.Character) != EMetaHumanCharacterRigState::Rigged)
	{
		Out->SetStringField(TEXT("hint"), TEXT("Rig not applied yet: a non-blocking request is still running, or the editor is not signed in to the MetaHuman service (sign in via the MetaHuman Character editor toolbar / Epic account) - check the Output Log."));
	}
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::RemoveFaceRig(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	Ctx.Subsystem->RemoveFaceRig(Ctx.Character);
	return CharacterResult(Ctx);
}

FString UMetaHumanCharacterService::GetAutoRigStatus(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, false);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	const bool bRunning = Ctx.Subsystem->IsAutoRiggingFace(Ctx.Character);
	const EMetaHumanCharacterRigState RigState = Ctx.Subsystem->GetRiggingState(Ctx.Character);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("status"), bRunning ? TEXT("running") : (RigState == EMetaHumanCharacterRigState::Rigged ? TEXT("done") : TEXT("failed_or_not_requested")));
	Out->SetBoolField(TEXT("running"), bRunning);
	Out->SetBoolField(TEXT("has_face_dna"), Ctx.Character->HasFaceDNA());
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::RequestTextureSources(const FString& CharacterPath, bool bBlocking)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FMetaHumanCharacterTextureRequestParams Params;
	Params.bBlocking = bBlocking;
	Params.bReportProgress = !bBlocking;
	Ctx.Subsystem->RequestTextureSources(Ctx.Character, Params);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("blocking"), bBlocking);
	Out->SetBoolField(TEXT("requesting_high_res_textures"), Ctx.Subsystem->IsRequestingHighResolutionTextures(Ctx.Character));
	Out->SetBoolField(TEXT("has_high_resolution_textures"), Ctx.Character->HasHighResolutionTextures());
	return CharacterResult(Ctx, Out);
}

// =====================================================================
// Wardrobe
// =====================================================================

FString UMetaHumanCharacterService::GetWardrobe(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, false);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCollection* Collection = LiveCollection(Ctx.Subsystem, Ctx.Character);
	if (!Collection)
	{
		return VmhJson::Error(TEXT("NO_COLLECTION"), TEXT("Character has no internal collection"));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("from_edit_preview"), Collection != Ctx.Character->GetInternalCollection());
	Out->SetArrayField(TEXT("slots"), VmhJson::NameArray(UMetaHumanCollectionBlueprintLibrary::GetSlotNames(Collection)));

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FMetaHumanPaletteItemKey& Key : UMetaHumanCollectionBlueprintLibrary::GetAllItemKeys(Collection))
	{
		Items.Add(MakeShared<FJsonValueObject>(ItemKeyToJson(Collection, Key)));
	}
	Out->SetArrayField(TEXT("items"), Items);

	TArray<TSharedPtr<FJsonValue>> Selections;
	for (const FMetaHumanPipelineSlotSelectionData& Data : Collection->GetMutableDefaultInstance()->GetSlotSelectionData())
	{
		TSharedPtr<FJsonObject> Item = VmhJson::Obj();
		Item->SetStringField(TEXT("slot"), Data.Selection.SlotName.ToString());
		Item->SetStringField(TEXT("item"), Data.Selection.SelectedItem.ToAssetNameString());
		Selections.Add(MakeShared<FJsonValueObject>(Item));
	}
	Out->SetArrayField(TEXT("selections"), Selections);
	Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::AddWardrobeItem(const FString& CharacterPath, const FString& SlotName, const FString& WardrobeItemPath, bool bSelect)
{
	UMetaHumanWardrobeItem* Item = VmhAssets::Load<UMetaHumanWardrobeItem>(WardrobeItemPath);
	if (!Item)
	{
		return VmhJson::Error(TEXT("WARDROBE_ITEM_NOT_FOUND"), FString::Printf(TEXT("No MetaHuman Wardrobe Item at %s (use list_wardrobe_items or EditorToolset.AssetToolset.FindAssets for MetaHumanWardrobeItem)"), *WardrobeItemPath));
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCollection* Collection = LiveCollection(Ctx.Subsystem, Ctx.Character);
	if (!Collection)
	{
		return VmhJson::Error(TEXT("NO_COLLECTION"), TEXT("Character has no internal collection"));
	}
	const FName Slot(*SlotName);
	FMetaHumanPaletteItemKey Key;
	TArray<FMetaHumanPaletteItemKey> Existing = UMetaHumanCollectionBlueprintLibrary::GetItemKeysForWardrobeItem(Collection, Item);
	if (Existing.Num() > 0)
	{
		Key = Existing[0];
	}
	else if (!Collection->TryAddItemFromWardrobeItem(Slot, Item, Key))
	{
		TArray<FName> Slots = UMetaHumanCollectionBlueprintLibrary::GetSlotNames(Collection);
		TArray<FString> SlotStrings;
		for (const FName& S : Slots) SlotStrings.Add(S.ToString());
		return VmhJson::Error(TEXT("ADD_ITEM_FAILED"), FString::Printf(TEXT("Could not add %s to slot '%s' (incompatible item or unknown slot). Slots: %s"), *Item->GetName(), *SlotName, *FString::Join(SlotStrings, TEXT(", "))));
	}

	if (bSelect)
	{
		Collection->GetMutableDefaultInstance()->SetSingleSlotSelection(Slot, Key);
	}
	if (bSelect || Existing.Num() == 0)
	{
		CommitWardrobeEdit(Ctx, Collection);
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("slot"), SlotName);
	Out->SetStringField(TEXT("wardrobe_item"), Item->GetPathName());
	Out->SetObjectField(TEXT("item_key"), ItemKeyToJson(Collection, Key));
	Out->SetBoolField(TEXT("selected"), bSelect);
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SelectWardrobeItem(const FString& CharacterPath, const FString& SlotName, const FString& WardrobeItemPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCollection* Collection = LiveCollection(Ctx.Subsystem, Ctx.Character);
	if (!Collection)
	{
		return VmhJson::Error(TEXT("NO_COLLECTION"), TEXT("Character has no internal collection"));
	}
	const FName Slot(*SlotName);
	FMetaHumanPaletteItemKey Key; // null key clears the slot
	if (!WardrobeItemPath.IsEmpty())
	{
		UMetaHumanWardrobeItem* Item = VmhAssets::Load<UMetaHumanWardrobeItem>(WardrobeItemPath);
		if (!Item)
		{
			return VmhJson::Error(TEXT("WARDROBE_ITEM_NOT_FOUND"), FString::Printf(TEXT("No MetaHuman Wardrobe Item at %s"), *WardrobeItemPath));
		}
		TArray<FMetaHumanPaletteItemKey> Keys = UMetaHumanCollectionBlueprintLibrary::GetItemKeysForWardrobeItem(Collection, Item);
		if (Keys.Num() == 0)
		{
			return VmhJson::Error(TEXT("ITEM_NOT_IN_COLLECTION"), TEXT("That wardrobe item is not in the character's collection yet; call add_wardrobe_item first"));
		}
		Key = Keys[0];
	}
	Collection->GetMutableDefaultInstance()->SetSingleSlotSelection(Slot, Key);
	CommitWardrobeEdit(Ctx, Collection);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("slot"), SlotName);
	Out->SetStringField(TEXT("item"), Key.IsNull() ? TEXT("") : Key.ToAssetNameString());
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::GetWardrobeItemParameters(const FString& CharacterPath, const FString& SlotName)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCollection* Collection = LiveCollection(Ctx.Subsystem, Ctx.Character);
	if (!Collection)
	{
		return VmhJson::Error(TEXT("NO_COLLECTION"), TEXT("Character has no internal collection"));
	}
	UMetaHumanInstance* Instance = Collection->GetMutableDefaultInstance();
	EnsureAssembled(Ctx, Instance);

	const TMap<FMetaHumanPaletteItemPath, FInstancedPropertyBag> Defaults = Instance->GetAssemblyParameters();
	const TMap<FMetaHumanPaletteItemPath, FInstancedPropertyBag> Overrides = Instance->GetOverriddenInstanceParameters();

	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FMetaHumanPipelineSlotSelectionData& Data : Instance->GetSlotSelectionData())
	{
		const FString Slot = Data.Selection.SlotName.ToString();
		const bool bWanted = SlotName.IsEmpty()
			? Data.Selection.SlotName != UE::MetaHuman::CharacterPipelineSlots::Character
			: Slot.Equals(SlotName, ESearchCase::IgnoreCase);
		if (Data.Selection.SelectedItem.IsNull() || !bWanted)
		{
			continue;
		}
		const FMetaHumanPaletteItemPath ItemPath = Data.Selection.GetSelectedItemPath();
		TSharedPtr<FJsonObject> Item = VmhJson::Obj();
		Item->SetStringField(TEXT("slot"), Slot);
		Item->SetStringField(TEXT("item"), Data.Selection.SelectedItem.ToAssetNameString());

		const FInstancedPropertyBag Current = Instance->GetCurrentInstanceParametersForItem(ItemPath);
		if (Current.IsValid() && Current.GetNumPropertiesInBag() > 0)
		{
			TSharedPtr<FJsonObject> Types;
			Item->SetObjectField(TEXT("parameters"), BagToJson(Current, &Types));
			Item->SetObjectField(TEXT("types"), Types);
		}
		else
		{
			Item->SetObjectField(TEXT("parameters"), VmhJson::Obj());
			Item->SetStringField(TEXT("note"), TEXT("no instance parameters for this item"));
		}
		if (const FInstancedPropertyBag* Default = Defaults.Find(ItemPath))
		{
			Item->SetObjectField(TEXT("defaults"), BagToJson(*Default));
		}
		const FInstancedPropertyBag* Override = Overrides.Find(ItemPath);
		Item->SetArrayField(TEXT("overridden"), VmhJson::StringArray(Override ? BagNames(*Override) : TArray<FString>()));
		Items.Add(MakeShared<FJsonValueObject>(Item));
	}
	if (Items.Num() == 0 && !SlotName.IsEmpty())
	{
		return VmhJson::Error(TEXT("NOTHING_SELECTED"), FString::Printf(TEXT("Nothing is selected in slot '%s' (get_wardrobe lists the selections)"), *SlotName));
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetArrayField(TEXT("items"), Items);
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SetWardrobeItemParameters(const FString& CharacterPath, const FString& SlotName, const FString& ParametersJson, const FString& ItemName, bool bClearOverrides)
{
	TSharedPtr<FJsonObject> Patch;
	if (!ParametersJson.TrimStartAndEnd().IsEmpty())
	{
		FString ParseError;
		Patch = VmhJson::Parse(ParametersJson, ParseError);
		if (!Patch.IsValid())
		{
			return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
		}
	}
	if (!Patch.IsValid() && !bClearOverrides)
	{
		return VmhJson::Error(TEXT("NOTHING_TO_DO"), TEXT("Pass ParametersJson (e.g. {\"Melanin\": 0.48, \"Redness\": 0.53}) and/or bClearOverrides=true"));
	}

	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UMetaHumanCollection* Collection = LiveCollection(Ctx.Subsystem, Ctx.Character);
	if (!Collection)
	{
		return VmhJson::Error(TEXT("NO_COLLECTION"), TEXT("Character has no internal collection"));
	}
	UMetaHumanInstance* Instance = Collection->GetMutableDefaultInstance();
	FMetaHumanPipelineSlotSelection Selection;
	FString Error;
	if (!FindSlotSelection(Instance, SlotName, ItemName, Selection, Error))
	{
		return VmhJson::Error(TEXT("NOTHING_SELECTED"), Error);
	}
	EnsureAssembled(Ctx, Instance);
	const FMetaHumanPaletteItemPath ItemPath = Selection.GetSelectedItemPath();
	const FString ItemAssetName = Selection.SelectedItem.ToAssetNameString();

	if (bClearOverrides)
	{
		Instance->ClearOverriddenInstanceParameters(ItemPath);
	}

	FInstancedPropertyBag Bag = Instance->GetCurrentInstanceParametersForItem(ItemPath);
	if (!Bag.IsValid() || Bag.GetNumPropertiesInBag() == 0)
	{
		return VmhJson::Error(TEXT("NO_PARAMETERS"), FString::Printf(TEXT("%s in slot '%s' has no instance parameters (or the preview did not assemble; see the Output Log)"),
			*ItemAssetName, *SlotName));
	}

	TArray<FString> Changed;
	if (Patch.IsValid())
	{
		const UPropertyBag* BagStruct = Bag.GetPropertyBagStruct();
		void* BagMemory = Bag.GetMutableValue().GetMemory();
		for (const TPair<FString, TSharedPtr<FJsonValue>>& Pair : Patch->Values)
		{
			FProperty* Prop = VmhJson::FindPropertyLoose(BagStruct, Pair.Key);
			if (!Prop)
			{
				return VmhJson::Error(TEXT("UNKNOWN_PARAMETER"), FString::Printf(TEXT("%s has no parameter '%s'. Parameters: %s"),
					*ItemAssetName, *Pair.Key, *FString::Join(BagNames(Bag), TEXT(", "))));
			}
			TSharedPtr<FJsonValue> Value = Pair.Value;
			// colours as [r, g, b] or [r, g, b, a] as well as {"R": ...} or "#RRGGBB"
			const FStructProperty* StructProp = CastField<FStructProperty>(Prop);
			if (StructProp && StructProp->Struct == TBaseStructure<FLinearColor>::Get() && Value->Type == EJson::Array)
			{
				const TArray<TSharedPtr<FJsonValue>>& A = Value->AsArray();
				if (A.Num() < 3)
				{
					return VmhJson::Error(TEXT("INVALID_VALUE"), FString::Printf(TEXT("'%s' needs [r, g, b] or [r, g, b, a]"), *Pair.Key));
				}
				TSharedPtr<FJsonObject> Color = VmhJson::Obj();
				Color->SetNumberField(TEXT("R"), A[0]->AsNumber());
				Color->SetNumberField(TEXT("G"), A[1]->AsNumber());
				Color->SetNumberField(TEXT("B"), A[2]->AsNumber());
				Color->SetNumberField(TEXT("A"), A.Num() > 3 ? A[3]->AsNumber() : 1.0);
				Value = MakeShared<FJsonValueObject>(Color);
			}
			if (!FJsonObjectConverter::JsonValueToUProperty(Value, Prop, Prop->ContainerPtrToValuePtr<void>(BagMemory), 0, 0))
			{
				return VmhJson::Error(TEXT("INVALID_VALUE"), FString::Printf(TEXT("Could not set '%s' from the value given for '%s'"), *Prop->GetName(), *Pair.Key));
			}
			Changed.Add(Prop->GetName());
		}
	}

	const EMetaHumanInstanceParameterOverrideResult Result = Changed.Num() > 0
		? Instance->OverrideInstanceParameters(ItemPath, Bag)
		: EMetaHumanInstanceParameterOverrideResult::NoParametersWereModified;
	CommitWardrobeEdit(Ctx, Collection);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("slot"), Selection.SlotName.ToString());
	Out->SetStringField(TEXT("item"), ItemAssetName);
	Out->SetArrayField(TEXT("changed"), VmhJson::StringArray(Changed));
	Out->SetBoolField(TEXT("cleared_overrides"), bClearOverrides);
	Out->SetBoolField(TEXT("reassembly_required"), EnumHasAnyFlags(Result, EMetaHumanInstanceParameterOverrideResult::ReassemblyRequiredToApplyNewParameters));
	// the selection's item path is re-resolved on the live collection: the commit may have rebuilt the preview
	UMetaHumanInstance* Live = LiveCollection(Ctx.Subsystem, Ctx.Character)->GetMutableDefaultInstance();
	FMetaHumanPipelineSlotSelection After;
	FString Unused;
	if (FindSlotSelection(Live, SlotName, ItemAssetName, After, Unused))
	{
		Out->SetObjectField(TEXT("parameters"), BagToJson(Live->GetCurrentInstanceParametersForItem(After.GetSelectedItemPath())));
	}
	Out->SetStringField(TEXT("next"), TEXT("save the character (save routes to save_deferred while editing); a build carries these into the Blueprint"));
	return CharacterResult(Ctx, Out);
}

// =====================================================================
// Assembly and placement
// =====================================================================

FString UMetaHumanCharacterService::AssembleForPreview(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	Ctx.Subsystem->RunCharacterEditorPipelineForPreview(Ctx.Character);
	return CharacterResult(Ctx);
}

FString UMetaHumanCharacterService::CanBuild(const FString& CharacterPath)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FText Reason;
	const bool bCan = Ctx.Subsystem->CanBuildMetaHuman(Ctx.Character, Reason);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("can_build"), bCan);
	if (!bCan)
	{
		Out->SetStringField(TEXT("reason"), Reason.ToString());
	}
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::Build(const FString& CharacterPath, const FString& PipelineType, const FString& Quality, const FString& BuildPath, const FString& NameOverride, bool bPreserveBlueprintCustomization)
{
	const UEnum* PipelineEnum = StaticEnum<EMetaHumanDefaultPipelineType>();
	const UEnum* QualityEnum = StaticEnum<EMetaHumanQualityLevel>();
	const int64 PipelineValue = PipelineType.IsEmpty() ? static_cast<int64>(EMetaHumanDefaultPipelineType::Cinematic) : VmhJson::ParseEnum(PipelineEnum, PipelineType);
	const int64 QualityValue = Quality.IsEmpty() ? static_cast<int64>(EMetaHumanQualityLevel::Cinematic) : VmhJson::ParseEnum(QualityEnum, Quality);
	if (PipelineValue == INDEX_NONE)
	{
		return VmhJson::Error(TEXT("BAD_PIPELINE"), FString::Printf(TEXT("PipelineType must be one of: %s"), *VmhJson::EnumNames(PipelineEnum)));
	}
	if (QualityValue == INDEX_NONE)
	{
		return VmhJson::Error(TEXT("BAD_QUALITY"), FString::Printf(TEXT("Quality must be one of: %s"), *VmhJson::EnumNames(QualityEnum)));
	}

	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	FText Reason;
	if (!Ctx.Subsystem->CanBuildMetaHuman(Ctx.Character, Reason))
	{
		return VmhJson::Error(TEXT("CANNOT_BUILD"), Reason.ToString());
	}

	FMetaHumanCharacterEditorBuildParameters Params;
	Params.PipelineType = static_cast<EMetaHumanDefaultPipelineType>(PipelineValue);
	Params.PipelineQuality = static_cast<EMetaHumanQualityLevel>(QualityValue);
	Params.AbsoluteBuildPath = BuildPath;
	Params.NameOverride = NameOverride;

	const FString Root = BuildPath.IsEmpty() ? TEXT("/Game/MetaHumans") : BuildPath;
	const FString Leaf = NameOverride.IsEmpty() ? Ctx.Character->GetName() : NameOverride;

	// the build regenerates an existing Blueprint: record each one first when asked to keep its customization
	TArray<TPair<FString, TSharedPtr<FJsonObject>>> Snapshots;
	if (bPreserveBlueprintCustomization)
	{
		for (const FString& BlueprintPath : VmhAssets::FindAssetsOfClass(TEXT("/Script/Engine.Blueprint"), Root / Leaf))
		{
			if (UBlueprint* Existing = VmhAssets::Load<UBlueprint>(BlueprintPath))
			{
				Snapshots.Emplace(BlueprintPath, VmhBlueprintSnapshot::Take(Existing));
			}
		}
	}

	const double Start = FPlatformTime::Seconds();
	Ctx.Subsystem->BuildMetaHuman(Ctx.Character, Params);
	const double Seconds = FPlatformTime::Seconds() - Start;

	TArray<FString> Blueprints = VmhAssets::FindAssetsOfClass(TEXT("/Script/Engine.Blueprint"), Root / Leaf);
	if (Blueprints.Num() == 0)
	{
		Blueprints = VmhAssets::FindAssetsOfClass(TEXT("/Script/Engine.Blueprint"), Root);
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("pipeline_type"), PipelineEnum->GetNameStringByValue(PipelineValue));
	Out->SetStringField(TEXT("quality"), QualityEnum->GetNameStringByValue(QualityValue));
	Out->SetStringField(TEXT("output_folder"), Root / Leaf);
	Out->SetNumberField(TEXT("seconds"), Seconds);
	Out->SetArrayField(TEXT("blueprints"), VmhJson::StringArray(Blueprints));

	if (bPreserveBlueprintCustomization)
	{
		TArray<TSharedPtr<FJsonValue>> Preserved;
		for (const TPair<FString, TSharedPtr<FJsonObject>>& Snap : Snapshots)
		{
			UBlueprint* Rebuilt = VmhAssets::Load<UBlueprint>(Snap.Key);
			FString RestoreError;
			const TSharedPtr<FJsonObject> Report = Rebuilt ? VmhBlueprintSnapshot::Restore(Rebuilt, Snap.Value, VmhBlueprintSnapshot::FRestoreOptions(), RestoreError) : nullptr;
			if (Report.IsValid())
			{
				Preserved.Add(MakeShared<FJsonValueObject>(Report));
			}
			else
			{
				Preserved.Add(MakeShared<FJsonValueString>(Snap.Key + TEXT(": ") + (Rebuilt ? RestoreError : TEXT("no Blueprint at this path after the build"))));
			}
		}
		Out->SetArrayField(TEXT("preserved"), Preserved);
	}

	// a build leaves dirty T_PreBakedGroom_* packages that were never written to disk; saving them fails (2026-11-12)
	TArray<UPackage*> Dirty;
	FEditorFileUtils::GetDirtyContentPackages(Dirty);
	TArray<FString> Ghosts;
	for (const UPackage* Package : Dirty)
	{
		const FString Name = Package ? Package->GetName() : FString();
		if (Name.StartsWith(Root / Leaf) && Name.Contains(TEXT("T_PreBakedGroom")) && !FPackageName::DoesPackageExist(Name))
		{
			Ghosts.Add(Name);
		}
	}
	if (Ghosts.Num() > 0)
	{
		Out->SetArrayField(TEXT("unsaved_ghost_packages"), VmhJson::StringArray(Ghosts));
		Out->SetStringField(TEXT("ghost_hint"), TEXT("leave these unsaved (their save fails): they clear on a level change plus garbage collection; save the other dirty packages by name"));
	}
	Out->SetStringField(TEXT("hint"), TEXT("Place the Blueprint in a level; its Live Link / animation inputs are on the actor. Save all packages afterwards."));
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::BuildAsync(const FString& CharacterPath, const FString& PipelineType, const FString& Quality, const FString& BuildPath, const FString& NameOverride, bool bPreserveBlueprintCustomization)
{
	// validate up front, exactly like build(), so a bad request fails now rather than on the deferred tick
	const UEnum* PipelineEnum = StaticEnum<EMetaHumanDefaultPipelineType>();
	const UEnum* QualityEnum = StaticEnum<EMetaHumanQualityLevel>();
	if (!PipelineType.IsEmpty() && VmhJson::ParseEnum(PipelineEnum, PipelineType) == INDEX_NONE)
	{
		return VmhJson::Error(TEXT("BAD_PIPELINE"), FString::Printf(TEXT("PipelineType must be one of: %s"), *VmhJson::EnumNames(PipelineEnum)));
	}
	if (!Quality.IsEmpty() && VmhJson::ParseEnum(QualityEnum, Quality) == INDEX_NONE)
	{
		return VmhJson::Error(TEXT("BAD_QUALITY"), FString::Printf(TEXT("Quality must be one of: %s"), *VmhJson::EnumNames(QualityEnum)));
	}
	FString LoadErr;
	UMetaHumanCharacter* Character = LoadCharacter(CharacterPath, LoadErr);
	if (!Character)
	{
		return VmhJson::Error(TEXT("CHARACTER_NOT_FOUND"), LoadErr);
	}
	FText Reason;
	UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
	if (Subsystem && !Subsystem->CanBuildMetaHuman(Character, Reason))
	{
		return VmhJson::Error(TEXT("CANNOT_BUILD"), Reason.ToString());
	}
	const FString Path = Character->GetPathName();

	// never a second build on top of one that is queued or running (a retry after a client timeout would otherwise run a
	// second multi-minute build into the same Blueprint)
	if (const FBuildAsyncState* Busy = GPendingBuilds.Find(Path))
	{
		if (Busy->Status == TEXT("queued") || Busy->Status == TEXT("running"))
		{
			return VmhJson::Error(TEXT("ALREADY_BUILDING"), FString::Printf(TEXT("a build of this character is %s; poll get_build_status"), *Busy->Status));
		}
	}

	FBuildAsyncState& State = GPendingBuilds.Add(Path);
	State = FBuildAsyncState();
	State.OutputFolder = BuildPath.IsEmpty() ? TEXT("/Game/MetaHumans") : BuildPath;

	FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateLambda(
		[Path, PipelineType, Quality, BuildPath, NameOverride, bPreserveBlueprintCustomization](float) -> bool
		{
			FBuildAsyncState* S = GPendingBuilds.Find(Path);
			if (!S)
			{
				return false;
			}
			S->Status = TEXT("running");
			const double Start = FPlatformTime::Seconds();
			const FString Result = UMetaHumanCharacterService::Build(Path, PipelineType, Quality, BuildPath, NameOverride, bPreserveBlueprintCustomization);
			S = GPendingBuilds.Find(Path); // the map may have grown during the build (another character's request)
			if (!S)
			{
				return false;
			}
			S->Seconds = FPlatformTime::Seconds() - Start;
			FString ParseErr;
			TSharedPtr<FJsonObject> Parsed = VmhJson::Parse(Result, ParseErr);
			bool bOk = false;
			if (Parsed.IsValid())
			{
				Parsed->TryGetBoolField(TEXT("success"), bOk);
			}
			S->Status = bOk ? TEXT("done") : TEXT("failed");
			if (Parsed.IsValid())
			{
				if (!bOk) { Parsed->TryGetStringField(TEXT("error"), S->Error); }
				Parsed->TryGetStringField(TEXT("output_folder"), S->OutputFolder);
				const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
				if (Parsed->TryGetArrayField(TEXT("blueprints"), Arr))
				{
					for (const TSharedPtr<FJsonValue>& V : *Arr) { S->Blueprints.Add(V->AsString()); }
				}
			}
			else
			{
				S->Error = TEXT("build() returned unparseable JSON");
			}
			return false; // run once
		}), 0.f);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Path);
	Out->SetStringField(TEXT("status"), TEXT("queued"));
	Out->SetStringField(TEXT("hint"), TEXT("poll get_build_status(character_path); the build itself still blocks the game thread for several minutes once it starts"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::GetBuildStatus(const FString& CharacterPath)
{
	FString LoadErr;
	UMetaHumanCharacter* Character = LoadCharacter(CharacterPath, LoadErr);
	const FString Path = Character ? Character->GetPathName() : CharacterPath;
	const FBuildAsyncState* State = GPendingBuilds.Find(Path);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("character"), Path);
	if (!State)
	{
		Out->SetStringField(TEXT("status"), TEXT("not_requested"));
		return VmhJson::Ok(Out);
	}
	Out->SetStringField(TEXT("status"), State->Status);
	Out->SetNumberField(TEXT("seconds"), State->Seconds);
	Out->SetStringField(TEXT("output_folder"), State->OutputFolder);
	Out->SetArrayField(TEXT("blueprints"), VmhJson::StringArray(State->Blueprints));
	if (!State->Error.IsEmpty())
	{
		Out->SetStringField(TEXT("error"), State->Error);
	}
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::SpawnInLevel(const FString& CharacterPath, FVector Location, FRotator Rotation, bool bKeepTransient)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	AActor* Actor = Ctx.Subsystem->SpawnMetaHumanActor(Ctx.Character, bKeepTransient);
	if (!Actor)
	{
		return VmhJson::Error(TEXT("SPAWN_FAILED"), TEXT("SpawnMetaHumanActor returned null (no editor world?)"));
	}
	Actor->SetActorLocationAndRotation(Location, Rotation);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("actor"), Actor->GetPathName());
#if WITH_EDITOR
	Out->SetStringField(TEXT("label"), Actor->GetActorLabel());
#endif
	Out->SetStringField(TEXT("class"), Actor->GetClass()->GetName());
	Out->SetBoolField(TEXT("transient"), bKeepTransient);
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::ExportDna(const FString& CharacterPath, const FString& ProjectPath, const FString& ExternalPath, bool bHead, bool bBody)
{
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	UClass* Library = VmhJson::FindClass(TEXT("/Script/MetaHumanCharacterEditor.MetaHumanCharacterExportBlueprintLibrary"));
	if (!Library)
	{
		return VmhJson::Error(TEXT("NO_EXPORT_LIBRARY"), TEXT("MetaHumanCharacterExportBlueprintLibrary not found"));
	}
	TSharedPtr<FJsonObject> Params = VmhJson::Obj();
	Params->SetStringField(TEXT("ProjectPath"), ProjectPath);
	Params->SetStringField(TEXT("ExternalPath"), ExternalPath);
	Params->SetBoolField(TEXT("bDNAHead"), bHead);
	Params->SetBoolField(TEXT("bDNABody"), bBody);
	Params->SetBoolField(TEXT("bOverwriteExistingAssets"), true);
	TSharedPtr<FJsonObject> Args = VmhJson::Obj();
	Args->SetStringField(TEXT("InCharacter"), Ctx.Character->GetPathName());
	Args->SetObjectField(TEXT("InParams"), Params);
	TSharedPtr<FJsonObject> Result;
	FString Error;
	if (!VmhJson::CallStaticFunction(Library, TEXT("ExportDNA"), Args, Result, Error))
	{
		return VmhJson::Error(TEXT("EXPORT_FAILED"), Error);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("project_path"), ProjectPath);
	Out->SetStringField(TEXT("external_path"), ExternalPath);
	return CharacterResult(Ctx, Out);
}

// =====================================================================
// Custom mesh to MetaHuman (2026-10-04): what the Mesh Import tool does behind "Import > from custom mesh",
// without its viewport. The recipe is the tool's own (MetaHumanCharacterEditorMeshImportTool.cpp,
// MetaHumanCharacterEditorMeshTargetContourMechanic.cpp): the target's vertices and triangles through
// GetMeshDataForConforming, the head framed from the mesh's top 15%, a front-on render whose face landmarks are
// tracked and stored on the character, then ConformToTargetMeshes with the "combined" pipeline.
// =====================================================================

namespace
{
	/** Load a static or skeletal mesh asset (anything else is an error). */
	UObject* LoadMeshAsset(const FString& Path, FString& OutError)
	{
		UObject* Object = VmhAssets::Load(Path);
		if (!Object)
		{
			OutError = FString::Printf(TEXT("No mesh asset at %s"), *Path);
			return nullptr;
		}
		if (!Object->IsA<UStaticMesh>() && !Object->IsA<USkeletalMesh>())
		{
			OutError = FString::Printf(TEXT("%s is a %s, not a static or skeletal mesh"), *Path, *Object->GetClass()->GetName());
			return nullptr;
		}
		return Object;
	}

	/** The key the character stores the conform's state under: the combined mesh, or body + head. */
	bool BuildTargetMeshKey(const FString& MeshPath, const FString& HeadMeshPath, FMetaHumanCharacterTargetMeshKey& OutKey, UObject*& OutMesh, UObject*& OutHead, FString& OutError)
	{
		OutMesh = LoadMeshAsset(MeshPath, OutError);
		if (!OutMesh)
		{
			return false;
		}
		OutHead = nullptr;
		if (!HeadMeshPath.IsEmpty())
		{
			OutHead = LoadMeshAsset(HeadMeshPath, OutError);
			if (!OutHead)
			{
				return false;
			}
			OutKey.BodyMesh = OutMesh;
			OutKey.HeadMesh = OutHead;
		}
		else
		{
			OutKey.CombinedMesh = OutMesh;
		}
		return true;
	}

	FBox BoundsOf(const TArray<FVector3f>& Vertices)
	{
		FBox Box(ForceInit);
		for (const FVector3f& V : Vertices)
		{
			Box += FVector(V);
		}
		return Box;
	}

	/** Which way the mesh faces, read off the feet: the toes stand ahead of the ankles (Master Smith's mh_conform heuristic). */
	FVector FacingOf(const TArray<FVector3f>& Vertices, const FBox& Bounds)
	{
		const double H = Bounds.Max.Z - Bounds.Min.Z;
		FVector Toes(0), Ankles(0);
		int32 NToes = 0, NAnkles = 0;
		for (const FVector3f& V : Vertices)
		{
			if (V.Z < Bounds.Min.Z + 0.04 * H) { Toes += FVector(V); ++NToes; }
			else if (V.Z > Bounds.Min.Z + 0.08 * H && V.Z < Bounds.Min.Z + 0.12 * H) { Ankles += FVector(V); ++NAnkles; }
		}
		if (NToes < 10 || NAnkles < 10)
		{
			return FVector(0, 1, 0);
		}
		FVector F = Toes / NToes - Ankles / NAnkles;
		F.Z = 0;
		return F.SizeSquared() > KINDA_SMALL_NUMBER ? F.GetSafeNormal() : FVector(0, 1, 0);
	}

	/** The tool's head box on a combined mesh: the top 15% of the height, tightened to 60% sideways. */
	FBox HeadBoundsOf(const FBox& MeshBounds)
	{
		const double H = MeshBounds.Max.Z - MeshBounds.Min.Z;
		FBox Head(FVector(MeshBounds.Min.X, MeshBounds.Min.Y, MeshBounds.Max.Z - H * 0.15), MeshBounds.Max);
		const FVector C = Head.GetCenter();
		FVector E = Head.GetExtent();
		E.X *= 0.6; E.Y *= 0.6;
		return FBox(C - E, C + E);
	}

	/**
	 * The face contour tracker run as Epic's Mesh Import tool runs it (MetaHumanCharacterEditorLandmarkTracker::TrackImage):
	 * the Hyprsense pipeline on the render, and every contour's DENSE points handed to the solver. The subsystem's
	 * TrackFaceLandmarksFromImage returns the curve controller's visible-curve points instead, and a solve fed those
	 * stood the posed body 19 cm behind the mesh, then yawed it 28 degrees (the AINavigator, 2026-10-05).
	 */
	bool TrackDenseContours(const TArray<FColor>& Pixels, int32 Width, int32 Height, TMap<FString, FTrackingPoints>& OutCurves, FString& OutError)
	{
		OutCurves.Reset();
		UMetaHumanFaceContourTrackerAsset* Tracker = UMetaHumanFaceContourTrackerAsset::LoadDefaultTracker();
		if (!Tracker || !Tracker->LoadTrackersSynchronous() || !Tracker->CanProcess())
		{
			OutError = TEXT("the default face contour tracker asset could not load its models");
			return false;
		}
		UE::MetaHuman::Pipeline::FPipeline Pipeline;
		TSharedPtr<UE::MetaHuman::Pipeline::FFColorToUEImageNode> Image = Pipeline.MakeNode<UE::MetaHuman::Pipeline::FFColorToUEImageNode>("RenderTarget");
		TSharedPtr<UE::MetaHuman::Pipeline::FHyprsenseNode> Generic = Pipeline.MakeNode<UE::MetaHuman::Pipeline::FHyprsenseNode>("GenericTracker");
		Image->Samples = Pixels;
		Image->Width = Width;
		Image->Height = Height;
		PRAGMA_DISABLE_DEPRECATION_WARNINGS
		#define VMH_EFFECTIVE_TRACKER(T) TSharedPtr<UE::NNE::IModelInstanceRunSync> T = Tracker->T.IsValid() ? Tracker->T : Tracker->T##Model;
			VMH_EFFECTIVE_TRACKER(FullFaceTracker)
			VMH_EFFECTIVE_TRACKER(FaceDetector)
			VMH_EFFECTIVE_TRACKER(BrowsDenseTracker)
			VMH_EFFECTIVE_TRACKER(EyesDenseTracker)
			VMH_EFFECTIVE_TRACKER(MouthDenseTracker)
			VMH_EFFECTIVE_TRACKER(LipzipDenseTracker)
			VMH_EFFECTIVE_TRACKER(NasioLabialsDenseTracker)
			VMH_EFFECTIVE_TRACKER(ChinDenseTracker)
			VMH_EFFECTIVE_TRACKER(TeethDenseTracker)
			VMH_EFFECTIVE_TRACKER(TeethConfidenceTracker)
		#undef VMH_EFFECTIVE_TRACKER
		PRAGMA_ENABLE_DEPRECATION_WARNINGS
		if (!Generic->SetTrackers(FullFaceTracker, FaceDetector, BrowsDenseTracker, EyesDenseTracker, MouthDenseTracker, LipzipDenseTracker,
		                          NasioLabialsDenseTracker, ChinDenseTracker, TeethDenseTracker, TeethConfidenceTracker))
		{
			OutError = Generic->GetErrorMessage();
			return false;
		}
		Pipeline.MakeConnection(Image, Generic);
		FFrameTrackingContourData Result;
		bool bOk = true;
		UE::MetaHuman::Pipeline::FFrameComplete OnFrame;
		UE::MetaHuman::Pipeline::FProcessComplete OnDone;
		const FString Pin = Generic->Name + TEXT(".Contours Out");
		OnFrame.AddLambda([Pin, &Result](TSharedPtr<UE::MetaHuman::Pipeline::FPipelineData> Data)
		{
			Result = Data->GetData<FFrameTrackingContourData>(Pin);
		});
		OnDone.AddLambda([&bOk](TSharedPtr<UE::MetaHuman::Pipeline::FPipelineData> Data)
		{
			bOk = Data->GetExitStatus() == UE::MetaHuman::Pipeline::EPipelineExitStatus::Ok;
		});
		UE::MetaHuman::Pipeline::FPipelineRunParameters Run;
		Run.SetStartFrame(0);
		Run.SetEndFrame(1);
		Run.SetOnFrameComplete(OnFrame);
		Run.SetOnProcessComplete(OnDone);
		Run.SetGpuToUse(UE::MetaHuman::Pipeline::FPipeline::PickPhysicalDevice());
		Run.SetMode(UE::MetaHuman::Pipeline::EPipelineMode::PushSyncNodes);
		Pipeline.Run(Run);
		if (!bOk || !Result.ContainsData())
		{
			OutError = bOk ? TEXT("the tracker found no face") : TEXT("the tracking pipeline failed");
			return false;
		}
		for (const TPair<FString, FTrackingContour>& Pair : Result.TrackingContours)
		{
			if (Pair.Value.DensePoints.Num() == 0)
			{
				continue;
			}
			FTrackingPoints Points;
			Points.TrackingPoints = Pair.Value.DensePoints;
			OutCurves.Add(Pair.Key, Points);
		}
		return OutCurves.Num() > 0;
	}

	/**
	 * Render the target mesh's head front-on in a private preview scene and track the face landmarks, as the tool's
	 * contour mechanic does with auto framing. Fills the tracking results (camera, image size, curves).
	 */
	bool TrackFaceOnMesh(UMetaHumanCharacterEditorSubsystem* Subsystem, UObject* Mesh, const TArray<FVector3f>& Vertices, const TArray<int32>& Triangles,
	                     const FBox& HeadBounds, const FVector& Facing, int32 ImageSize,
	                     FMetaHumanCharacterTargetTrackingResults& OutResults, TMap<FString, FTrackingPoints>& OutCurves, TSharedPtr<FJsonObject> Report, FString& OutError)
	{
		const FVector HeadCenter = HeadBounds.GetCenter();
		const float HeadSize = HeadBounds.GetExtent().Size();
		if (HeadSize <= KINDA_SMALL_NUMBER)
		{
			OutError = TEXT("degenerate head bounds");
			return false;
		}
		// the tool's framing: 1.8 head sizes away along the facing, raised a fifth of the head, the head half the frame
		const float CameraDistance = HeadSize * 1.8f;
		const FVector CameraPosition = HeadCenter + Facing * CameraDistance + FVector(0, 0, HeadSize * 0.2f);
		const FRotator CameraRotation = (HeadCenter - CameraPosition).Rotation();
		const float FOVDegrees = FMath::Clamp(FMath::RadiansToDegrees(2.0f * FMath::Atan(HeadSize / (CameraDistance * 2.f))), 35.0f, 110.0f);

		// what the tracker is given, success or not
		TSharedPtr<FJsonObject> Track = VmhJson::Obj();
		Track->SetStringField(TEXT("head_center_cm"), HeadCenter.ToString());
		Track->SetNumberField(TEXT("head_size_cm"), HeadSize);
		Track->SetStringField(TEXT("camera_cm"), CameraPosition.ToString());
		Track->SetStringField(TEXT("camera_rotation"), CameraRotation.ToString());
		Track->SetNumberField(TEXT("fov_deg"), FOVDegrees);
		Track->SetNumberField(TEXT("image_size"), ImageSize);
		if (Report.IsValid())
		{
			Report->SetObjectField(TEXT("face_tracking"), Track);
		}

		// lit from the camera, softly: the default key from above left the face in hard shadow (2026-10-04)
		// lit from the camera, softly (2.5 / 1.5 blew a white default material out to a flat mask, 2026-10-05)
		FPreviewScene Scene{ FPreviewScene::ConstructionValues().SetSkyBrightness(0.6f).SetLightBrightness(1.2f) };
		Scene.SetLightDirection(CameraRotation);
		// The mesh with its own textures when it can be rendered as it is: a face painted on the seed gives the tracker
		// eyes and brows. A Nanite static mesh cannot (the capture draws its 40-face fallback), so it goes in as a
		// dynamic mesh of the conform's own vertices, as the Mesh Import tool renders, in the tool's skin material.
		UStaticMesh* StaticMesh = Cast<UStaticMesh>(Mesh);
		USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Mesh);
		UMeshComponent* MeshComponent = nullptr;
		FString RenderedAs;
		if (StaticMesh && !StaticMesh->IsNaniteEnabled())
		{
			UStaticMeshComponent* C = NewObject<UStaticMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			C->SetStaticMesh(StaticMesh);
			C->SetForcedLodModel(1);
			MeshComponent = C;
			RenderedAs = TEXT("static mesh, its own materials, LOD0");
		}
		else if (SkeletalMesh)
		{
			USkeletalMeshComponent* C = NewObject<USkeletalMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			C->SetSkeletalMeshAsset(SkeletalMesh);
			C->SetForcedLOD(1);
			MeshComponent = C;
			RenderedAs = TEXT("skeletal mesh, its own materials, LOD0");
		}
		else
		{
			UE::Geometry::FDynamicMesh3 DynamicMesh;
			DynamicMesh.EnableVertexNormals(FVector3f::UnitZ());
			for (const FVector3f& V : Vertices)
			{
				DynamicMesh.AppendVertex(FVector3d(V));
			}
			for (int32 i = 0; i + 2 < Triangles.Num(); i += 3)
			{
				DynamicMesh.AppendTriangle(Triangles[i], Triangles[i + 1], Triangles[i + 2]);
			}
			UE::Geometry::FMeshNormals::QuickComputeVertexNormals(DynamicMesh);
			UDynamicMeshComponent* C = NewObject<UDynamicMeshComponent>(GetTransientPackage(), NAME_None, RF_Transient);
			C->SetMesh(MoveTemp(DynamicMesh));
			MeshComponent = C;
			RenderedAs = TEXT("dynamic mesh of the conform vertices (a Nanite static mesh), the tool's skin material");
		}
		Scene.AddComponent(MeshComponent, FTransform::Identity);
		UMaterialInterface* Skin = LoadObject<UMaterialInterface>(nullptr, TEXT("/MetaHumanCharacter/Materials/MI_M2M_Head.MI_M2M_Head"));
		if (!Skin)
		{
			Skin = UMaterial::GetDefaultMaterial(MD_Surface);
		}
		for (int32 i = 0; i < FMath::Max(1, MeshComponent->GetNumMaterials()); ++i)
		{
			if (!MeshComponent->GetMaterial(i) || Cast<UDynamicMeshComponent>(MeshComponent))
			{
				MeshComponent->SetMaterial(i, Skin);
			}
		}
		Track->SetStringField(TEXT("rendered_as"), RenderedAs);
		Track->SetNumberField(TEXT("render_triangles"), Triangles.Num() / 3);

		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		RenderTarget->RenderTargetFormat = RTF_RGBA8;
		RenderTarget->InitAutoFormat(ImageSize, ImageSize);
		RenderTarget->UpdateResourceImmediate(false);
		USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Capture->TextureTarget = RenderTarget;
		Capture->CaptureSource = ESceneCaptureSource::SCS_FinalToneCurveHDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->ProjectionType = ECameraProjectionMode::Perspective;
		Capture->FOVAngle = FOVDegrees;
		Scene.AddComponent(Capture, FTransform(CameraRotation, CameraPosition));
		Capture->CaptureScene();

		TArray<FColor> Pixels;
		if (!UKismetRenderingLibrary::ReadRenderTarget(Capture, RenderTarget, Pixels) || Pixels.Num() != ImageSize * ImageSize)
		{
			OutError = TEXT("could not read the face render");
			return false;
		}
		// the render on disk, success or not: the agent reads it when the tracker finds no face
		const FString DumpPath = FPaths::ProjectSavedDir() / TEXT("VibeMetaHumans") / FString::Printf(TEXT("FaceTrack_%s.png"), *Mesh->GetName());
		{
			TArray64<uint8> Png;
			FImageUtils::PNGCompressImageArray(ImageSize, ImageSize, Pixels, Png);
			IFileManager::Get().MakeDirectory(*FPaths::GetPath(DumpPath), true);
			FFileHelper::SaveArrayToFile(Png, *DumpPath);
			Track->SetStringField(TEXT("render"), DumpPath);
			if (Report.IsValid())
			{
				Report->SetStringField(TEXT("face_render"), DumpPath);
			}
		}
		FString DenseError;
		if (TrackDenseContours(Pixels, ImageSize, ImageSize, OutCurves, DenseError))
		{
			Track->SetStringField(TEXT("tracked_with"), TEXT("dense contours (the Mesh Import tool's path)"));
		}
		else
		{
			Track->SetStringField(TEXT("tracked_with"), FString::Printf(TEXT("the subsystem's visible-curve points (dense contours failed: %s)"), *DenseError));
			if (!Subsystem->TrackFaceLandmarksFromImage(Pixels, ImageSize, ImageSize, OutCurves) || OutCurves.IsEmpty())
			{
				OutError = FString::Printf(TEXT("the face landmark tracker found no face in the front-on render %s (is the head bald and facing +Y, the mesh in centimetres with its feet on z=0?)"), *DumpPath);
				return false;
			}
		}

		FMinimalViewInfo ViewInfo;
		Capture->GetCameraView(0.0f, ViewInfo);
		ViewInfo.AspectRatio = 1.0f;
		OutResults.CameraViewInfo = ViewInfo;
		OutResults.ImageSize = FIntPoint(ImageSize, ImageSize);
		OutResults.CurveTrackingPoints.Reset();
		int32 PointCount = 0;
		for (const TPair<FString, FTrackingPoints>& Pair : OutCurves)
		{
			FMetaHumanCharacterCurveTrackingPoints Curve;
			Curve.Points = Pair.Value.TrackingPoints;
			PointCount += Curve.Points.Num();
			OutResults.CurveTrackingPoints.Add(Pair.Key, Curve);
		}
		Track->SetNumberField(TEXT("curves"), OutCurves.Num());
		Track->SetNumberField(TEXT("points"), PointCount);
		return true;
	}
}

FString UMetaHumanCharacterService::ImportFromCustomMesh(const FString& CharacterPath, const FString& MeshPath, const FString& HeadMeshPath, bool bAutoSolve, bool bTrackFace, int32 TrackingImageSize)
{
	FMetaHumanCharacterTargetMeshKey Key;
	UObject* Mesh = nullptr;
	UObject* Head = nullptr;
	FString Error;
	if (!BuildTargetMeshKey(MeshPath, HeadMeshPath, Key, Mesh, Head, Error))
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), Error);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}

	FConformTargetMesh Target;
	if (!UMetaHumanCharacterEditorSubsystem::GetMeshDataForConforming(Mesh, Target.BodyVertices, Target.BodyVertexIndices) || Target.BodyVertices.IsEmpty())
	{
		return VmhJson::Error(TEXT("MESH_DATA_FAILED"), FString::Printf(TEXT("GetMeshDataForConforming gave no vertices for %s"), *MeshPath));
	}
	Target.TargetPartsType = ETargetPartsType::Combined;
	if (Head)
	{
		if (!UMetaHumanCharacterEditorSubsystem::GetMeshDataForConforming(Head, Target.HeadVertices, Target.HeadVertexIndices) || Target.HeadVertices.IsEmpty())
		{
			return VmhJson::Error(TEXT("MESH_DATA_FAILED"), FString::Printf(TEXT("GetMeshDataForConforming gave no vertices for %s"), *HeadMeshPath));
		}
		Target.TargetPartsType = ETargetPartsType::HeadAndBody;
	}

	const FBox MeshBounds = BoundsOf(Target.BodyVertices);
	const FVector Facing = FacingOf(Target.BodyVertices, MeshBounds);
	const FBox HeadBounds = Head ? BoundsOf(Target.HeadVertices) : HeadBoundsOf(MeshBounds);

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	Out->SetStringField(TEXT("parts"), Head ? TEXT("HeadAndBody") : TEXT("Combined"));
	Out->SetNumberField(TEXT("vertices"), Target.BodyVertices.Num() + Target.HeadVertices.Num());
	Out->SetNumberField(TEXT("triangles"), (Target.BodyVertexIndices.Num() + Target.HeadVertexIndices.Num()) / 3);
	Out->SetNumberField(TEXT("height_cm"), MeshBounds.Max.Z - MeshBounds.Min.Z);
	Out->SetNumberField(TEXT("feet_z_cm"), MeshBounds.Min.Z);
	Out->SetStringField(TEXT("facing"), Facing.ToString());
	TArray<FString> Warnings;
	if (FMath::Abs(MeshBounds.Min.Z) > 5.0)
	{
		Warnings.Add(FString::Printf(TEXT("the feet stand at z=%.1f cm, not on the floor"), MeshBounds.Min.Z));
	}
	const double HeightCm = MeshBounds.Max.Z - MeshBounds.Min.Z;
	if (HeightCm < 100 || HeightCm > 260)
	{
		Warnings.Add(FString::Printf(TEXT("the mesh is %.0f cm tall: not a human in centimetres (a metre-unit import?)"), HeightCm));
	}
	if (FVector::DotProduct(Facing, FVector(0, 1, 0)) < 0.94)
	{
		Warnings.Add(FString::Printf(TEXT("the mesh faces %s; MetaHumans face +Y - re-import it turned, or expect a rough solve"), *Facing.ToString()));
	}

	FConformTargetParams Params;
	Params.ConformTargetMesh = Target;
	Params.bEstimateBodyJointsFromMesh = false;
	Params.bAutoSolve = bAutoSolve;
	if (bAutoSolve && Params.BodyConformSolveSettings.PipelineName.IsEmpty())
	{
		Params.BodyConformSolveSettings.PipelineName = Head ? TEXT("head_body") : TEXT("combined");
	}
	if (bTrackFace)
	{
		FMetaHumanCharacterTargetTrackingResults Tracking;
		TMap<FString, FTrackingPoints> Curves;
		FString TrackError;
		if (TrackFaceOnMesh(Ctx.Subsystem, Head ? Head : Mesh, Head ? Target.HeadVertices : Target.BodyVertices, Head ? Target.HeadVertexIndices : Target.BodyVertexIndices,
		                    HeadBounds, Facing, FMath::Clamp(TrackingImageSize, 256, 4096), Tracking, Curves, Out, TrackError))
		{
			Ctx.Subsystem->CommitTargetMeshTrackingResults(Ctx.Character, Key, Tracking);
			Params.CurveTrackingPoints = Curves;
			Params.CameraViewInfo = Tracking.CameraViewInfo;
			Params.ImageSize = Tracking.ImageSize;
		}
		else
		{
			Warnings.Add(FString::Printf(TEXT("face tracking failed (%s): the solve runs on the body alone and the face stays the archetype's"), *TrackError));
		}
	}
	Out->SetArrayField(TEXT("warnings"), VmhJson::StringArray(Warnings));

	const double Started = FPlatformTime::Seconds();
	const bool bSolved = Ctx.Subsystem->ConformToTargetMeshes(Ctx.Character, Key, Params);
	Out->SetBoolField(TEXT("solved"), bSolved);
	Out->SetNumberField(TEXT("seconds"), FPlatformTime::Seconds() - Started);
	if (!bSolved)
	{
		Out->SetStringField(TEXT("character"), Ctx.Character->GetPathName());
		return VmhJson::Error(TEXT("CONFORM_FAILED"), TEXT("ConformToTargetMeshes returned false (see the Output Log; a mesh in metres, lying down, or without a head is the usual cause)"), Out);
	}
	Out->SetStringField(TEXT("next"), TEXT("save_posed_dna (the baking target, in this pose) then commit_a_pose, request_auto_rig, request_texture_sources, build"));
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::SavePosedDna(const FString& CharacterPath, const FString& MeshPath, const FString& HeadMeshPath, const FString& ProjectPath, const FString& ExternalPath, const FString& AssetName)
{
	if (ProjectPath.IsEmpty() && ExternalPath.IsEmpty())
	{
		return VmhJson::Error(TEXT("NO_DESTINATION"), TEXT("give ProjectPath (a content folder) and/or ExternalPath (a disk folder)"));
	}
	FMetaHumanCharacterTargetMeshKey Key;
	UObject* Mesh = nullptr;
	UObject* Head = nullptr;
	FString Error;
	if (!BuildTargetMeshKey(MeshPath, HeadMeshPath, Key, Mesh, Head, Error))
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), Error);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	if (!ExternalPath.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*ExternalPath, true);
	}
	FMetaHumanPosedDNAExportParams Params;
	Params.ProjectPath = ProjectPath;
	Params.ExternalPath = ExternalPath;
	Params.TargetMeshKey = Key;
	Params.AssetName = AssetName;
	Params.bOverwriteExistingAssets = true;
	UMetaHumanCharacterExportBlueprintLibrary::ExportPosedDNA(Ctx.Character, Params);

	const FString Name = AssetName.IsEmpty() ? Ctx.Character->GetName() + TEXT("_Posed") : AssetName;
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	if (!ProjectPath.IsEmpty())
	{
		const FString AssetPath = ProjectPath / Name;
		Out->SetStringField(TEXT("dna_asset"), AssetPath);
		Out->SetBoolField(TEXT("dna_asset_exists"), VmhAssets::Load(AssetPath) != nullptr);
	}
	if (!ExternalPath.IsEmpty())
	{
		const FString File = ExternalPath / Name + TEXT(".dna");
		Out->SetStringField(TEXT("dna_file"), File);
		Out->SetBoolField(TEXT("dna_file_exists"), FPaths::FileExists(File));
		Out->SetNumberField(TEXT("dna_file_bytes"), FPaths::FileExists(File) ? static_cast<double>(IFileManager::Get().FileSize(*File)) : 0.0);
	}
	Out->SetStringField(TEXT("hint"), TEXT("nothing written means no posed state was stored for this mesh key: run import_from_custom_mesh with the same MeshPath first"));
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::CommitAPose(const FString& CharacterPath, const FString& MeshPath, const FString& HeadMeshPath)
{
	FMetaHumanCharacterTargetMeshKey Key;
	UObject* Mesh = nullptr;
	UObject* Head = nullptr;
	FString Error;
	if (!BuildTargetMeshKey(MeshPath, HeadMeshPath, Key, Mesh, Head, Error))
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), Error);
	}
	FEditContext Ctx = OpenForEdit(CharacterPath, true);
	if (!Ctx.IsValid())
	{
		return Ctx.ToError();
	}
	Ctx.Subsystem->CommitPosedStateAsAPose(Ctx.Character, Key);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("state"), TEXT("the body and face states are now in the MetaHuman A-pose; a bake against the target mesh no longer lines up (use the posed DNA)"));
	return CharacterResult(Ctx, Out);
}

FString UMetaHumanCharacterService::GenerateSkeletalMeshFromDna(const FString& DnaPath, const FString& PackagePath, const FString& AssetName, const FString& Skeleton)
{
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("PackagePath and AssetName are required"));
	}
	TSharedPtr<IDNAReader> Reader;
	UDNA* DnaAsset = nullptr;
	FDNAConfig Config = FDNAConfig::Legacy(ECoordinateSystemTransformPolicy::Transform);
	if (DnaPath.EndsWith(TEXT(".dna"), ESearchCase::IgnoreCase) || DnaPath.EndsWith(TEXT(".mhdna"), ESearchCase::IgnoreCase))
	{
		if (!FPaths::FileExists(DnaPath))
		{
			return VmhJson::Error(TEXT("DNA_NOT_FOUND"), FString::Printf(TEXT("No file at %s"), *DnaPath));
		}
		Reader = ReadDNAFromFile(DnaPath, EDNADataLayer::All);
	}
	else
	{
		DnaAsset = VmhAssets::Load<UDNA>(DnaPath);
		if (!DnaAsset)
		{
			return VmhJson::Error(TEXT("DNA_NOT_FOUND"), FString::Printf(TEXT("No DNA asset at %s"), *DnaPath));
		}
		Reader = DnaAsset->GetDNAReader();
		Config = DnaAsset->DNAConfig;
	}
	if (!Reader.IsValid())
	{
		return VmhJson::Error(TEXT("DNA_UNREADABLE"), FString::Printf(TEXT("Could not read the DNA at %s"), *DnaPath));
	}
	USkeleton* SkeletonAsset = nullptr;
	const FString Kind = Skeleton.ToLower();
	if (Kind == TEXT("body"))
	{
		SkeletonAsset = LoadObject<USkeleton>(nullptr, TEXT("/MetaHumanCharacter/Female/Medium/NormalWeight/Body/metahuman_base_skel.metahuman_base_skel"));
	}
	else if (Kind == TEXT("face"))
	{
		SkeletonAsset = LoadObject<USkeleton>(nullptr, TEXT("/MetaHumanCharacter/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton"));
	}
	else if (!Kind.IsEmpty() && Kind != TEXT("none"))
	{
		SkeletonAsset = VmhAssets::Load<USkeleton>(Skeleton);
		if (!SkeletonAsset)
		{
			return VmhJson::Error(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("No skeleton at %s (use body, face, none or a path)"), *Skeleton));
		}
	}
	USkeletalMesh* SkeletalMesh = FInterchangeDnaModule::GetModule().ImportSync(AssetName, PackagePath, Reader, SkeletonAsset, Config);
	if (!SkeletalMesh)
	{
		return VmhJson::Error(TEXT("GENERATE_FAILED"), TEXT("the DNA interchange import made no skeletal mesh (see the Output Log)"));
	}
	if (DnaAsset)
	{
		UDNAAssetUserData* UserData = NewObject<UDNAAssetUserData>(SkeletalMesh);
		UserData->DNAAsset = DnaAsset;
		SkeletalMesh->AddAssetUserData(UserData);
	}
	else
	{
		FInterchangeDnaModule::GetModule().CreateAndAttachDNAToSkeletalMesh(SkeletalMesh, Reader);
	}
	SkeletalMesh->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("skeletal_mesh"), SkeletalMesh->GetPathName());
	Out->SetStringField(TEXT("skeleton"), SkeletalMesh->GetSkeleton() ? SkeletalMesh->GetSkeleton()->GetPathName() : TEXT(""));
	Out->SetNumberField(TEXT("bones"), SkeletalMesh->GetRefSkeleton().GetNum());
	const FBoxSphereBounds Bounds = SkeletalMesh->GetBounds();
	Out->SetStringField(TEXT("bounds_extent_cm"), Bounds.BoxExtent.ToString());
	Out->SetStringField(TEXT("bounds_origin_cm"), Bounds.Origin.ToString());
	if (SkeletalMesh->GetImportedModel() && SkeletalMesh->GetImportedModel()->LODModels.Num() > 0)
	{
		Out->SetNumberField(TEXT("lod0_vertices"), SkeletalMesh->GetImportedModel()->LODModels[0].NumVertices);
	}
	Out->SetStringField(TEXT("next"), TEXT("export_fbx it for a bake against the target mesh; save the package when it should stay"));
	return VmhJson::Ok(Out);
}

// =====================================================================
// Round trips with a DCC
// =====================================================================

FString UMetaHumanCharacterService::ExportFbx(const FString& AssetPath, const FString& FilePath)
{
	FString Error;
	UObject* Asset = LoadMeshAsset(AssetPath, Error);
	if (!Asset)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), Error);
	}
	if (FilePath.IsEmpty() || !FilePath.EndsWith(TEXT(".fbx"), ESearchCase::IgnoreCase))
	{
		return VmhJson::Error(TEXT("BAD_PATH"), TEXT("FilePath must end in .fbx"));
	}
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(FilePath), true);

	// an empty material slot took the exporter down with an array assertion (2026-10-04): fill them for the export
	TArray<int32> Filled;
	UMaterialInterface* Default = UMaterial::GetDefaultMaterial(MD_Surface);
	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
	{
		TArray<FSkeletalMaterial>& Materials = SkeletalMesh->GetMaterials();
		for (int32 i = 0; i < Materials.Num(); ++i)
		{
			if (!Materials[i].MaterialInterface)
			{
				Materials[i].MaterialInterface = Default;
				Filled.Add(i);
			}
		}
		if (!SkeletalMesh->GetResourceForRendering() || SkeletalMesh->GetResourceForRendering()->LODRenderData.IsEmpty())
		{
			return VmhJson::Error(TEXT("NO_RENDER_DATA"), TEXT("the skeletal mesh has no LOD render data to export"));
		}
		// a section pointing past the material slots takes the exporter down with an array assertion even after the
		// slots are filled (the optional content's archetype_SkelMesh, 2026-10-04): refuse instead of crashing
		for (const FSkelMeshRenderSection& Section : SkeletalMesh->GetResourceForRendering()->LODRenderData[0].RenderSections)
		{
			if (Section.MaterialIndex < 0 || Section.MaterialIndex >= Materials.Num())
			{
				for (int32 i : Filled) { Materials[i].MaterialInterface = nullptr; }
				return VmhJson::Error(TEXT("BAD_MATERIAL_INDEX"), FString::Printf(TEXT("LOD0 section uses material index %d but the mesh has %d material slots; the FBX exporter would assert. Duplicate the mesh and fix its materials first"), Section.MaterialIndex, Materials.Num()));
			}
		}
		// (a mesh generated from a DNA carries 8 LOD info entries over 4 LODs and exports fine: not a reason to refuse)
	}
	else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
	{
		TArray<FStaticMaterial>& Materials = StaticMesh->GetStaticMaterials();
		for (int32 i = 0; i < Materials.Num(); ++i)
		{
			if (!Materials[i].MaterialInterface)
			{
				Materials[i].MaterialInterface = Default;
				Filled.Add(i);
			}
		}
		if (!StaticMesh->GetRenderData() || StaticMesh->GetRenderData()->LODResources.IsEmpty())
		{
			return VmhJson::Error(TEXT("NO_RENDER_DATA"), TEXT("the static mesh has no LOD render data to export"));
		}
	}

	UAssetExportTask* Task = NewObject<UAssetExportTask>();
	Task->Object = Asset;
	Task->Filename = FilePath;
	Task->bAutomated = true;
	Task->bReplaceIdentical = true;
	Task->bPrompt = false;
	Task->bSelected = false;
	Task->bUseFileArchive = false;
	Task->bWriteEmptyFiles = false;
	Task->Exporter = Asset->IsA<USkeletalMesh>() ? static_cast<UExporter*>(NewObject<USkeletalMeshExporterFBX>()) : static_cast<UExporter*>(NewObject<UStaticMeshExporterFBX>());
	UFbxExportOption* Options = NewObject<UFbxExportOption>();
	Options->bASCII = false;
	Options->LevelOfDetail = false;
	Options->Collision = false;
	Options->VertexColor = false;
	Options->bExportMorphTargets = false;
	Options->bExportPreviewMesh = false;
	Options->bForceFrontXAxis = false;
	Options->MapSkeletalMotionToRoot = false;
	Task->Options = Options;
	const bool bOk = UExporter::RunAssetExportTask(Task);

	// put the empty slots back
	if (USkeletalMesh* SkeletalMesh = Cast<USkeletalMesh>(Asset))
	{
		for (int32 i : Filled) { SkeletalMesh->GetMaterials()[i].MaterialInterface = nullptr; }
	}
	else if (UStaticMesh* StaticMesh = Cast<UStaticMesh>(Asset))
	{
		for (int32 i : Filled) { StaticMesh->GetStaticMaterials()[i].MaterialInterface = nullptr; }
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("asset"), Asset->GetPathName());
	Out->SetStringField(TEXT("file"), FilePath);
	Out->SetBoolField(TEXT("exists"), FPaths::FileExists(FilePath));
	Out->SetNumberField(TEXT("bytes"), FPaths::FileExists(FilePath) ? static_cast<double>(IFileManager::Get().FileSize(*FilePath)) : 0.0);
	Out->SetNumberField(TEXT("empty_slots_filled"), Filled.Num());
	Out->SetArrayField(TEXT("errors"), VmhJson::StringArray(Task->Errors));
	Out->SetStringField(TEXT("units"), TEXT("centimetres; Blender's FBX importer reads it in metres with the scale on the root"));
	if (!bOk || !FPaths::FileExists(FilePath))
	{
		return VmhJson::Error(TEXT("EXPORT_FAILED"), TEXT("the FBX exporter wrote nothing (see errors and the Output Log)"), Out);
	}
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::ReimportSkeletalMesh(const FString& AssetPath, const FString& FbxPath, bool bKeepMaterials)
{
	if (!FPaths::FileExists(FbxPath))
	{
		return VmhJson::Error(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("No file at %s"), *FbxPath));
	}
	USkeletalMesh* Existing = VmhAssets::Load<USkeletalMesh>(AssetPath);
	if (!Existing)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No skeletal mesh at %s"), *AssetPath));
	}
	USkeleton* Skeleton = Existing->GetSkeleton();
	TArray<TPair<FName, FString>> SlotsBefore;
	for (const FSkeletalMaterial& M : Existing->GetMaterials())
	{
		SlotsBefore.Add(TPair<FName, FString>(M.MaterialSlotName, M.MaterialInterface ? M.MaterialInterface->GetPathName() : FString()));
	}
	const FBoxSphereBounds BoundsBefore = Existing->GetBounds();

	FString PackagePath, AssetName;
	VmhAssets::SplitAssetPath(AssetPath, PackagePath, AssetName);

	UFbxImportUI* ImportUI = NewObject<UFbxImportUI>();
	ImportUI->bImportMesh = true;
	ImportUI->bImportAsSkeletal = true;
	ImportUI->MeshTypeToImport = FBXIT_SkeletalMesh;
	ImportUI->Skeleton = Skeleton;
	ImportUI->bCreatePhysicsAsset = false;
	ImportUI->bImportAnimations = false;
	ImportUI->bImportMaterials = false;
	ImportUI->bImportTextures = false;
	if (UFbxSkeletalMeshImportData* SkelData = ImportUI->SkeletalMeshImportData)
	{
		SkelData->bConvertScene = true;
		SkelData->bForceFrontXAxis = false;
		SkelData->bConvertSceneUnit = false;
		SkelData->ImportUniformScale = 1.f;
		SkelData->ImportTranslation = FVector::ZeroVector;
		SkelData->ImportRotation = FRotator::ZeroRotator;
		SkelData->bUpdateSkeletonReferencePose = false;
		SkelData->bUseT0AsRefPose = false;
		SkelData->bImportMorphTargets = false;
		SkelData->bImportMeshesInBoneHierarchy = false;
		SkelData->ImportContentType = EFBXImportContentType::FBXICT_All;
		SkelData->NormalImportMethod = FBXNIM_ImportNormalsAndTangents;
	}

	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = FbxPath;
	Task->DestinationPath = PackagePath;
	Task->DestinationName = AssetName;
	Task->bReplaceExisting = true;
	Task->bAutomated = true;
	Task->bSave = false;
	Task->Options = ImportUI;
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });

	USkeletalMesh* Reimported = VmhAssets::Load<USkeletalMesh>(AssetPath);
	if (!Reimported)
	{
		return VmhJson::Error(TEXT("REIMPORT_FAILED"), FString::Printf(TEXT("no skeletal mesh came out of %s (see the Output Log)"), *FbxPath));
	}
	if (Reimported->GetSkeleton() != Skeleton)
	{
		return VmhJson::Error(TEXT("SKELETON_MISMATCH"), TEXT("the reimported mesh is on a different skeleton than the one it replaced; the Blueprint would stop following the body's pose"));
	}

	TArray<FString> MaterialsRestored;
	if (bKeepMaterials)
	{
		TMap<FName, FString> ByName;
		for (const TPair<FName, FString>& S : SlotsBefore)
		{
			if (!S.Value.IsEmpty()) { ByName.Add(S.Key, S.Value); }
		}
		for (FSkeletalMaterial& M : Reimported->GetMaterials())
		{
			if (const FString* Path = ByName.Find(M.MaterialSlotName))
			{
				if (UMaterialInterface* Mat = VmhAssets::Load<UMaterialInterface>(*Path))
				{
					M.MaterialInterface = Mat;
					MaterialsRestored.Add(M.MaterialSlotName.ToString());
				}
			}
		}
	}
	Reimported->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> SlotsBeforeJson, SlotsAfterJson;
	for (const TPair<FName, FString>& S : SlotsBefore)
	{
		SlotsBeforeJson.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(S.Key.ToString()), MakeShared<FJsonValueString>(S.Value) }));
	}
	for (const FSkeletalMaterial& M : Reimported->GetMaterials())
	{
		SlotsAfterJson.Add(MakeShared<FJsonValueArray>(TArray<TSharedPtr<FJsonValue>>{ MakeShared<FJsonValueString>(M.MaterialSlotName.ToString()), MakeShared<FJsonValueString>(M.MaterialInterface ? M.MaterialInterface->GetPathName() : FString()) }));
	}
	const FBoxSphereBounds BoundsAfter = Reimported->GetBounds();

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("skeletal_mesh"), Reimported->GetPathName());
	Out->SetStringField(TEXT("fbx"), FbxPath);
	Out->SetStringField(TEXT("skeleton"), Skeleton ? Skeleton->GetPathName() : TEXT(""));
	Out->SetArrayField(TEXT("slots_before"), SlotsBeforeJson);
	Out->SetArrayField(TEXT("slots_after"), SlotsAfterJson);
	Out->SetArrayField(TEXT("materials_restored"), VmhJson::StringArray(MaterialsRestored));
	Out->SetStringField(TEXT("bounds_before_extent_cm"), BoundsBefore.BoxExtent.ToString());
	Out->SetStringField(TEXT("bounds_after_extent_cm"), BoundsAfter.BoxExtent.ToString());
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::ImportTexture(const FString& FilePath, const FString& PackagePath, const FString& AssetName, const FString& Kind, bool bFlipGreen)
{
	if (!FPaths::FileExists(FilePath))
	{
		return VmhJson::Error(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("No file at %s"), *FilePath));
	}
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("PackagePath and AssetName are required"));
	}
	const FString K = Kind.ToLower();
	const bool bNormal = K == TEXT("normal");
	const bool bColor = K.IsEmpty() || K == TEXT("color") || K == TEXT("colour") || K == TEXT("basecolor");
	const bool bMask = K == TEXT("mask") || K == TEXT("masks");

	UTextureFactory* Factory = NewObject<UTextureFactory>();
	UTextureFactory::SuppressImportOverwriteDialog();
	Factory->bFlipNormalMapGreenChannel = bNormal && bFlipGreen;
	Factory->CompressionSettings = bNormal ? TC_Normalmap : bMask ? TC_Masks : TC_Default;
	Factory->LODGroup = bNormal ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = FilePath;
	Task->DestinationPath = PackagePath;
	Task->DestinationName = AssetName;
	Task->bAutomated = true;
	Task->bReplaceExisting = true;
	Task->bSave = false;
	Task->Factory = Factory;
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });

	UTexture2D* Texture = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Texture = Cast<UTexture2D>(Object);
		if (Texture) break;
	}
	if (!Texture)
	{
		Texture = VmhAssets::Load<UTexture2D>(PackagePath / AssetName);
	}
	if (!Texture)
	{
		return VmhJson::Error(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("no Texture2D came out of %s (see the Output Log)"), *FilePath));
	}
	Texture->PreEditChange(nullptr);
	Texture->SRGB = bColor;
	Texture->CompressionSettings = bNormal ? TC_Normalmap : bMask ? TC_Masks : TC_Default;
	Texture->LODGroup = bNormal ? TEXTUREGROUP_WorldNormalMap : TEXTUREGROUP_World;
	if (bNormal && bFlipGreen && !Factory->bFlipNormalMapGreenChannel)
	{
		Texture->bFlipGreenChannel = true;
	}
	Texture->PostEditChange();

	// finish compiling and check it actually built, before marking the package dirty (2026-10-07): after long editor
	// sessions the texture pipeline stalls and a fresh import comes back a 32x32 placeholder with no miplevels
	// ("contains no miplevels" in the Output Log) - it rendered a face white, and the next save of any material
	// instance that uses it hangs in SavePackage. Restart the editor rather than using a texture that built this way.
	FTextureCompilingManager::Get().FinishCompilation({ Texture });
	const int32 W = Texture->GetSizeX();
	const int32 H = Texture->GetSizeY();
	const int64 SrcW = Texture->Source.GetSizeX();
	const int64 SrcH = Texture->Source.GetSizeY();
	const int32 NumMips = Texture->GetPlatformData() ? Texture->GetPlatformData()->Mips.Num() : 0;
	// the built size must match what was actually imported (SrcW/H, from the PNG/TGA/EXR on disk): a stalled
	// pipeline builds a 32x32 placeholder regardless of the source's real size, so comparing to a fixed size
	// threshold alone would also flag a deliberately tiny texture. No miplevels at all is always a failure.
	const bool bPlaceholder = NumMips == 0 || (SrcW > 0 && SrcH > 0 && (W != SrcW || H != SrcH));

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("texture"), Texture->GetPathName());
	Out->SetNumberField(TEXT("width"), W);
	Out->SetNumberField(TEXT("height"), H);
	Out->SetNumberField(TEXT("mip_count"), NumMips);
	Out->SetBoolField(TEXT("srgb"), Texture->SRGB);
	Out->SetStringField(TEXT("compression"), StaticEnum<TextureCompressionSettings>()->GetNameStringByValue(static_cast<int64>(Texture->CompressionSettings)));
	Out->SetBoolField(TEXT("flip_green"), bNormal && bFlipGreen);
	if (bPlaceholder)
	{
		return VmhJson::Error(TEXT("IMPORT_INCOMPLETE"), FString::Printf(TEXT("%s built as %dx%d with %d miplevels but the source image is %lldx%lld; restart the editor before using it on any material instance"), *Texture->GetPathName(), W, H, NumMips, SrcW, SrcH), Out);
	}
	Texture->MarkPackageDirty();
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::SetMaterialTexture(const FString& MaterialInstancePath, const FString& ParameterName, const FString& TexturePath)
{
	UMaterialInstanceConstant* Instance = VmhAssets::Load<UMaterialInstanceConstant>(MaterialInstancePath);
	if (!Instance)
	{
		return VmhJson::Error(TEXT("MATERIAL_NOT_FOUND"), FString::Printf(TEXT("No material instance constant at %s"), *MaterialInstancePath));
	}
	UTexture* Texture = VmhAssets::Load<UTexture>(TexturePath);
	if (!Texture)
	{
		return VmhJson::Error(TEXT("TEXTURE_NOT_FOUND"), FString::Printf(TEXT("No texture at %s"), *TexturePath));
	}
	TArray<FMaterialParameterInfo> Infos;
	TArray<FGuid> Ids;
	Instance->GetAllTextureParameterInfo(Infos, Ids);
	TArray<FString> Names;
	bool bKnown = false;
	for (const FMaterialParameterInfo& Info : Infos)
	{
		Names.Add(Info.Name.ToString());
		bKnown |= Info.Name == FName(*ParameterName);
	}
	if (!bKnown)
	{
		return VmhJson::Error(TEXT("NO_SUCH_PARAMETER"), FString::Printf(TEXT("%s has no texture parameter %s; it has: %s"), *MaterialInstancePath, *ParameterName, *FString::Join(Names, TEXT(", "))));
	}
	// a texture imported a moment ago is still compiling, and the editor library refuses it (2026-10-04)
	FTextureCompilingManager::Get().FinishCompilation({ Texture });
	const FName ParamName(*ParameterName);
	const FMaterialParameterInfo Info{ ParamName };
	const bool bSet = UMaterialEditingLibrary::SetMaterialInstanceTextureParameterValue(Instance, ParamName, Texture);
	if (!bSet)
	{
		Instance->SetTextureParameterValueEditorOnly(Info, Texture);
	}
	UMaterialEditingLibrary::UpdateMaterialInstance(Instance);
	Instance->MarkPackageDirty();
	UTexture* ReadBack = nullptr;
	Instance->GetTextureParameterValue(FHashedMaterialParameterInfo(Info), ReadBack);
	const bool bApplied = ReadBack == Texture;
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("material_instance"), Instance->GetPathName());
	Out->SetStringField(TEXT("parameter"), ParameterName);
	Out->SetStringField(TEXT("texture"), Texture->GetPathName());
	Out->SetBoolField(TEXT("set"), bApplied);
	Out->SetStringField(TEXT("reads_back"), ReadBack ? ReadBack->GetPathName() : TEXT(""));
	return bApplied ? VmhJson::Ok(Out) : VmhJson::Error(TEXT("SET_FAILED"), TEXT("the parameter does not read back as the texture after setting it"), Out);
}

FString UMetaHumanCharacterService::AttachSkeletalMeshToBlueprint(const FString& BlueprintPath, const FString& ComponentName, const FString& SkeletalMeshPath, const FString& ParentComponentName, bool bLeaderPose, const FString& OverrideMaterials)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	USkeletalMesh* Mesh = VmhAssets::Load<USkeletalMesh>(SkeletalMeshPath);
	if (!Mesh)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No skeletal mesh at %s"), *SkeletalMeshPath));
	}
	if (ComponentName.IsEmpty())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("ComponentName is required"));
	}
	// override materials: one path, or a JSON array with one entry per slot ("" keeps the slot's own)
	TArray<UMaterialInterface*> Overrides;
	{
		const FString Spec = OverrideMaterials.TrimStartAndEnd();
		TArray<FString> Paths;
		if (Spec.StartsWith(TEXT("[")))
		{
			TArray<TSharedPtr<FJsonValue>> Values;
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Spec);
			if (!FJsonSerializer::Deserialize(Reader, Values))
			{
				return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("OverrideMaterials is a material path or a JSON array of paths"));
			}
			for (const TSharedPtr<FJsonValue>& V : Values) { Paths.Add(V->AsString()); }
		}
		else if (!Spec.IsEmpty())
		{
			Paths.Add(Spec);
		}
		for (const FString& P : Paths)
		{
			UMaterialInterface* Material = P.IsEmpty() ? nullptr : VmhAssets::Load<UMaterialInterface>(P);
			if (!P.IsEmpty() && !Material)
			{
				return VmhJson::Error(TEXT("MATERIAL_NOT_FOUND"), FString::Printf(TEXT("No material at %s"), *P));
			}
			Overrides.Add(Material);
		}
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* Parent = nullptr;
	if (!ParentComponentName.IsEmpty())
	{
		Parent = SCS->FindSCSNode(FName(*ParentComponentName));
		if (!Parent)
		{
			TArray<FString> Names;
			for (USCS_Node* Node : SCS->GetAllNodes()) { Names.Add(Node->GetVariableName().ToString()); }
			return VmhJson::Error(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("%s has no component %s; it has: %s"), *BlueprintPath, *ParentComponentName, *FString::Join(Names, TEXT(", "))));
		}
	}
	if (SCS->FindSCSNode(FName(*ComponentName)))
	{
		return VmhJson::Error(TEXT("NAME_TAKEN"), FString::Printf(TEXT("%s already has a component named %s"), *BlueprintPath, *ComponentName));
	}
	USCS_Node* Node = SCS->CreateNode(USkeletalMeshComponent::StaticClass(), FName(*ComponentName));
	USkeletalMeshComponent* Template = Cast<USkeletalMeshComponent>(Node->ComponentTemplate);
	if (!Template)
	{
		return VmhJson::Error(TEXT("NODE_FAILED"), TEXT("the construction script made no SkeletalMeshComponent template"));
	}
	Template->SetSkeletalMeshAsset(Mesh);
	for (int32 Slot = 0; Slot < Overrides.Num(); ++Slot)
	{
		if (Overrides[Slot])
		{
			if (Template->OverrideMaterials.Num() <= Slot) { Template->OverrideMaterials.SetNum(Slot + 1); }
			Template->OverrideMaterials[Slot] = Overrides[Slot];
		}
	}
	bool bLeader = false;
	USkeletalMesh* LeaderMesh = nullptr;
	if (Parent)
	{
		Parent->AddChildNode(Node);
		if (bLeaderPose)
		{
			if (USkinnedMeshComponent* ParentTemplate = Cast<USkinnedMeshComponent>(Parent->ComponentTemplate))
			{
				Template->LeaderPoseComponent = ParentTemplate;
				// the follower's own bounds stay at its rest pose: without the leader's bounds it is culled in close-ups (2026-11-12)
				Template->bUseBoundsFromLeaderPoseComponent = true;
				Template->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::OnlyTickPoseWhenRendered;
				LeaderMesh = Cast<USkeletalMesh>(ParentTemplate->GetSkinnedAsset());
				bLeader = true;
			}
		}
	}
	else
	{
		SCS->AddNode(Node);
	}
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Blueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("component"), ComponentName);
	Out->SetStringField(TEXT("parent"), Parent ? ParentComponentName : TEXT(""));
	Out->SetStringField(TEXT("skeletal_mesh"), Mesh->GetPathName());
	Out->SetBoolField(TEXT("leader_pose"), bLeader);
	Out->SetBoolField(TEXT("use_bounds_from_leader"), bLeader);
	Out->SetNumberField(TEXT("override_materials"), Overrides.Num());
	Out->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	Out->SetStringField(TEXT("skeleton"), Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : TEXT(""));
	TArray<FString> Warnings;
	if (LeaderMesh)
	{
		const TSharedPtr<FJsonObject> Check = UMetaHumanWardrobeService::CheckFollower(Mesh, LeaderMesh, 0);
		Out->SetObjectField(TEXT("follower_check"), Check);
		double Dead = 0, Missing = 0;
		if (Check->TryGetNumberField(TEXT("unevaluated_bone_vertices"), Dead) && Dead > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%d vertices are weighted to bones %s does not evaluate: they will collapse to the actor origin (spikes). Fix a copy: MetaHumanWardrobeService.fix_unevaluated_bone_weights(<copy>, %s)"), static_cast<int32>(Dead), *LeaderMesh->GetName(), *LeaderMesh->GetPathName()));
		}
		if (Check->TryGetNumberField(TEXT("missing_bone_vertices"), Missing) && Missing > 0)
		{
			Warnings.Add(FString::Printf(TEXT("%d vertices are weighted to bones %s lacks: they will not follow the body"), static_cast<int32>(Missing), *LeaderMesh->GetName()));
		}
	}
	Out->SetArrayField(TEXT("warnings"), VmhJson::StringArray(Warnings));
	Out->SetStringField(TEXT("hint"), TEXT("the mesh follows the parent only when it shares the parent's skeleton; a marketplace garment keeps its modelled size (no refit). Hide the built default outfit component if it shows through. Save the Blueprint's package when it should stay"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::AttachStaticMeshToBlueprint(const FString& BlueprintPath, const FString& ComponentName, const FString& StaticMeshPath, const FString& ParentComponentName, const FString& SocketName, FVector RelativeLocation, FRotator RelativeRotation)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	UStaticMesh* Mesh = VmhAssets::Load<UStaticMesh>(StaticMeshPath);
	if (!Mesh)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No static mesh at %s"), *StaticMeshPath));
	}
	if (ComponentName.IsEmpty() || ParentComponentName.IsEmpty())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("ComponentName and ParentComponentName are required"));
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* Parent = SCS->FindSCSNode(FName(*ParentComponentName));
	if (!Parent)
	{
		TArray<FString> Names;
		for (USCS_Node* Node : SCS->GetAllNodes()) { Names.Add(Node->GetVariableName().ToString()); }
		return VmhJson::Error(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("%s has no component %s; it has: %s"), *BlueprintPath, *ParentComponentName, *FString::Join(Names, TEXT(", "))));
	}
	bool bSocketKnown = true;
	if (!SocketName.IsEmpty())
	{
		if (USkinnedMeshComponent* ParentSkinned = Cast<USkinnedMeshComponent>(Parent->ComponentTemplate))
		{
			bSocketKnown = ParentSkinned->DoesSocketExist(FName(*SocketName));
		}
	}
	if (USCS_Node* Old = SCS->FindSCSNode(FName(*ComponentName)))
	{
		SCS->RemoveNodeAndPromoteChildren(Old);
	}
	USCS_Node* Node = SCS->CreateNode(UStaticMeshComponent::StaticClass(), FName(*ComponentName));
	UStaticMeshComponent* Template = Cast<UStaticMeshComponent>(Node->ComponentTemplate);
	if (!Template)
	{
		return VmhJson::Error(TEXT("NODE_FAILED"), TEXT("the construction script made no StaticMeshComponent template"));
	}
	Template->SetStaticMesh(Mesh);
	Template->SetRelativeLocation_Direct(RelativeLocation);
	Template->SetRelativeRotation_Direct(RelativeRotation);
	Node->AttachToName = SocketName.IsEmpty() ? NAME_None : FName(*SocketName);
	Parent->AddChildNode(Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Blueprint->MarkPackageDirty();

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("component"), ComponentName);
	Out->SetStringField(TEXT("parent"), ParentComponentName);
	Out->SetStringField(TEXT("socket"), SocketName);
	Out->SetBoolField(TEXT("socket_known"), bSocketKnown);
	Out->SetStringField(TEXT("static_mesh"), Mesh->GetPathName());
	Out->SetStringField(TEXT("relative_location"), RelativeLocation.ToString());
	Out->SetStringField(TEXT("relative_rotation"), RelativeRotation.ToString());
	Out->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	Out->SetStringField(TEXT("hint"), TEXT("a placed instance made before this call keeps its old components: respawn it; save the Blueprint's package when it should stay"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::RemoveComponentFromBlueprint(const FString& BlueprintPath, const FString& ComponentName)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	USCS_Node* Node = SCS->FindSCSNode(FName(*ComponentName));
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("component"), ComponentName);
	if (!Node)
	{
		Out->SetBoolField(TEXT("removed"), false);
		Out->SetStringField(TEXT("hint"), TEXT("no such component (already removed, or never added)"));
		return VmhJson::Ok(Out);
	}
	SCS->RemoveNodeAndPromoteChildren(Node);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Blueprint->MarkPackageDirty();
	Out->SetBoolField(TEXT("removed"), true);
	Out->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	return VmhJson::Ok(Out);
}

// =====================================================================
// Hair (2026-10-07)
// =====================================================================

FString UMetaHumanCharacterService::ListWardrobeItems(const FString& SlotName)
{
	const FString Root = SlotName.IsEmpty() ? TEXT("/MetaHumanCharacter/Optional/Grooms/Bindings") : (TEXT("/MetaHumanCharacter/Optional/Grooms/Bindings/") + SlotName);
	TArray<FString> Paths = VmhAssets::FindAssetsOfClass(TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem"), Root, false);
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FString& Path : Paths)
	{
		UMetaHumanWardrobeItem* Item = VmhAssets::Load<UMetaHumanWardrobeItem>(Path);
		if (!Item)
		{
			continue;
		}
		TSharedPtr<FJsonObject> Rec = VmhJson::Obj();
		Rec->SetStringField(TEXT("path"), Item->GetPathName());
		Rec->SetStringField(TEXT("name"), Item->GetName());
		FString Left, Right;
		if (Path.Split(TEXT("/Bindings/"), &Left, &Right))
		{
			FString Slot;
			Right.Split(TEXT("/"), &Slot, nullptr);
			Rec->SetStringField(TEXT("slot"), Slot);
		}
		Items.Add(MakeShared<FJsonValueObject>(Rec));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("root"), Root);
	Out->SetNumberField(TEXT("count"), Items.Num());
	Out->SetArrayField(TEXT("items"), Items);
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::AddGroomToBlueprint(const FString& BlueprintPath, const FString& ComponentName, const FString& GroomAssetPath, const FString& TargetComponent, const FString& MaterialFromComponent, bool bSaveBinding, const FString& SocketName, bool bInverseSocketTransform, FVector RelativeLocation, FRotator RelativeRotation)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	if (ComponentName.IsEmpty())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("ComponentName is required"));
	}
	USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
	const FString Parent = TargetComponent.IsEmpty() ? TEXT("Face") : TargetComponent;
	USCS_Node* ParentNode = SCS->FindSCSNode(FName(*Parent));
	if (!ParentNode)
	{
		TArray<FString> Names;
		for (USCS_Node* Node : SCS->GetAllNodes()) { Names.Add(Node->GetVariableName().ToString()); }
		return VmhJson::Error(TEXT("PARENT_NOT_FOUND"), FString::Printf(TEXT("%s has no component %s; it has: %s"), *BlueprintPath, *Parent, *FString::Join(Names, TEXT(", "))));
	}

	UObject* Loaded = VmhAssets::Load(GroomAssetPath);
	if (!Loaded)
	{
		return VmhJson::Error(TEXT("GROOM_NOT_FOUND"), FString::Printf(TEXT("No asset at %s"), *GroomAssetPath));
	}
	UGroomAsset* Groom = Cast<UGroomAsset>(Loaded);
	UGroomBindingAsset* PluginBinding = Cast<UGroomBindingAsset>(Loaded);
	FString StyleName;
	if (UMetaHumanWardrobeItem* WI = Cast<UMetaHumanWardrobeItem>(Loaded))
	{
		FString Folder, Leaf;
		WI->GetPathName().Split(TEXT("/"), &Folder, &Leaf, ESearchCase::IgnoreCase, ESearchDir::FromEnd);
		Leaf.Split(TEXT("."), &Leaf, nullptr); // "WI_Hair_M_FauxMohawk.WI_Hair_M_FauxMohawk" -> "WI_Hair_M_FauxMohawk"
		StyleName = Leaf;
		StyleName.RemoveFromStart(TEXT("WI_"));
		const FString BindingPath = Folder / (StyleName + TEXT("_Binding"));
		PluginBinding = VmhAssets::Load<UGroomBindingAsset>(BindingPath);
		if (!PluginBinding)
		{
			return VmhJson::Error(TEXT("BINDING_NOT_FOUND"), FString::Printf(TEXT("No plugin binding at %s for wardrobe item %s"), *BindingPath, *GroomAssetPath));
		}
	}
	if (PluginBinding)
	{
		Groom = PluginBinding->GetGroom();
	}
	if (!Groom)
	{
		return VmhJson::Error(TEXT("GROOM_NOT_FOUND"), FString::Printf(TEXT("%s has no groom asset (a wardrobe item, a GroomBindingAsset, or a bare GroomAsset is expected)"), *GroomAssetPath));
	}

	// duplicate the plugin's binding into the Blueprint's own folder and retarget it onto the built face mesh: the
	// same recipe Build()'s own groom pipeline uses (MetaHumanGroomEditorPipeline.cpp ProcessSkeletalMeshes -
	// DuplicateObject, SetTargetSkeletalMesh, let the async RBF rebuild run - DrHart's HairTop groom, 2026-10-07).
	UGroomBindingAsset* BindingToUse = nullptr;
	FString BindingPathUsed;
	bool bDuplicated = false;
	if (PluginBinding)
	{
		USkeletalMeshComponent* ParentSkel = Cast<USkeletalMeshComponent>(ParentNode->ComponentTemplate);
		USkeletalMesh* TargetFace = ParentSkel ? ParentSkel->GetSkeletalMeshAsset() : nullptr;
		if (!TargetFace)
		{
			return VmhJson::Error(TEXT("NO_TARGET_MESH"), FString::Printf(TEXT("%s's %s component has no skeletal mesh set yet; build the character first"), *BlueprintPath, *Parent));
		}
		if (StyleName.IsEmpty())
		{
			StyleName = PluginBinding->GetName();
			StyleName.RemoveFromEnd(TEXT("_Binding"));
		}
		const FString DestFolder = FPaths::GetPath(BlueprintPath) / TEXT("Accessories") / ComponentName;
		BindingPathUsed = DestFolder / (StyleName + TEXT("_Binding_") + Blueprint->GetName());
		BindingToUse = VmhAssets::Load<UGroomBindingAsset>(BindingPathUsed);
		if (!BindingToUse)
		{
			FString DupPackage, DupName;
			VmhAssets::SplitAssetPath(BindingPathUsed, DupPackage, DupName);
			BindingToUse = Cast<UGroomBindingAsset>(FAssetToolsModule::GetModule().Get().DuplicateAsset(DupName, DupPackage, PluginBinding));
			bDuplicated = true;
		}
		if (!BindingToUse)
		{
			return VmhJson::Error(TEXT("DUPLICATE_FAILED"), FString::Printf(TEXT("could not duplicate the plugin binding to %s"), *BindingPathUsed));
		}
		if (BindingToUse->GetTargetSkeletalMesh() != TargetFace)
		{
			BindingToUse->SetTargetSkeletalMesh(TargetFace); // kicks the async RBF rebuild (PostEditChange)
		}
	}

	if (USCS_Node* Old = SCS->FindSCSNode(FName(*ComponentName)))
	{
		SCS->RemoveNodeAndPromoteChildren(Old);
	}
	USCS_Node* Node = SCS->CreateNode(UGroomComponent::StaticClass(), FName(*ComponentName));
	UGroomComponent* Template = Cast<UGroomComponent>(Node->ComponentTemplate);
	if (!Template)
	{
		return VmhJson::Error(TEXT("NODE_FAILED"), TEXT("the construction script made no GroomComponent template"));
	}
	Template->GroomAsset = Groom;
	if (BindingToUse)
	{
		Template->BindingAsset = BindingToUse;
	}
	// a bone/socket of the parent mesh: a custom groom with no binding rides it rigidly (2026-10-07)
	FTransform SocketInverse = FTransform::Identity;
	bool bSocketFound = false;
	if (!SocketName.IsEmpty())
	{
		USkeletalMeshComponent* ParentSkel = Cast<USkeletalMeshComponent>(ParentNode->ComponentTemplate);
		USkeletalMesh* ParentMesh = ParentSkel ? ParentSkel->GetSkeletalMeshAsset() : nullptr;
		if (ParentMesh)
		{
			const FReferenceSkeleton& Ref = ParentMesh->GetRefSkeleton();
			int32 BoneIndex = Ref.FindBoneIndex(FName(*SocketName));
			FTransform Local = FTransform::Identity;
			if (BoneIndex == INDEX_NONE)
			{
				if (USkeletalMeshSocket* Sock = ParentMesh->FindSocket(FName(*SocketName)))
				{
					BoneIndex = Ref.FindBoneIndex(Sock->BoneName);
					Local = Sock->GetSocketLocalTransform();
				}
			}
			if (BoneIndex != INDEX_NONE)
			{
				FTransform CS = Local;
				for (int32 B = BoneIndex; B != INDEX_NONE; B = Ref.GetParentIndex(B))
				{
					CS = CS * Ref.GetRefBonePose()[B];
				}
				SocketInverse = CS.Inverse();
				bSocketFound = true;
			}
		}
		if (!bSocketFound)
		{
			return VmhJson::Error(TEXT("SOCKET_NOT_FOUND"), FString::Printf(TEXT("%s's %s mesh has no bone or socket %s"), *BlueprintPath, *Parent, *SocketName));
		}
		Node->AttachToName = FName(*SocketName);
	}
	FTransform Rel(RelativeRotation, RelativeLocation);
	if (bInverseSocketTransform)
	{
		Rel = Rel * SocketInverse;
	}
	Template->SetRelativeLocation_Direct(Rel.GetLocation());
	Template->SetRelativeRotation_Direct(Rel.Rotator());
	ParentNode->AddChildNode(Node);

	TArray<FString> MaterialsCopied;
	if (!MaterialFromComponent.IsEmpty())
	{
		if (USCS_Node* SourceNode = SCS->FindSCSNode(FName(*MaterialFromComponent)))
		{
			if (UGroomComponent* SourceGroom = Cast<UGroomComponent>(SourceNode->ComponentTemplate))
			{
				const TArray<FName> SrcSlots = SourceGroom->GetMaterialSlotNames();
				const TArray<FName> DstSlots = Template->GetMaterialSlotNames();
				for (int32 i = 0; i < DstSlots.Num(); ++i)
				{
					int32 SrcIndex = SrcSlots.IndexOfByKey(DstSlots[i]);
					if (SrcIndex == INDEX_NONE && SrcSlots.IsValidIndex(i))
					{
						SrcIndex = i; // positional fallback when the slot names do not line up
					}
					if (SrcIndex != INDEX_NONE)
					{
						if (UMaterialInterface* Mat = SourceGroom->GetMaterial(SrcIndex))
						{
							Template->SetMaterial(i, Mat);
							MaterialsCopied.Add(DstSlots[i].ToString());
						}
					}
				}
			}
		}
	}

	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(Blueprint);
	FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
	Blueprint->MarkPackageDirty();

	bool bBindingSaved = false;
	if (bSaveBinding && BindingToUse)
	{
		FString SaveErr;
		bBindingSaved = VmhAssets::Save(BindingToUse, SaveErr);
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("component"), ComponentName);
	Out->SetStringField(TEXT("parent"), Parent);
	Out->SetStringField(TEXT("groom"), Groom->GetPathName());
	Out->SetStringField(TEXT("socket"), SocketName);
	Out->SetStringField(TEXT("relative_location"), Template->GetRelativeLocation().ToString());
	Out->SetStringField(TEXT("relative_rotation"), Template->GetRelativeRotation().ToString());
	Out->SetStringField(TEXT("binding"), BindingToUse ? BindingToUse->GetPathName() : TEXT(""));
	Out->SetBoolField(TEXT("binding_duplicated"), bDuplicated);
	Out->SetBoolField(TEXT("binding_saved"), bBindingSaved);
	Out->SetArrayField(TEXT("material_slots"), VmhJson::NameArray(Template->GetMaterialSlotNames()));
	Out->SetArrayField(TEXT("materials_copied"), VmhJson::StringArray(MaterialsCopied));
	Out->SetBoolField(TEXT("compiled"), Blueprint->Status != BS_Error);
	Out->SetStringField(TEXT("hint"), TEXT("a groom binding builds asynchronously; if bSaveBinding was false, wait at least a minute before saving it yourself (a save mid-build can hang the editor), and never spawn a preview of a character wearing grooms before the face is finished sculpting"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanCharacterService::ImportGroom(const FString& AlembicPath, const FString& PackagePath, const FString& AssetName, bool bRigidBinding, float HairWidthCm, float TipScale)
{
	if (!FPaths::FileExists(AlembicPath))
	{
		return VmhJson::Error(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("No file at %s"), *AlembicPath));
	}
	if (PackagePath.IsEmpty() || AssetName.IsEmpty())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("PackagePath and AssetName are required"));
	}
	UAssetImportTask* Task = NewObject<UAssetImportTask>();
	Task->Filename = AlembicPath;
	Task->DestinationPath = PackagePath;
	Task->DestinationName = AssetName;
	Task->bAutomated = true; // UHairStrandsFactory::FactoryCreateFile skips its options dialog when automated
	Task->bReplaceExisting = true;
	Task->bSave = false;
	// the hair factory named explicitly: left to choose, the static-mesh AlembicImportFactory claimed a Blender groom
	// .abc and made nothing (2026-10-07). It reads .abc only when the Alembic Groom Importer plugin is on (this
	// plugin's .uplugin enables it): without it the factory answered "Unknown extension 'abc'".
	Task->Factory = NewObject<UHairStrandsFactory>();
	FAssetToolsModule::GetModule().Get().ImportAssetTasks({ Task });

	UGroomAsset* Groom = nullptr;
	for (UObject* Object : Task->GetObjects())
	{
		Groom = Cast<UGroomAsset>(Object);
		if (Groom) break;
	}
	if (!Groom)
	{
		Groom = VmhAssets::Load<UGroomAsset>(PackagePath / AssetName);
	}
	if (!Groom)
	{
		return VmhJson::Error(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("no groom came out of %s (see the Output Log; .abc guides/strands only)"), *AlembicPath));
	}
	// what a custom (Blender) groom needs to render, 2026-10-07 on DrHart: every LOD Rigid (Skinning draws nothing
	// without a binding asset) and a width override (the .abc has no widths: a hair-thin default, a ghost)
	int32 LODsRigid = 0;
	if (bRigidBinding)
	{
		for (FHairGroupsLOD& GroupLOD : Groom->GetHairGroupsLOD())
		{
			for (FHairLODSettings& LOD : GroupLOD.LODs)
			{
				LOD.BindingType = EGroomBindingType::Rigid;
				++LODsRigid;
			}
		}
	}
	if (HairWidthCm > 0.f)
	{
		for (FHairGroupsRendering& Rendering : Groom->GetHairGroupsRendering())
		{
			Rendering.GeometrySettings.HairWidth_Override = true;
			Rendering.GeometrySettings.HairWidth = HairWidthCm;
			Rendering.GeometrySettings.HairRootScale = 1.f;
			Rendering.GeometrySettings.HairTipScale = TipScale;
		}
	}
	if (bRigidBinding || HairWidthCm > 0.f)
	{
		Groom->PostEditChange();
	}
	Groom->MarkPackageDirty();

	TArray<TSharedPtr<FJsonValue>> Groups;
	int32 TotalCurves = 0, TotalGuides = 0;
	for (const FHairGroupInfoWithVisibility& Info : Groom->GetHairGroupsInfo())
	{
		TSharedPtr<FJsonObject> G = VmhJson::Obj();
		G->SetNumberField(TEXT("group_id"), Info.GroupID);
		G->SetNumberField(TEXT("curves"), Info.NumCurves);
		G->SetNumberField(TEXT("guides"), Info.NumGuides);
		TotalCurves += Info.NumCurves;
		TotalGuides += Info.NumGuides;
		Groups.Add(MakeShared<FJsonValueObject>(G));
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("groom"), Groom->GetPathName());
	Out->SetArrayField(TEXT("groups"), Groups);
	Out->SetNumberField(TEXT("total_curves"), TotalCurves);
	Out->SetNumberField(TEXT("total_guides"), TotalGuides);
	Out->SetNumberField(TEXT("lods_rigid"), LODsRigid);
	Out->SetNumberField(TEXT("hair_width_cm"), HairWidthCm > 0.f ? HairWidthCm : 0.f);
	Out->SetStringField(TEXT("hint"), TEXT("save the groom in a LATER call (a groom saved in the call that changed it can deadlock the editor); attach it rigidly with add_groom_to_blueprint(SocketName='head', bInverseSocketTransform=true) - binding 40000 strands to a face mesh took 16+ minutes"));
	return VmhJson::Ok(Out);
}
