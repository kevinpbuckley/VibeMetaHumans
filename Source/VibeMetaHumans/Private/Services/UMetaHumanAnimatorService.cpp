// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanAnimatorService.h"

#include "Animation/AnimBlueprint.h"
#include "Animation/AnimCurveTypes.h"
#include "Animation/AnimData/CurveIdentifier.h"
#include "Animation/AnimData/IAnimationDataController.h"
#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "AnimGraphNode_CopyPoseFromMesh.h"
#include "AnimGraphNode_LayeredBoneBlend.h"
#include "AnimGraphNode_ModifyCurve.h"
#include "AnimGraphNode_Root.h"
#include "AnimGraphNode_SequencePlayer.h"
#include "AnimGraphNode_Slot.h"
#include "AnimationGraphSchema.h"
#include "AssetToolsModule.h"
#include "CaptureData.h"
#include "EdGraph/EdGraph.h"
#include "Factories/AnimBlueprintFactory.h"
#include "IAssetTools.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/Crc.h"
#include "LevelSequence.h"
#include "MetaHumanIdentity.h"
#include "MetaHumanPerformance.h"
#include "MetaHumanPerformanceExportUtils.h"
#include "Misc/Paths.h"
#include "Sound/SoundWave.h"
#include "Utils/VmhAssets.h"
#include "Utils/VmhJson.h"
#include "VibeMetaHumansModule.h"

namespace
{
	const TCHAR* PerformanceFactoryPath = TEXT("/Script/MetaHumanPerformance.MetaHumanPerformanceFactoryNew");

	UMetaHumanPerformance* LoadPerformance(const FString& Path, FString& OutError)
	{
		UObject* Object = VmhAssets::Load(Path);
		UMetaHumanPerformance* Performance = Cast<UMetaHumanPerformance>(Object);
		if (!Performance)
		{
			OutError = Object
				? FString::Printf(TEXT("%s is a %s, not a MetaHumanPerformance"), *Path, *Object->GetClass()->GetName())
				: FString::Printf(TEXT("No MetaHuman Performance at %s"), *Path);
		}
		return Performance;
	}

	FString EnumValueName(const UEnum* Enum, int64 Value)
	{
		return Enum ? Enum->GetNameStringByValue(Value) : FString::FromInt(static_cast<int32>(Value));
	}

	TSharedPtr<FJsonObject> StatusJson(UMetaHumanPerformance* Performance)
	{
		TSharedPtr<FJsonObject> Out = VmhJson::Obj();
		Out->SetStringField(TEXT("performance"), Performance->GetPathName());
		static const TArray<FString> Props = {
			TEXT("InputType"), TEXT("FootageCaptureData"), TEXT("Identity"), TEXT("Audio"), TEXT("Camera"),
			TEXT("HeadMovementMode"), TEXT("SolveType"), TEXT("StartFrameToProcess"), TEXT("EndFrameToProcess"),
			TEXT("bSkipDiagnostics"), TEXT("bHeadStabilization"), TEXT("AudioDrivenAnimationOutputControls"),
			TEXT("AudioDrivenAnimationSolveOverrides"), TEXT("bBodyTracking"),
		};
		Out->SetObjectField(TEXT("settings"), VmhJson::ObjectToJson(Performance, Props));
		Out->SetBoolField(TEXT("can_process"), Performance->CanProcess());
		Out->SetBoolField(TEXT("is_processing"), Performance->IsProcessing());
		Out->SetNumberField(TEXT("processed_frames"), Performance->GetNumberOfProcessedFrames());
		Out->SetBoolField(TEXT("can_export_animation"), Performance->CanExportAnimation());
		// Reflection call: the diagnostics UFUNCTION is not part of the module's exported API.
		TSharedPtr<FJsonObject> Diag;
		FString DiagError;
		if (VmhJson::CallObjectFunction(Performance, TEXT("DiagnosticsIndicatesProcessingIssue"), VmhJson::Obj(), Diag, DiagError))
		{
			bool bIssue = false;
			FString Message;
			if (Diag->TryGetBoolField(TEXT("ReturnValue"), bIssue) && bIssue && Diag->TryGetStringField(TEXT("OutDiagnosticsWarningMessage"), Message))
			{
				Out->SetStringField(TEXT("diagnostics_warning"), Message);
			}
		}
		return Out;
	}
}

