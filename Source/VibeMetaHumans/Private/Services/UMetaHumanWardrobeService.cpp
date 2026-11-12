// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanWardrobeService.h"

#include "Animation/AnimSequence.h"
#include "BoneWeights.h"
#include "Components/SkeletalMeshComponent.h"
#include "DNA.h"
#include "Dom/JsonValue.h"
#include "EditorAssetLibrary.h"
#include "Engine/Blueprint.h"
#include "Engine/SCS_Node.h"
#include "Engine/SimpleConstructionScript.h"
#include "Engine/SkeletalMesh.h"
#include "GroomAsset.h"
#include "GroomAssetCards.h"
#include "GroomAssetInterpolation.h"
#include "GroomAssetMeshes.h"
#include "HAL/FileManager.h"
#include "Kismet2/BlueprintEditorUtils.h"
#include "Kismet2/KismetEditorUtilities.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Rendering/SkeletalMeshLODRenderData.h"
#include "Rendering/SkeletalMeshRenderData.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Services/UMetaHumanCharacterService.h"
#include "SkeletalMeshAttributes.h"
#include "Utils/VmhAssets.h"
#include "Utils/VmhBlueprintSnapshot.h"
#include "Utils/VmhJson.h"
#include "Utils/VmhSkin.h"

using namespace VmhSkin;

namespace
{
	// ------------------------------------------------------------------ options

	double OptNum(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, double Default)
	{
		double V = Default;
		return (O.IsValid() && O->TryGetNumberField(Key, V)) ? V : Default;
	}

	bool OptBool(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, bool Default)
	{
		bool V = Default;
		return (O.IsValid() && O->TryGetBoolField(Key, V)) ? V : Default;
	}

	FString OptStr(const TSharedPtr<FJsonObject>& O, const TCHAR* Key, const FString& Default)
	{
		FString V;
		return (O.IsValid() && O->TryGetStringField(Key, V) && !V.IsEmpty()) ? V : Default;
	}

	TArray<TSharedPtr<FJsonValue>> OptArr(const TSharedPtr<FJsonObject>& O, const TCHAR* Key)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		return (O.IsValid() && O->TryGetArrayField(Key, Arr)) ? *Arr : TArray<TSharedPtr<FJsonValue>>();
	}

	TArray<int32> OptLods(const TSharedPtr<FJsonObject>& O, int32 NumLods)
	{
		TArray<int32> Lods;
		for (const TSharedPtr<FJsonValue>& V : OptArr(O, TEXT("lods")))
		{
			const int32 L = static_cast<int32>(V->AsNumber());
			if (L >= 0 && L < NumLods) { Lods.AddUnique(L); }
		}
		if (Lods.Num() == 0)
		{
			for (int32 L = 0; L < NumLods; ++L) { Lods.Add(L); }
		}
		return Lods;
	}

	/** Content under /Game that is not a marketplace pack: the only meshes the tools edit in place. */
	bool IsEditablePath(const FString& Path)
	{
		return Path.StartsWith(TEXT("/Game/")) && !Path.StartsWith(TEXT("/Game/Fab/"));
	}

	// ------------------------------------------------------------------ Blueprint components

	struct FComp
	{
		USCS_Node* Node = nullptr;
		USkeletalMeshComponent* Template = nullptr;
		USkeletalMesh* Mesh = nullptr;
	};

	FTransform ToRoot(USimpleConstructionScript* SCS, USCS_Node* Node)
	{
		FTransform T = FTransform::Identity;
		for (USCS_Node* N = Node; N; N = SCS->FindParentNode(N))
		{
			if (const USceneComponent* Scene = Cast<USceneComponent>(N->ComponentTemplate))
			{
				T = T * Scene->GetRelativeTransform();
			}
		}
		return T;
	}

	bool FindComp(UBlueprint* Blueprint, const FString& Name, FComp& Out, FString& OutError)
	{
		USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr;
		USCS_Node* Node = SCS ? SCS->FindSCSNode(FName(*Name)) : nullptr;
		if (!Node)
		{
			TArray<FString> Names;
			if (SCS) { for (USCS_Node* N : SCS->GetAllNodes()) { Names.Add(N->GetVariableName().ToString()); } }
			OutError = FString::Printf(TEXT("the Blueprint has no component %s; it has: %s"), *Name, *FString::Join(Names, TEXT(", ")));
			return false;
		}
		Out.Node = Node;
		Out.Template = Cast<USkeletalMeshComponent>(Node->ComponentTemplate);
		Out.Mesh = Out.Template ? Out.Template->GetSkeletalMeshAsset() : nullptr;
		if (!Out.Mesh)
		{
			OutError = FString::Printf(TEXT("component %s is not a skeletal mesh component with a mesh"), *Name);
			return false;
		}
		return true;
	}

	FTransform CompToLeader(UBlueprint* Blueprint, const FComp& Comp, const FComp& Leader)
	{
		USimpleConstructionScript* SCS = Blueprint->SimpleConstructionScript;
		return ToRoot(SCS, Comp.Node) * ToRoot(SCS, Leader.Node).Inverse();
	}

	/** Rest pose plus the listed animation poses, on the leader mesh. */
	bool ReadPoses(const TSharedPtr<FJsonObject>& Options, const USkeletalMesh* Leader, TArray<FPose>& OutPoses, FString& OutError)
	{
		OutPoses.Add(RefPose(Leader->GetRefSkeleton()));
		for (const TSharedPtr<FJsonValue>& V : OptArr(Options, TEXT("poses")))
		{
			const TSharedPtr<FJsonObject> P = V->AsObject();
			const FString AnimPath = P.IsValid() ? P->GetStringField(TEXT("anim")) : V->AsString();
			const double Time = P.IsValid() ? OptNum(P, TEXT("time"), 0.0) : 0.0;
			const UAnimSequence* Anim = VmhAssets::Load<UAnimSequence>(AnimPath);
			if (!Anim)
			{
				OutError = FString::Printf(TEXT("no AnimSequence at %s (poses take {\"anim\": path, \"time\": seconds})"), *AnimPath);
				return false;
			}
			FPose Pose;
			if (!AnimPose(Leader->GetRefSkeleton(), Anim, Time, Pose, OutError))
			{
				return false;
			}
			OutPoses.Add(MoveTemp(Pose));
		}
		return true;
	}

	/** Each pose's label and how far it moves the bones from the first (rest) pose: a pose that moves nothing is a mistake. */
	TArray<TSharedPtr<FJsonValue>> PoseLabels(const TArray<FPose>& Poses)
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (const FPose& P : Poses)
		{
			double MaxMove = 0.0;
			for (const TPair<FName, FTransform>& Bone : P.BoneCS)
			{
				if (const FTransform* Rest = Poses[0].BoneCS.Find(Bone.Key))
				{
					MaxMove = FMath::Max(MaxMove, FVector::Dist(Rest->GetLocation(), Bone.Value.GetLocation()));
				}
			}
			TSharedPtr<FJsonObject> O = VmhJson::Obj();
			O->SetStringField(TEXT("pose"), P.Label);
			O->SetNumberField(TEXT("max_bone_move_cm"), MaxMove);
			Out.Add(MakeShared<FJsonValueObject>(O));
		}
		return Out;
	}

	/** Skin a follower LOD into the leader's space for Pose. */
	void SkinFollower(const FLodGeom& Geom, const USkeletalMesh* Mesh, const FPose& Pose, const FTransform& ToLeader, TArray<FVector3d>& OutPos, TArray<FMatrix44d>* OutBlend = nullptr, const TArray<FVector3d>* BindOverride = nullptr, int32* OutMissing = nullptr)
	{
		TArray<FMatrix44d> Bones;
		int32 Missing = 0;
		FollowerSkinMatrices(Mesh->GetRefSkeleton(), Pose, Bones, Missing);
		Skin(Geom, Bones, ToLeader, OutPos, OutBlend, BindOverride);
		if (OutMissing) { *OutMissing = Missing; }
	}

	// ------------------------------------------------------------------ leader bone evaluation

	bool LeaderRequired(const USkeletalMesh* Leader, int32 Lod, TSet<int32>& OutRequired, int32& OutLeaderLod, FString& OutError)
	{
		const FSkeletalMeshRenderData* RenderData = Leader ? Leader->GetResourceForRendering() : nullptr;
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			OutError = TEXT("the leader mesh has no render data");
			return false;
		}
		OutLeaderLod = FMath::Clamp(Lod, 0, RenderData->LODRenderData.Num() - 1);
		for (const FBoneIndexType Bone : RenderData->LODRenderData[OutLeaderLod].RequiredBones)
		{
			OutRequired.Add(Bone);
		}
		if (OutRequired.Num() == 0)
		{
			OutError = TEXT("the leader LOD lists no required bones");
			return false;
		}
		return true;
	}

	/**
	 * Per follower bone: the follower bone its weights should move to (itself when the leader evaluates it or lacks it).
	 * OutDead marks the remapped bones; OutMissing the follower bones the leader lacks.
	 */
	void BuildRemap(const FReferenceSkeleton& FollowerRef, const FReferenceSkeleton& LeaderRef, const TSet<int32>& Required, TArray<int32>& OutMap, TArray<bool>& OutDead, TArray<bool>& OutMissing)
	{
		const int32 Num = FollowerRef.GetNum();
		OutMap.SetNum(Num);
		OutDead.Init(false, Num);
		OutMissing.Init(false, Num);
		for (int32 Bone = 0; Bone < Num; ++Bone)
		{
			OutMap[Bone] = Bone;
			const int32 LeaderBone = LeaderRef.FindBoneIndex(FollowerRef.GetBoneName(Bone));
			if (LeaderBone == INDEX_NONE)
			{
				OutMissing[Bone] = true;
				continue;
			}
			if (Required.Contains(LeaderBone))
			{
				continue;
			}
			OutDead[Bone] = true;
			for (int32 A = LeaderRef.GetParentIndex(LeaderBone); A != INDEX_NONE; A = LeaderRef.GetParentIndex(A))
			{
				if (Required.Contains(A))
				{
					const int32 Target = FollowerRef.FindBoneIndex(LeaderRef.GetBoneName(A));
					if (Target != INDEX_NONE)
					{
						OutMap[Bone] = Target;
						break;
					}
				}
			}
		}
	}

	TArray<TSharedPtr<FJsonValue>> TopCounts(const TMap<FString, int32>& Counts, int32 Max)
	{
		TArray<TPair<FString, int32>> Sorted;
		for (const TPair<FString, int32>& It : Counts) { Sorted.Add(It); }
		Sorted.Sort([](const TPair<FString, int32>& A, const TPair<FString, int32>& B) { return A.Value > B.Value; });
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 I = 0; I < Sorted.Num() && I < Max; ++I)
		{
			Out.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s (%d)"), *Sorted[I].Key, Sorted[I].Value)));
		}
		return Out;
	}

	// ------------------------------------------------------------------ geometry helpers

	FVector3d ClosestOnSegment(const FVector3d& P, const FVector3d& A, const FVector3d& B)
	{
		const FVector3d AB = B - A;
		const double Len2 = AB.SizeSquared();
		const double T = Len2 > 0.0 ? FMath::Clamp(((P - A) | AB) / Len2, 0.0, 1.0) : 0.0;
		return A + AB * T;
	}

	/** Bone segments (parent -> child, child not a leaf, >= 1 cm) of the leader in a pose: the "inside" reference. */
	void BoneSegments(const FReferenceSkeleton& LeaderRef, const FPose& Pose, TArray<TPair<FVector3d, FVector3d>>& Out)
	{
		TArray<bool> HasChild;
		HasChild.Init(false, LeaderRef.GetNum());
		for (int32 B = 0; B < LeaderRef.GetNum(); ++B)
		{
			const int32 P = LeaderRef.GetParentIndex(B);
			if (P != INDEX_NONE) { HasChild[P] = true; }
		}
		for (int32 B = 0; B < LeaderRef.GetNum(); ++B)
		{
			const int32 P = LeaderRef.GetParentIndex(B);
			if (P == INDEX_NONE || !HasChild[B])
			{
				continue;
			}
			const FTransform* TB = Pose.BoneCS.Find(LeaderRef.GetBoneName(B));
			const FTransform* TP = Pose.BoneCS.Find(LeaderRef.GetBoneName(P));
			if (TB && TP && FVector3d::Dist(TB->GetLocation(), TP->GetLocation()) >= 1.0)
			{
				Out.Emplace(TP->GetLocation(), TB->GetLocation());
			}
		}
	}

	FVector3d NearestBonePoint(const TArray<TPair<FVector3d, FVector3d>>& Segments, const FVector3d& P)
	{
		double Best = TNumericLimits<double>::Max();
		FVector3d BestPoint = P;
		for (const TPair<FVector3d, FVector3d>& S : Segments)
		{
			const FVector3d C = ClosestOnSegment(P, S.Key, S.Value);
			const double D = FVector3d::DistSquared(C, P);
			if (D < Best) { Best = D; BestPoint = C; }
		}
		return BestPoint;
	}

	/** Spatial hash for matching vertices between two meshes. */
	struct FGrid
	{
		double Cell = 1.0;
		TMultiMap<FIntVector, int32> Map;
		const TArray<FVector3d>* Points = nullptr;

		FIntVector Key(const FVector3d& P) const
		{
			return FIntVector(FMath::FloorToInt(P.X / Cell), FMath::FloorToInt(P.Y / Cell), FMath::FloorToInt(P.Z / Cell));
		}
		void Build(const TArray<FVector3d>& InPoints, double InCell)
		{
			Points = &InPoints;
			Cell = InCell;
			for (int32 I = 0; I < InPoints.Num(); ++I) { Map.Add(Key(InPoints[I]), I); }
		}
		double Nearest(const FVector3d& P) const
		{
			const FIntVector K = Key(P);
			double Best = TNumericLimits<double>::Max();
			TArray<int32> Found;
			for (int32 X = -1; X <= 1; ++X) for (int32 Y = -1; Y <= 1; ++Y) for (int32 Z = -1; Z <= 1; ++Z)
			{
				Found.Reset();
				Map.MultiFind(K + FIntVector(X, Y, Z), Found);
				for (const int32 I : Found) { Best = FMath::Min(Best, FVector3d::DistSquared((*Points)[I], P)); }
			}
			return Best == TNumericLimits<double>::Max() ? Best : FMath::Sqrt(Best);
		}
	};

	USkeletalMesh* DuplicateMesh(const USkeletalMesh* Source, const FString& Dest, FString& OutError)
	{
		FString Package, Name;
		VmhAssets::SplitAssetPath(Dest, Package, Name);
		const FString Object = Package / Name;
		if (UEditorAssetLibrary::DoesAssetExist(Object))
		{
			return VmhAssets::Load<USkeletalMesh>(Object);
		}
		UObject* Copy = UEditorAssetLibrary::DuplicateAsset(Source->GetPathName(), Object);
		USkeletalMesh* Mesh = Cast<USkeletalMesh>(Copy);
		if (!Mesh)
		{
			OutError = FString::Printf(TEXT("could not duplicate %s to %s"), *Source->GetPathName(), *Object);
		}
		return Mesh;
	}

	void AssignMesh(UBlueprint* Blueprint, USkeletalMeshComponent* Template, USkeletalMesh* Mesh)
	{
		Template->Modify();
		Template->SetSkeletalMeshAsset(Mesh);
		FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
		FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
		Blueprint->MarkPackageDirty();
	}
}

