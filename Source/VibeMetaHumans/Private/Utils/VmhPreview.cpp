// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Utils/VmhPreview.h"

#include "Animation/AnimSequence.h"
#include "Components/MeshComponent.h"
#include "Components/PrimitiveComponent.h"
#include "AssetCompilingManager.h"
#include "ContentStreaming.h"
#include "ShaderCompiler.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/SkeletalMeshComponent.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "Engine/Blueprint.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Kismet/KismetRenderingLibrary.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "PreviewScene.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "RenderingThread.h"
#include "UObject/Package.h"

namespace VmhPreview
{
	FCharacter::FCharacter() = default;

	FCharacter::~FCharacter()
	{
		if (SpawnedActor && Scene && Scene->GetWorld())
		{
			Scene->GetWorld()->DestroyActor(SpawnedActor);
		}
		SpawnedActor = nullptr;
		BodyComponent = nullptr;
		Scene.Reset();
	}

	UWorld* FCharacter::World()
	{
		if (!Scene)
		{
			Scene = MakeUnique<FPreviewScene>(FPreviewScene::ConstructionValues().SetSkyBrightness(1.0f).SetLightBrightness(3.0f));
		}
		return Scene->GetWorld();
	}

	void FCharacter::Adopt(AActor* InActor)
	{
		SpawnedActor = InActor;
		BodyComponent = nullptr;
		if (!InActor)
		{
			return;
		}
		TArray<USkeletalMeshComponent*> Skinned;
		InActor->GetComponents(Skinned);
		for (USkeletalMeshComponent* C : Skinned)
		{
			if (C->GetName() == BodyComponentName) { BodyComponent = C; }
		}
		TArray<UMeshComponent*> Meshes;
		InActor->GetComponents(Meshes);
		for (UMeshComponent* M : Meshes)
		{
			M->SetTextureForceResidentFlag(true);
			M->PrestreamTextures(30.f, true);
		}
		Evaluate(0.f);
	}

	bool FCharacter::Spawn(UBlueprint* Blueprint, FString& OutError)
	{
		UClass* Class = Blueprint ? Blueprint->GeneratedClass.Get() : nullptr;
		if (!Class || !Class->IsChildOf(AActor::StaticClass()))
		{
			OutError = TEXT("not a compiled actor Blueprint");
			return false;
		}
		UWorld* World = this->World();
		FActorSpawnParameters Params;
		Params.ObjectFlags = RF_Transient;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
		SpawnedActor = World ? World->SpawnActor<AActor>(Class, FTransform::Identity, Params) : nullptr;
		if (!SpawnedActor)
		{
			OutError = TEXT("the Blueprint could not be spawned in a preview world");
			return false;
		}
		TArray<USkeletalMeshComponent*> Skinned;
		SpawnedActor->GetComponents(Skinned);
		for (USkeletalMeshComponent* C : Skinned)
		{
			// the asset tools judge the pose, not the visibility: never skip an update because nothing renders it
			C->VisibilityBasedAnimTickOption = EVisibilityBasedAnimTickOption::AlwaysTickPoseAndRefreshBones;
			C->bEnableUpdateRateOptimizations = false;
			if (C->GetName() == BodyComponentName || C->GetFName() == FName(*BodyComponentName))
			{
				BodyComponent = C;
			}
		}
		if (!BodyComponent)
		{
			OutError = FString::Printf(TEXT("the spawned Blueprint has no skeletal mesh component named %s"), *BodyComponentName);
			return false;
		}
		// full-resolution textures for review captures (streaming placeholders drew a grey checker skin)
		TArray<UMeshComponent*> Meshes;
		SpawnedActor->GetComponents(Meshes);
		for (UMeshComponent* M : Meshes)
		{
			M->SetTextureForceResidentFlag(true);
			M->PrestreamTextures(30.f, true);
		}
		Evaluate(0.f);
		return true;
	}

