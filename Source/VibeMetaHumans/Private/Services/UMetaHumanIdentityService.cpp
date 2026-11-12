// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanIdentityService.h"

#include "CaptureData.h"
#include "DNACommon.h"
#include "MetaHumanIdentity.h"
#include "MetaHumanIdentityParts.h"
#include "MetaHumanIdentityPose.h"
#include "MetaHumanIdentityPromotedFrames.h"
#include "Misc/Paths.h"
#include "PromotedFrameUtils.h"
#include "UObject/Package.h"
#include "Utils/VmhAssets.h"
#include "Utils/VmhJson.h"
#include "VibeMetaHumansModule.h"

namespace
{
	const TCHAR* IdentityFactoryPath = TEXT("/Script/MetaHumanIdentityEditor.MetaHumanIdentityFactoryNew");

	UMetaHumanIdentity* LoadIdentity(const FString& Path, FString& OutError)
	{
		UObject* Object = VmhAssets::Load(Path);
		UMetaHumanIdentity* Identity = Cast<UMetaHumanIdentity>(Object);
		if (!Identity)
		{
			OutError = Object
				? FString::Printf(TEXT("%s is a %s, not a MetaHumanIdentity"), *Path, *Object->GetClass()->GetName())
				: FString::Printf(TEXT("No MetaHuman Identity at %s"), *Path);
		}
		return Identity;
	}

	UMetaHumanIdentityFace* GetFace(UMetaHumanIdentity* Identity, bool bCreate)
	{
		UMetaHumanIdentityPart* Part = bCreate
			? Identity->GetOrCreatePartOfClass(UMetaHumanIdentityFace::StaticClass())
			: Identity->FindPartOfClass(UMetaHumanIdentityFace::StaticClass());
		return Cast<UMetaHumanIdentityFace>(Part);
	}

	FString IdentityErrorName(EIdentityErrorCode Code)
	{
		if (const UEnum* Enum = StaticEnum<EIdentityErrorCode>())
		{
			return Enum->GetNameStringByValue(static_cast<int64>(Code));
		}
		return FString::FromInt(static_cast<int32>(Code));
	}

	TSharedPtr<FJsonObject> StatusJson(UMetaHumanIdentity* Identity)
	{
		TSharedPtr<FJsonObject> Out = VmhJson::Obj();
		Out->SetStringField(TEXT("identity"), Identity->GetPathName());
		Out->SetBoolField(TEXT("logged_in"), Identity->IsLoggedInToService());
		Out->SetBoolField(TEXT("auto_rig_in_progress"), Identity->IsAutoRiggingInProgress());
		Out->SetBoolField(TEXT("tracking_in_progress"), Identity->IsFrameTrackingPipelineProcessing());
		FText Diagnostics;
		if (Identity->DiagnosticsIndicatesProcessingIssue(Diagnostics))
		{
			Out->SetStringField(TEXT("diagnostics_warning"), Diagnostics.ToString());
		}
		if (UMetaHumanIdentityFace* Face = GetFace(Identity, false))
		{
			TSharedPtr<FJsonObject> FaceJson = VmhJson::Obj();
			FaceJson->SetBoolField(TEXT("has_dna"), Face->HasDNABuffer());
			FaceJson->SetBoolField(TEXT("has_predictive_solvers"), Face->HasPredictiveSolvers());
			FaceJson->SetBoolField(TEXT("conformal_rig_valid"), Face->IsConformalRigValid());
			TArray<TSharedPtr<FJsonValue>> Poses;
			for (const UMetaHumanIdentityPose* Pose : Face->GetPoses())
			{
				if (!Pose)
				{
					continue;
				}
				TSharedPtr<FJsonObject> PoseJson = VmhJson::Obj();
				if (const UEnum* PoseEnum = StaticEnum<EIdentityPoseType>())
				{
					PoseJson->SetStringField(TEXT("type"), PoseEnum->GetNameStringByValue(static_cast<int64>(Pose->PoseType)));
				}
				PoseJson->SetBoolField(TEXT("fit_eyes"), Pose->bFitEyes);
				PoseJson->SetStringField(TEXT("camera"), Pose->Camera);
				PoseJson->SetStringField(TEXT("capture_data"), VmhAssets::PathOf(Pose->GetCaptureData()));
				PoseJson->SetNumberField(TEXT("promoted_frames"), Pose->PromotedFrames.Num());
				Poses.Add(MakeShared<FJsonValueObject>(PoseJson));
			}
			FaceJson->SetArrayField(TEXT("poses"), Poses);
			Out->SetObjectField(TEXT("face"), FaceJson);
		}
		else
		{
			Out->SetStringField(TEXT("face"), TEXT("none"));
		}
		if (UMetaHumanIdentityBody* Body = Cast<UMetaHumanIdentityBody>(Identity->FindPartOfClass(UMetaHumanIdentityBody::StaticClass())))
		{
			TSharedPtr<FJsonObject> BodyJson = VmhJson::Obj();
			BodyJson->SetNumberField(TEXT("body_type_index"), Body->BodyTypeIndex);
			BodyJson->SetNumberField(TEXT("height"), Body->Height);
			Out->SetObjectField(TEXT("body"), BodyJson);
		}
		return Out;
	}
}

