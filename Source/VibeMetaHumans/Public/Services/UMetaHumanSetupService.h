// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "UMetaHumanSetupService.generated.h"

/**
 * MetaHuman Setup Service - environment checks and asset discovery for MetaHuman work.
 *
 * Call get_status() first in any MetaHuman session: it reports which MetaHuman plugins are enabled,
 * whether the "MetaHuman Creator Core Data" optional content is installed (presets, texture
 * synthesis and the wardrobe library all depend on it), and what MetaHuman assets the project
 * already has.
 *
 * Python Usage:
 *   import unreal, json
 *   svc = unreal.MetaHumanSetupService
 *   print(json.loads(svc.get_status()))
 *   # asset discovery: EditorToolset.AssetToolset.FindAssets; opening an editor: EditorToolset.EditorAppToolset.OpenEditorForAsset
 */
UCLASS(BlueprintType)
class VIBEMETAHUMANS_API UMetaHumanSetupService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Environment summary as JSON: plugins (name -> enabled), optional_content_installed, editor subsystem
	 * availability, preset/wardrobe folders, counts of MetaHuman assets in the project, Live Link source count.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Setup")
	static FString GetStatus();

	/** Names of the enum values an agent can pass to the other services (body types, rig types, pipeline types, quality, input types, moods...). */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Setup")
	static FString GetEnumOptions();
};