// =====================================================================
// Performance
// =====================================================================

FString UMetaHumanAnimatorService::CreatePerformance(const FString& PackagePath, const FString& AssetName, const FString& InputType, const FString& CaptureDataPath, const FString& IdentityPath, const FString& AudioPath)
{
	const UEnum* InputEnum = StaticEnum<EDataInputType>();
	const int64 InputValue = VmhJson::ParseEnum(InputEnum, InputType);
	if (InputValue == INDEX_NONE)
	{
		return VmhJson::Error(TEXT("BAD_INPUT_TYPE"), FString::Printf(TEXT("InputType must be one of: %s"), *VmhJson::EnumNames(InputEnum)));
	}
	const EDataInputType Type = static_cast<EDataInputType>(InputValue);

	UFootageCaptureData* CaptureData = nullptr;
	UMetaHumanIdentity* Identity = nullptr;
	USoundWave* Audio = nullptr;
	if (Type == EDataInputType::Audio)
	{
		Audio = VmhAssets::Load<USoundWave>(AudioPath);
		if (!Audio)
		{
			return VmhJson::Error(TEXT("AUDIO_NOT_FOUND"), FString::Printf(TEXT("No SoundWave at %s"), *AudioPath));
		}
	}
	else
	{
		CaptureData = VmhAssets::Load<UFootageCaptureData>(CaptureDataPath);
		if (!CaptureData)
		{
			return VmhJson::Error(TEXT("CAPTURE_DATA_NOT_FOUND"), FString::Printf(TEXT("No Footage Capture Data at %s"), *CaptureDataPath));
		}
		if (Type == EDataInputType::DepthFootage)
		{
			Identity = VmhAssets::Load<UMetaHumanIdentity>(IdentityPath);
			if (!Identity)
			{
				return VmhJson::Error(TEXT("IDENTITY_NOT_FOUND"), TEXT("DepthFootage needs a prepared MetaHuman Identity (IdentityPath)"));
			}
			if (CaptureData->DepthSequences.Num() == 0)
			{
				return VmhJson::Error(TEXT("NO_DEPTH"), TEXT("The capture data has no depth sequence; use InputType MonoFootage for plain video"));
			}
		}
	}

	FString Error;
	UObject* Created = VmhAssets::Create(PackagePath, AssetName, UMetaHumanPerformance::StaticClass(), PerformanceFactoryPath, Error);
	UMetaHumanPerformance* Performance = Cast<UMetaHumanPerformance>(Created);
	if (!Performance)
	{
		return VmhJson::Error(TEXT("CREATE_FAILED"), Error.IsEmpty() ? TEXT("Factory returned no MetaHumanPerformance") : Error);
	}

	// Setters trigger the PostEditChange handling the asset relies on.
	Performance->SetInputType(Type);
	if (CaptureData)
	{
		Performance->SetFootageCaptureData(CaptureData);
	}
	if (Identity)
	{
		Performance->SetIdentity(Identity);
	}
	if (Audio)
	{
		Performance->SetAudio(Audio);
	}

	FString SaveError;
	if (!VmhAssets::Save(Performance, SaveError))
	{
		UE_LOG(LogVibeMetaHumans, Warning, TEXT("%s"), *SaveError);
	}
	TSharedPtr<FJsonObject> Out = StatusJson(Performance);
	Out->SetStringField(TEXT("input_type"), EnumValueName(InputEnum, InputValue));
	return VmhJson::Ok(Out);
}