FString UMetaHumanIdentityService::CreateIdentityFromFootage(const FString& PackagePath, const FString& AssetName, const FString& CaptureDataPath, int32 NeutralFrame, int32 BodyTypeIndex, bool bRequestAutoRig, bool bPrepareForPerformance)
{
	UFootageCaptureData* CaptureData = VmhAssets::Load<UFootageCaptureData>(CaptureDataPath);
	if (!CaptureData)
	{
		return VmhJson::Error(TEXT("CAPTURE_DATA_NOT_FOUND"), FString::Printf(TEXT("No Footage Capture Data at %s (ingest a take first with MetaHumanAnimatorService)"), *CaptureDataPath));
	}
	if (CaptureData->DepthSequences.Num() == 0)
	{
		return VmhJson::Error(TEXT("NO_DEPTH"), TEXT("This capture data has no depth sequence. Identities need depth footage (Live Link Face in MetaHuman Animator mode, or stereo HMC); monocular video cannot create an identity."));
	}

	FString Error;
	UObject* Created = VmhAssets::Create(PackagePath, AssetName, UMetaHumanIdentity::StaticClass(), IdentityFactoryPath, Error);
	UMetaHumanIdentity* Identity = Cast<UMetaHumanIdentity>(Created);
	if (!Identity)
	{
		return VmhJson::Error(TEXT("CREATE_FAILED"), Error.IsEmpty() ? TEXT("Factory returned no MetaHumanIdentity") : Error);
	}

	UMetaHumanIdentityFace* Face = GetFace(Identity, true);
	if (!Face)
	{
		return VmhJson::Error(TEXT("NO_FACE_PART"), TEXT("Could not create the Face part on the identity"));
	}

	// Neutral pose on the face, pointing at the footage.
	UMetaHumanIdentityPose* Pose = NewObject<UMetaHumanIdentityPose>(Face);
	Face->AddPoseOfType(EIdentityPoseType::Neutral, Pose);
	Pose->SetCaptureData(CaptureData);
	Pose->bFitEyes = true;
	Pose->LoadDefaultTracker();

	// Promote the neutral frame.
	int32 PromotedIndex = INDEX_NONE;
	UMetaHumanIdentityPromotedFrame* Frame = Pose->AddNewPromotedFrame(PromotedIndex);
	UMetaHumanIdentityFootageFrame* FootageFrame = Cast<UMetaHumanIdentityFootageFrame>(Frame);
	if (!FootageFrame)
	{
		return VmhJson::Error(TEXT("PROMOTE_FAILED"), TEXT("Could not add a promoted footage frame to the neutral pose"));
	}
	FootageFrame->bIsFrontView = true;
	FootageFrame->SetNavigationLocked(true);
	FootageFrame->FrameNumber = NeutralFrame;

	if (!UPromotedFrameUtils::InitializeContourDataForFootageFrame(Pose, FootageFrame))
	{
		return VmhJson::Error(TEXT("CONTOUR_INIT_FAILED"), FString::Printf(TEXT("Could not initialise contour data for frame %d (is the frame inside the take?)"), NeutralFrame));
	}

	const FString ImagePath = UPromotedFrameUtils::GetImagePathForFrame(CaptureData, Pose->Camera, NeutralFrame, true, Pose->TimecodeAlignment);
	const FString DepthPath = UPromotedFrameUtils::GetImagePathForFrame(CaptureData, Pose->Camera, NeutralFrame, false, Pose->TimecodeAlignment);
	FIntPoint ImageSize(0, 0);
	TArray<FColor> Samples;
	if (!UPromotedFrameUtils::GetPromotedFrameAsPixelArrayFromDisk(ImagePath, ImageSize, Samples) || ImageSize.X <= 0 || ImageSize.Y <= 0)
	{
		return VmhJson::Error(TEXT("FRAME_READ_FAILED"), FString::Printf(TEXT("Could not read the promoted frame image at %s"), *ImagePath));
	}

	// Track the frame (blocking) and conform the template mesh.
	Identity->SetBlockingProcessing(true);
	Identity->StartFrameTrackingPipeline(Samples, ImageSize.X, ImageSize.Y, DepthPath, Pose, Frame, /*bShowProgress*/ false);

	const EIdentityErrorCode ConformResult = Face->Conform();
	if (!UMetaHumanIdentity::HandleError(ConformResult, /*bLogOnly*/ true))
	{
		TSharedPtr<FJsonObject> Extra = VmhJson::Obj();
		Extra->SetStringField(TEXT("identity"), Identity->GetPathName());
		return VmhJson::Error(TEXT("CONFORM_FAILED"), FString::Printf(TEXT("Face conform returned %s"), *IdentityErrorName(ConformResult)), Extra);
	}

	if (UMetaHumanIdentityBody* Body = Cast<UMetaHumanIdentityBody>(Identity->GetOrCreatePartOfClass(UMetaHumanIdentityBody::StaticClass())))
	{
		Body->BodyTypeIndex = FMath::Clamp(BodyTypeIndex, 1, 6);
	}

	TSharedPtr<FJsonObject> Out = StatusJson(Identity);
	Out->SetStringField(TEXT("capture_data"), CaptureData->GetPathName());
	Out->SetNumberField(TEXT("neutral_frame"), NeutralFrame);
	Out->SetStringField(TEXT("conform_result"), IdentityErrorName(ConformResult));

	if (bRequestAutoRig)
	{
		if (!Identity->IsLoggedInToService())
		{
			Identity->LogInToAutoRigService();
		}
		if (Identity->IsLoggedInToService())
		{
			Identity->CreateDNAForIdentity(/*bLogOnly*/ true);
			Out->SetStringField(TEXT("auto_rig"), TEXT("requested (asynchronous) - poll get_status until auto_rig_in_progress is false and face.has_dna is true, then call MetaHumanAnimatorIdentityTools.PrepareIdentityForPerformance"));
		}
		else
		{
			Out->SetStringField(TEXT("auto_rig"), TEXT("skipped: not logged in to the MetaHuman service. Call log_in_to_service, then MetaHumanAnimatorIdentityTools.GenerateIdentityRig."));
		}
	}
	else if (bPrepareForPerformance)
	{
		if (Face->HasDNABuffer())
		{
			Out->SetBoolField(TEXT("prepared_for_performance"), Face->RunPredictiveSolverTraining());
		}
		else
		{
			Out->SetStringField(TEXT("prepare_for_performance"), TEXT("skipped: no DNA yet (auto-rig first)"));
		}
	}

	FString SaveError;
	if (!VmhAssets::Save(Identity, SaveError))
	{
		UE_LOG(LogVibeMetaHumans, Warning, TEXT("%s"), *SaveError);
	}
	return VmhJson::Ok(Out);
}

