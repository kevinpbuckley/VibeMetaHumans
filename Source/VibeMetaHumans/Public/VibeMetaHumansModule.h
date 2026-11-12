// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Modules/ModuleManager.h"

DECLARE_LOG_CATEGORY_EXTERN(LogVibeMetaHumans, Log, All);

/**
 * VibeMetaHumans editor module.
 *
 * Registers every non-abstract UToolsetDefinition subclass declared in this module with Epic's
 * ToolsetRegistry, which in turn exposes their AICallable static UFUNCTIONs as tools on the
 * engine's native MCP endpoint (ModelContextProtocol plugin) and as unreal.<Service>.* in editor
 * Python. Skill packs under <plugin>/Skills are discovered natively by UE 5.9's ToolsetRegistry
 * (MarkdownAgentSkills) and served as /Skills/VibeMetaHumans/<pack>.
 */
class FVibeMetaHumansModule : public IModuleInterface
{
public:
	virtual void StartupModule() override;
	virtual void ShutdownModule() override;

	/** Register this module's service toolsets with UE 5.9's ToolsetRegistry. */
	void RegisterToolsets();
	/** Unregister this module's service toolsets. */
	void UnregisterToolsets();

	static inline FVibeMetaHumansModule& Get()
	{
		return FModuleManager::LoadModuleChecked<FVibeMetaHumansModule>("VibeMetaHumans");
	}

	static inline bool IsAvailable()
	{
		return FModuleManager::Get().IsModuleLoaded("VibeMetaHumans");
	}

private:
	// False when running as a commandlet (cook, etc.) — toolsets are skipped so the headless
	// editor can exit cleanly.
	bool bServicesInitialized = false;
	bool bToolsetsRegistered = false;
};