// =====================================================================================================================
// Follower check (used by attach_skeletal_mesh_to_blueprint)
// =====================================================================================================================

TSharedPtr<FJsonObject> UMetaHumanWardrobeService::CheckFollower(const USkeletalMesh* Garment, const USkeletalMesh* Leader, int32 GarmentLod)
{
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	FMeshDescription Desc;
	FLodGeom Geom;
	FString Error;
	TSet<int32> Required;
	int32 LeaderLod = 0;
	if (!Garment || !Leader || !ReadLod(Garment, GarmentLod, Desc, Geom, Error) || !LeaderRequired(Leader, GarmentLod, Required, LeaderLod, Error))
	{
		Out->SetStringField(TEXT("error"), Error.IsEmpty() ? TEXT("no garment or leader mesh") : Error);
		return Out;
	}
	TArray<int32> Map;
	TArray<bool> Dead, Missing;
	BuildRemap(Garment->GetRefSkeleton(), Leader->GetRefSkeleton(), Required, Map, Dead, Missing);
	TMap<FString, int32> DeadUse, MissingUse;
	int32 DeadVerts = 0, MissingVerts = 0;
	for (const TArray<TPair<int32, float>>& W : Geom.Weights)
	{
		bool bDead = false, bMissing = false;
		for (const TPair<int32, float>& BW : W)
		{
			if (BW.Value <= 0.f || !Dead.IsValidIndex(BW.Key)) { continue; }
			const FString Name = Garment->GetRefSkeleton().GetBoneName(BW.Key).ToString();
			if (Dead[BW.Key]) { bDead = true; DeadUse.FindOrAdd(Name)++; }
			if (Missing[BW.Key]) { bMissing = true; MissingUse.FindOrAdd(Name)++; }
		}
		DeadVerts += bDead ? 1 : 0;
		MissingVerts += bMissing ? 1 : 0;
	}
	Out->SetNumberField(TEXT("garment_lod"), GarmentLod);
	Out->SetNumberField(TEXT("leader_lod"), LeaderLod);
	Out->SetNumberField(TEXT("unevaluated_bone_vertices"), DeadVerts);
	Out->SetArrayField(TEXT("unevaluated_bones"), TopCounts(DeadUse, 12));
	Out->SetNumberField(TEXT("missing_bone_vertices"), MissingVerts);
	Out->SetArrayField(TEXT("missing_bones"), TopCounts(MissingUse, 12));
	return Out;
}

// =====================================================================================================================
// fix_unevaluated_bone_weights
// =====================================================================================================================

