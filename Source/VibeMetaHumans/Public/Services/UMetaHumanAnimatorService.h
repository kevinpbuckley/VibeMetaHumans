// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "UMetaHumanAnimatorService.generated.h"

/**
 * MetaHuman Animator Service - offline facial performance capture: footage or audio in,
 * animation out.
 *
 * Three routes, matching the input types of a MetaHuman Performance asset:
 *  - MonoFootage: a webcam/DSLR video file (ingested with Epic's CaptureManagerIngestTools.IngestMonoVideo) - no identity needed.
 *  - DepthFootage: an iPhone Live Link Face take recorded in MetaHuman Animator mode
 *    (Epic's CaptureManagerIngestTools.IngestLiveLinkFace) plus a prepared MetaHuman Identity of the performer.
 *  - Audio: a SoundWave asset - no footage, no identity.
 * Footage ingest, processing (MetaHumanAnimatorPerformanceTools.ProcessPerformance, async) and
 * animation export (MetaHumanAnimatorExportTools.ExportPerformanceAnimationSequence) are Epic's own
 * AI tools; this service creates and configures the Performance, reports status, cancels processing
 * and exports a Level Sequence that targets a MetaHuman Blueprint.
 *
 * Python Usage:
 *   svc = unreal.MetaHumanAnimatorService
 *   # cd = (await call_tool("CaptureManagerToolset.CaptureManagerIngestTools.IngestMonoVideo", videoFilePath=..., audioFilePath="", slate="line01", takeNumber=1))["captureDataAssetPath"]
 *   p  = json.loads(svc.create_performance("/Game/MHA", "Perf_line01", "MonoFootage", cd, "", ""))["performance"]
 *   # await call_tool("MetaHumanAnimatorToolset.MetaHumanAnimatorPerformanceTools.ProcessPerformance", performance=p, params={})
 *   # audio only:
 *   p = json.loads(svc.create_performance("/Game/MHA", "Perf_vo01", "Audio", "", "", "/Game/Audio/vo01"))["performance"]
 *   # mood / output controls / head movement: EditorToolset.ObjectToolset.SetProperties on the Performance
 */
UCLASS(BlueprintType)
class VIBEMETAHUMANS_API UMetaHumanAnimatorService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	// =================================================================
	// Performance
	// =================================================================

	/**
	 * Create a MetaHuman Performance asset. InputType: MonoFootage | DepthFootage | Audio.
	 * CaptureDataPath for footage routes; IdentityPath required for DepthFootage; AudioPath (SoundWave) for Audio.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString CreatePerformance(const FString& PackagePath, const FString& AssetName, const FString& InputType, const FString& CaptureDataPath, const FString& IdentityPath, const FString& AudioPath);

	/**
	 * Patch any performance property (JSON by property name): HeadMovementMode, SolveType, bSkipDiagnostics,
	 * bHeadStabilization, StartFrameToProcess/EndFrameToProcess, FocalLength, UserExcludedFrames, ...
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString ConfigurePerformance(const FString& PerformancePath, const FString& JsonPatch);

	/** Cancel a running pipeline. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString CancelProcessing(const FString& PerformancePath);

	/** Status JSON: input type, inputs, can_process, is_processing, processed_frames, can_export, diagnostics. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString GetStatus(const FString& PerformancePath);

	// =================================================================
	// Export
	// =================================================================

	/**
	 * Export to a Level Sequence. TargetMetaHumanBlueprintPath: an assembled MetaHuman Blueprint to drive (empty = identity
	 * + camera only). Toggles for the identity, camera, audio and video tracks.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString ExportLevelSequence(const FString& PerformancePath, const FString& PackagePath, const FString& AssetName, const FString& TargetMetaHumanBlueprintPath, bool bExportIdentity = false, bool bExportCamera = true, bool bExportAudio = true, bool bExportVideo = false);

	// =================================================================
	// Face clips in a game (2026-11-14, from shipping six MetaHumans with baked lip sync)
	// =================================================================

	/**
	 * Audio-driven face bakes barely blink (blink curves peaked at 0.05; many clips had none), so faces stare. Insert a
	 * blink on CTRL_expressions_eyeBlinkL/R every MinIntervalSeconds..MaxIntervalSeconds (first one 0.6-1.4 s in)
	 * wherever the clip has no blink of its own (a value >= 0.6) within 1.2 s: 0.07 s up from the clip's own value, 1.0
	 * held 0.05 s, back 0.17 s after the peak, linear keys. Placement is deterministic per asset name, so a re-run adds
	 * nothing. AnimOrFolderPath: one AnimSequence, or a folder (recursive). Curve names are matched case-insensitively
	 * (bakes store them lowercased). OptionsJson: {"curves":[...], "existing_peak":0.6, "clear":1.2, "first_min":0.6,
	 * "first_max":1.4, "name_prefix":"FA_", "save":false}. bDryRun lists the planned blink times only.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString AddBlinks(const FString& AnimOrFolderPath, float MinIntervalSeconds = 2.8f, float MaxIntervalSeconds = 4.2f, bool bDryRun = false, const FString& OptionsJson = TEXT("{}"));

	/**
	 * Create a face Animation Blueprint that can PLAY face clips (the plugin's /MetaHumanCharacter/Face/ABP_Face is copy
	 * pose only, with no Slot: a face clip played on a slot is silently ignored). Graph: Copy Pose From Mesh (attached
	 * parent, curves) as the base; a second copy into Slot SlotName (default DefaultSlot) blended in by a Layered Blend
	 * Per Bone below FACIAL_C_FacialRoot with curves overriding (a plain slot after the copy snapped head and neck to the
	 * reference pose); Modify Curve (Scale) HeadControlSwitch x 0 (clips exported with head movement otherwise snap the
	 * head ~4.4 cm to the archetype pose); optionally a looping blink-only clip blended over it by max curve value
	 * (blinks between lines). SkeletonPath default /MetaHumanCharacter/Face/Face_Archetype_Skeleton. Set the result as
	 * the Face component's anim class on every built Blueprint (MetaHumanWardrobeService.post_build_fixup
	 * face_anim_class) and play lines with PlaySlotAnimationAsDynamicMontage on the Face's anim instance. Not saved.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Animator")
	static FString CreateFaceAnimBlueprint(const FString& PackagePath, const FString& AssetName = TEXT("ABP_Face_Slot"), const FString& SkeletonPath = TEXT(""), const FString& BlinkLoopAnimPath = TEXT(""), const FString& SlotName = TEXT("DefaultSlot"));
};