FString UMetaHumanAnimatorService::ConfigurePerformance(const FString& PerformancePath, const FString& JsonPatch)
{
	FString Error;
	UMetaHumanPerformance* Performance = LoadPerformance(PerformancePath, Error);
	if (!Performance)
	{
		return VmhJson::Error(TEXT("PERFORMANCE_NOT_FOUND"), Error);
	}
	FString ParseError;
	TSharedPtr<FJsonObject> Patch = VmhJson::Parse(JsonPatch, ParseError);
	if (!Patch.IsValid())
	{
		return VmhJson::Error(TEXT("INVALID_JSON"), ParseError);
	}
	TArray<FString> Changed;
	if (!VmhJson::PatchObject(Performance, Patch, Changed, Error))
	{
		return VmhJson::Error(TEXT("PATCH_FAILED"), Error);
	}
	TSharedPtr<FJsonObject> Out = StatusJson(Performance);
	Out->SetArrayField(TEXT("changed"), VmhJson::StringArray(Changed));
	return VmhJson::Ok(Out);
}

FString UMetaHumanAnimatorService::CancelProcessing(const FString& PerformancePath)
{
	FString Error;
	UMetaHumanPerformance* Performance = LoadPerformance(PerformancePath, Error);
	if (!Performance)
	{
		return VmhJson::Error(TEXT("PERFORMANCE_NOT_FOUND"), Error);
	}
	const bool bWas = Performance->IsProcessing();
	if (bWas)
	{
		TSharedPtr<FJsonObject> Result;
		FString CallError;
		if (!VmhJson::CallObjectFunction(Performance, TEXT("CancelPipeline"), VmhJson::Obj(), Result, CallError))
		{
			return VmhJson::Error(TEXT("CANCEL_FAILED"), CallError);
		}
	}
	TSharedPtr<FJsonObject> Out = StatusJson(Performance);
	Out->SetBoolField(TEXT("was_processing"), bWas);
	return VmhJson::Ok(Out);
}

FString UMetaHumanAnimatorService::GetStatus(const FString& PerformancePath)
{
	FString Error;
	UMetaHumanPerformance* Performance = LoadPerformance(PerformancePath, Error);
	if (!Performance)
	{
		return VmhJson::Error(TEXT("PERFORMANCE_NOT_FOUND"), Error);
	}
	return VmhJson::Ok(StatusJson(Performance));
}

// =====================================================================
// Export
// =====================================================================

FString UMetaHumanAnimatorService::ExportLevelSequence(const FString& PerformancePath, const FString& PackagePath, const FString& AssetName, const FString& TargetMetaHumanBlueprintPath, bool bExportIdentity, bool bExportCamera, bool bExportAudio, bool bExportVideo)
{
	FString Error;
	UMetaHumanPerformance* Performance = LoadPerformance(PerformancePath, Error);
	if (!Performance)
	{
		return VmhJson::Error(TEXT("PERFORMANCE_NOT_FOUND"), Error);
	}
	if (!Performance->CanExportAnimation())
	{
		return VmhJson::Error(TEXT("NOTHING_TO_EXPORT"), TEXT("The performance has no processed animation; call process first"));
	}
	UMetaHumanPerformanceExportLevelSequenceSettings* Settings = UMetaHumanPerformanceExportUtils::GetExportLevelSequenceSettings(Performance);
	if (!Settings)
	{
		return VmhJson::Error(TEXT("NO_SETTINGS"), TEXT("Could not create export settings"));
	}
	TSharedPtr<FJsonObject> Patch = VmhJson::Obj();
	Patch->SetBoolField(TEXT("bShowExportDialog"), false);
	if (!PackagePath.IsEmpty())
	{
		Patch->SetStringField(TEXT("PackagePath"), PackagePath);
	}
	if (!AssetName.IsEmpty())
	{
		Patch->SetStringField(TEXT("AssetName"), AssetName);
	}
	if (!TargetMetaHumanBlueprintPath.IsEmpty())
	{
		UObject* Target = VmhAssets::Load(TargetMetaHumanBlueprintPath);
		if (!Target)
		{
			return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint at %s"), *TargetMetaHumanBlueprintPath));
		}
		Patch->SetStringField(TEXT("TargetMetaHumanClass"), Target->GetPathName());
		Patch->SetBoolField(TEXT("bEnableMetaHumanHeadMovement"), true);
	}
	Patch->SetBoolField(TEXT("bExportIdentity"), bExportIdentity);
	Patch->SetBoolField(TEXT("bExportCamera"), bExportCamera);
	Patch->SetBoolField(TEXT("bExportAudioTrack"), bExportAudio);
	Patch->SetBoolField(TEXT("bExportVideoTrack"), bExportVideo);
	Patch->SetBoolField(TEXT("bExportControlRigTrack"), true);
	Patch->SetBoolField(TEXT("bKeepFrameRange"), true);
	TArray<FString> Changed;
	if (!VmhJson::PatchObject(Settings, Patch, Changed, Error))
	{
		return VmhJson::Error(TEXT("SETTINGS_FAILED"), Error);
	}

	ULevelSequence* Sequence = UMetaHumanPerformanceExportUtils::ExportLevelSequence(Performance, Settings);
	if (!Sequence)
	{
		return VmhJson::Error(TEXT("EXPORT_FAILED"), TEXT("ExportLevelSequence returned null (see Output Log)"));
	}
	FString SaveError;
	VmhAssets::Save(Sequence, SaveError);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("performance"), Performance->GetPathName());
	Out->SetStringField(TEXT("level_sequence"), Sequence->GetPathName());
	Out->SetArrayField(TEXT("settings_changed"), VmhJson::StringArray(Changed));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// add_blinks