FString UMetaHumanWardrobeService::FixUnevaluatedBoneWeights(const FString& GarmentMeshPath, const FString& LeaderMeshPath, bool bDryRun)
{
	USkeletalMesh* Garment = VmhAssets::Load<USkeletalMesh>(GarmentMeshPath);
	const USkeletalMesh* Leader = VmhAssets::Load<USkeletalMesh>(LeaderMeshPath);
	if (!Garment || !Leader)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("need skeletal meshes at %s and %s"), *GarmentMeshPath, *LeaderMeshPath));
	}
	if (!bDryRun && !IsEditablePath(Garment->GetPathName()))
	{
		return VmhJson::Error(TEXT("PROTECTED_ASSET"), FString::Printf(TEXT("%s is outside /Game or in /Game/Fab: duplicate it into your project and fix the copy"), *Garment->GetPathName()));
	}

	TMap<int32, FMeshDescription> Edited;
	TArray<TSharedPtr<FJsonValue>> LodReports;
	int32 Total = 0;
	for (int32 Lod = 0; Lod < Garment->GetLODNum(); ++Lod)
	{
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetNumberField(TEXT("lod"), Lod);
		FMeshDescription Desc;
		FLodGeom Geom;
		FString Error;
		TSet<int32> Required;
		int32 LeaderLod = 0;
		if (!ReadLod(Garment, Lod, Desc, Geom, Error) || !LeaderRequired(Leader, Lod, Required, LeaderLod, Error))
		{
			R->SetStringField(TEXT("skipped"), Error);
			LodReports.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}
		TArray<int32> Map;
		TArray<bool> Dead, Missing;
		BuildRemap(Garment->GetRefSkeleton(), Leader->GetRefSkeleton(), Required, Map, Dead, Missing);

		FSkeletalMeshAttributes Attr(Desc);
		FSkinWeightsVertexAttributesRef SkinWeights = Attr.GetVertexSkinWeights();
		TMap<FString, int32> Used;
		int32 Fixed = 0;
		for (int32 V = 0; V < Geom.Weights.Num(); ++V)
		{
			bool bTouches = false;
			for (const TPair<int32, float>& W : Geom.Weights[V])
			{
				if (Map.IsValidIndex(W.Key) && Map[W.Key] != W.Key && W.Value > 0.f)
				{
					bTouches = true;
					Used.FindOrAdd(Garment->GetRefSkeleton().GetBoneName(W.Key).ToString())++;
				}
			}
			if (!bTouches)
			{
				continue;
			}
			++Fixed;
			TMap<int32, float> Merged;
			for (const TPair<int32, float>& W : Geom.Weights[V])
			{
				const int32 Target = Map.IsValidIndex(W.Key) ? Map[W.Key] : W.Key;
				Merged.FindOrAdd(Target) += W.Value;
			}
			TArray<UE::AnimationCore::FBoneWeight> NewWeights;
			for (const TPair<int32, float>& M : Merged)
			{
				NewWeights.Add(UE::AnimationCore::FBoneWeight(static_cast<FBoneIndexType>(M.Key), M.Value));
			}
			SkinWeights.Set(Geom.VertexIds[V], NewWeights);
		}
		int32 DeadBones = 0;
		for (const bool B : Dead) { DeadBones += B ? 1 : 0; }
		R->SetNumberField(TEXT("leader_lod"), LeaderLod);
		R->SetNumberField(TEXT("unevaluated_bones_in_skeleton"), DeadBones);
		R->SetNumberField(TEXT("vertices"), Geom.Weights.Num());
		R->SetNumberField(TEXT("vertices_fixed"), Fixed);
		R->SetArrayField(TEXT("bones_used"), TopCounts(Used, 12));
		LodReports.Add(MakeShared<FJsonValueObject>(R));
		Total += Fixed;
		if (Fixed > 0 && !bDryRun)
		{
			Edited.Add(Lod, MoveTemp(Desc));
		}
	}
	FString CommitError;
	if (Edited.Num() > 0 && !CommitLods(Garment, Edited, CommitError))
	{
		return VmhJson::Error(TEXT("COMMIT_FAILED"), CommitError);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("garment"), Garment->GetPathName());
	Out->SetStringField(TEXT("leader"), Leader->GetPathName());
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetNumberField(TEXT("vertices_fixed"), Total);
	Out->SetArrayField(TEXT("lods"), LodReports);
	Out->SetStringField(TEXT("hint"), bDryRun ? TEXT("run again with bDryRun false to write the weights") : TEXT("save the garment package; every Blueprint using this mesh picks the fix up"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// make_full_body_from_dna
// =====================================================================================================================

FString UMetaHumanWardrobeService::MakeFullBodyFromDna(const FString& BodyMeshPath, const FString& OutMeshPath, const FString& DnaMeshPath, const FString& CharacterPath, bool bDryRun)
{
	USkeletalMesh* Body = VmhAssets::Load<USkeletalMesh>(BodyMeshPath);
	if (!Body)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No skeletal mesh at %s"), *BodyMeshPath));
	}
	FString BodyPackage, BodyName;
	VmhAssets::SplitAssetPath(Body->GetPathName(), BodyPackage, BodyName);
	const FString OutPath = OutMeshPath.IsEmpty() ? BodyPackage / (BodyName + TEXT("_Full")) : OutMeshPath;
	FString OutPackage, OutName;
	VmhAssets::SplitAssetPath(OutPath, OutPackage, OutName);
	if ((OutPackage / OutName) == (BodyPackage / BodyName))
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("OutMeshPath must differ from BodyMeshPath (the built body is the source)"));
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	const FString SourceName = OutName + TEXT("_DnaSource");
	// a skeletal mesh from a DNA ASSET: the asset's own DNAConfig places it in the built body's space. (Reading an
	// exported .dna FILE uses a fixed legacy config and came out in another space: 0.4% of the vertices matched.)
	auto GenerateFrom = [&](const FString& DnaAssetPath, FString& OutError) -> USkeletalMesh*
	{
		if (USkeletalMesh* Existing = VmhAssets::Load<USkeletalMesh>(OutPackage / SourceName))
		{
			return Existing; // delete <OutName>_DnaSource to regenerate it
		}
		FString ParseError;
		bool bOk = false;
		const TSharedPtr<FJsonObject> Gen = VmhJson::Parse(UMetaHumanCharacterService::GenerateSkeletalMeshFromDna(DnaAssetPath, OutPackage, SourceName, TEXT("body")), ParseError);
		if (!Gen.IsValid() || !Gen->TryGetBoolField(TEXT("success"), bOk) || !bOk)
		{
			OutError = Gen.IsValid() ? Gen->GetStringField(TEXT("error")) : ParseError;
			return nullptr;
		}
		return VmhAssets::Load<USkeletalMesh>(Gen->GetStringField(TEXT("skeletal_mesh")));
	};
	USkeletalMesh* Dna = nullptr;
	FString GenError;
	if (!DnaMeshPath.IsEmpty())
	{
		UObject* Source = VmhAssets::Load(DnaMeshPath);
		if (USkeletalMesh* Mesh = Cast<USkeletalMesh>(Source))
		{
			Dna = Mesh;
		}
		else if (Source && Source->IsA<UDNA>())
		{
			Dna = GenerateFrom(Source->GetPathName(), GenError);
			Out->SetStringField(TEXT("dna_asset"), Source->GetPathName());
		}
		else
		{
			return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("%s is neither a skeletal mesh nor a DNA asset"), *DnaMeshPath));
		}
	}
	else if (!CharacterPath.IsEmpty())
	{
		const FString WorkFolder = OutPackage / (OutName + TEXT("_Dna"));
		FString DnaAsset;
		for (const FString& P : VmhAssets::FindAssetsOfClass(TEXT("/Script/RigLogicModule.DNA"), WorkFolder))
		{
			if (P.Contains(TEXT("body"), ESearchCase::IgnoreCase)) { DnaAsset = P; break; }
		}
		if (DnaAsset.IsEmpty())
		{
			FString ParseError;
			bool bOk = false;
			const TSharedPtr<FJsonObject> Export = VmhJson::Parse(UMetaHumanCharacterService::ExportDna(CharacterPath, WorkFolder, TEXT(""), false, true), ParseError);
			if (!Export.IsValid() || !Export->TryGetBoolField(TEXT("success"), bOk) || !bOk)
			{
				return VmhJson::Error(TEXT("DNA_EXPORT_FAILED"), Export.IsValid() ? Export->GetStringField(TEXT("error")) : ParseError);
			}
			const TArray<FString> Found = VmhAssets::FindAssetsOfClass(TEXT("/Script/RigLogicModule.DNA"), WorkFolder);
			for (const FString& P : Found)
			{
				if (P.Contains(TEXT("body"), ESearchCase::IgnoreCase)) { DnaAsset = P; break; }
			}
			if (DnaAsset.IsEmpty() && Found.Num() == 1) { DnaAsset = Found[0]; }
			if (DnaAsset.IsEmpty())
			{
				return VmhJson::Error(TEXT("DNA_EXPORT_FAILED"), FString::Printf(TEXT("no body DNA asset in %s (found: %s)"), *WorkFolder, *FString::Join(Found, TEXT(", "))));
			}
		}
		Out->SetStringField(TEXT("dna_asset"), DnaAsset);
		Dna = GenerateFrom(DnaAsset, GenError);
	}
	else
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("give DnaMeshPath (the body DNA asset, or a mesh generated from it) or CharacterPath (to export the body DNA)"));
	}
	if (!Dna)
	{
		return VmhJson::Error(TEXT("DNA_MESH_FAILED"), GenError.IsEmpty() ? TEXT("the DNA mesh could not be generated or loaded") : GenError);
	}
	Out->SetStringField(TEXT("dna_mesh"), Dna->GetPathName());

	// the DNA mesh must share the body's skeleton (bone names); weights are remapped by name when the order differs
	const FReferenceSkeleton& BodyRef = Body->GetRefSkeleton();
	const FReferenceSkeleton& DnaRef = Dna->GetRefSkeleton();
	TArray<int32> BoneMap;
	BoneMap.SetNum(DnaRef.GetNum());
	bool bSameOrder = DnaRef.GetNum() == BodyRef.GetNum();
	for (int32 B = 0; B < DnaRef.GetNum(); ++B)
	{
		BoneMap[B] = BodyRef.FindBoneIndex(DnaRef.GetBoneName(B));
		if (BoneMap[B] == INDEX_NONE)
		{
			return VmhJson::Error(TEXT("SKELETON_MISMATCH"), FString::Printf(TEXT("the DNA mesh bone %s is not in the body"), *DnaRef.GetBoneName(B).ToString()));
		}
		bSameOrder &= BoneMap[B] == B;
	}

	// read every DNA LOD once
	TArray<FLodGeom> DnaGeoms;
	TArray<FMeshDescription> DnaDescs;
	DnaGeoms.SetNum(Dna->GetLODNum());
	DnaDescs.SetNum(Dna->GetLODNum());
	TArray<FGrid> Grids;
	Grids.SetNum(Dna->GetLODNum());
	TArray<bool> DnaOk;
	DnaOk.Init(false, Dna->GetLODNum());
	for (int32 M = 0; M < Dna->GetLODNum(); ++M)
	{
		FString Ignored;
		DnaOk[M] = ReadLod(Dna, M, DnaDescs[M], DnaGeoms[M], Ignored);
		if (DnaOk[M]) { Grids[M].Build(DnaGeoms[M].BindPos, 1.0); }
	}

	TMap<int32, FMeshDescription> Edited;
	TArray<TSharedPtr<FJsonValue>> LodReports;
	for (int32 N = 0; N < Body->GetLODNum(); ++N)
	{
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetNumberField(TEXT("lod"), N);
		FMeshDescription BodyDesc;
		FLodGeom BodyGeom;
		FString Error;
		if (!ReadLod(Body, N, BodyDesc, BodyGeom, Error))
		{
			R->SetStringField(TEXT("skipped"), Error);
			LodReports.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}
		int32 BestLod = INDEX_NONE;
		double BestRatio = 0.0, BestMax = 0.0;
		TArray<FString> Tried;
		for (int32 M = 0; M < DnaGeoms.Num(); ++M)
		{
			if (!DnaOk[M] || DnaGeoms[M].BindPos.Num() < BodyGeom.BindPos.Num()) { continue; }
			int32 Matched = 0;
			double MaxDist = 0.0;
			TArray<double> Dists;
			for (const FVector3d& P : BodyGeom.BindPos)
			{
				const double D = Grids[M].Nearest(P);
				Dists.Add(D);
				if (D <= 0.05) { ++Matched; MaxDist = FMath::Max(MaxDist, D); }
			}
			Dists.Sort();
			const double Median = Dists.Num() > 0 ? Dists[Dists.Num() / 2] : 0.0;
			Tried.Add(FString::Printf(TEXT("DNA LOD %d: %.1f%% within 0.05 cm, median %s"), M, Dists.Num() ? 100.0 * Matched / Dists.Num() : 0.0,
				Median > 1e6 ? TEXT("> 1 cm") : *FString::Printf(TEXT("%.3f cm"), Median)));
			const double Ratio = BodyGeom.BindPos.Num() > 0 ? double(Matched) / BodyGeom.BindPos.Num() : 0.0;
			if (Ratio > BestRatio + 1e-6 || (FMath::IsNearlyEqual(Ratio, BestRatio) && BestLod != INDEX_NONE && DnaGeoms[M].BindPos.Num() < DnaGeoms[BestLod].BindPos.Num()))
			{
				BestLod = M; BestRatio = Ratio; BestMax = MaxDist;
			}
		}
		R->SetNumberField(TEXT("built_vertices"), BodyGeom.BindPos.Num());
		R->SetNumberField(TEXT("built_triangles"), BodyGeom.Tris.Num());
		if (BestLod == INDEX_NONE || BestRatio < 0.99)
		{
			R->SetStringField(TEXT("skipped"), FString::Printf(TEXT("no DNA LOD holds this LOD's vertices (best %.1f%%); left as built. The character's body probably changed since this build: rebuild, or pass the DNA asset the build came from"), BestRatio * 100.0));
			R->SetArrayField(TEXT("tried"), VmhJson::StringArray(Tried));
			LodReports.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}
		R->SetNumberField(TEXT("dna_lod"), BestLod);
		R->SetNumberField(TEXT("matched_percent"), BestRatio * 100.0);
		R->SetNumberField(TEXT("max_offset_cm"), BestMax);
		R->SetNumberField(TEXT("full_vertices"), DnaGeoms[BestLod].BindPos.Num());
		R->SetNumberField(TEXT("full_triangles"), DnaGeoms[BestLod].Tris.Num());
		LodReports.Add(MakeShared<FJsonValueObject>(R));
		if (bDryRun)
		{
			continue;
		}

		FMeshDescription NewDesc = DnaDescs[BestLod];
		FSkeletalMeshAttributes NewAttr(NewDesc);
		if (!bSameOrder)
		{
			// the generated mesh carries its own bone list (DNA joint order): rewrite it as the body's reference skeleton
			if (NewAttr.HasBones())
			{
				TArray<FBoneID> OldBones;
				for (const FBoneID Bone : NewAttr.Bones().GetElementIDs()) { OldBones.Add(Bone); }
				for (const FBoneID Bone : OldBones) { NewAttr.DeleteBone(Bone); }
				NewAttr.ReserveNewBones(BodyRef.GetRawBoneNum());
				auto Names = NewAttr.GetBoneNames();
				auto Parents = NewAttr.GetBoneParentIndices();
				auto BonePoses = NewAttr.GetBonePoses();
				for (int32 B = 0; B < BodyRef.GetRawBoneNum(); ++B)
				{
					const FBoneID Id(B);
					NewAttr.CreateBone(Id);
					Names.Set(Id, BodyRef.GetBoneName(B));
					Parents.Set(Id, BodyRef.GetParentIndex(B));
					BonePoses.Set(Id, BodyRef.GetRefBonePose()[B]);
				}
			}
			FSkinWeightsVertexAttributesRef SkinWeights = NewAttr.GetVertexSkinWeights();
			const FLodGeom& G = DnaGeoms[BestLod];
			for (int32 V = 0; V < G.Weights.Num(); ++V)
			{
				TArray<UE::AnimationCore::FBoneWeight> W;
				for (const TPair<int32, float>& BW : G.Weights[V])
				{
					W.Add(UE::AnimationCore::FBoneWeight(static_cast<FBoneIndexType>(BoneMap[BW.Key]), BW.Value));
				}
				SkinWeights.Set(G.VertexIds[V], W);
			}
		}
		// material slot names follow the built LOD (by group order; all to the first slot when the counts differ)
		TArray<FName> BuiltSlots;
		{
			FSkeletalMeshAttributes BodyAttr(BodyDesc);
			TPolygonGroupAttributesRef<FName> Slots = BodyAttr.GetPolygonGroupMaterialSlotNames();
			for (const FPolygonGroupID G : BodyDesc.PolygonGroups().GetElementIDs()) { BuiltSlots.Add(Slots.Get(G)); }
		}
		TPolygonGroupAttributesRef<FName> NewSlots = NewAttr.GetPolygonGroupMaterialSlotNames();
		int32 GroupIndex = 0;
		const int32 NewGroups = NewDesc.PolygonGroups().Num();
		for (const FPolygonGroupID G : NewDesc.PolygonGroups().GetElementIDs())
		{
			if (BuiltSlots.Num() > 0)
			{
				NewSlots.Set(G, NewGroups == BuiltSlots.Num() ? BuiltSlots[GroupIndex] : BuiltSlots[0]);
			}
			++GroupIndex;
		}
		Edited.Add(N, MoveTemp(NewDesc));
	}

	Out->SetStringField(TEXT("body"), Body->GetPathName());
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetArrayField(TEXT("lods"), LodReports);
	if (!bDryRun)
	{
		if (Edited.Num() == 0)
		{
			return VmhJson::Error(TEXT("NO_MATCH"), TEXT("no built LOD matched a DNA LOD; is the DNA mesh from this character's body?"), Out);
		}
		FString Error;
		USkeletalMesh* Full = DuplicateMesh(Body, OutPath, Error);
		if (!Full)
		{
			return VmhJson::Error(TEXT("DUPLICATE_FAILED"), Error, Out);
		}
		if (!CommitLods(Full, Edited, Error))
		{
			return VmhJson::Error(TEXT("COMMIT_FAILED"), Error, Out);
		}
		Out->SetStringField(TEXT("full_body"), Full->GetPathName());
	}
	Out->SetStringField(TEXT("hint"), TEXT("hide_body_under_garments with options.source_body = this mesh writes the dressed body; save both packages; the _DnaSource mesh is only an input"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// hide_body_under_garments
// =====================================================================================================================

FString UMetaHumanWardrobeService::HideBodyUnderGarments(const FString& BlueprintPath, const FString& GarmentComponentsJson, const FString& OutBodyMeshPath, const FString& OptionsJson, bool bAssignToBlueprint, bool bDryRun)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	FString Error;
	const TSharedPtr<FJsonObject> Options = VmhJson::Parse(OptionsJson, Error);
	if (!Options.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
	}
	FComp BodyComp;
	if (!FindComp(Blueprint, OptStr(Options, TEXT("body_component"), TEXT("Body")), BodyComp, Error))
	{
		return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), Error);
	}
	const FString SourcePath = OptStr(Options, TEXT("source_body"), BodyComp.Mesh->GetPathName());
	USkeletalMesh* Source = VmhAssets::Load<USkeletalMesh>(SourcePath);
	if (!Source)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No source body at %s"), *SourcePath));
	}
	FString OutPackage, OutName, SrcPackage, SrcName;
	VmhAssets::SplitAssetPath(OutBodyMeshPath, OutPackage, OutName);
	VmhAssets::SplitAssetPath(Source->GetPathName(), SrcPackage, SrcName);
	if (OutBodyMeshPath.IsEmpty() || (OutPackage / OutName) == (SrcPackage / SrcName))
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("OutBodyMeshPath is required and must differ from the source body (every run starts again from the source)"));
	}
	if (!bDryRun && !IsEditablePath(OutPackage / OutName))
	{
		return VmhJson::Error(TEXT("PROTECTED_ASSET"), TEXT("OutBodyMeshPath must be under /Game (and not /Game/Fab)"));
	}

	// garments: ["G_Jacket", {"component":"G_Pants","shell":3}]
	FString ParseErr;
	TArray<TSharedPtr<FJsonValue>> GarmentList;
	{
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(GarmentComponentsJson);
		if (!FJsonSerializer::Deserialize(Reader, GarmentList) || GarmentList.Num() == 0)
		{
			return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("GarmentComponentsJson is a JSON array of component names or {\"component\":name,\"shell\":cm}"));
		}
	}
	const double DefaultShell = OptNum(Options, TEXT("shell"), 2.0);
	struct FGarment { FString Name; FComp Comp; double Shell; FTransform ToLeader; FLodGeom Geom; };
	TArray<FGarment> Garments;
	for (const TSharedPtr<FJsonValue>& V : GarmentList)
	{
		FGarment G;
		const TSharedPtr<FJsonObject> O = V->Type == EJson::Object ? V->AsObject() : nullptr;
		G.Name = O.IsValid() ? O->GetStringField(TEXT("component")) : V->AsString();
		G.Shell = O.IsValid() ? OptNum(O, TEXT("shell"), DefaultShell) : DefaultShell;
		if (!FindComp(Blueprint, G.Name, G.Comp, Error))
		{
			return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), Error);
		}
		G.ToLeader = CompToLeader(Blueprint, G.Comp, BodyComp);
		FMeshDescription Unused;
		if (!ReadLod(G.Comp.Mesh, 0, Unused, G.Geom, Error))
		{
			return VmhJson::Error(TEXT("GARMENT_UNREADABLE"), Error);
		}
		Garments.Add(MoveTemp(G));
	}

	TArray<FPose> Poses;
	if (!ReadPoses(Options, BodyComp.Mesh, Poses, Error))
	{
		return VmhJson::Error(TEXT("BAD_POSE"), Error);
	}
	TArray<FBox3d> KeepBoxes, DeleteBoxes;
	if (!ParseBoxes(OptArr(Options, TEXT("keep_boxes")), KeepBoxes, Error) || !ParseBoxes(OptArr(Options, TEXT("delete_boxes")), DeleteBoxes, Error))
	{
		return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
	}
	const int32 Contract = static_cast<int32>(OptNum(Options, TEXT("contract"), 2));
	const bool bUseRays = OptBool(Options, TEXT("use_rays"), true);
	const double RayOut = OptNum(Options, TEXT("ray_out"), 6.0);
	const double RayIn = OptNum(Options, TEXT("ray_in"), 5.0);
	const int32 RayContract = static_cast<int32>(OptNum(Options, TEXT("ray_contract"), 1));
	const double WindingMin = OptNum(Options, TEXT("winding"), 0.5);
	const bool bAll = OptStr(Options, TEXT("pose_combine"), TEXT("all")).Equals(TEXT("all"), ESearchCase::IgnoreCase);

	// garments posed: one soup per garment (winding + shell) and one combined soup (rays), per pose
	TArray<TArray<FSoup>> GarmentSoups;
	TArray<FSoup> RaySoups;
	GarmentSoups.SetNum(Poses.Num());
	RaySoups.SetNum(Poses.Num());
	for (int32 P = 0; P < Poses.Num(); ++P)
	{
		TArray<FVector3d> AllPos;
		TArray<FIntVector3> AllTris;
		for (const FGarment& G : Garments)
		{
			TArray<FVector3d> X;
			SkinFollower(G.Geom, G.Comp.Mesh, Poses[P], G.ToLeader, X);
			GarmentSoups[P].AddDefaulted_GetRef().Build(X, G.Geom.Tris, true);
			const int32 Base = AllPos.Num();
			AllPos.Append(X);
			for (const FIntVector3& T : G.Geom.Tris) { AllTris.Add(FIntVector3(T.X + Base, T.Y + Base, T.Z + Base)); }
		}
		RaySoups[P].Build(AllPos, AllTris, false);
	}

	TMap<int32, FMeshDescription> Edited;
	TArray<TSharedPtr<FJsonValue>> LodReports;
	for (const int32 Lod : OptLods(Options, Source->GetLODNum()))
	{
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetNumberField(TEXT("lod"), Lod);
		FMeshDescription Desc;
		FLodGeom Geom;
		if (!ReadLod(Source, Lod, Desc, Geom, Error))
		{
			R->SetStringField(TEXT("skipped"), Error);
			LodReports.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}
		const int32 NV = Geom.BindPos.Num();
		TArray<bool> C1, C2;
		C1.Init(bAll, NV);
		C2.Init(bAll && bUseRays, NV);
		for (int32 P = 0; P < Poses.Num(); ++P)
		{
			TArray<FVector3d> X, N;
			SkinFollower(Geom, Source, Poses[P], FTransform::Identity, X);
			VertexNormals(X, Geom.Tris, N);
			for (int32 V = 0; V < NV; ++V)
			{
				bool b1 = false;
				for (int32 G = 0; G < Garments.Num() && !b1; ++G)
				{
					FVector3d S, Nrm; int32 Tri; double D;
					b1 = (Garments[G].Shell > 0.0 && GarmentSoups[P][G].Nearest(X[V], Garments[G].Shell, S, Nrm, Tri, D))
						|| FMath::Abs(GarmentSoups[P][G].Winding(X[V])) >= WindingMin;
				}
				bool b2 = false;
				if (bUseRays && !N[V].IsNearlyZero())
				{
					b2 = RaySoups[P].Ray(X[V] + N[V] * 0.05, N[V], RayOut) >= 0.0 || RaySoups[P].Ray(X[V] - N[V] * 0.05, -N[V], RayIn) >= 0.0;
				}
				C1[V] = bAll ? (C1[V] && b1) : (C1[V] || b1);
				C2[V] = bAll ? (C2[V] && b2) : (C2[V] || b2);
			}
		}
		const int32 NT = Geom.Tris.Num();
		TArray<bool> S1, S2;
		S1.Init(false, NT);
		S2.Init(false, NT);
		for (int32 T = 0; T < NT; ++T)
		{
			const FIntVector3& Tri = Geom.Tris[T];
			S1[T] = C1[Tri.X] && C1[Tri.Y] && C1[Tri.Z];
			S2[T] = bUseRays && C2[Tri.X] && C2[Tri.Y] && C2[Tri.Z];
		}
		ContractSelection(Geom, S1, Contract);
		ContractSelection(Geom, S2, RayContract);
		TArray<FTriangleID> Delete;
		int32 N1 = 0, N2 = 0, NBox = 0, NKept = 0;
		for (int32 T = 0; T < NT; ++T)
		{
			const FIntVector3& Tri = Geom.Tris[T];
			const FVector3d A = Geom.BindPos[Tri.X], B = Geom.BindPos[Tri.Y], Cc = Geom.BindPos[Tri.Z];
			bool bDel = S1[T] || S2[T];
			N1 += S1[T] ? 1 : 0;
			N2 += S2[T] ? 1 : 0;
			for (const FBox3d& Box : DeleteBoxes)
			{
				if (!bDel && Box.IsInsideOrOn(A) && Box.IsInsideOrOn(B) && Box.IsInsideOrOn(Cc)) { bDel = true; ++NBox; }
			}
			for (const FBox3d& Box : KeepBoxes)
			{
				if (bDel && (Box.IsInsideOrOn(A) || Box.IsInsideOrOn(B) || Box.IsInsideOrOn(Cc))) { bDel = false; ++NKept; }
			}
			if (bDel) { Delete.Add(Geom.TriIds[T]); }
		}
		R->SetNumberField(TEXT("triangles"), NT);
		R->SetNumberField(TEXT("covered_inside_or_shell"), N1);
		R->SetNumberField(TEXT("covered_by_rays"), N2);
		R->SetNumberField(TEXT("deleted_by_box"), NBox);
		R->SetNumberField(TEXT("kept_by_box"), NKept);
		R->SetNumberField(TEXT("deleted"), Delete.Num());
		LodReports.Add(MakeShared<FJsonValueObject>(R));
		if (!bDryRun)
		{
			DeleteTriangles(Desc, Delete);
			Edited.Add(Lod, MoveTemp(Desc));
		}
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("source_body"), Source->GetPathName());
	Out->SetArrayField(TEXT("poses"), PoseLabels(Poses));
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetArrayField(TEXT("lods"), LodReports);
	if (!bDryRun && Edited.Num() > 0)
	{
		USkeletalMesh* Dressed = DuplicateMesh(Source, OutBodyMeshPath, Error);
		if (!Dressed)
		{
			return VmhJson::Error(TEXT("DUPLICATE_FAILED"), Error, Out);
		}
		if (!CommitLods(Dressed, Edited, Error))
		{
			return VmhJson::Error(TEXT("COMMIT_FAILED"), Error, Out);
		}
		Out->SetStringField(TEXT("dressed_body"), Dressed->GetPathName());
		if (bAssignToBlueprint)
		{
			AssignMesh(Blueprint, BodyComp.Template, Dressed);
			Out->SetBoolField(TEXT("assigned"), true);
		}
	}
	Out->SetStringField(TEXT("hint"), TEXT("look at it standing AND in the seated pose before keeping it; save the dressed body (and the Blueprint when assigned)"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// push_garment_out_of_mesh
// =====================================================================================================================

FString UMetaHumanWardrobeService::PushGarmentOutOfMesh(const FString& BlueprintPath, const FString& GarmentComponent, const FString& ObstacleComponent, const FString& OutMeshPath, const FString& OptionsJson, bool bDryRun)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	FString Error;
	const TSharedPtr<FJsonObject> Options = VmhJson::Parse(OptionsJson, Error);
	if (!Options.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
	}
	FComp BodyComp, GarmentComp, ObstacleComp;
	if (!FindComp(Blueprint, OptStr(Options, TEXT("body_component"), TEXT("Body")), BodyComp, Error)
		|| !FindComp(Blueprint, GarmentComponent, GarmentComp, Error)
		|| !FindComp(Blueprint, ObstacleComponent.IsEmpty() ? TEXT("Face") : ObstacleComponent, ObstacleComp, Error))
	{
		return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), Error);
	}

	// which mesh gets written
	USkeletalMesh* Target = GarmentComp.Mesh;
	bool bAssign = false;
	if (!OutMeshPath.IsEmpty())
	{
		FString P, N;
		VmhAssets::SplitAssetPath(OutMeshPath, P, N);
		if (!IsEditablePath(P / N))
		{
			return VmhJson::Error(TEXT("PROTECTED_ASSET"), TEXT("OutMeshPath must be under /Game (and not /Game/Fab)"));
		}
		if (USkeletalMesh* Existing = VmhAssets::Load<USkeletalMesh>(P / N))
		{
			Target = Existing;
		}
		else if (!bDryRun)
		{
			Target = DuplicateMesh(GarmentComp.Mesh, P / N, Error);
			if (!Target)
			{
				return VmhJson::Error(TEXT("DUPLICATE_FAILED"), Error);
			}
		}
		bAssign = Target != GarmentComp.Mesh;
	}
	else if (!bDryRun && !IsEditablePath(Target->GetPathName()))
	{
		return VmhJson::Error(TEXT("PROTECTED_ASSET"), FString::Printf(TEXT("%s is outside /Game or in /Game/Fab: pass OutMeshPath to work on a copy"), *Target->GetPathName()));
	}

	TArray<FPose> Poses;
	if (!ReadPoses(Options, BodyComp.Mesh, Poses, Error))
	{
		return VmhJson::Error(TEXT("BAD_POSE"), Error);
	}
	const double Margin = OptNum(Options, TEXT("margin"), 0.5);
	const double Reach = OptNum(Options, TEXT("reach"), 3.0);
	const int32 Passes = FMath::Max(1, static_cast<int32>(OptNum(Options, TEXT("passes"), 3)));
	const int32 SmoothIterations = static_cast<int32>(OptNum(Options, TEXT("smooth_iterations"), 6));
	const double Spread = OptNum(Options, TEXT("spread"), 3.0);
	TArray<FBox3d> Region;
	{
		TArray<TSharedPtr<FJsonValue>> RegionValues;
		const TSharedPtr<FJsonValue> RegionField = Options->TryGetField(TEXT("region"));
		if (RegionField.IsValid() && RegionField->Type != EJson::Null) { RegionValues.Add(RegionField); }
		if (!ParseBoxes(RegionValues, Region, Error))
		{
			return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
		}
	}
	const bool bRegion = Region.Num() > 0;
	const FBox3d SpreadBox = bRegion ? Region[0].ExpandBy(Spread) : FBox3d(ForceInit);

	// obstacle (LOD0) and bone segments, per pose
	const FTransform GarmentToLeader = CompToLeader(Blueprint, GarmentComp, BodyComp);
	const FTransform ObstacleToLeader = CompToLeader(Blueprint, ObstacleComp, BodyComp);
	FLodGeom ObstacleGeom;
	{
		FMeshDescription Unused;
		if (!ReadLod(ObstacleComp.Mesh, 0, Unused, ObstacleGeom, Error))
		{
			return VmhJson::Error(TEXT("OBSTACLE_UNREADABLE"), Error);
		}
	}
	// outer garments the push must stay inside of (a knit under a jacket): pushes are capped at their surface minus a gap
	struct FOuter { FComp Comp; FTransform ToLeader; FLodGeom Geom; };
	TArray<FOuter> Outers;
	TArray<FString> OuterNames;
	for (const TSharedPtr<FJsonValue>& V : OptArr(Options, TEXT("under")))
	{
		FOuter O;
		if (!FindComp(Blueprint, V->AsString(), O.Comp, Error))
		{
			return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), Error);
		}
		FMeshDescription Unused;
		if (!ReadLod(O.Comp.Mesh, 0, Unused, O.Geom, Error))
		{
			return VmhJson::Error(TEXT("GARMENT_UNREADABLE"), Error);
		}
		O.ToLeader = CompToLeader(Blueprint, O.Comp, BodyComp);
		OuterNames.Add(V->AsString());
		Outers.Add(MoveTemp(O));
	}
	const double UnderGap = OptNum(Options, TEXT("under_gap"), 0.3);
	constexpr double Tolerance = 0.01; // cm: float noise at the margin is not a violation

	TArray<FSoup> Obstacles, OuterSoups;
	TArray<TArray<TPair<FVector3d, FVector3d>>> Segments;
	Obstacles.SetNum(Poses.Num());
	OuterSoups.SetNum(Poses.Num());
	Segments.SetNum(Poses.Num());
	for (int32 P = 0; P < Poses.Num(); ++P)
	{
		TArray<FVector3d> X;
		SkinFollower(ObstacleGeom, ObstacleComp.Mesh, Poses[P], ObstacleToLeader, X);
		Obstacles[P].Build(X, ObstacleGeom.Tris, false);
		BoneSegments(BodyComp.Mesh->GetRefSkeleton(), Poses[P], Segments[P]);
		if (Outers.Num() > 0)
		{
			TArray<FVector3d> All;
			TArray<FIntVector3> AllTris;
			for (const FOuter& O : Outers)
			{
				TArray<FVector3d> OX;
				SkinFollower(O.Geom, O.Comp.Mesh, Poses[P], O.ToLeader, OX);
				const int32 Base = All.Num();
				All.Append(OX);
				for (const FIntVector3& T : O.Geom.Tris) { AllTris.Add(FIntVector3(T.X + Base, T.Y + Base, T.Z + Base)); }
			}
			OuterSoups[P].Build(All, AllTris, false);
		}
	}

	TMap<int32, FMeshDescription> Edited;
	TArray<TSharedPtr<FJsonValue>> LodReports;
	int64 Checks = 0, Flips = 0;
	for (const int32 Lod : OptLods(Options, Target->GetLODNum()))
	{
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetNumberField(TEXT("lod"), Lod);
		FMeshDescription Desc;
		FLodGeom Geom;
		if (!ReadLod(Target, Lod, Desc, Geom, Error))
		{
			R->SetStringField(TEXT("skipped"), Error);
			LodReports.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}
		const int32 NV = Geom.BindPos.Num();
		TArray<FVector3d> Rest;
		SkinFollower(Geom, Target, Poses[0], GarmentToLeader, Rest);
		TArray<FVector3d> Bind = Geom.BindPos;

		// a push must not carry a vertex through an outer garment, in any pose
		auto ClampToOuter = [&](TArray<FVector3d>& D)
		{
			if (Outers.Num() == 0)
			{
				return;
			}
			for (int32 P = 0; P < Poses.Num(); ++P)
			{
				TArray<FVector3d> X;
				TArray<FMatrix44d> M;
				SkinFollower(Geom, Target, Poses[P], GarmentToLeader, X, &M, &Bind);
				for (int32 V = 0; V < NV; ++V)
				{
					if (D[V].SizeSquared() < 1e-10) { continue; }
					const FVector3d Move = M[V].TransformVector(D[V]);
					const double Len = Move.Size();
					const double Hit = Len > 1e-6 ? OuterSoups[P].Ray(X[V], Move / Len, Len + UnderGap) : -1.0;
					if (Hit >= 0.0)
					{
						D[V] *= FMath::Max(0.0, Hit - UnderGap) / Len;
					}
				}
			}
		};

		// violations at the current positions; OutNeed = the bind-space push each vertex needs (largest over poses).
		// Returns the violations that can still be fixed; OutBlocked counts those an outer garment leaves no room for.
		int32 Blocked = 0;
		auto Evaluate = [&](TArray<FVector3d>& OutNeed, double& OutWorst, bool bCount) -> int32
		{
			OutNeed.Init(FVector3d::ZeroVector, NV);
			OutWorst = 0.0;
			TArray<bool> Bad, NoRoom;
			Bad.Init(false, NV);
			NoRoom.Init(false, NV);
			for (int32 P = 0; P < Poses.Num(); ++P)
			{
				TArray<FVector3d> X;
				TArray<FMatrix44d> M;
				SkinFollower(Geom, Target, Poses[P], GarmentToLeader, X, &M, &Bind);
				for (int32 V = 0; V < NV; ++V)
				{
					if (bRegion && !Region[0].IsInsideOrOn(Rest[V]))
					{
						continue;
					}
					FVector3d S, Nrm; int32 Tri; double Dist;
					if (!Obstacles[P].Nearest(X[V], Reach, S, Nrm, Tri, Dist) || Nrm.IsNearlyZero())
					{
						continue;
					}
					const FVector3d Inside = NearestBonePoint(Segments[P], S);
					if (bCount) { ++Checks; }
					if ((Nrm | (S - Inside)) < 0.0)
					{
						Nrm = -Nrm;
						if (bCount) { ++Flips; }
					}
					const double D = (X[V] - S) | Nrm;
					if (D < Margin - Tolerance)
					{
						double Push = Margin - D;
						if (!OuterSoups[P].IsEmpty())
						{
							const double Hit = OuterSoups[P].Ray(X[V], Nrm, Push + UnderGap);
							if (Hit >= 0.0)
							{
								Push = FMath::Max(0.0, Hit - UnderGap);
							}
						}
						if (Push <= Tolerance)
						{
							NoRoom[V] = true;
							continue;
						}
						Bad[V] = true;
						OutWorst = FMath::Max(OutWorst, Margin - D);
						const FVector3d DeltaBind = M[V].Inverse().TransformVector(Nrm * Push);
						if (DeltaBind.SizeSquared() > OutNeed[V].SizeSquared()) { OutNeed[V] = DeltaBind; }
					}
				}
			}
			// the same push must have room in EVERY pose (a jacket that bunches when seated leaves less room there)
			if (Outers.Num() > 0)
			{
				TArray<FVector3d> Clamped = OutNeed;
				ClampToOuter(Clamped);
				for (int32 V = 0; V < NV; ++V)
				{
					if (Bad[V] && Clamped[V].Size() <= Tolerance)
					{
						Bad[V] = false;
						NoRoom[V] = true;
					}
					OutNeed[V] = Bad[V] ? Clamped[V] : FVector3d::ZeroVector;
				}
			}
			int32 Count = 0;
			Blocked = 0;
			for (int32 V = 0; V < NV; ++V)
			{
				Count += Bad[V] ? 1 : 0;
				Blocked += (NoRoom[V] && !Bad[V]) ? 1 : 0;
			}
			return Count;
		};

		TArray<FVector3d> Need;
		double Worst = 0.0;
		const int32 Before = Evaluate(Need, Worst, true);
		const double WorstBefore = Worst;
		const int32 BlockedBefore = Blocked;
		TArray<FVector3d> Total;
		Total.Init(FVector3d::ZeroVector, NV);
		int32 PassesRun = 0;
		int32 Remaining = Before;
		while (Remaining > 0 && PassesRun < Passes && !bDryRun)
		{
			// diffuse the push over the garment's edges, never below what a vertex needs
			TArray<FVector3d> D = Need;
			for (int32 It = 0; It < SmoothIterations; ++It)
			{
				TArray<FVector3d> Next = D;
				for (int32 V = 0; V < NV; ++V)
				{
					if (Geom.Neighbors[V].Num() == 0 || (bRegion && !SpreadBox.IsInsideOrOn(Rest[V])))
					{
						continue;
					}
					FVector3d Acc = FVector3d::ZeroVector;
					for (const int32 Nb : Geom.Neighbors[V]) { Acc += D[Nb]; }
					FVector3d Val = D[V] * 0.5 + Acc * (0.5 / Geom.Neighbors[V].Num());
					if (Val.SizeSquared() < Need[V].SizeSquared()) { Val = Need[V]; }
					Next[V] = Val;
				}
				D = MoveTemp(Next);
			}
			ClampToOuter(D);
			for (int32 V = 0; V < NV; ++V) { Bind[V] += D[V]; Total[V] += D[V]; }
			++PassesRun;
			Remaining = Evaluate(Need, Worst, false);
		}
		int32 Moved = 0;
		double MaxMove = 0.0;
		FVector3d MeanMove = FVector3d::ZeroVector;
		for (int32 V = 0; V < NV; ++V)
		{
			if (Total[V].SizeSquared() > 1e-8) { ++Moved; MaxMove = FMath::Max(MaxMove, Total[V].Size()); MeanMove += Total[V]; }
		}
		R->SetNumberField(TEXT("vertices"), NV);
		R->SetNumberField(TEXT("violations_before"), Before);
		R->SetNumberField(TEXT("worst_before_cm"), WorstBefore);
		R->SetNumberField(TEXT("passes"), PassesRun);
		R->SetNumberField(TEXT("vertices_moved"), Moved);
		R->SetNumberField(TEXT("max_move_cm"), MaxMove);
		R->SetStringField(TEXT("mean_move_bind_cm"), (Moved > 0 ? MeanMove / Moved : FVector3d::ZeroVector).ToString());
		R->SetNumberField(TEXT("violations_after"), bDryRun ? Before : Remaining);
		R->SetNumberField(TEXT("worst_after_cm"), bDryRun ? WorstBefore : Worst);
		R->SetNumberField(TEXT("blocked_by_outer_before"), BlockedBefore);
		R->SetNumberField(TEXT("blocked_by_outer_after"), Blocked);
		LodReports.Add(MakeShared<FJsonValueObject>(R));
		if (!bDryRun && Moved > 0)
		{
			FSkeletalMeshAttributes Attr(Desc);
			TVertexAttributesRef<FVector3f> Positions = Attr.GetVertexPositions();
			for (int32 V = 0; V < NV; ++V) { Positions.Set(Geom.VertexIds[V], FVector3f(Bind[V])); }
			Edited.Add(Lod, MoveTemp(Desc));
		}
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("garment_mesh"), Target->GetPathName());
	Out->SetStringField(TEXT("obstacle"), ObstacleComp.Mesh->GetPathName());
	Out->SetArrayField(TEXT("poses"), PoseLabels(Poses));
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetNumberField(TEXT("outward_checks"), static_cast<double>(Checks));
	Out->SetNumberField(TEXT("triangle_normals_flipped_outward"), static_cast<double>(Flips));
	Out->SetArrayField(TEXT("under"), VmhJson::StringArray(OuterNames));
	Out->SetArrayField(TEXT("lods"), LodReports);
	if (Edited.Num() > 0)
	{
		if (!CommitLods(Target, Edited, Error))
		{
			return VmhJson::Error(TEXT("COMMIT_FAILED"), Error, Out);
		}
		if (bAssign)
		{
			AssignMesh(Blueprint, GarmentComp.Template, Target);
			Out->SetBoolField(TEXT("assigned"), true);
		}
	}
	Out->SetStringField(TEXT("hint"), TEXT("a push on a crew neck can tear the collar: look at the result; limit it with options.region; save the garment (and the Blueprint when assigned)"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// snapshot / restore
// =====================================================================================================================

FString UMetaHumanWardrobeService::SnapshotBlueprintComponents(const FString& BlueprintPath, const FString& OutFilePath)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint at %s"), *BlueprintPath));
	}
	const TSharedPtr<FJsonObject> Snap = VmhBlueprintSnapshot::Take(Blueprint);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	if (!OutFilePath.IsEmpty())
	{
		IFileManager::Get().MakeDirectory(*FPaths::GetPath(OutFilePath), true);
		if (!FFileHelper::SaveStringToFile(VmhJson::Stringify(Snap, true), *OutFilePath, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
		{
			return VmhJson::Error(TEXT("WRITE_FAILED"), FString::Printf(TEXT("could not write %s"), *OutFilePath));
		}
		Out->SetStringField(TEXT("file"), OutFilePath);
	}
	const TArray<TSharedPtr<FJsonValue>>* Components = nullptr;
	Snap->TryGetArrayField(TEXT("components"), Components);
	Out->SetNumberField(TEXT("components"), Components ? Components->Num() : 0);
	Out->SetObjectField(TEXT("snapshot"), Snap);
	return VmhJson::Ok(Out);
}

FString UMetaHumanWardrobeService::RestoreBlueprintComponents(const FString& BlueprintPath, const FString& Snapshot, const FString& SkipPatternsJson, bool bAddMissing, bool bApplyToExisting, bool bDryRun)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint at %s"), *BlueprintPath));
	}
	FString Text = Snapshot.TrimStartAndEnd();
	if (!Text.StartsWith(TEXT("{")))
	{
		if (!FFileHelper::LoadFileToString(Text, *Snapshot))
		{
			return VmhJson::Error(TEXT("SNAPSHOT_NOT_FOUND"), FString::Printf(TEXT("Snapshot is neither JSON nor a readable file: %s"), *Snapshot));
		}
	}
	FString Error;
	TSharedPtr<FJsonObject> Snap = VmhJson::Parse(Text, Error);
	if (!Snap.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_SNAPSHOT"), Error);
	}
	const TSharedPtr<FJsonObject>* Inner = nullptr;
	if (Snap->TryGetObjectField(TEXT("snapshot"), Inner)) { Snap = *Inner; } // the whole snapshot_blueprint_components result

	VmhBlueprintSnapshot::FRestoreOptions Options;
	Options.bAddMissing = bAddMissing;
	Options.bApplyToExisting = bApplyToExisting;
	Options.bDryRun = bDryRun;
	if (!SkipPatternsJson.IsEmpty())
	{
		TArray<TSharedPtr<FJsonValue>> Patterns;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(SkipPatternsJson);
		if (!FJsonSerializer::Deserialize(Reader, Patterns))
		{
			return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("SkipPatternsJson is a JSON array of wildcard strings"));
		}
		for (const TSharedPtr<FJsonValue>& P : Patterns) { Options.SkipPatterns.Add(P->AsString()); }
	}
	const TSharedPtr<FJsonObject> Report = VmhBlueprintSnapshot::Restore(Blueprint, Snap, Options, Error);
	if (!Report.IsValid())
	{
		return VmhJson::Error(TEXT("RESTORE_FAILED"), Error);
	}
	return VmhJson::Ok(Report);
}

