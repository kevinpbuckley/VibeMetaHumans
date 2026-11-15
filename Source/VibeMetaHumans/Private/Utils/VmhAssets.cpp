// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Utils/VmhAssets.h"

#include "AssetRegistry/AssetRegistryModule.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "AssetToolsModule.h"
#include "Editor.h"
#include "EditorAssetLibrary.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "Factories/Factory.h"
#include "GameFramework/Actor.h"
#include "IAssetTools.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "Subsystems/AssetEditorSubsystem.h"
#include "UObject/Package.h"
#include "UObject/TopLevelAssetPath.h"
#include "UObject/UObjectHash.h"
#include "Utils/VmhJson.h"

namespace VmhAssets
{
	UObject* Load(const FString& Path)
	{
		const FString Trimmed = Path.TrimStartAndEnd();
		if (Trimmed.IsEmpty())
		{
			return nullptr;
		}
		// A bare package path (/Game/Dir/Name) names the asset Name inside that package; never
		// hand back the UPackage itself.
		FString ObjectPath = Trimmed;
		if (!Trimmed.Contains(TEXT(".")) && !Trimmed.Contains(TEXT(":")))
		{
			FString Clean = Trimmed;
			while (Clean.EndsWith(TEXT("/")))
			{
				Clean.LeftChopInline(1);
			}
			ObjectPath = Clean + TEXT(".") + FPackageName::GetShortName(Clean);
		}
		if (UObject* Found = FindObject<UObject>(nullptr, *ObjectPath))
		{
			return Found;
		}
		if (UEditorAssetLibrary::DoesAssetExist(ObjectPath))
		{
			if (UObject* Loaded = UEditorAssetLibrary::LoadAsset(ObjectPath))
			{
				return Loaded;
			}
		}
		if (UObject* Loaded = LoadObject<UObject>(nullptr, *ObjectPath))
		{
			return Loaded;
		}
		// Last resort: whatever the caller named, as long as it is not a package.
		UObject* Raw = LoadObject<UObject>(nullptr, *Trimmed);
		if (UPackage* Pkg = Cast<UPackage>(Raw))
		{
			UObject* Inner = nullptr;
			ForEachObjectWithPackage(Pkg, [&Inner](UObject* Obj)
			{
				if (Obj->IsAsset()) { Inner = Obj; return false; }
				return true;
			}, EGetObjectsFlags::None);
			return Inner;
		}
		return Raw;
	}

	void SplitAssetPath(const FString& Path, FString& OutPackagePath, FString& OutAssetName)
	{
		FString Clean = Path.TrimStartAndEnd();
		int32 DotIndex = INDEX_NONE;
		if (Clean.FindChar(TEXT('.'), DotIndex))
		{
			Clean = Clean.Left(DotIndex);
		}
		while (Clean.EndsWith(TEXT("/")))
		{
			Clean.LeftChopInline(1);
		}
		OutPackagePath = FPackageName::GetLongPackagePath(Clean);
		OutAssetName = FPackageName::GetShortName(Clean);
	}