FString UMetaHumanIdentityService::CreateIdentityFromDna(const FString& PackagePath, const FString& AssetName, const FString& DnaFilePath, const FString& BrowsJsonPath, bool bPrepareForPerformance)
{
	if (!FPaths::FileExists(DnaFilePath))
	{
		return VmhJson::Error(TEXT("FILE_NOT_FOUND"), FString::Printf(TEXT("No DNA file at %s"), *DnaFilePath));
	}
	FString Error;
	UObject* Created = VmhAssets::Create(PackagePath, AssetName, UMetaHumanIdentity::StaticClass(), IdentityFactoryPath, Error);
	UMetaHumanIdentity* Identity = Cast<UMetaHumanIdentity>(Created);
	if (!Identity)
	{
		return VmhJson::Error(TEXT("CREATE_FAILED"), Error.IsEmpty() ? TEXT("Factory returned no MetaHumanIdentity") : Error);
	}
	UMetaHumanIdentityFace* Face = GetFace(Identity, true);
	if (!Face)
	{
		return VmhJson::Error(TEXT("NO_FACE_PART"), TEXT("Could not create the Face part on the identity"));
	}
	const EIdentityErrorCode ImportResult = Identity->ImportDNAFile(DnaFilePath, EDNADataLayer::All, BrowsJsonPath);
	TSharedPtr<FJsonObject> Out = StatusJson(Identity);
	Out->SetStringField(TEXT("import_result"), IdentityErrorName(ImportResult));
	if (ImportResult != EIdentityErrorCode::None)
	{
		return VmhJson::Error(TEXT("IMPORT_FAILED"), FString::Printf(TEXT("ImportDNAFile returned %s (DNA/brows json not compatible with this plugin version?)"), *IdentityErrorName(ImportResult)), Out);
	}
	if (bPrepareForPerformance)
	{
		Out->SetBoolField(TEXT("prepared_for_performance"), Face->RunPredictiveSolverTraining());
	}
	FString SaveError;
	if (!VmhAssets::Save(Identity, SaveError))
	{
		UE_LOG(LogVibeMetaHumans, Warning, TEXT("%s"), *SaveError);
	}
	return VmhJson::Ok(Out);
}

