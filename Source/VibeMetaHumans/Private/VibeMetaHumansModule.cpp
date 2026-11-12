// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "VibeMetaHumansModule.h"

#include "Editor.h"
#include "HAL/IConsoleManager.h"
#include "Misc/CoreDelegates.h"
#include "Modules/ModuleManager.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "ToolsetRegistry/UToolsetRegistry.h"
#include "UObject/UObjectHash.h"
#include "UObject/UObjectIterator.h"

DEFINE_LOG_CATEGORY(LogVibeMetaHumans);

#define LOCTEXT_NAMESPACE "FVibeMetaHumansModule"

namespace
{
	// Collect every non-abstract UToolsetDefinition subclass defined in this module
	// (/Script/VibeMetaHumans) that exposes at least one AICallable tool. Reflection-based, so a
	// new U*Service class is picked up without editing this file.
	void GatherToolsetClasses(TArray<UClass*>& OutClasses)
	{
		TArray<UClass*> Derived;
		GetDerivedClasses(UToolsetDefinition::StaticClass(), Derived, /*bRecursive*/ true);

		const UPackage* ThisPackage = FindPackage(nullptr, TEXT("/Script/VibeMetaHumans"));
		for (UClass* Class : Derived)
		{
			if (!Class || Class->HasAnyClassFlags(CLASS_Abstract) || Class->GetOutermost() != ThisPackage)
			{
				continue;
			}

			bool bHasAICallable = false;
			for (TFieldIterator<UFunction> It(Class); It && !bHasAICallable; ++It)
			{
				const TValueOrError<bool, FString> Result = UToolsetDefinition::IsFunctionAICallable(*It);
				bHasAICallable = Result.HasValue() && Result.GetValue();
			}
			if (bHasAICallable)
			{
				OutClasses.Add(Class);
			}
		}
	}

	void ListToolsets()
	{
		TArray<UClass*> Classes;
		GatherToolsetClasses(Classes);
		UE_LOG(LogVibeMetaHumans, Display, TEXT("=== VibeMetaHumans toolsets (%d) ==="), Classes.Num());
		for (UClass* Class : Classes)
		{
			int32 ToolCount = 0;
			for (TFieldIterator<UFunction> It(Class); It; ++It)
			{
				const TValueOrError<bool, FString> Result = UToolsetDefinition::IsFunctionAICallable(*It);
				if (Result.HasValue() && Result.GetValue())
				{
					++ToolCount;
				}
			}
			UE_LOG(LogVibeMetaHumans, Display, TEXT("  VibeMetaHumans.%s  (%d tool(s))"), *Class->GetName(), ToolCount);
		}
	}

	FAutoConsoleCommand ListToolsetsCommand(
		TEXT("VibeMetaHumans.ListToolsets"),
		TEXT("List the VibeMetaHumans service toolsets and their AICallable tool counts"),
		FConsoleCommandDelegate::CreateStatic(ListToolsets));

	// No skill-reload command: UE 5.9 discovers <Plugin>/Skills/**/SKILL.md natively
	// (ToolsetRegistry MarkdownAgentSkills) and re-reads the files on every GetSkills call.
}

void FVibeMetaHumansModule::StartupModule()
{
	UE_LOG(LogVibeMetaHumans, Display, TEXT("VibeMetaHumans module started"));

	// Skip toolsets when running as a commandlet (e.g. -run=Cook): nothing to serve, and the
	// registry wants GEditor.
	if (IsRunningCommandlet())
	{
		return;
	}
	bServicesInitialized = true;

	// The registry needs GEditor; register now if it's already up (late/hot-reload load),
	// otherwise defer to PostEngineInit.
	if (GEditor)
	{
		RegisterToolsets();
	}
	else
	{
		FCoreDelegates::GetOnPostEngineInit().AddRaw(this, &FVibeMetaHumansModule::RegisterToolsets);
	}
}

void FVibeMetaHumansModule::RegisterToolsets()
{
	if (bToolsetsRegistered)
	{
		return;
	}

	if (!UToolsetRegistry::IsAvailable())
	{
		UE_LOG(LogVibeMetaHumans, Warning, TEXT("VibeMetaHumans: ToolsetRegistry not available; service toolsets not registered."));
		return;
	}

	TArray<UClass*> ToolsetClasses;
	GatherToolsetClasses(ToolsetClasses);
	for (UClass* Class : ToolsetClasses)
	{
		UToolsetRegistry::RegisterToolsetClass(Class);
	}
	bToolsetsRegistered = true;
	UE_LOG(LogVibeMetaHumans, Display, TEXT("VibeMetaHumans: registered %d service toolset(s) with ToolsetRegistry."), ToolsetClasses.Num());
}

void FVibeMetaHumansModule::UnregisterToolsets()
{
	if (!bToolsetsRegistered || !UToolsetRegistry::IsAvailable())
	{
		return;
	}

	TArray<UClass*> ToolsetClasses;
	GatherToolsetClasses(ToolsetClasses);
	for (UClass* Class : ToolsetClasses)
	{
		UToolsetRegistry::UnregisterToolsetClass(Class);
	}
	bToolsetsRegistered = false;
}

void FVibeMetaHumansModule::ShutdownModule()
{
	if (bServicesInitialized)
	{
		FCoreDelegates::GetOnPostEngineInit().RemoveAll(this);
		UnregisterToolsets();
	}
	UE_LOG(LogVibeMetaHumans, Display, TEXT("VibeMetaHumans module shut down"));
}

#undef LOCTEXT_NAMESPACE

IMPLEMENT_MODULE(FVibeMetaHumansModule, VibeMetaHumans)