	void FCharacter::Evaluate(float DeltaTime)
	{
		if (!SpawnedActor)
		{
			return;
		}
		TArray<USkeletalMeshComponent*> Skinned;
		SpawnedActor->GetComponents(Skinned);
		// the body first (the face copies its pose), then every other component that runs its own graph
		Skinned.Sort([this](const USkeletalMeshComponent& A, const USkeletalMeshComponent& B) { return (&A == BodyComponent) > (&B == BodyComponent); });
		for (int32 Pass = 0; Pass < 2; ++Pass)
		{
			for (USkeletalMeshComponent* C : Skinned)
			{
				if (!C->GetSkeletalMeshAsset() || C->LeaderPoseComponent.IsValid())
				{
					continue;
				}
				C->TickAnimation(DeltaTime, false);
				C->RefreshBoneTransforms(nullptr);
			}
		}
		for (USkeletalMeshComponent* C : Skinned)
		{
			C->UpdateBounds();
			C->MarkRenderTransformDirty();
			C->MarkRenderDynamicDataDirty();
		}
		if (UWorld* World = Scene ? Scene->GetWorld() : nullptr)
		{
			World->SendAllEndOfFrameUpdates();
		}
	}

	bool FCharacter::Pose(UAnimSequence* Anim, double Time, FString& OutError)
	{
		if (!BodyComponent)
		{
			OutError = TEXT("spawn the character first");
			return false;
		}
		if (Anim)
		{
			BodyComponent->SetAnimationMode(EAnimationMode::AnimationSingleNode);
			BodyComponent->SetAnimation(Anim);
			BodyComponent->SetPlayRate(0.f);
			BodyComponent->SetPosition(static_cast<float>(Time), false);
		}
		Evaluate(0.f);
		return true;
	}

	void FCharacter::ForceLod(int32 Lod)
	{
		if (!SpawnedActor)
		{
			return;
		}
		TArray<USkinnedMeshComponent*> Skinned;
		SpawnedActor->GetComponents(Skinned);
		for (USkinnedMeshComponent* C : Skinned)
		{
			C->SetForcedLOD(Lod < 0 ? 0 : Lod + 1);
		}
		Evaluate(0.f);
	}

	VmhSkin::FPose FCharacter::BodyPose(const FString& Label) const
	{
		VmhSkin::FPose Pose;
		Pose.Label = Label;
		if (!BodyComponent || !BodyComponent->GetSkeletalMeshAsset())
		{
			return Pose;
		}
		const USkeletalMesh* Mesh = BodyComponent->GetSkeletalMeshAsset();
		const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
		const TArray<FTransform>& CS = BodyComponent->GetComponentSpaceTransforms();
		// bones the LOD does not evaluate (the ~144 hand helpers of an Optimized body) hold stale transforms at the
		// component origin: give them their reference offset from the (evaluated) parent, as the wardrobe skinning does
		TSet<int32> Required;
		const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
		const int32 Lod = FMath::Clamp(BodyComponent->GetPredictedLODLevel(), 0, RenderData ? RenderData->LODRenderData.Num() - 1 : 0);
		if (RenderData && RenderData->LODRenderData.IsValidIndex(Lod))
		{
			for (const FBoneIndexType B : RenderData->LODRenderData[Lod].RequiredBones) { Required.Add(B); }
		}
		TArray<FTransform> Out;
		Out.SetNum(Ref.GetNum());
		for (int32 B = 0; B < Ref.GetNum(); ++B)
		{
			const int32 Parent = Ref.GetParentIndex(B);
			const bool bEvaluated = B < CS.Num() && (Required.Num() == 0 || Required.Contains(B));
			Out[B] = bEvaluated ? CS[B] : (Parent == INDEX_NONE ? Ref.GetRefBonePose()[B] : Ref.GetRefBonePose()[B] * Out[Parent]);
			Pose.BoneCS.Add(Ref.GetBoneName(B), Out[B]);
		}
		return Pose;
	}

