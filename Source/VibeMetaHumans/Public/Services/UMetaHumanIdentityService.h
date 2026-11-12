// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "UMetaHumanIdentityService.generated.h"

/**
 * MetaHuman Identity Service - "Mesh to MetaHuman" from captured footage or DNA.
 *
 * A MetaHuman Identity asset turns a neutral-expression frame of depth footage (an iPhone
 * Live Link Face take captured in MetaHuman Animator mode, ingested with
 * Epic's CaptureManagerIngestTools.IngestLiveLinkFace) into a conformed template mesh, then the
 * MetaHuman cloud service turns that into a rigged face (DNA). A prepared identity is required to
 * process DEPTH footage performances; it can also be imported into a MetaHuman Character
 * (MetaHumanCharacterService.import_from_identity) to get a likeness.
 *
 * Monocular webcam video does not produce depth, so it cannot create an identity; use it for
 * MonoFootage performances instead.
 *
 * Python Usage:
 *   svc = unreal.MetaHumanIdentityService
 *   r = svc.create_identity_from_footage("/Game/MHA", "ID_Kevin", "/Game/MHA/Take01_FootageCaptureData", 120, 1, True, False)
 *   svc.get_status("/Game/MHA/ID_Kevin")                 # poll auto_rig_in_progress until false and has_dna is true
 *   # then Epic's MetaHumanAnimatorIdentityTools.PrepareIdentityForPerformance (identity=...) trains the predictive solvers (needed for depth performances); save with AssetToolset.SaveAsset
 */
UCLASS(BlueprintType)
class VIBEMETAHUMANS_API UMetaHumanIdentityService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Create an Identity from a depth Footage Capture Data asset: adds a Face part and a Neutral pose, promotes
	 * NeutralFrame, tracks it, conforms the template mesh, sets the body type index (1-6), then optionally sends the
	 * auto-rig request (async, needs Epic login) and/or trains the predictive solvers (only valid once DNA exists).
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Identity")
	static FString CreateIdentityFromFootage(const FString& PackagePath, const FString& AssetName, const FString& CaptureDataPath, int32 NeutralFrame, int32 BodyTypeIndex = 1, bool bRequestAutoRig = true, bool bPrepareForPerformance = false);

	/** Create an Identity from a .dna file (+ optional brows .json) and optionally prepare it for performance. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Identity")
	static FString CreateIdentityFromDna(const FString& PackagePath, const FString& AssetName, const FString& DnaFilePath, const FString& BrowsJsonPath, bool bPrepareForPerformance = true);

	/** Status JSON: logged_in, auto_rig_in_progress, tracking_in_progress, has_dna, has_predictive_solvers, conformal_rig_valid, poses/frames, body type, diagnostics. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Identity")
	static FString GetStatus(const FString& IdentityPath);

	/** Open the Epic login flow for the MetaHuman service (auto-rig). Pass any identity path, or "" to use a temporary one. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Identity")
	static FString LogInToService(const FString& IdentityPath);

	/** Export the identity's DNA (and brows json) to files on disk. */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Identity")
	static FString ExportDna(const FString& IdentityPath, const FString& DnaOutPath, const FString& BrowsOutPath);
};