// =====================================================================================================================

namespace
{
	double Rand01(const FString& Seed, int32 I)
	{
		return static_cast<double>(FCrc::StrCrc32(*FString::Printf(TEXT("%s#%d"), *Seed, I)) & 0xFFFF) / 65535.0;
	}

	/** A rich curve's linear value at T (keys sorted). */
	float CurveAt(const TArray<FRichCurveKey>& Keys, double T)
	{
		if (Keys.Num() == 0) { return 0.f; }
		if (T <= Keys[0].Time) { return Keys[0].Value; }
		for (int32 I = 1; I < Keys.Num(); ++I)
		{
			if (T <= Keys[I].Time)
			{
				const double A = (T - Keys[I - 1].Time) / FMath::Max(1e-6, static_cast<double>(Keys[I].Time - Keys[I - 1].Time));
				return static_cast<float>(Keys[I - 1].Value + A * (Keys[I].Value - Keys[I - 1].Value));
			}
		}
		return Keys.Last().Value;
	}

	const FFloatCurve* FindFloatCurve(const UAnimSequence* Seq, const FName& Name)
	{
		for (const FFloatCurve& C : Seq->GetDataModel()->GetCurveData().FloatCurves)
		{
			if (C.GetName().IsEqual(Name, ENameCase::IgnoreCase)) { return &C; }
		}
		return nullptr;
	}
}