FString UMetaHumanIdentityService::GetStatus(const FString& IdentityPath)
{
	FString Error;
	UMetaHumanIdentity* Identity = LoadIdentity(IdentityPath, Error);
	if (!Identity)
	{
		return VmhJson::Error(TEXT("IDENTITY_NOT_FOUND"), Error);
	}
	return VmhJson::Ok(StatusJson(Identity));
}

FString UMetaHumanIdentityService::LogInToService(const FString& IdentityPath)
{
	UMetaHumanIdentity* Identity = nullptr;
	FString Error;
	if (!IdentityPath.IsEmpty())
	{
		Identity = LoadIdentity(IdentityPath, Error);
		if (!Identity)
		{
			return VmhJson::Error(TEXT("IDENTITY_NOT_FOUND"), Error);
		}
	}
	else
	{
		Identity = NewObject<UMetaHumanIdentity>(GetTransientPackage(), NAME_None, RF_Transient);
	}
	if (!Identity->IsLoggedInToService())
	{
		Identity->LogInToAutoRigService();
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("logged_in"), Identity->IsLoggedInToService());
	if (!Identity->IsLoggedInToService())
	{
		Out->SetStringField(TEXT("hint"), TEXT("A browser/Epic login window may have opened; finish it and call again."));
	}
	return VmhJson::Ok(Out);
}

FString UMetaHumanIdentityService::ExportDna(const FString& IdentityPath, const FString& DnaOutPath, const FString& BrowsOutPath)
{
	FString Error;
	UMetaHumanIdentity* Identity = LoadIdentity(IdentityPath, Error);
	if (!Identity)
	{
		return VmhJson::Error(TEXT("IDENTITY_NOT_FOUND"), Error);
	}
	if (!Identity->ExportDNADataToFiles(DnaOutPath, BrowsOutPath))
	{
		return VmhJson::Error(TEXT("EXPORT_FAILED"), TEXT("ExportDNADataToFiles returned false (no DNA on the identity, or the paths are not writable)"));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("identity"), Identity->GetPathName());
	Out->SetStringField(TEXT("dna"), DnaOutPath);
	Out->SetStringField(TEXT("brows"), BrowsOutPath);
	return VmhJson::Ok(Out);
}
