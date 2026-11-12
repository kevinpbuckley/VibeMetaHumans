// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * Resolves VibeMetaHumans plugin paths wherever the plugin is installed
 * (Project/Plugins, Engine/Plugins/Marketplace, or elsewhere).
 */
class VIBEMETAHUMANS_API FVibeMetaHumansPaths
{
public:
	/** Absolute path to the plugin directory, or empty if the plugin cannot be located. */
	static FString GetPluginDir();

	/** Absolute path to <plugin>/Content, or empty if the plugin cannot be located. */
	static FString GetPluginContentDir();

	/** Absolute path to <plugin>/Skills (UE 5.9's native skill discovery root), or empty if the plugin cannot be located. */
	static FString GetSkillsDir();

	/** Absolute path to Project/Saved/VibeMetaHumans (created on demand; empty on failure). */
	static FString GetSavedDir();

	/** The plugin's VersionName from its descriptor, or "unknown". */
	static FString GetPluginVersionName();
};