FString UMetaHumanAnimatorService::AddBlinks(const FString& AnimOrFolderPath, float MinIntervalSeconds, float MaxIntervalSeconds, bool bDryRun, const FString& OptionsJson)
{
	FString Error;
	const TSharedPtr<FJsonObject> Options = VmhJson::Parse(OptionsJson, Error);
	if (!Options.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
	}
	TArray<FString> Paths;
	if (Cast<UAnimSequence>(VmhAssets::Load(AnimOrFolderPath)))
	{
		Paths.Add(AnimOrFolderPath);
	}
	else
	{
		Paths = VmhAssets::FindAssetsOfClass(TEXT("/Script/Engine.AnimSequence"), AnimOrFolderPath);
	}
	FString Prefix;
	Options->TryGetStringField(TEXT("name_prefix"), Prefix);
	if (Paths.Num() == 0)
	{
		return VmhJson::Error(TEXT("ANIM_NOT_FOUND"), FString::Printf(TEXT("%s is neither an AnimSequence nor a folder holding any"), *AnimOrFolderPath));
	}
	TArray<FName> Curves;
	const TArray<TSharedPtr<FJsonValue>>* CurveList = nullptr;
	if (Options->TryGetArrayField(TEXT("curves"), CurveList))
	{
		for (const TSharedPtr<FJsonValue>& C : *CurveList) { Curves.Add(FName(*C->AsString())); }
	}
	else
	{
		Curves = { TEXT("CTRL_expressions_eyeBlinkL"), TEXT("CTRL_expressions_eyeBlinkR") };
	}
	double Peak = 0.6, Clear = 1.2, FirstMin = 0.6, FirstMax = 1.4;
	Options->TryGetNumberField(TEXT("existing_peak"), Peak);   // a baked value at or over this is a blink already
	Options->TryGetNumberField(TEXT("clear"), Clear);          // no new blink this close (s) to another
	Options->TryGetNumberField(TEXT("first_min"), FirstMin);
	Options->TryGetNumberField(TEXT("first_max"), FirstMax);
	const double GapMin = FMath::Max(0.5, static_cast<double>(MinIntervalSeconds));
	const double GapMax = FMath::Max(GapMin, static_cast<double>(MaxIntervalSeconds));
	bool bSave = false;
	Options->TryGetBoolField(TEXT("save"), bSave);
	// blink shape around its peak: (offset s, value; < 0 = the bake's own value there), linear keys
	const TArray<TPair<double, double>> Shape = { { -0.07, -1.0 }, { 0.0, 1.0 }, { 0.05, 1.0 }, { 0.17, -1.0 } };

	TArray<TSharedPtr<FJsonValue>> Report;
	int32 Added = 0, Modified = 0, Kept = 0;
	for (const FString& Path : Paths)
	{
		UAnimSequence* Seq = VmhAssets::Load<UAnimSequence>(Path);
		if (!Seq || (!Prefix.IsEmpty() && !Seq->GetName().StartsWith(Prefix)))
		{
			continue;
		}
		const double Length = Seq->GetPlayLength();
		const FFloatCurve* Lead = FindFloatCurve(Seq, Curves[0]);
		TArray<FRichCurveKey> LeadKeys = Lead ? Lead->FloatCurve.GetConstRefOfKeys() : TArray<FRichCurveKey>();
		TArray<double> Have;
		for (int32 I = 0; I < LeadKeys.Num(); ++I)
		{
			if (LeadKeys[I].Value >= Peak && (I == 0 || LeadKeys[I - 1].Value < Peak)) { Have.Add(LeadKeys[I].Time); }
		}
		Kept += Have.Num();
		// deterministic per asset name: a re-run plans the same blinks and finds them already there
		TArray<double> Blinks;
		int32 I = 0;
		for (double T = FirstMin + Rand01(Seq->GetName(), 0) * (FirstMax - FirstMin); T + Shape.Last().Key < Length - 0.05; T += GapMin + Rand01(Seq->GetName(), ++I) * (GapMax - GapMin))
		{
			bool bFree = true;
			for (const double P : Have) { bFree &= FMath::Abs(T - P) >= Clear; }
			for (const double P : Blinks) { bFree &= FMath::Abs(T - P) >= Clear; }
			if (bFree) { Blinks.Add(FMath::RoundToDouble(T * 1000.0) / 1000.0); }
		}
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetStringField(TEXT("anim"), Seq->GetPathName());
		R->SetNumberField(TEXT("length_s"), Length);
		R->SetNumberField(TEXT("baked_blinks"), Have.Num());
		TArray<TSharedPtr<FJsonValue>> BlinkTimes;
		for (const double B : Blinks) { BlinkTimes.Add(MakeShared<FJsonValueNumber>(B)); }
		R->SetArrayField(TEXT("added_at"), BlinkTimes);
		Report.Add(MakeShared<FJsonValueObject>(R));
		Added += Blinks.Num();
		if (bDryRun || Blinks.Num() == 0)
		{
			continue;
		}
		IAnimationDataController& Controller = Seq->GetController();
		Controller.OpenBracket(FText::FromString(TEXT("VibeMetaHumans add_blinks")));
		for (const FName& CurveName : Curves)
		{
			const FFloatCurve* Existing = FindFloatCurve(Seq, CurveName);
			const FName Name = Existing ? Existing->GetName() : CurveName;
			const FAnimationCurveIdentifier Id(Name, ERawCurveTrackTypes::RCT_Float);
			const TArray<FRichCurveKey> Old = Existing ? Existing->FloatCurve.GetConstRefOfKeys() : TArray<FRichCurveKey>();
			if (!Existing)
			{
				Controller.AddCurve(Id);
			}
			TArray<TPair<double, float>> Points;
			for (const FRichCurveKey& K : Old)
			{
				const bool bInside = Blinks.ContainsByPredicate([&K, &Shape](double B) { return K.Time >= B + Shape[0].Key && K.Time <= B + Shape.Last().Key; });
				if (!bInside) { Points.Emplace(K.Time, K.Value); }
			}
			for (const double B : Blinks)
			{
				for (const TPair<double, double>& S : Shape)
				{
					const double T = FMath::Max(0.0, B + S.Key);
					Points.Emplace(T, S.Value < 0.0 ? CurveAt(Old, T) : static_cast<float>(S.Value));
				}
			}
			Points.Sort([](const TPair<double, float>& A, const TPair<double, float>& B) { return A.Key < B.Key; });
			TArray<FRichCurveKey> Keys;
			for (const TPair<double, float>& P : Points)
			{
				FRichCurveKey K(static_cast<float>(P.Key), P.Value);
				K.InterpMode = RCIM_Linear;
				Keys.Add(K);
			}
			Controller.SetCurveKeys(Id, Keys);
		}
		Controller.CloseBracket();
		Seq->MarkPackageDirty();
		++Modified;
		if (bSave)
		{
			FString SaveError;
			VmhAssets::Save(Seq, SaveError);
		}
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetNumberField(TEXT("anims"), Report.Num());
	Out->SetNumberField(TEXT("blinks_added"), Added);
	Out->SetNumberField(TEXT("baked_blinks_kept"), Kept);
	Out->SetNumberField(TEXT("anims_modified"), Modified);
	Out->SetArrayField(TEXT("report"), Report);
	Out->SetStringField(TEXT("hint"), bDryRun ? TEXT("run again with bDryRun false to write the keys")
		: (bSave ? TEXT("saved") : TEXT("save the modified AnimSequences (options.save true saves each one)")));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// create_face_anim_blueprint
// =====================================================================================================================

namespace
{
	template <typename TNode>
	TNode* AddAnimNode(UEdGraph* Graph, int32 X, int32 Y, TFunctionRef<void(TNode*)> Setup)
	{
		FGraphNodeCreator<TNode> Creator(*Graph);
		TNode* Node = Creator.CreateNode();
		Node->NodePosX = X;
		Node->NodePosY = Y;
		Setup(Node);
		Creator.Finalize();
		return Node;
	}

	UEdGraphPin* PosePin(UEdGraphNode* Node, EEdGraphPinDirection Direction, const TCHAR* Name = nullptr)
	{
		for (UEdGraphPin* Pin : Node->Pins)
		{
			if (Pin->Direction == Direction && Pin->PinType.PinCategory == UAnimationGraphSchema::PC_Struct && (!Name || Pin->PinName == FName(Name)))
			{
				return Pin;
			}
		}
		return nullptr;
	}

	bool Link(UEdGraph* Graph, UEdGraphNode* From, UEdGraphNode* To, const TCHAR* ToPin, FString& OutError)
	{
		UEdGraphPin* Out = PosePin(From, EGPD_Output);
		UEdGraphPin* In = PosePin(To, EGPD_Input, ToPin);
		if (!Out || !In || !Graph->GetSchema()->TryCreateConnection(Out, In))
		{
			OutError = FString::Printf(TEXT("could not link %s -> %s.%s"), *From->GetName(), *To->GetName(), ToPin ? ToPin : TEXT("?"));
			return false;
		}
		return true;
	}
}

FString UMetaHumanAnimatorService::CreateFaceAnimBlueprint(const FString& PackagePath, const FString& AssetName, const FString& SkeletonPath, const FString& BlinkLoopAnimPath, const FString& SlotName)
{
	const FString SkelPath = SkeletonPath.IsEmpty() ? TEXT("/MetaHumanCharacter/Face/Face_Archetype_Skeleton") : SkeletonPath;
	USkeleton* Skeleton = VmhAssets::Load<USkeleton>(SkelPath);
	if (!Skeleton)
	{
		return VmhJson::Error(TEXT("SKELETON_NOT_FOUND"), FString::Printf(TEXT("No skeleton at %s (the face archetype skeleton ships with the MetaHumanCharacter plugin: /MetaHumanCharacter/Face/Face_Archetype_Skeleton)"), *SkelPath));
	}
	UAnimSequence* BlinkLoop = nullptr;
	if (!BlinkLoopAnimPath.IsEmpty())
	{
		BlinkLoop = VmhAssets::Load<UAnimSequence>(BlinkLoopAnimPath);
		if (!BlinkLoop)
		{
			return VmhJson::Error(TEXT("ANIM_NOT_FOUND"), FString::Printf(TEXT("No AnimSequence at %s"), *BlinkLoopAnimPath));
		}
	}
	const FString Name = AssetName.IsEmpty() ? TEXT("ABP_Face_Slot") : AssetName;
	if (VmhAssets::Load(PackagePath / Name))
	{
		return VmhJson::Error(TEXT("ASSET_EXISTS"), FString::Printf(TEXT("%s already exists; pick another name or delete it (never delete a copy of a plugin AnimBP: that crashed the editor)"), *(PackagePath / Name)));
	}
	UAnimBlueprintFactory* Factory = NewObject<UAnimBlueprintFactory>();
	Factory->TargetSkeleton = Skeleton;
	Factory->ParentClass = UAnimInstance::StaticClass();
	Factory->BlueprintType = BPTYPE_Normal;
	IAssetTools& AssetTools = FModuleManager::LoadModuleChecked<FAssetToolsModule>("AssetTools").Get();
	UAnimBlueprint* AnimBP = Cast<UAnimBlueprint>(AssetTools.CreateAsset(Name, PackagePath, UAnimBlueprint::StaticClass(), Factory));
	if (!AnimBP)
	{
		return VmhJson::Error(TEXT("CREATE_FAILED"), FString::Printf(TEXT("could not create an Animation Blueprint at %s"), *(PackagePath / Name)));
	}
	UEdGraph* Graph = nullptr;
	for (UEdGraph* G : AnimBP->FunctionGraphs)
	{
		if (G && G->Schema && G->Schema->IsChildOf(UAnimationGraphSchema::StaticClass())) { Graph = G; break; }
	}
	UAnimGraphNode_Root* Root = nullptr;
	if (Graph)
	{
		TArray<UAnimGraphNode_Root*> Roots;
		Graph->GetNodesOfClass(Roots);
		Root = Roots.Num() > 0 ? Roots[0] : nullptr;
	}
	if (!Graph || !Root)
	{
		return VmhJson::Error(TEXT("NO_ANIM_GRAPH"), TEXT("the new Animation Blueprint has no AnimGraph output node"));
	}
	const FName Slot = SlotName.IsEmpty() ? FName(TEXT("DefaultSlot")) : FName(*SlotName);

	// the face follows the body's head and neck: copy pose from the attached parent (Body), curves too
	auto CopyPose = [](UAnimGraphNode_CopyPoseFromMesh* N) { N->Node.bUseAttachedParent = true; N->Node.bCopyCurves = true; };
	UAnimGraphNode_CopyPoseFromMesh* Base = AddAnimNode<UAnimGraphNode_CopyPoseFromMesh>(Graph, -900, 0, CopyPose);
	UAnimGraphNode_CopyPoseFromMesh* SlotSource = AddAnimNode<UAnimGraphNode_CopyPoseFromMesh>(Graph, -1200, 250, CopyPose);
	UAnimGraphNode_Slot* SlotNode = AddAnimNode<UAnimGraphNode_Slot>(Graph, -900, 250, [Slot](UAnimGraphNode_Slot* N) { N->Node.SlotName = Slot; });
	// the clip only below the facial root, curves overriding: a plain slot after the copy snapped head and neck to the
	// reference pose (an exported face clip is a root plus ~260 curves)
	UAnimGraphNode_LayeredBoneBlend* Layered = AddAnimNode<UAnimGraphNode_LayeredBoneBlend>(Graph, -600, 0, [](UAnimGraphNode_LayeredBoneBlend* N)
	{
		if (N->Node.LayerSetup.Num() == 0) { N->Node.LayerSetup.AddDefaulted(); }
		FBranchFilter Filter;
		Filter.BoneName = TEXT("FACIAL_C_FacialRoot");
		Filter.BlendDepth = 0;
		N->Node.LayerSetup[0].BranchFilters = { Filter };
		N->Node.CurveBlendOption = ECurveBlendOption::Override;
	});
	// HeadControlSwitch x 0: with head movement on it gates the post-process head IK and snaps the head (~4.4 cm)
	UAnimGraphNode_ModifyCurve* Modify = AddAnimNode<UAnimGraphNode_ModifyCurve>(Graph, -300, 0, [](UAnimGraphNode_ModifyCurve* N)
	{
		// UAnimGraphNode_ModifyCurve::Node is private: reach it by reflection
		const FStructProperty* NodeProp = FindFProperty<FStructProperty>(UAnimGraphNode_ModifyCurve::StaticClass(), TEXT("Node"));
		if (FAnimNode_ModifyCurve* M = NodeProp ? NodeProp->ContainerPtrToValuePtr<FAnimNode_ModifyCurve>(N) : nullptr)
		{
			M->ApplyMode = EModifyCurveApplyMode::Scale;
			M->CurveMap.Add(TEXT("HeadControlSwitch"), 0.f);
			M->Alpha = 1.f;
		}
	});
	FString LinkError;
	bool bLinked = Link(Graph, Base, Layered, TEXT("BasePose"), LinkError)
		&& Link(Graph, SlotSource, SlotNode, TEXT("Source"), LinkError)
		&& Link(Graph, SlotNode, Layered, TEXT("BlendPoses_0"), LinkError)
		&& Link(Graph, Layered, Modify, TEXT("SourcePose"), LinkError);
	UEdGraphNode* Last = Modify;
	if (bLinked && BlinkLoop)
	{
		// blinks in the silence between lines: a looping blink-only clip, curves blended by max, no bones
		UAnimGraphNode_SequencePlayer* Player = AddAnimNode<UAnimGraphNode_SequencePlayer>(Graph, -300, 250, [BlinkLoop](UAnimGraphNode_SequencePlayer* N)
		{
			N->SetAnimationAsset(BlinkLoop);   // loops by default
		});
		UAnimGraphNode_LayeredBoneBlend* Blinks = AddAnimNode<UAnimGraphNode_LayeredBoneBlend>(Graph, 0, 0, [](UAnimGraphNode_LayeredBoneBlend* N)
		{
			if (N->Node.LayerSetup.Num() == 0) { N->Node.LayerSetup.AddDefaulted(); }
			N->Node.LayerSetup[0].BranchFilters.Reset();
			N->Node.CurveBlendOption = ECurveBlendOption::UseMaxValue;
		});
		bLinked = Link(Graph, Modify, Blinks, TEXT("BasePose"), LinkError) && Link(Graph, Player, Blinks, TEXT("BlendPoses_0"), LinkError);
		Last = Blinks;
	}
	bLinked = bLinked && Link(Graph, Last, Root, TEXT("Result"), LinkError);
	FBlueprintEditorUtils::MarkBlueprintAsStructurallyModified(AnimBP);
	FKismetEditorUtilities::CompileBlueprint(AnimBP, EBlueprintCompileOptions::SkipGarbageCollection);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("anim_blueprint"), AnimBP->GetPathName());
	Out->SetStringField(TEXT("skeleton"), Skeleton->GetPathName());
	Out->SetStringField(TEXT("slot"), Slot.ToString());
	Out->SetBoolField(TEXT("blink_loop"), BlinkLoop != nullptr);
	Out->SetBoolField(TEXT("linked"), bLinked);
	Out->SetBoolField(TEXT("compiled"), AnimBP->Status != BS_Error);
	if (!bLinked)
	{
		return VmhJson::Error(TEXT("GRAPH_FAILED"), LinkError, Out);
	}
	Out->SetStringField(TEXT("hint"), TEXT("save it, then set it as the Face component's anim class on every built Blueprint (post_build_fixup face_anim_class does it, and again after every rebuild); play lines with PlaySlotAnimationAsDynamicMontage on the Face's anim instance"));
	return VmhJson::Ok(Out);
}
