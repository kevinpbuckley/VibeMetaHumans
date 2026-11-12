// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanSetupService.h"

#include "Editor.h"
#include "Features/IModularFeatures.h"
#include "ILiveLinkClient.h"
#include "Interfaces/IPluginManager.h"
#include "MetaHumanBodyType.h"
#include "MetaHumanCharacterAssemblySettings.h"
#include "MetaHumanCharacterEditorModule.h"
#include "MetaHumanCharacterEditorSubsystem.h"
#include "MetaHumanCharacterEditorWardrobeSettings.h"
#include "MetaHumanPerformance.h"
#include "MetaHumanTypes.h"
#include "Misc/Paths.h"
#include "Utils/VmhAssets.h"
#include "Utils/VmhJson.h"
#include "Utils/VibeMetaHumansPaths.h"
#include "VibeMetaHumansModule.h"

FString UMetaHumanSetupService::GetStatus()
{
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();

	// Plugins
	TSharedPtr<FJsonObject> Plugins = VmhJson::Obj();
	static const TCHAR* PluginNames[] = {
		TEXT("MetaHumanCharacter"), TEXT("MetaHuman"), TEXT("MetaHumanSDK"), TEXT("MetaHumanCoreTech"),
		TEXT("MetaHumanLiveLink"), TEXT("MetaHumanAnimationTools"), TEXT("MetaHumanCrowd"),
		TEXT("MetaHumanCalibrationProcessing"), TEXT("MetaHumanCharacterUAF"), TEXT("MetaHumanGenerator"),
		TEXT("LiveLink"), TEXT("LiveLinkHub"), TEXT("CaptureManagerEditor"), TEXT("Takes"),
		TEXT("AppleARKitFaceSupport"), TEXT("ToolsetRegistry"), TEXT("ModelContextProtocol"), TEXT("VibeUE"),
	};
	for (const TCHAR* Name : PluginNames)
	{
		const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(Name);
		if (!Plugin.IsValid())
		{
			Plugins->SetStringField(Name, TEXT("missing"));
		}
		else
		{
			Plugins->SetStringField(Name, Plugin->IsEnabled() ? TEXT("enabled") : TEXT("disabled"));
		}
	}
	Out->SetObjectField(TEXT("plugins"), Plugins);

	// Optional content (presets, texture synthesis model, body textures, wardrobe)
	const bool bOptional = FMetaHumanCharacterEditorModule::IsOptionalMetaHumanContentInstalled();
	Out->SetBoolField(TEXT("optional_content_installed"), bOptional);
	if (!bOptional)
	{
		Out->SetStringField(TEXT("optional_content_hint"),
			TEXT("Install 'MetaHuman Creator Core Data' from the Epic Games Launcher (Library > Unreal Engine 5.9 > Options). "
			     "Without it there are no presets, no texture synthesis and no wardrobe library; create_character still works from the blank template."));
	}
	if (const TSharedPtr<IPlugin> MHC = IPluginManager::Get().FindPlugin(TEXT("MetaHumanCharacter")))
	{
		Out->SetStringField(TEXT("optional_content_dir"), FPaths::ConvertRelativePathToFull(MHC->GetContentDir() / TEXT("Optional")));
	}

	// Editor subsystem
	UMetaHumanCharacterEditorSubsystem* Subsystem = GEditor ? GEditor->GetEditorSubsystem<UMetaHumanCharacterEditorSubsystem>() : nullptr;
	Out->SetBoolField(TEXT("character_editor_subsystem"), Subsystem != nullptr);
	if (Subsystem)
	{
		Out->SetBoolField(TEXT("texture_synthesis_enabled"), Subsystem->IsTextureSynthesisEnabled());
	}

	// Preset / wardrobe folders from the plugin's editor settings (read by reflection)
	if (const UMetaHumanCharacterEditorWardrobeSettings* Wardrobe = GetDefault<UMetaHumanCharacterEditorWardrobeSettings>())
	{
		Out->SetObjectField(TEXT("wardrobe_settings"), VmhJson::ObjectToJson(Wardrobe, { TEXT("PresetDirectories"), TEXT("WardrobeSections") }));
	}

	// Asset counts
	TSharedPtr<FJsonObject> Counts = VmhJson::Obj();
	Counts->SetNumberField(TEXT("characters"), VmhAssets::FindAssetsOfClass(TEXT("/Script/MetaHumanCharacter.MetaHumanCharacter"), TEXT("/Game")).Num());
	Counts->SetNumberField(TEXT("presets"), VmhAssets::FindAssetsOfClass(TEXT("/Script/MetaHumanCharacter.MetaHumanCharacter"), TEXT("/MetaHumanCharacter/Optional/Presets")).Num());
	Counts->SetNumberField(TEXT("identities"), VmhAssets::FindAssetsOfClass(TEXT("/Script/MetaHumanIdentity.MetaHumanIdentity"), TEXT("/Game")).Num());
	Counts->SetNumberField(TEXT("performances"), VmhAssets::FindAssetsOfClass(TEXT("/Script/MetaHumanPerformance.MetaHumanPerformance"), TEXT("/Game")).Num());
	Counts->SetNumberField(TEXT("capture_data"), VmhAssets::FindAssetsOfClass(TEXT("/Script/CaptureDataCore.CaptureData"), TEXT("/Game")).Num());
	Counts->SetNumberField(TEXT("wardrobe_items"), VmhAssets::FindAssetsOfClass(TEXT("/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem"), TEXT("")).Num());
	Counts->SetNumberField(TEXT("metahuman_blueprints"), VmhAssets::FindAssetsOfClass(TEXT("/Script/Engine.Blueprint"), TEXT("/Game/MetaHumans")).Num());
	Out->SetObjectField(TEXT("asset_counts"), Counts);

	// Live Link
	if (IModularFeatures::Get().IsModularFeatureAvailable(ILiveLinkClient::ModularFeatureName))
	{
		ILiveLinkClient& Client = IModularFeatures::Get().GetModularFeature<ILiveLinkClient>(ILiveLinkClient::ModularFeatureName);
		Out->SetNumberField(TEXT("live_link_sources"), Client.GetSources().Num());
		Out->SetNumberField(TEXT("live_link_subjects"), Client.GetSubjects(true, true).Num());
	}
	else
	{
		Out->SetStringField(TEXT("live_link"), TEXT("unavailable"));
	}

	Out->SetStringField(TEXT("plugin_version"), FVibeMetaHumansPaths::GetPluginVersionName());
	return VmhJson::Ok(Out);
}

