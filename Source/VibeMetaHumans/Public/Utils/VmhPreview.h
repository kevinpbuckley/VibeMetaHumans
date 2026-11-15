// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Utils/VmhSkin.h"

class AActor;
class FPreviewScene;
class UAnimSequence;
class UBlueprint;
class USkeletalMeshComponent;
class UWorld;

/**
 * A built MetaHuman Blueprint spawned in a private preview world and posed the way the game poses it (2026-11-14).
 *
 * The editor level does not evaluate a scripted pose (override_animation_data on a script-spawned actor leaves the
 * bones at rest) and sampling an AnimSequence on the asset skips the body's post-process AnimBP, whose RigLogic
 * correctives moved bones up to 14 cm in seated poses. Here the Body plays the clip in single-node mode and every
 * skeletal mesh component's anim graph (post-process instance included) is evaluated synchronously, so the bones match
 * PIE. No level change, no PIE, no viewport: used by capture_fit_views, dump_runtime_poses and the "runtime" pose source
 * of the wardrobe tools.
 */
namespace VmhPreview
{
	class VIBEMETAHUMANS_API FCharacter
	{
	public:
		FCharacter();
		~FCharacter();

		/** Spawn the Blueprint's class in a new preview world (lit by a soft key and a sky light). */
		bool Spawn(UBlueprint* Blueprint, FString& OutError);

		/** The preview world (created on first use), for actors spawned by someone else (the Creator's editing actor). */
		UWorld* World();

		/** Use an actor already spawned in World() (its skeletal mesh component BodyComponentName is the body, if any). */
		void Adopt(AActor* InActor);

		/** Play Anim on the body component at Time (nullptr: the rest / Blueprint pose) and evaluate every anim graph. */
		bool Pose(UAnimSequence* Anim, double Time, FString& OutError);

		/** Force every skeletal mesh and groom LOD (-1 = automatic). */
		void ForceLod(int32 Lod);

		/** Component-space bone transforms of the body component, by name (a VmhSkin leader pose). */
		VmhSkin::FPose BodyPose(const FString& Label) const;

		/** World bounds of every visible primitive, and of the named body bones (empty = everything). */
		FBox Bounds() const;
		FBox BoneBox(const TArray<FName>& Bones, double Pad) const;

		/**
		 * Render a view with a scene capture. Camera at Eye looking at Target, FOVDegrees horizontal (or orthographic,
		 * OrthoWidth cm across, when OrthoWidth > 0); bBaseColor captures unlit base colour; the key light comes from the view direction turned by LightOffset. Pixels are RGBA8 with alpha
		 * forced to 255 (captures come back with alpha 0, base colour especially).
		 */
		bool Capture(const FVector& Eye, const FVector& Target, float FOVDegrees, int32 Width, int32 Height, float ExposureBias, TArray<FColor>& OutPixels, FString& OutError, float OrthoWidth = 0.f, bool bBaseColor = false, const FRotator& LightOffset = FRotator(-25.f, 30.f, 0.f));

		USkeletalMeshComponent* Body() const { return BodyComponent; }
		AActor* Actor() const { return SpawnedActor; }
		FString BodyComponentName = TEXT("Body");

	private:
		void Evaluate(float DeltaTime);

		TUniquePtr<FPreviewScene> Scene;
		AActor* SpawnedActor = nullptr;
		USkeletalMeshComponent* BodyComponent = nullptr;
	};

	/** PNG to disk (directory created). */
	VIBEMETAHUMANS_API bool SavePng(const FString& Path, int32 Width, int32 Height, const TArray<FColor>& Pixels, FString& OutError);

	/** A leader pose as JSON: {"label", "bones": {name: [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]}} (body component space, cm). */
	VIBEMETAHUMANS_API TSharedPtr<class FJsonObject> PoseToJson(const VmhSkin::FPose& Pose);

	/** Parse PoseToJson output (or a dump written in PIE in the same shape). */
	VIBEMETAHUMANS_API bool PoseFromJson(const TSharedPtr<class FJsonObject>& Json, VmhSkin::FPose& OutPose, FString& OutError);
}