	UObject* Create(const FString& PackagePath, const FString& AssetName, UClass* AssetClass, const FString& FactoryClassPathOrName, FString& OutError)
	{
		if (!AssetClass)
		{
			OutError = TEXT("Create: null asset class");
			return nullptr;
		}
		FString CleanPackage = PackagePath.TrimStartAndEnd();
		while (CleanPackage.EndsWith(TEXT("/")))
		{
			CleanPackage.LeftChopInline(1);
		}
		const FString CleanName = AssetName.TrimStartAndEnd();
		if (CleanPackage.IsEmpty() || CleanName.IsEmpty())
		{
			OutError = TEXT("Create: package path and asset name are required (e.g. /Game/MetaHumans, MH_Hero)");
			return nullptr;
		}
		if (!FPackageName::IsValidLongPackageName(CleanPackage / CleanName))
		{
			OutError = FString::Printf(TEXT("Create: '%s/%s' is not a valid long package name"), *CleanPackage, *CleanName);
			return nullptr;
		}
		if (UEditorAssetLibrary::DoesAssetExist(CleanPackage / CleanName))
		{
			OutError = FString::Printf(TEXT("An asset already exists at %s/%s"), *CleanPackage, *CleanName);
			return nullptr;
		}

		UFactory* Factory = nullptr;
		if (!FactoryClassPathOrName.IsEmpty())
		{
			UClass* FactoryClass = VmhJson::FindClass(FactoryClassPathOrName);
			if (!FactoryClass || !FactoryClass->IsChildOf(UFactory::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Factory class '%s' was not found (is its plugin enabled?)"), *FactoryClassPathOrName);
				return nullptr;
			}
			Factory = NewObject<UFactory>(GetTransientPackage(), FactoryClass);
		}

		IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
		UObject* Created = AssetTools.CreateAsset(CleanName, CleanPackage, AssetClass, Factory);
		if (!Created)
		{
			OutError = FString::Printf(TEXT("AssetTools could not create %s at %s/%s"), *AssetClass->GetName(), *CleanPackage, *CleanName);
		}
		return Created;
	}

	bool Save(UObject* Asset, FString& OutError)
	{
		if (!Asset)
		{
			OutError = TEXT("Save: null asset");
			return false;
		}
		Asset->MarkPackageDirty();
		if (!UEditorAssetLibrary::SaveLoadedAsset(Asset, /*bOnlyIfIsDirty*/ false))
		{
			OutError = FString::Printf(TEXT("Could not save %s"), *Asset->GetPathName());
			return false;
		}
		return true;
	}

	FString PathOf(const UObject* Object)
	{
		return Object ? Object->GetPathName() : FString();
	}

	UWorld* EditorWorld()
	{
		return GEditor ? GEditor->GetEditorWorldContext().World() : nullptr;
	}

	AActor* FindActor(const FString& LabelOrName)
	{
		UWorld* World = EditorWorld();
		if (!World || LabelOrName.IsEmpty())
		{
			return nullptr;
		}
		AActor* ByName = nullptr;
		for (TActorIterator<AActor> It(World); It; ++It)
		{
			AActor* Actor = *It;
			if (!Actor)
			{
				continue;
			}
#if WITH_EDITOR
			if (Actor->GetActorLabel().Equals(LabelOrName, ESearchCase::IgnoreCase))
			{
				return Actor;
			}
#endif
			if (!ByName && (Actor->GetName().Equals(LabelOrName, ESearchCase::IgnoreCase) || Actor->GetPathName() == LabelOrName))
			{
				ByName = Actor;
			}
		}
		return ByName;
	}

	TArray<FString> FindAssetsOfClass(const FString& ClassPath, const FString& SearchRoot, bool bRecursiveClasses)
	{
		TArray<FString> Out;
		UClass* Class = VmhJson::FindClass(ClassPath);
		if (!Class)
		{
			return Out;
		}
		IAssetRegistry& Registry = FModuleManager::LoadModuleChecked<FAssetRegistryModule>("AssetRegistry").Get();
		FARFilter Filter;
		Filter.ClassPaths.Add(Class->GetClassPathName());
		Filter.bRecursiveClasses = bRecursiveClasses;
		if (!SearchRoot.IsEmpty())
		{
			Filter.PackagePaths.Add(FName(*SearchRoot));
			Filter.bRecursivePaths = true;
		}
		TArray<FAssetData> Assets;
		Registry.GetAssets(Filter, Assets);
		for (const FAssetData& Asset : Assets)
		{
			Out.Add(Asset.GetObjectPathString());
		}
		Out.Sort();
		return Out;
	}

	bool OpenEditor(UObject* Asset)
	{
		if (!Asset || !GEditor)
		{
			return false;
		}
		UAssetEditorSubsystem* Subsystem = GEditor->GetEditorSubsystem<UAssetEditorSubsystem>();
		return Subsystem && Subsystem->OpenEditorForAsset(Asset);
	}
}
