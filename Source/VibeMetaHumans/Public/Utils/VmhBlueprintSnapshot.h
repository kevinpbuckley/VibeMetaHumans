// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

class UBlueprint;

/**
 * Record a Blueprint's components (and class defaults) and put them back after a MetaHuman rebuild regenerates it
 * (2026-11-12). A rebuild of BP_<Name> drops every component added after the build (garments, props, a speaker) and
 * resets edited properties (Body mesh, Face anim class, override materials, tags).
 *
 * The snapshot keeps, per construction-script component: class, parent, attach socket, and every editable property
 * whose value differs from the component class default, as exported text. Restore re-creates missing components and
 * re-imports differing property values; it never deletes anything.
 */
namespace VmhBlueprintSnapshot
{
	/** {"blueprint", "components":[{name,class,parent,socket,properties:{Name:Text}}], "class_defaults":{Name:Text}} */
	VIBEMETAHUMANS_API TSharedPtr<FJsonObject> Take(UBlueprint* Blueprint);

	struct FRestoreOptions
	{
		bool bAddMissing = true;          // re-create components the Blueprint no longer has
		bool bApplyToExisting = true;     // re-apply recorded property values on components it still has
		bool bClassDefaults = true;       // re-apply recorded class default values (Tags, ...)
		bool bDryRun = false;
		TArray<FString> SkipPatterns;     // "Component.Property" or "Property" wildcards (e.g. "LODSync.*", "*.RelativeLocation")
	};

	/** Apply Snapshot to Blueprint; returns the report (added, applied, failed, skipped). Compiles unless dry run. */
	VIBEMETAHUMANS_API TSharedPtr<FJsonObject> Restore(UBlueprint* Blueprint, const TSharedPtr<FJsonObject>& Snapshot, const FRestoreOptions& Options, FString& OutError);
}