// =====================================================================================================================
// fill_groom_lods
// =====================================================================================================================

FString UMetaHumanWardrobeService::FillGroomLods(const FString& GroomPath, bool bMakeHiddenLodsVisible, bool bDryRun)
{
	UGroomAsset* Groom = VmhAssets::Load<UGroomAsset>(GroomPath);
	if (!Groom)
	{
		return VmhJson::Error(TEXT("GROOM_NOT_FOUND"), FString::Printf(TEXT("No groom asset at %s"), *GroomPath));
	}
	if (!bDryRun && !IsEditablePath(Groom->GetPathName()))
	{
		return VmhJson::Error(TEXT("PROTECTED_ASSET"), FString::Printf(TEXT("%s is outside /Game or in /Game/Fab: fill the built character's own groom copy"), *Groom->GetPathName()));
	}
	TArray<FHairGroupsLOD> LODs = Groom->GetHairGroupsLOD();
	TArray<FHairGroupsCardsSourceDescription> Cards = Groom->GetHairGroupsCards();
	TArray<FHairGroupsMeshesSourceDescription> Meshes = Groom->GetHairGroupsMeshes();

	auto FindCards = [&Cards](int32 Group, int32 Lod) -> int32
	{
		return Cards.IndexOfByPredicate([&](const FHairGroupsCardsSourceDescription& D) { return D.GroupIndex == Group && D.LODIndex == Lod && D.ImportedMesh; });
	};
	auto FindMesh = [&Meshes](int32 Group, int32 Lod) -> int32
	{
		return Meshes.IndexOfByPredicate([&](const FHairGroupsMeshesSourceDescription& D) { return D.GroupIndex == Group && D.LODIndex == Lod && D.ImportedMesh; });
	};
	auto NearestCards = [&](int32 Group, int32 Lod) -> int32
	{
		int32 Best = INDEX_NONE, BestDist = MAX_int32;
		for (int32 I = 0; I < Cards.Num(); ++I)
		{
			if (Cards[I].GroupIndex == Group && Cards[I].ImportedMesh && FMath::Abs(Cards[I].LODIndex - Lod) < BestDist) { Best = I; BestDist = FMath::Abs(Cards[I].LODIndex - Lod); }
		}
		return Best;
	};
	auto NearestMesh = [&](int32 Group, int32 Lod) -> int32
	{
		int32 Best = INDEX_NONE, BestDist = MAX_int32;
		for (int32 I = 0; I < Meshes.Num(); ++I)
		{
			if (Meshes[I].GroupIndex == Group && Meshes[I].ImportedMesh && FMath::Abs(Meshes[I].LODIndex - Lod) < BestDist) { Best = I; BestDist = FMath::Abs(Meshes[I].LODIndex - Lod); }
		}
		return Best;
	};

	TArray<TSharedPtr<FJsonValue>> Changes, Gaps;
	bool bCardsChanged = false, bMeshesChanged = false, bLodChanged = false;
	for (int32 Group = 0; Group < LODs.Num(); ++Group)
	{
		for (int32 Lod = 0; Lod < LODs[Group].LODs.Num(); ++Lod)
		{
			FHairLODSettings& S = LODs[Group].LODs[Lod];
			const FString Where = FString::Printf(TEXT("group %d LOD %d"), Group, Lod);
			if (!S.bVisible)
			{
				if (!bMakeHiddenLodsVisible)
				{
					continue;
				}
				const int32 Donor = NearestCards(Group, Lod);
				const int32 MeshDonor = Donor == INDEX_NONE ? NearestMesh(Group, Lod) : INDEX_NONE;
				if (Donor == INDEX_NONE && MeshDonor == INDEX_NONE)
				{
					Gaps.Add(MakeShared<FJsonValueString>(Where + TEXT(": hidden, and the group has no cards or meshes to show")));
					continue;
				}
				S.bVisible = true;
				S.GeometryType = Donor != INDEX_NONE ? EGroomGeometryType::Cards : EGroomGeometryType::Meshes;
				bLodChanged = true;
				Changes.Add(MakeShared<FJsonValueString>(Where + TEXT(": hidden -> visible ") + (Donor != INDEX_NONE ? TEXT("cards") : TEXT("meshes"))));
			}
			if (S.GeometryType == EGroomGeometryType::Cards && FindCards(Group, Lod) == INDEX_NONE)
			{
				const int32 Donor = NearestCards(Group, Lod);
				if (Donor == INDEX_NONE)
				{
					Gaps.Add(MakeShared<FJsonValueString>(Where + TEXT(": cards LOD, and the group has no cards at all")));
					continue;
				}
				FHairGroupsCardsSourceDescription Copy = Cards[Donor];
				Copy.LODIndex = Lod;
				Changes.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: cards copied from LOD %d"), *Where, Cards[Donor].LODIndex)));
				Cards.Add(Copy);
				bCardsChanged = true;
			}
			else if (S.GeometryType == EGroomGeometryType::Meshes && FindMesh(Group, Lod) == INDEX_NONE)
			{
				const int32 Donor = NearestMesh(Group, Lod);
				if (Donor != INDEX_NONE)
				{
					FHairGroupsMeshesSourceDescription Copy = Meshes[Donor];
					Copy.LODIndex = Lod;
					Changes.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: mesh copied from LOD %d"), *Where, Meshes[Donor].LODIndex)));
					Meshes.Add(Copy);
					bMeshesChanged = true;
				}
				else if (const int32 CardDonor = NearestCards(Group, Lod); CardDonor != INDEX_NONE)
				{
					S.GeometryType = EGroomGeometryType::Cards;
					FHairGroupsCardsSourceDescription Copy = Cards[CardDonor];
					Copy.LODIndex = Lod;
					Changes.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("%s: meshes LOD with no mesh -> cards from LOD %d"), *Where, Cards[CardDonor].LODIndex)));
					Cards.Add(Copy);
					bCardsChanged = bLodChanged = true;
				}
				else
				{
					Gaps.Add(MakeShared<FJsonValueString>(Where + TEXT(": meshes LOD, and the group has no meshes or cards")));
				}
			}
		}
	}

	if (!bDryRun && (bCardsChanged || bMeshesChanged || bLodChanged))
	{
		Groom->Modify();
		if (bLodChanged)
		{
			Groom->GetHairGroupsLOD() = LODs;
			FPropertyChangedEvent Event(FindFProperty<FProperty>(UGroomAsset::StaticClass(), UGroomAsset::GetHairGroupsLODMemberName()), EPropertyChangeType::ValueSet);
			Groom->PostEditChangeProperty(Event);
		}
		if (bMeshesChanged)
		{
			Groom->GetHairGroupsMeshes() = Meshes;
			FPropertyChangedEvent Event(FindFProperty<FProperty>(UGroomAsset::StaticClass(), UGroomAsset::GetHairGroupsMeshesMemberName()), EPropertyChangeType::ArrayAdd);
			Groom->PostEditChangeProperty(Event);
		}
		if (bCardsChanged)
		{
			Groom->GetHairGroupsCards() = Cards;
			FPropertyChangedEvent Event(FindFProperty<FProperty>(UGroomAsset::StaticClass(), UGroomAsset::GetHairGroupsCardsMemberName()), EPropertyChangeType::ArrayAdd);
			Groom->PostEditChangeProperty(Event);
		}
		Groom->MarkPackageDirty();
	}

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("groom"), Groom->GetPathName());
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetNumberField(TEXT("groups"), LODs.Num());
	Out->SetArrayField(TEXT("changes"), Changes);
	Out->SetArrayField(TEXT("gaps"), Gaps);
	Out->SetStringField(TEXT("hint"), Changes.Num() == 0 ? TEXT("every LOD already has geometry") : TEXT("the bindings rebuild on their own (about a minute; the log prints Waiting for groom bindings); save the groom"));
	return VmhJson::Ok(Out);
}
