// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Utils/VibeMetaHumansPaths.h"

#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Interfaces/IPluginManager.h"
#include "Misc/Paths.h"
#include "PluginDescriptor.h"

namespace
{
	const TCHAR* PluginName = TEXT("VibeMetaHumans");
}

FString FVibeMetaHumansPaths::GetPluginDir()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (Plugin.IsValid())
	{
		return FPaths::ConvertRelativePathToFull(Plugin->GetBaseDir());
	}

	// Fallback for the unusual case where the plugin manager does not know us yet.
	const FString ProjectPath = FPaths::ConvertRelativePathToFull(FPaths::ProjectPluginsDir() / PluginName);
	if (FPaths::FileExists(ProjectPath / FString(PluginName) + TEXT(".uplugin")))
	{
		return ProjectPath;
	}
	return FString();
}

FString FVibeMetaHumansPaths::GetPluginContentDir()
{
	const FString PluginDir = GetPluginDir();
	return PluginDir.IsEmpty() ? FString() : PluginDir / TEXT("Content");
}

FString FVibeMetaHumansPaths::GetSkillsDir()
{
	const FString PluginDir = GetPluginDir();
	return PluginDir.IsEmpty() ? FString() : PluginDir / TEXT("Skills");
}

FString FVibeMetaHumansPaths::GetSavedDir()
{
	const FString SavedDir = FPaths::ProjectSavedDir() / PluginName;
	IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
	if (!PlatformFile.DirectoryExists(*SavedDir) && !PlatformFile.CreateDirectoryTree(*SavedDir))
	{
		return FString();
	}
	return FPaths::ConvertRelativePathToFull(SavedDir);
}

FString FVibeMetaHumansPaths::GetPluginVersionName()
{
	const TSharedPtr<IPlugin> Plugin = IPluginManager::Get().FindPlugin(PluginName);
	if (Plugin.IsValid() && !Plugin->GetDescriptor().VersionName.IsEmpty())
	{
		return Plugin->GetDescriptor().VersionName;
	}
	return TEXT("unknown");
}