	FBox FCharacter::Bounds() const
	{
		FBox Box(ForceInit);
		if (!SpawnedActor)
		{
			return Box;
		}
		TArray<UPrimitiveComponent*> Prims;
		SpawnedActor->GetComponents(Prims);
		for (UPrimitiveComponent* P : Prims)
		{
			if (P && P->IsRegistered() && P->IsVisible() && !P->bHiddenInGame)
			{
				Box += P->Bounds.GetBox();
			}
		}
		return Box;
	}

	FBox FCharacter::BoneBox(const TArray<FName>& Bones, double Pad) const
	{
		FBox Box(ForceInit);
		if (!BodyComponent)
		{
			return Box;
		}
		for (const FName& Bone : Bones)
		{
			const int32 Index = BodyComponent->GetBoneIndex(Bone);
			if (Index != INDEX_NONE)
			{
				Box += BodyComponent->GetBoneTransform(Index).GetLocation();
			}
		}
		return Box.IsValid ? Box.ExpandBy(Pad) : Box;
	}

	bool FCharacter::Capture(const FVector& Eye, const FVector& Target, float FOVDegrees, int32 Width, int32 Height, float ExposureBias, TArray<FColor>& OutPixels, FString& OutError, float OrthoWidth, bool bBaseColor, const FRotator& LightOffset)
	{
		if (!Scene || !Scene->GetWorld())
		{
			OutError = TEXT("no preview world");
			return false;
		}
		const FRotator Rotation = (Target - Eye).Rotation();
		Scene->SetLightDirection(Rotation + LightOffset);
		UTextureRenderTarget2D* RenderTarget = NewObject<UTextureRenderTarget2D>(GetTransientPackage(), NAME_None, RF_Transient);
		RenderTarget->RenderTargetFormat = RTF_RGBA8;
		RenderTarget->ClearColor = FLinearColor(0.18f, 0.18f, 0.2f, 1.f);
		RenderTarget->InitAutoFormat(Width, Height);
		RenderTarget->UpdateResourceImmediate(true);
		USceneCaptureComponent2D* Capture = NewObject<USceneCaptureComponent2D>(GetTransientPackage(), NAME_None, RF_Transient);
		Capture->TextureTarget = RenderTarget;
		Capture->CaptureSource = bBaseColor ? ESceneCaptureSource::SCS_BaseColor : ESceneCaptureSource::SCS_FinalToneCurveHDR;
		Capture->bCaptureEveryFrame = false;
		Capture->bCaptureOnMovement = false;
		Capture->bAlwaysPersistRenderingState = true;
		Capture->ProjectionType = OrthoWidth > 0.f ? ECameraProjectionMode::Orthographic : ECameraProjectionMode::Perspective;
		Capture->FOVAngle = FOVDegrees;
		Capture->OrthoWidth = OrthoWidth > 0.f ? OrthoWidth : Capture->OrthoWidth;
		// fixed exposure, without the physical camera (with it the preview scene's lights rendered black)
		Capture->PostProcessSettings.bOverride_AutoExposureMethod = true;
		Capture->PostProcessSettings.AutoExposureMethod = EAutoExposureMethod::AEM_Manual;
		Capture->PostProcessSettings.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
		Capture->PostProcessSettings.AutoExposureApplyPhysicalCameraExposure = false;
		Capture->PostProcessSettings.bOverride_AutoExposureBias = true;
		Capture->PostProcessSettings.AutoExposureBias = ExposureBias;
		Scene->AddComponent(Capture, FTransform(Rotation, Eye));
		Scene->GetWorld()->SendAllEndOfFrameUpdates();
		// textures in (all mips) and several captures: the first one after a pose change can still draw the previous
		// skinning, and virtual texture pages arrive through the feedback of earlier frames
		// materials still compiling draw as the grey checker of the default material, and they are only queued when
		// first drawn: one warm-up capture, then finish shaders and assets
		Capture->CaptureScene();
		FlushRenderingCommands();
		FAssetCompilingManager::Get().FinishAllCompilation();
		if (GShaderCompilingManager)
		{
			GShaderCompilingManager->FinishAllCompilation();
		}
		IStreamingManager::Get().StreamAllResources(5.0f);
		for (int32 Frame = 0; Frame < 4; ++Frame)
		{
			Capture->CaptureScene();
			FlushRenderingCommands();
			IStreamingManager::Get().StreamAllResources(1.0f);
		}
		Capture->CaptureScene();
		FlushRenderingCommands();
		const bool bRead = UKismetRenderingLibrary::ReadRenderTarget(Capture, RenderTarget, OutPixels);
		Scene->RemoveComponent(Capture);
		if (!bRead || OutPixels.Num() != Width * Height)
		{
			OutError = TEXT("could not read the capture (is the editor running with -nullrhi?)");
			return false;
		}
		for (FColor& C : OutPixels)
		{
			C.A = 255; // captures come back with alpha 0 (scene colour alpha), which every viewer shows as transparent
		}
		return true;
	}