FString UMetaHumanSetupService::GetEnumOptions()
{
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("body_type"), VmhJson::EnumNames(StaticEnum<EMetaHumanBodyType>()));
	Out->SetStringField(TEXT("rig_type"), VmhJson::EnumNames(StaticEnum<EMetaHumanRigType>()));
	Out->SetStringField(TEXT("pipeline_type"), VmhJson::EnumNames(StaticEnum<EMetaHumanDefaultPipelineType>()));
	Out->SetStringField(TEXT("quality_level"), VmhJson::EnumNames(StaticEnum<EMetaHumanQualityLevel>()));
	Out->SetStringField(TEXT("performance_input_type"), VmhJson::EnumNames(StaticEnum<EDataInputType>()));
	Out->SetStringField(TEXT("head_movement_mode"), VmhJson::EnumNames(StaticEnum<EPerformanceHeadMovementMode>()));
	if (const UEnum* Mood = FindFirstObject<UEnum>(TEXT("EAudioDrivenAnimationMood"), EFindFirstObjectOptions::NativeFirst))
	{
		Out->SetStringField(TEXT("audio_mood"), VmhJson::EnumNames(Mood));
	}
	if (const UEnum* Controls = FindFirstObject<UEnum>(TEXT("EAudioDrivenAnimationOutputControls"), EFindFirstObjectOptions::NativeFirst))
	{
		Out->SetStringField(TEXT("audio_output_controls"), VmhJson::EnumNames(Controls));
	}
	Out->SetStringField(TEXT("settings_sections"), TEXT("skin, eyes, makeup, head, face_evaluation, viewport"));
	Out->SetStringField(TEXT("wardrobe_slots"), TEXT("Hair, Eyebrows, Eyelashes, Mustache, Beard, Peachfuzz, Outfits, SkeletalMesh"));
	return VmhJson::Ok(Out);
}