	bool SavePng(const FString& Path, int32 Width, int32 Height, const TArray<FColor>& Pixels, FString& OutError)
	{
		TArray64<uint8> Png;
		FImageUtils::PNGCompressImageArray(Width, Height, Pixels, Png);
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
		if (Png.Num() == 0 || !FFileHelper::SaveArrayToFile(Png, *Path))
		{
			OutError = FString::Printf(TEXT("could not write %s"), *Path);
			return false;
		}
		return true;
	}

	TSharedPtr<FJsonObject> PoseToJson(const VmhSkin::FPose& Pose)
	{
		TSharedPtr<FJsonObject> Out = MakeShared<FJsonObject>();
		Out->SetStringField(TEXT("label"), Pose.Label);
		TSharedPtr<FJsonObject> Bones = MakeShared<FJsonObject>();
		for (const TPair<FName, FTransform>& B : Pose.BoneCS)
		{
			const FVector T = B.Value.GetLocation();
			const FQuat Q = B.Value.GetRotation();
			const FVector S = B.Value.GetScale3D();
			TArray<TSharedPtr<FJsonValue>> V;
			for (const double X : { T.X, T.Y, T.Z, Q.X, Q.Y, Q.Z, Q.W, S.X, S.Y, S.Z })
			{
				V.Add(MakeShared<FJsonValueNumber>(FMath::RoundToDouble(X * 1e5) / 1e5));
			}
			Bones->SetArrayField(B.Key.ToString(), V);
		}
		Out->SetObjectField(TEXT("bones"), Bones);
		return Out;
	}

	bool PoseFromJson(const TSharedPtr<FJsonObject>& Json, VmhSkin::FPose& OutPose, FString& OutError)
	{
		const TSharedPtr<FJsonObject>* Bones = nullptr;
		if (!Json.IsValid() || !Json->TryGetObjectField(TEXT("bones"), Bones))
		{
			OutError = TEXT("a pose is {\"label\":..., \"bones\": {name: [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]}}");
			return false;
		}
		Json->TryGetStringField(TEXT("label"), OutPose.Label);
		for (const TPair<FString, TSharedPtr<FJsonValue>>& B : (*Bones)->Values)
		{
			const TArray<TSharedPtr<FJsonValue>>& V = B.Value->AsArray();
			if (V.Num() < 7)
			{
				OutError = FString::Printf(TEXT("bone %s: need at least [tx,ty,tz, qx,qy,qz,qw]"), *B.Key);
				return false;
			}
			const FVector T(V[0]->AsNumber(), V[1]->AsNumber(), V[2]->AsNumber());
			FQuat Q(V[3]->AsNumber(), V[4]->AsNumber(), V[5]->AsNumber(), V[6]->AsNumber());
			Q.Normalize();
			const FVector S = V.Num() >= 10 ? FVector(V[7]->AsNumber(), V[8]->AsNumber(), V[9]->AsNumber()) : FVector::OneVector;
			OutPose.BoneCS.Add(FName(*B.Key), FTransform(Q, T, S));
		}
		if (OutPose.BoneCS.Num() == 0)
		{
			OutError = TEXT("the pose has no bones");
			return false;
		}
		return true;
	}
}
