// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Services/UMetaHumanWardrobeService.h"

#include "Animation/AnimInstance.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "BoneWeights.h"
#include "Engine/SkeletalMeshLODSettings.h"
#include "Engine/Texture.h"
#include "EngineUtils.h"
#include "FileHelpers.h"
#include "GroomComponent.h"
#include "MaterialEditingLibrary.h"
#include "Materials/MaterialInstanceConstant.h"
#include "PreviewScene.h"
#include "Engine/SkeletalMeshSocket.h"
#include "Engine/SkinnedAssetCommon.h"
#include "PhysicsEngine/PhysicsAsset.h"
#include "Components/PrimitiveComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "DNA.h"
#include "Editor.h"
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
#include "IPythonScriptPlugin.h"
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
#include "Utils/VmhPreview.h"
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

	/**
	 * Rest pose plus the listed poses, on the leader mesh. Entries of options.poses:
	 *   "path" or {"anim": path, "time": s} | {"anim", "times": [s...]} | {"anim", "stride": s} (every s seconds),
	 *     evaluated per entry "evaluate" (default options.evaluate, else DefaultEvaluate): "asset" samples the clip on the
	 *     skeleton (no post-process AnimBP: body correctives at rest), "runtime" plays it on the Blueprint spawned in a
	 *     preview world with every anim graph evaluated (what PIE shows);
	 *   {"file": path} - poses written by dump_runtime_poses / dump_actor_pose (or by hand, same JSON);
	 *   {"bones": {name: [tx,ty,tz, qx,qy,qz,qw, sx,sy,sz]}, "label": ...} - one pose inline (body component space).
	 */
	bool ReadPoses(const TSharedPtr<FJsonObject>& Options, const USkeletalMesh* Leader, TArray<FPose>& OutPoses, FString& OutError, UBlueprint* Blueprint = nullptr, const TCHAR* DefaultEvaluate = TEXT("asset"))
	{
		OutPoses.Add(RefPose(Leader->GetRefSkeleton()));
		const FString GlobalEvaluate = OptStr(Options, TEXT("evaluate"), DefaultEvaluate);
		TUniquePtr<VmhPreview::FCharacter> Runtime;
		auto AddPoseList = [&OutPoses, &OutError](const TArray<TSharedPtr<FJsonValue>>& List) -> bool
		{
			for (const TSharedPtr<FJsonValue>& Item : List)
			{
				FPose Pose;
				if (!VmhPreview::PoseFromJson(Item->AsObject(), Pose, OutError))
				{
					return false;
				}
				if (Pose.Label.IsEmpty()) { Pose.Label = FString::Printf(TEXT("dump %d"), OutPoses.Num()); }
				OutPoses.Add(MoveTemp(Pose));
			}
			return true;
		};
		for (const TSharedPtr<FJsonValue>& V : OptArr(Options, TEXT("poses")))
		{
			const TSharedPtr<FJsonObject> P = V->Type == EJson::Object ? V->AsObject() : nullptr;
			FString File;
			if (P.IsValid() && P->TryGetStringField(TEXT("file"), File))
			{
				FString Text;
				TSharedPtr<FJsonValue> Parsed;
				if (!FFileHelper::LoadFileToString(Text, *File) || !FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Parsed) || !Parsed.IsValid())
				{
					OutError = FString::Printf(TEXT("could not read the pose file %s"), *File);
					return false;
				}
				const TSharedPtr<FJsonObject> Root = Parsed->Type == EJson::Object ? Parsed->AsObject() : nullptr;
				const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
				if (Parsed->Type == EJson::Array) { if (!AddPoseList(Parsed->AsArray())) { return false; } }
				else if (Root.IsValid() && Root->TryGetArrayField(TEXT("poses"), List)) { if (!AddPoseList(*List)) { return false; } }
				else if (!AddPoseList({ Parsed })) { return false; }
				continue;
			}
			if (P.IsValid() && P->HasField(TEXT("bones")))
			{
				if (!AddPoseList({ V })) { return false; }
				continue;
			}
			const FString AnimPath = P.IsValid() ? P->GetStringField(TEXT("anim")) : V->AsString();
			UAnimSequence* Anim = VmhAssets::Load<UAnimSequence>(AnimPath);
			if (!Anim)
			{
				OutError = FString::Printf(TEXT("no AnimSequence at %s (poses take {\"anim\": path, \"time\": s}, {\"file\": dump.json} or {\"bones\": {...}})"), *AnimPath);
				return false;
			}
			TArray<double> Times;
			const TArray<TSharedPtr<FJsonValue>>* TimeList = nullptr;
			if (P.IsValid() && P->TryGetArrayField(TEXT("times"), TimeList))
			{
				for (const TSharedPtr<FJsonValue>& T : *TimeList) { Times.Add(T->AsNumber()); }
			}
			else if (P.IsValid() && OptNum(P, TEXT("stride"), 0.0) > 0.0)
			{
				const double Stride = FMath::Max(0.05, OptNum(P, TEXT("stride"), 0.5));
				for (double T = 0.0; T <= Anim->GetPlayLength() + 1e-6 && Times.Num() < 400; T += Stride) { Times.Add(FMath::Min(T, static_cast<double>(Anim->GetPlayLength()))); }
			}
			else
			{
				Times.Add(P.IsValid() ? OptNum(P, TEXT("time"), 0.0) : 0.0);
			}
			const bool bRuntime = OptStr(P, TEXT("evaluate"), GlobalEvaluate).Equals(TEXT("runtime"), ESearchCase::IgnoreCase);
			if (bRuntime && !Runtime)
			{
				if (!Blueprint)
				{
					OutError = TEXT("evaluate \"runtime\" needs the character Blueprint");
					return false;
				}
				Runtime = MakeUnique<VmhPreview::FCharacter>();
				Runtime->BodyComponentName = OptStr(Options, TEXT("body_component"), TEXT("Body"));
				if (!Runtime->Spawn(Blueprint, OutError))
				{
					OutError = TEXT("runtime pose: ") + OutError;
					return false;
				}
			}
			for (const double Time : Times)
			{
				FPose Pose;
				if (bRuntime)
				{
					if (!Runtime->Pose(Anim, Time, OutError))
					{
						return false;
					}
					Pose = Runtime->BodyPose(FString::Printf(TEXT("%s @ %.2fs (runtime)"), *Anim->GetName(), Time));
				}
				else if (!AnimPose(Leader->GetRefSkeleton(), Anim, Time, Pose, OutError))
				{
					return false;
				}
				OutPoses.Add(MoveTemp(Pose));
			}
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

	/**
	 * The leader bones a garment LOD can rely on. Garment LOD n is drawn while the leader is at LOD n, and the garment's
	 * LAST LOD also at every lower leader LOD (a one-LOD garment is drawn at body LOD 0, 1 and 2), so the last LOD gets
	 * the intersection of the leader's RequiredBones over leader LODs n..last. (Clamping to one leader LOD let a one-LOD
	 * dress streak at body LOD1-2, where the body drops corrective and helper bones: 2026-11-14, issue #3.)
	 */
	bool LeaderRequired(const USkeletalMesh* Garment, const USkeletalMesh* Leader, int32 GarmentLod, TSet<int32>& OutRequired, int32& OutFirst, int32& OutLast, FString& OutError)
	{
		const FSkeletalMeshRenderData* RenderData = Leader ? Leader->GetResourceForRendering() : nullptr;
		if (!RenderData || RenderData->LODRenderData.Num() == 0)
		{
			OutError = TEXT("the leader mesh has no render data");
			return false;
		}
		const int32 LeaderLast = RenderData->LODRenderData.Num() - 1;
		const int32 GarmentLast = Garment->GetLODNum() - 1;
		auto Range = [LeaderLast, GarmentLast](int32 G, int32& First, int32& Last)
		{
			First = FMath::Clamp(G, 0, LeaderLast);
			Last = G >= GarmentLast ? LeaderLast : First;
		};
		Range(GarmentLod, OutFirst, OutLast);
		// a LOD generated by reduction from this one (no mesh description of its own) inherits these weights: its
		// leader LODs count here too (a 4-LOD shoe with generated LOD1-3 is drawn at every body LOD from LOD0's weights)
		for (int32 G = GarmentLod + 1; G <= GarmentLast; ++G)
		{
			// follow the chain: a LOD reduced from a LOD that was itself reduced still carries the first one's weights
			int32 Base = G;
			for (int32 Guard = 0; Guard <= GarmentLast && Base > GarmentLod && !Garment->HasMeshDescription(Base); ++Guard)
			{
				const FSkeletalMeshLODInfo* Info = Garment->GetLODInfo(Base);
				if (!Info || Info->ReductionSettings.BaseLOD >= Base)
				{
					break;
				}
				Base = Info->ReductionSettings.BaseLOD;
			}
			if (Base == GarmentLod && Base != G)
			{
				int32 F = 0, L = 0;
				Range(G, F, L);
				OutFirst = FMath::Min(OutFirst, F);
				OutLast = FMath::Max(OutLast, L);
			}
		}
		OutRequired.Reset();
		for (int32 L = OutFirst; L <= OutLast; ++L)
		{
			TSet<int32> Lod;
			for (const FBoneIndexType Bone : RenderData->LODRenderData[L].RequiredBones)
			{
				Lod.Add(Bone);
			}
			OutRequired = L == OutFirst ? MoveTemp(Lod) : OutRequired.Intersect(Lod);
		}
		if (OutRequired.Num() == 0)
		{
			OutError = TEXT("the leader LOD lists no required bones");
			return false;
		}
		return true;
	}

	FString LodRange(int32 First, int32 Last)
	{
		return First == Last ? FString::FromInt(First) : FString::Printf(TEXT("%d-%d"), First, Last);
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
	if (GarmentLod < 0)
	{
		// every LOD: totals at the top (what attach_skeletal_mesh_to_blueprint warns on), one report per LOD below
		TSharedPtr<FJsonObject> All = VmhJson::Obj();
		TArray<TSharedPtr<FJsonValue>> Lods;
		double Dead = 0, Missing = 0;
		TArray<FString> DeadBones, MissingBones;
		const int32 Num = Garment ? Garment->GetLODNum() : 0;
		for (int32 L = 0; L < Num; ++L)
		{
			const TSharedPtr<FJsonObject> R = CheckFollower(Garment, Leader, L);
			double D = 0, M = 0;
			if (R->TryGetNumberField(TEXT("unevaluated_bone_vertices"), D)) { Dead += D; }
			if (R->TryGetNumberField(TEXT("missing_bone_vertices"), M)) { Missing += M; }
			const TArray<TSharedPtr<FJsonValue>>* Names = nullptr;
			if (R->TryGetArrayField(TEXT("unevaluated_bones"), Names)) { for (const TSharedPtr<FJsonValue>& V : *Names) { DeadBones.AddUnique(V->AsString().Left(V->AsString().Find(TEXT(" (")))); } }
			if (R->TryGetArrayField(TEXT("missing_bones"), Names)) { for (const TSharedPtr<FJsonValue>& V : *Names) { MissingBones.AddUnique(V->AsString().Left(V->AsString().Find(TEXT(" (")))); } }
			Lods.Add(MakeShared<FJsonValueObject>(R));
		}
		if (Num == 0)
		{
			All->SetStringField(TEXT("error"), TEXT("no garment or leader mesh"));
		}
		All->SetNumberField(TEXT("garment_lods"), Num);
		All->SetNumberField(TEXT("unevaluated_bone_vertices"), Dead);
		All->SetArrayField(TEXT("unevaluated_bones"), VmhJson::StringArray(DeadBones));
		All->SetNumberField(TEXT("missing_bone_vertices"), Missing);
		All->SetArrayField(TEXT("missing_bones"), VmhJson::StringArray(MissingBones));
		All->SetArrayField(TEXT("lods"), Lods);
		return All;
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	FMeshDescription Desc;
	FLodGeom Geom;
	FString Error;
	TSet<int32> Required;
	int32 LeaderFirst = 0, LeaderLast = 0;
	if (!Garment || !Leader || !ReadLod(Garment, GarmentLod, Desc, Geom, Error) || !LeaderRequired(Garment, Leader, GarmentLod, Required, LeaderFirst, LeaderLast, Error))
	{
		Out->SetNumberField(TEXT("garment_lod"), GarmentLod);
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
	Out->SetNumberField(TEXT("leader_lod"), LeaderFirst); // kept for readers of the earlier single-LOD report
	Out->SetStringField(TEXT("leader_lods"), LodRange(LeaderFirst, LeaderLast));
	Out->SetNumberField(TEXT("unevaluated_bone_vertices"), DeadVerts);
	Out->SetArrayField(TEXT("unevaluated_bones"), TopCounts(DeadUse, 12));
	Out->SetNumberField(TEXT("missing_bone_vertices"), MissingVerts);
	Out->SetArrayField(TEXT("missing_bones"), TopCounts(MissingUse, 12));
	return Out;
}

// =====================================================================================================================
// describe_skeletal_mesh
// =====================================================================================================================

FString UMetaHumanWardrobeService::DescribeSkeletalMesh(const FString& MeshPath, const FString& LeaderMeshPath, bool bListAllBones)
{
	USkeletalMesh* Mesh = VmhAssets::Load<USkeletalMesh>(MeshPath);
	if (!Mesh)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No skeletal mesh at %s"), *MeshPath));
	}
	const USkeletalMesh* Leader = LeaderMeshPath.IsEmpty() ? nullptr : VmhAssets::Load<USkeletalMesh>(LeaderMeshPath);
	if (!LeaderMeshPath.IsEmpty() && !Leader)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No leader skeletal mesh at %s"), *LeaderMeshPath));
	}
	const int32 MaxNames = bListAllBones ? MAX_int32 : 40;
	auto Names = [MaxNames](const TArray<FString>& All) -> TArray<TSharedPtr<FJsonValue>>
	{
		TArray<TSharedPtr<FJsonValue>> Out;
		for (int32 I = 0; I < All.Num() && I < MaxNames; ++I) { Out.Add(MakeShared<FJsonValueString>(All[I])); }
		if (All.Num() > MaxNames) { Out.Add(MakeShared<FJsonValueString>(FString::Printf(TEXT("... %d more (bListAllBones)"), All.Num() - MaxNames))); }
		return Out;
	};

	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	const FReferenceSkeleton& Ref = Mesh->GetRefSkeleton();
	Out->SetStringField(TEXT("mesh"), Mesh->GetPathName());
	Out->SetStringField(TEXT("skeleton"), Mesh->GetSkeleton() ? Mesh->GetSkeleton()->GetPathName() : TEXT(""));
	Out->SetStringField(TEXT("physics_asset"), Mesh->GetPhysicsAsset() ? Mesh->GetPhysicsAsset()->GetPathName() : TEXT(""));
	Out->SetStringField(TEXT("post_process_anim_blueprint"), Mesh->GetPostProcessAnimBlueprint() ? Mesh->GetPostProcessAnimBlueprint()->GetPathName() : TEXT(""));
	Out->SetStringField(TEXT("lod_settings"), Mesh->GetLODSettings() ? Mesh->GetLODSettings()->GetPathName() : TEXT(""));
	Out->SetNumberField(TEXT("bones"), Ref.GetNum());
	Out->SetNumberField(TEXT("morph_targets"), Mesh->GetMorphTargets().Num());
	const FBoxSphereBounds Bounds = Mesh->GetImportedBounds();
	Out->SetStringField(TEXT("bounds_min_cm"), (Bounds.Origin - Bounds.BoxExtent).ToString());
	Out->SetStringField(TEXT("bounds_max_cm"), (Bounds.Origin + Bounds.BoxExtent).ToString());
	TArray<FString> Sockets;
	for (int32 I = 0; I < Mesh->NumSockets(); ++I)
	{
		if (const USkeletalMeshSocket* S = Mesh->GetSocketByIndex(I)) { Sockets.Add(S->SocketName.ToString() + TEXT(" -> ") + S->BoneName.ToString()); }
	}
	Out->SetArrayField(TEXT("sockets"), VmhJson::StringArray(Sockets));
	TArray<TSharedPtr<FJsonValue>> Slots;
	for (int32 I = 0; I < Mesh->GetMaterials().Num(); ++I)
	{
		const FSkeletalMaterial& M = Mesh->GetMaterials()[I];
		TSharedPtr<FJsonObject> S = VmhJson::Obj();
		S->SetNumberField(TEXT("index"), I);
		S->SetStringField(TEXT("slot"), M.MaterialSlotName.ToString());
		S->SetStringField(TEXT("imported_slot"), M.ImportedMaterialSlotName.ToString());
		S->SetStringField(TEXT("material"), M.MaterialInterface ? M.MaterialInterface->GetPathName() : TEXT(""));
		Slots.Add(MakeShared<FJsonValueObject>(S));
	}
	Out->SetArrayField(TEXT("material_slots"), Slots);

	const FSkeletalMeshRenderData* RenderData = Mesh->GetResourceForRendering();
	TArray<TSharedPtr<FJsonValue>> Lods;
	TSet<int32> PrevRequired;
	for (int32 L = 0; L < Mesh->GetLODNum(); ++L)
	{
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetNumberField(TEXT("lod"), L);
		R->SetBoolField(TEXT("has_mesh_description"), Mesh->HasMeshDescription(L));
		if (const FSkeletalMeshLODInfo* Info = Mesh->GetLODInfo(L))
		{
			R->SetNumberField(TEXT("screen_size"), Info->ScreenSize.Default);
			R->SetBoolField(TEXT("simplified"), Info->bHasBeenSimplified);
			TArray<FString> ToRemove;
			for (const FBoneReference& B : Info->BonesToRemove) { ToRemove.Add(B.BoneName.ToString()); }
			R->SetArrayField(TEXT("bones_to_remove_setting"), Names(ToRemove));
		}
		if (RenderData && RenderData->LODRenderData.IsValidIndex(L))
		{
			const FSkeletalMeshLODRenderData& LodData = RenderData->LODRenderData[L];
			R->SetNumberField(TEXT("vertices"), static_cast<double>(LodData.GetNumVertices()));
			int32 Triangles = 0;
			TArray<TSharedPtr<FJsonValue>> Sections;
			for (const FSkelMeshRenderSection& Section : LodData.RenderSections)
			{
				TSharedPtr<FJsonObject> S = VmhJson::Obj();
				S->SetNumberField(TEXT("material_index"), Section.MaterialIndex);
				S->SetNumberField(TEXT("triangles"), static_cast<double>(Section.NumTriangles));
				S->SetNumberField(TEXT("vertices"), static_cast<double>(Section.NumVertices));
				S->SetBoolField(TEXT("disabled"), Section.bDisabled);
				Sections.Add(MakeShared<FJsonValueObject>(S));
				Triangles += static_cast<int32>(Section.NumTriangles);
			}
			R->SetNumberField(TEXT("triangles"), Triangles);
			R->SetArrayField(TEXT("sections"), Sections);
			TSet<int32> Required;
			for (const FBoneIndexType B : LodData.RequiredBones) { Required.Add(B); }
			R->SetNumberField(TEXT("required_bones"), Required.Num());
			TArray<FString> NotEvaluated, DroppedHere;
			for (int32 B = 0; B < Ref.GetNum(); ++B)
			{
				if (Required.Contains(B)) { continue; }
				NotEvaluated.Add(Ref.GetBoneName(B).ToString());
				if (L > 0 && PrevRequired.Contains(B)) { DroppedHere.Add(Ref.GetBoneName(B).ToString()); }
			}
			R->SetNumberField(TEXT("bones_not_evaluated"), NotEvaluated.Num());
			R->SetArrayField(TEXT("bones_not_evaluated_names"), Names(NotEvaluated));
			if (L > 0)
			{
				R->SetArrayField(TEXT("bones_dropped_since_previous_lod"), Names(DroppedHere));
			}
			PrevRequired = MoveTemp(Required);
		}
		if (Leader)
		{
			R->SetObjectField(TEXT("as_follower"), CheckFollower(Mesh, Leader, L));
		}
		Lods.Add(MakeShared<FJsonValueObject>(R));
	}
	Out->SetNumberField(TEXT("lod_count"), Mesh->GetLODNum());
	Out->SetArrayField(TEXT("lods"), Lods);
	if (Leader)
	{
		Out->SetStringField(TEXT("leader"), Leader->GetPathName());
		const FSkeletalMeshRenderData* LeaderData = Leader->GetResourceForRendering();
		Out->SetNumberField(TEXT("leader_lod_count"), LeaderData ? LeaderData->LODRenderData.Num() : 0);
		Out->SetStringField(TEXT("hint"), TEXT("as_follower: each LOD against the leader LODs it is drawn with (the last LOD against every remaining one); unevaluated_bone_vertices > 0 streak or spike there: fix_unevaluated_bone_weights on a copy"));
	}
	return VmhJson::Ok(Out);
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
		int32 LeaderFirst = 0, LeaderLast = 0;
		if (!ReadLod(Garment, Lod, Desc, Geom, Error) || !LeaderRequired(Garment, Leader, Lod, Required, LeaderFirst, LeaderLast, Error))
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
		R->SetNumberField(TEXT("leader_lod"), LeaderFirst); // kept for readers of the earlier single-LOD report
		R->SetStringField(TEXT("leader_lods"), LodRange(LeaderFirst, LeaderLast));
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
	// the mesh that is cut: the body, or (trim_inner_garment) an inner garment under outer ones; poses always come from
	// the body (the leader) and the target is skinned as its follower
	FComp TargetComp = BodyComp;
	const FString TargetName = OptStr(Options, TEXT("target_component"), TEXT(""));
	if (!TargetName.IsEmpty() && !FindComp(Blueprint, TargetName, TargetComp, Error))
	{
		return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), Error);
	}
	const FTransform TargetToLeader = CompToLeader(Blueprint, TargetComp, BodyComp);
	const FString SourcePath = OptStr(Options, TEXT("source_body"), TargetComp.Mesh->GetPathName());
	USkeletalMesh* Source = VmhAssets::Load<USkeletalMesh>(SourcePath);
	if (!Source)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), FString::Printf(TEXT("No source mesh at %s"), *SourcePath));
	}
	const int32 MinFragment = static_cast<int32>(OptNum(Options, TEXT("min_fragment"), 0));
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
		if (G.Comp.Node == TargetComp.Node)
		{
			return VmhJson::Error(TEXT("BAD_ARGS"), FString::Printf(TEXT("%s is the mesh being cut; list only the garments over it"), *G.Name));
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
	if (!ReadPoses(Options, BodyComp.Mesh, Poses, Error, Blueprint))
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
			SkinFollower(Geom, Source, Poses[P], TargetToLeader, X);
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
		// stray islands the cut leaves (a few triangles of shirt floating inside the jacket)
		int32 NFragment = 0;
		if (MinFragment > 0)
		{
			TSet<FTriangleID> Deleted(Delete);
			TArray<int32> Comp;
			Comp.Init(INDEX_NONE, NT);
			TMultiMap<int32, int32> TrisOfVertex;
			for (int32 T = 0; T < NT; ++T)
			{
				if (Deleted.Contains(Geom.TriIds[T])) { continue; }
				TrisOfVertex.Add(Geom.Tris[T].X, T); TrisOfVertex.Add(Geom.Tris[T].Y, T); TrisOfVertex.Add(Geom.Tris[T].Z, T);
			}
			for (int32 Seed = 0; Seed < NT; ++Seed)
			{
				if (Comp[Seed] != INDEX_NONE || Deleted.Contains(Geom.TriIds[Seed])) { continue; }
				TArray<int32> Island, Stack = { Seed };
				Comp[Seed] = Seed;
				while (Stack.Num() > 0)
				{
					const int32 T = Stack.Pop(EAllowShrinking::No);
					Island.Add(T);
					for (const int32 Corner : { Geom.Tris[T].X, Geom.Tris[T].Y, Geom.Tris[T].Z })
					{
						for (auto It = TrisOfVertex.CreateConstKeyIterator(Corner); It; ++It)
						{
							if (Comp[It.Value()] == INDEX_NONE) { Comp[It.Value()] = Seed; Stack.Add(It.Value()); }
						}
					}
				}
				if (Island.Num() <= MinFragment && Island.Num() < NT - Delete.Num())
				{
					for (const int32 T : Island) { Delete.Add(Geom.TriIds[T]); }
					NFragment += Island.Num();
				}
			}
		}
		R->SetNumberField(TEXT("deleted_fragments"), NFragment);
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
			AssignMesh(Blueprint, TargetComp.Template, Dressed);
			Out->SetBoolField(TEXT("assigned"), true);
		}
	}
	Out->SetStringField(TEXT("target_component"), TargetComp.Node->GetVariableName().ToString());
	Out->SetStringField(TEXT("hint"), TEXT("deleting skin is for closed garments (trousers, shoes, long sleeves under a jacket); under a dress or an open garment use push_body_under_garments (a deleted patch shows as a hole from a low camera). Look at it standing AND seated (capture_fit_views) before keeping it; save the result (and the Blueprint when assigned)"));
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
	if (!ReadPoses(Options, BodyComp.Mesh, Poses, Error, Blueprint))
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

// =====================================================================================================================
// post_build_fixup
// =====================================================================================================================

namespace
{
	/** The groom components of a Blueprint's construction script. */
	struct FGroomUse { FString Component; UGroomComponent* Template = nullptr; };

	TArray<FGroomUse> GroomComponents(UBlueprint* Blueprint)
	{
		TArray<FGroomUse> Out;
		if (USimpleConstructionScript* SCS = Blueprint ? Blueprint->SimpleConstructionScript : nullptr)
		{
			for (USCS_Node* Node : SCS->GetAllNodes())
			{
				if (UGroomComponent* G = Node ? Cast<UGroomComponent>(Node->ComponentTemplate) : nullptr)
				{
					Out.Add({ Node->GetVariableName().ToString(), G });
				}
			}
		}
		return Out;
	}
}

FString UMetaHumanWardrobeService::PostBuildFixup(const FString& BlueprintPath, const FString& OptionsJson, bool bDryRun)
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
	FString Package, Name;
	VmhAssets::SplitAssetPath(Blueprint->GetPathName(), Package, Name);
	const FString Folder = OptStr(Options, TEXT("folder"), Package);
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("folder"), Folder);
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	TArray<FString> Dirty, Notes;

	// 1. groom LODs: every groom the Blueprint draws (hidden LODs made visible: the brows vanish from LOD4 otherwise)
	if (OptBool(Options, TEXT("groom_lods"), true))
	{
		const bool bShowHidden = OptBool(Options, TEXT("show_hidden_groom_lods"), true);
		TArray<TSharedPtr<FJsonValue>> Grooms;
		TSet<UGroomAsset*> Done;
		for (const FGroomUse& G : GroomComponents(Blueprint))
		{
			UGroomAsset* Asset = G.Template->GroomAsset;
			if (!Asset || Done.Contains(Asset))
			{
				continue;
			}
			Done.Add(Asset);
			TSharedPtr<FJsonObject> R = VmhJson::Obj();
			R->SetStringField(TEXT("component"), G.Component);
			R->SetStringField(TEXT("groom"), Asset->GetPathName());
			if (!IsEditablePath(Asset->GetPathName()))
			{
				R->SetStringField(TEXT("skipped"), TEXT("outside /Game or in /Game/Fab: not a build output"));
			}
			else
			{
				FString ParseError;
				const TSharedPtr<FJsonObject> Fill = VmhJson::Parse(FillGroomLods(Asset->GetPathName(), bShowHidden, bDryRun), ParseError);
				const TArray<TSharedPtr<FJsonValue>>* Changes = nullptr;
				const TArray<TSharedPtr<FJsonValue>>* Gaps = nullptr;
				if (Fill.IsValid() && Fill->TryGetArrayField(TEXT("changes"), Changes)) { R->SetArrayField(TEXT("changes"), *Changes); }
				if (Fill.IsValid() && Fill->TryGetArrayField(TEXT("gaps"), Gaps)) { R->SetArrayField(TEXT("gaps"), *Gaps); }
				if (!bDryRun && Changes && Changes->Num() > 0) { Dirty.Add(Asset->GetPathName()); }
			}
			Grooms.Add(MakeShared<FJsonValueObject>(R));
		}
		Out->SetArrayField(TEXT("grooms"), Grooms);
	}

	// 2. brow opacity: the build's Facial_Hair instance drew pale brows at OpacityNear/Far 0.8/0.4 (fixed with 2.0)
	{
		const double BrowOpacity = OptNum(Options, TEXT("brow_opacity"), 0.0);
		const FString BrowComponent = OptStr(Options, TEXT("brow_component"), TEXT("Eyebrows"));
		TArray<TSharedPtr<FJsonValue>> Brows;
		TSet<UMaterialInstanceConstant*> Seen;
		for (const FGroomUse& G : GroomComponents(Blueprint))
		{
			if (!G.Component.Contains(BrowComponent))
			{
				continue;
			}
			TArray<UMaterialInterface*> Materials;
			for (UMaterialInterface* M : G.Template->OverrideMaterials) { Materials.Add(M); }
			if (G.Template->GroomAsset)
			{
				for (const FHairGroupsMaterial& M : G.Template->GroomAsset->GetHairGroupsMaterials()) { Materials.Add(M.Material); }
			}
			for (UMaterialInterface* M : Materials)
			{
				UMaterialInstanceConstant* MIC = Cast<UMaterialInstanceConstant>(M);
				if (!MIC || Seen.Contains(MIC))
				{
					continue;
				}
				Seen.Add(MIC);
				float Near = 0.f, Far = 0.f;
				const bool bHasNear = MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("OpacityNear")), Near);
				const bool bHasFar = MIC->GetScalarParameterValue(FHashedMaterialParameterInfo(TEXT("OpacityFar")), Far);
				if (!bHasNear && !bHasFar)
				{
					continue;
				}
				TSharedPtr<FJsonObject> R = VmhJson::Obj();
				R->SetStringField(TEXT("material"), MIC->GetPathName());
				R->SetNumberField(TEXT("opacity_near"), Near);
				R->SetNumberField(TEXT("opacity_far"), Far);
				if (BrowOpacity > 0.0 && IsEditablePath(MIC->GetPathName()) && (!FMath::IsNearlyEqual(Near, static_cast<float>(BrowOpacity)) || !FMath::IsNearlyEqual(Far, static_cast<float>(BrowOpacity))))
				{
					R->SetNumberField(TEXT("set_to"), BrowOpacity);
					if (!bDryRun)
					{
						UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, TEXT("OpacityNear"), static_cast<float>(BrowOpacity));
						UMaterialEditingLibrary::SetMaterialInstanceScalarParameterValue(MIC, TEXT("OpacityFar"), static_cast<float>(BrowOpacity));
						UMaterialEditingLibrary::UpdateMaterialInstance(MIC);
						MIC->MarkPackageDirty();
						Dirty.Add(MIC->GetPathName());
					}
				}
				Brows.Add(MakeShared<FJsonValueObject>(R));
			}
		}
		Out->SetArrayField(TEXT("brow_materials"), Brows);
	}

	// 3. texture caps, per asset (a device-profile group cap does not reach virtual textures), only when asked for:
	//    options.texture_caps {"name wildcard": size}. Never flips VirtualTextureStreaming: that broke the materials
	//    ("Sampler type is Virtual Color, should be Color").
	{
		TArray<TPair<FString, int32>> Caps;
		const TSharedPtr<FJsonObject>* CapsObj = nullptr;
		if (Options->TryGetObjectField(TEXT("texture_caps"), CapsObj))
		{
			for (const TPair<FString, TSharedPtr<FJsonValue>>& C : (*CapsObj)->Values) { Caps.Emplace(C.Key, static_cast<int32>(C.Value->AsNumber())); }
		}
		TArray<TSharedPtr<FJsonValue>> Textures;
		if (Caps.Num() > 0)
		{
			for (const FString& Path : VmhAssets::FindAssetsOfClass(TEXT("/Script/Engine.Texture"), Folder))
			{
				FString P, TexName;
				VmhAssets::SplitAssetPath(Path, P, TexName);
				const TPair<FString, int32>* Cap = Caps.FindByPredicate([&TexName](const TPair<FString, int32>& C) { return TexName.MatchesWildcard(C.Key); });
				if (!Cap)
				{
					continue;
				}
				UTexture* Texture = VmhAssets::Load<UTexture>(Path);
				if (!Texture || !IsEditablePath(Texture->GetPathName()))
				{
					continue;
				}
				TSharedPtr<FJsonObject> R = VmhJson::Obj();
				R->SetStringField(TEXT("texture"), Texture->GetPathName());
				R->SetNumberField(TEXT("max_texture_size_before"), Texture->MaxTextureSize);
				R->SetNumberField(TEXT("cap"), Cap->Value);
				R->SetBoolField(TEXT("virtual_texture"), Texture->VirtualTextureStreaming != 0);
				// a cap only ever lowers: 0 is uncapped, and a smaller cap already set is kept
				const bool bLower = Cap->Value > 0 && (Texture->MaxTextureSize == 0 || Texture->MaxTextureSize > Cap->Value);
				R->SetBoolField(TEXT("would_change"), bLower);
				if (bLower && !bDryRun)
				{
					Texture->Modify();
					Texture->MaxTextureSize = Cap->Value;
					FPropertyChangedEvent Event(FindFProperty<FProperty>(UTexture::StaticClass(), GET_MEMBER_NAME_CHECKED(UTexture, MaxTextureSize)), EPropertyChangeType::ValueSet);
					Texture->PostEditChangeProperty(Event);
					Texture->MarkPackageDirty();
					Dirty.Add(Texture->GetPathName());
				}
				Textures.Add(MakeShared<FJsonValueObject>(R));
			}
		}
		Out->SetArrayField(TEXT("textures"), Textures);
	}

	// 4. the Face component's anim class (a rebuild puts the plugin's copy-pose-only ABP_Face back)
	{
		const FString FaceAnimClass = OptStr(Options, TEXT("face_anim_class"), TEXT(""));
		FComp Face;
		FString FaceError;
		if (FindComp(Blueprint, OptStr(Options, TEXT("face_component"), TEXT("Face")), Face, FaceError))
		{
			Out->SetStringField(TEXT("face_anim_class_before"), Face.Template->AnimClass ? Face.Template->AnimClass->GetPathName() : TEXT(""));
			if (!FaceAnimClass.IsEmpty())
			{
				UClass* Class = nullptr;
				if (UBlueprint* AnimBlueprint = VmhAssets::Load<UBlueprint>(FaceAnimClass)) { Class = AnimBlueprint->GeneratedClass; }
				if (!Class) { Class = LoadObject<UClass>(nullptr, *FaceAnimClass); }
				if (!Class || !Class->IsChildOf(UAnimInstance::StaticClass()))
				{
					return VmhJson::Error(TEXT("ANIM_CLASS_NOT_FOUND"), FString::Printf(TEXT("%s is not an Animation Blueprint or AnimInstance class"), *FaceAnimClass), Out);
				}
				if (Face.Template->AnimClass != Class && !bDryRun)
				{
					Face.Template->Modify();
					Face.Template->SetAnimInstanceClass(Class);
					FBlueprintEditorUtils::MarkBlueprintAsModified(Blueprint);
					FKismetEditorUtilities::CompileBlueprint(Blueprint, EBlueprintCompileOptions::SkipGarbageCollection);
					Blueprint->MarkPackageDirty();
					Dirty.Add(Blueprint->GetPathName());
				}
				Out->SetStringField(TEXT("face_anim_class"), Class->GetPathName());
			}
		}
		else
		{
			Notes.Add(FaceError);
		}
	}

	Out->SetArrayField(TEXT("dirty"), VmhJson::StringArray(Dirty));
	Out->SetArrayField(TEXT("notes"), VmhJson::StringArray(Notes));
	Out->SetStringField(TEXT("hint"), bDryRun
		? TEXT("dry run: nothing changed; run again with bDryRun false")
		: TEXT("save the dirty packages by name; groom bindings rebuild on their own (about a minute). Asset-level fits (hide/push body) are not redone here: re-run them on the fresh body"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// audit_placed_instances
// =====================================================================================================================

FString UMetaHumanWardrobeService::AuditPlacedInstances(const FString& BlueprintPath, bool bClearOverrides, const FString& MapPath)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->GeneratedClass)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No compiled Blueprint at %s"), *BlueprintPath));
	}
	if (!MapPath.IsEmpty())
	{
		FString MapPackage, MapName;
		VmhAssets::SplitAssetPath(MapPath, MapPackage, MapName);
		const UWorld* Open = VmhAssets::EditorWorld();
		if (!Open || Open->GetOutermost()->GetName() != MapPackage / MapName)
		{
			// loading a map replaces the open one without asking: never throw away unsaved level changes
			TArray<UPackage*> DirtyMaps;
			UEditorLoadingAndSavingUtils::GetDirtyMapPackages(DirtyMaps);
			if (DirtyMaps.Num() > 0)
			{
				return VmhJson::Error(TEXT("UNSAVED_LEVEL"), FString::Printf(TEXT("the open level has unsaved changes (%s%s): save or discard them, then call again (loading %s would drop them)"),
					*DirtyMaps[0]->GetName(), DirtyMaps.Num() > 1 ? TEXT(", ...") : TEXT(""), *MapPath));
			}
			if (!UEditorLoadingAndSavingUtils::LoadMap(MapPath))
			{
				return VmhJson::Error(TEXT("MAP_NOT_LOADED"), FString::Printf(TEXT("could not load %s"), *MapPath));
			}
		}
	}
	UWorld* World = VmhAssets::EditorWorld();
	if (!World)
	{
		return VmhJson::Error(TEXT("NO_WORLD"), TEXT("no editor world"));
	}
	static const TSet<FName> Placement = { TEXT("RelativeLocation"), TEXT("RelativeRotation"), TEXT("RelativeScale3D") };
	// the reference is a FRESH SPAWN of the class in a private world, not the component templates: the construction
	// script sets values of its own (LOD mapping, asset user data) that are not instance overrides
	FPreviewScene Reference(FPreviewScene::ConstructionValues().SetCreatePhysicsScene(false));
	FActorSpawnParameters SpawnParams;
	SpawnParams.ObjectFlags = RF_Transient;
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AActor* Fresh = Reference.GetWorld() ? Reference.GetWorld()->SpawnActor<AActor>(Blueprint->GeneratedClass, FTransform::Identity, SpawnParams) : nullptr;
	TMap<FName, UActorComponent*> FreshComps;
	if (Fresh)
	{
		for (UActorComponent* C : Fresh->GetComponents()) { if (C) { FreshComps.Add(C->GetFName(), C); } }
	}
	TArray<TSharedPtr<FJsonValue>> Actors;
	int32 TotalOverrides = 0, Cleared = 0;
	for (TActorIterator<AActor> It(World); It; ++It)
	{
		AActor* Actor = *It;
		if (!Actor || !Actor->GetClass()->IsChildOf(Blueprint->GeneratedClass))
		{
			continue;
		}
		TSharedPtr<FJsonObject> A = VmhJson::Obj();
		A->SetStringField(TEXT("actor"), Actor->GetActorLabel());
		A->SetStringField(TEXT("level"), Actor->GetLevel() ? Actor->GetLevel()->GetOutermost()->GetName() : TEXT(""));
		TArray<TSharedPtr<FJsonValue>> Overrides;
		TArray<FString> InstanceOnly;
		bool bChanged = false;
		for (UActorComponent* Comp : Actor->GetComponents())
		{
			if (!Comp)
			{
				continue;
			}
			UActorComponent* const* FreshComp = FreshComps.Find(Comp->GetFName());
			const UObject* Archetype = Comp->GetArchetype();
			if (!FreshComp || !Archetype || Comp->CreationMethod == EComponentCreationMethod::Instance)
			{
				InstanceOnly.Add(Comp->GetName());
				continue;
			}
			const UObject* Reference0 = *FreshComp;
			const bool bRoot = Comp == Actor->GetRootComponent();
			for (TFieldIterator<FProperty> P(Comp->GetClass()); P; ++P)
			{
				FProperty* Prop = *P;
				// editable or Blueprint-visible (SkinnedAsset is only the latter), never transient, not the deprecated
				// SkeletalMesh alias of SkinnedAsset, and not the root's placement
				if (!Prop->HasAnyPropertyFlags(CPF_Edit | CPF_BlueprintVisible) || Prop->HasAnyPropertyFlags(CPF_Transient | CPF_DuplicateTransient | CPF_Deprecated)
					|| Prop->IsA<FMulticastDelegateProperty>() || Prop->IsA<FDelegateProperty>()
					|| (bRoot && Placement.Contains(Prop->GetFName())) || Prop->GetFName() == TEXT("SkeletalMesh")
					|| !Reference0->IsA(Prop->GetOwnerClass()) || !Archetype->IsA(Prop->GetOwnerClass()))
				{
					continue;
				}
				if (Prop->Identical_InContainer(Comp, Reference0, 0, PPF_DeepComparison))
				{
					continue;
				}
				FString Instance, Class;
				Prop->ExportText_InContainer(0, Instance, Comp, nullptr, Comp, PPF_None);
				Prop->ExportText_InContainer(0, Class, Reference0, nullptr, const_cast<UObject*>(Reference0), PPF_None);
				if (Instance == Class)
				{
					continue; // instanced subobjects of two worlds: same values, different objects
				}
				TSharedPtr<FJsonObject> O = VmhJson::Obj();
				O->SetStringField(TEXT("component"), Comp->GetName());
				O->SetStringField(TEXT("property"), Prop->GetName());
				O->SetStringField(TEXT("instance"), Instance.Left(300));
				O->SetStringField(TEXT("blueprint"), Class.Left(300));
				++TotalOverrides;
				if (bClearOverrides)
				{
					// back to the template's value (never a fresh-world object); mesh assets through their setters
					Comp->Modify();
					if (USkinnedMeshComponent* Skinned = Cast<USkinnedMeshComponent>(Comp); Skinned && Prop->GetFName() == TEXT("SkinnedAsset"))
					{
						Skinned->SetSkinnedAssetAndUpdate(CastChecked<USkinnedMeshComponent>(Archetype)->GetSkinnedAsset(), false);
					}
					else if (Prop->HasAnyPropertyFlags(CPF_InstancedReference | CPF_ContainsInstancedReference))
					{
						O->SetStringField(TEXT("not_cleared"), TEXT("instanced subobject: reset it in the Details panel"));
						Overrides.Add(MakeShared<FJsonValueObject>(O));
						continue;
					}
					else
					{
						Comp->PreEditChange(Prop);
						Prop->CopyCompleteValue_InContainer(Comp, Archetype);
						FPropertyChangedEvent Event(Prop, EPropertyChangeType::ValueSet);
						Comp->PostEditChangeProperty(Event);
					}
					O->SetBoolField(TEXT("cleared"), true);
					++Cleared;
					bChanged = true;
				}
				Overrides.Add(MakeShared<FJsonValueObject>(O));
			}
		}
		if (bChanged)
		{
			Actor->RerunConstructionScripts();
			Actor->MarkPackageDirty();
		}
		A->SetArrayField(TEXT("overrides"), Overrides);
		A->SetArrayField(TEXT("instance_components"), VmhJson::StringArray(InstanceOnly));
		Actors.Add(MakeShared<FJsonValueObject>(A));
	}
	if (Fresh && Reference.GetWorld())
	{
		Reference.GetWorld()->DestroyActor(Fresh);
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("world"), World->GetOutermost()->GetName());
	Out->SetBoolField(TEXT("compared_with_fresh_spawn"), Fresh != nullptr);
	Out->SetNumberField(TEXT("instances"), Actors.Num());
	Out->SetNumberField(TEXT("overrides"), TotalOverrides);
	Out->SetNumberField(TEXT("cleared"), Cleared);
	Out->SetArrayField(TEXT("actors"), Actors);
	Out->SetStringField(TEXT("hint"), Actors.Num() == 0
		? TEXT("no instance of this Blueprint in the open level: pass MapPath, or open the level the game uses")
		: bClearOverrides ? TEXT("save the level (and its external actor packages) to keep the cleared values")
		                  : TEXT("an override of a mesh, material or anim class hides every Blueprint fix from that actor: clear it (bClearOverrides) unless it is meant"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// trim_inner_garment
// =====================================================================================================================

FString UMetaHumanWardrobeService::TrimInnerGarment(const FString& BlueprintPath, const FString& InnerComponent, const FString& OuterComponentsJson, const FString& OutMeshPath, const FString& OptionsJson, bool bAssignToBlueprint, bool bDryRun)
{
	FString Error;
	TSharedPtr<FJsonObject> Options = VmhJson::Parse(OptionsJson, Error);
	if (!Options.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
	}
	// the jam's shirt-under-jacket values: a thin shell, a stricter winding, no ray hits far outside, small islands go
	const TPair<const TCHAR*, double> Defaults[] = { { TEXT("shell"), 0.6 }, { TEXT("winding"), 0.6 }, { TEXT("contract"), 2 },
		{ TEXT("ray_out"), 5.0 }, { TEXT("ray_in"), 3.0 }, { TEXT("ray_contract"), 1 }, { TEXT("min_fragment"), 40 } };
	for (const TPair<const TCHAR*, double>& D : Defaults)
	{
		if (!Options->HasField(D.Key)) { Options->SetNumberField(D.Key, D.Value); }
	}
	Options->SetStringField(TEXT("target_component"), InnerComponent);
	if (!Options->HasField(TEXT("source_body")))
	{
		// every run starts from the inner component's current mesh unless a source is given (pass the uncut copy on re-runs)
		UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
		FComp Inner;
		if (Blueprint && FindComp(Blueprint, InnerComponent, Inner, Error))
		{
			Options->SetStringField(TEXT("source_body"), Inner.Mesh->GetPathName());
		}
	}
	return HideBodyUnderGarments(BlueprintPath, OuterComponentsJson, OutMeshPath, VmhJson::Stringify(Options), bAssignToBlueprint, bDryRun);
}

// =====================================================================================================================
// push_body_under_garments / measure_poke_through
// =====================================================================================================================

namespace
{
	/** Garments posed per pose, as one combined soup or one soup each, plus the leader's bone segments. */
	struct FPosedGarments
	{
		TArray<FString> Names;
		TArray<TArray<FSoup>> Each;        // [pose][garment]
		TArray<FSoup> All;                 // [pose]
		TArray<TArray<TPair<FVector3d, FVector3d>>> Segments; // [pose]
	};

	bool PoseGarments(UBlueprint* Blueprint, const FComp& BodyComp, const FString& GarmentComponentsJson, const TArray<FPose>& Poses, bool bEach, FPosedGarments& Out, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> List;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(GarmentComponentsJson), List) || List.Num() == 0)
		{
			OutError = TEXT("GarmentComponentsJson is a JSON array of component names, e.g. [\"G_Dress\"]");
			return false;
		}
		struct FG { FComp Comp; FTransform ToLeader; FLodGeom Geom; };
		TArray<FG> Gs;
		for (const TSharedPtr<FJsonValue>& V : List)
		{
			const FString Name = V->Type == EJson::Object ? V->AsObject()->GetStringField(TEXT("component")) : V->AsString();
			FG G;
			if (!FindComp(Blueprint, Name, G.Comp, OutError))
			{
				return false;
			}
			FMeshDescription Unused;
			if (!ReadLod(G.Comp.Mesh, 0, Unused, G.Geom, OutError))
			{
				return false;
			}
			G.ToLeader = CompToLeader(Blueprint, G.Comp, BodyComp);
			Out.Names.Add(Name);
			Gs.Add(MoveTemp(G));
		}
		Out.Each.SetNum(Poses.Num());
		Out.All.SetNum(Poses.Num());
		Out.Segments.SetNum(Poses.Num());
		for (int32 P = 0; P < Poses.Num(); ++P)
		{
			TArray<FVector3d> AllPos;
			TArray<FIntVector3> AllTris;
			for (const FG& G : Gs)
			{
				TArray<FVector3d> X;
				SkinFollower(G.Geom, G.Comp.Mesh, Poses[P], G.ToLeader, X);
				if (bEach) { Out.Each[P].AddDefaulted_GetRef().Build(X, G.Geom.Tris, false); }
				const int32 Base = AllPos.Num();
				AllPos.Append(X);
				for (const FIntVector3& T : G.Geom.Tris) { AllTris.Add(FIntVector3(T.X + Base, T.Y + Base, T.Z + Base)); }
			}
			Out.All[P].Build(AllPos, AllTris, false);
			BoneSegments(BodyComp.Mesh->GetRefSkeleton(), Poses[P], Out.Segments[P]);
		}
		return true;
	}

	/** Which body vertices may move: not excluded by box or dominant bone, inside only_boxes when given. */
	TArray<bool> PushCandidates(const FLodGeom& Geom, const FReferenceSkeleton& Ref, const TSharedPtr<FJsonObject>& Options, TArray<double>& OutLimit, FString& OutError)
	{
		TArray<FBox3d> Only, Exclude;
		ParseBoxes(OptArr(Options, TEXT("only_boxes")), Only, OutError);
		ParseBoxes(OptArr(Options, TEXT("exclude_boxes")), Exclude, OutError);
		TArray<FString> ExcludeBones;
		const TArray<TSharedPtr<FJsonValue>> BoneList = Options->HasField(TEXT("exclude_bones")) ? OptArr(Options, TEXT("exclude_bones"))
			: TArray<TSharedPtr<FJsonValue>>();
		for (const TSharedPtr<FJsonValue>& B : BoneList) { ExcludeBones.Add(B->AsString()); }
		if (!Options->HasField(TEXT("exclude_bones")))
		{
			// the face mesh covers head and neck; hands lie ON the cloth (a hand on the lap is not poke-through)
			ExcludeBones = { TEXT("head"), TEXT("neck_*"), TEXT("FACIAL_*"), TEXT("hand_*"), TEXT("thumb_*"), TEXT("index_*"), TEXT("middle_*"), TEXT("ring_*"), TEXT("pinky_*") };
		}
		const double MaxPush = OptNum(Options, TEXT("max_push"), 5.0);
		struct FLimit { FBox3d Box; double Max; };
		TArray<FLimit> Limits;
		for (const TSharedPtr<FJsonValue>& L : OptArr(Options, TEXT("limits")))
		{
			const TSharedPtr<FJsonObject> O = L->AsObject();
			TArray<FBox3d> Box;
			if (O.IsValid() && O->HasField(TEXT("box")) && ParseBoxes({ O->TryGetField(TEXT("box")) }, Box, OutError) && Box.Num() == 1)
			{
				Limits.Add({ Box[0], OptNum(O, TEXT("max_push"), MaxPush) });
			}
		}
		const int32 NV = Geom.BindPos.Num();
		TArray<bool> Cand;
		Cand.Init(true, NV);
		OutLimit.Init(MaxPush, NV);
		for (int32 V = 0; V < NV; ++V)
		{
			const FVector3d& P = Geom.BindPos[V];
			if (Only.Num() > 0 && !Only.ContainsByPredicate([&P](const FBox3d& B) { return B.IsInsideOrOn(P); })) { Cand[V] = false; continue; }
			if (Exclude.ContainsByPredicate([&P](const FBox3d& B) { return B.IsInsideOrOn(P); })) { Cand[V] = false; continue; }
			int32 Dominant = INDEX_NONE;
			float Best = 0.f;
			for (const TPair<int32, float>& W : Geom.Weights[V]) { if (W.Value > Best) { Best = W.Value; Dominant = W.Key; } }
			if (Dominant != INDEX_NONE && Dominant < Ref.GetNum())
			{
				const FString Bone = Ref.GetBoneName(Dominant).ToString();
				if (ExcludeBones.ContainsByPredicate([&Bone](const FString& Pattern) { return Bone.MatchesWildcard(Pattern); })) { Cand[V] = false; continue; }
			}
			for (const FLimit& L : Limits) { if (L.Box.IsInsideOrOn(P)) { OutLimit[V] = FMath::Min(OutLimit[V], L.Max); } }
		}
		return Cand;
	}

	struct FPokeResult
	{
		TArray<double> Depth;     // per vertex, worst over poses (> 0: the skin is outside the garment), -inf when not under it
		TArray<int32> WorstPose;  // per vertex
		TArray<double> PoseMax;   // per pose
		TArray<int32> PoseCount;  // per pose: vertices outside the garment
	};

	/**
	 * How far each candidate body vertex comes out through the garments, per pose. The garment's outward normal is
	 * taken away from the nearest bone (never from its winding); a vertex counts only when skin and cloth face the same
	 * way (an arm hanging beside a skirt faces it) and the nearest garment point lies along that normal (not a hem or
	 * neckline edge off to the side); skin more than max_depth outside is exposed skin, not poke-through.
	 */
	FPokeResult MeasurePoke(const FLodGeom& Geom, const USkeletalMesh* Source, const TArray<FVector3d>& Bind, const TArray<FPose>& Poses, const TArray<FSoup>& Soups,
		const TArray<TArray<TPair<FVector3d, FVector3d>>>& Segments, const TArray<bool>& Cand, const FTransform& ToLeader, double Reach, double MaxDepth, double FacingMin)
	{
		const int32 NV = Geom.BindPos.Num();
		FPokeResult R;
		R.Depth.Init(-TNumericLimits<double>::Max(), NV);
		R.WorstPose.Init(INDEX_NONE, NV);
		R.PoseMax.Init(-TNumericLimits<double>::Max(), Poses.Num());
		R.PoseCount.Init(0, Poses.Num());
		for (int32 P = 0; P < Poses.Num(); ++P)
		{
			TArray<FVector3d> X, Undented, N;
			SkinFollower(Geom, Source, Poses[P], ToLeader, X, nullptr, &Bind);
			SkinFollower(Geom, Source, Poses[P], ToLeader, Undented);   // the measuring normals: a dent tilts its own
			VertexNormals(Undented, Geom.Tris, N);
			for (int32 V = 0; V < NV; ++V)
			{
				if (!Cand[V] || N[V].IsNearlyZero())
				{
					continue;
				}
				FVector3d S, Ng; int32 Tri; double Dist;
				if (!Soups[P].Nearest(X[V], Reach, S, Ng, Tri, Dist) || Ng.IsNearlyZero())
				{
					continue;
				}
				if ((Ng | (S - NearestBonePoint(Segments[P], S))) < 0.0)
				{
					Ng = -Ng;
				}
				if ((N[V] | Ng) < FacingMin)
				{
					continue;
				}
				const FVector3d Off = X[V] - S;
				const double D = Off | Ng;
				if ((D > 0.05 && D < 0.85 * Off.Size()) || D > MaxDepth)
				{
					continue;
				}
				if (D > R.Depth[V]) { R.Depth[V] = D; R.WorstPose[V] = P; }
				R.PoseMax[P] = FMath::Max(R.PoseMax[P], D);
				R.PoseCount[P] += D > 0.0 ? 1 : 0;
			}
		}
		return R;
	}
}

FString UMetaHumanWardrobeService::PushBodyUnderGarments(const FString& BlueprintPath, const FString& GarmentComponentsJson, const FString& OutBodyMeshPath, const FString& OptionsJson, bool bAssignToBlueprint, bool bDryRun)
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
	USkeletalMesh* Source = VmhAssets::Load<USkeletalMesh>(OptStr(Options, TEXT("source_body"), BodyComp.Mesh->GetPathName()));
	if (!Source)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), TEXT("no source body (options.source_body: the uncut full body from make_full_body_from_dna)"));
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
	TArray<FPose> Poses;
	if (!ReadPoses(Options, BodyComp.Mesh, Poses, Error, Blueprint, TEXT("runtime")))
	{
		return VmhJson::Error(TEXT("BAD_POSE"), Error);
	}
	FPosedGarments Garments;
	if (!PoseGarments(Blueprint, BodyComp, GarmentComponentsJson, Poses, false, Garments, Error))
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), Error);
	}
	const double Margin = OptNum(Options, TEXT("margin"), 0.6);
	const double Reach = OptNum(Options, TEXT("reach"), 6.0);
	const double MaxDepth = OptNum(Options, TEXT("max_depth"), 2.0);
	const double FacingMin = OptNum(Options, TEXT("facing"), 0.3);
	const int32 Passes = FMath::Max(1, static_cast<int32>(OptNum(Options, TEXT("passes"), 4)));
	const int32 Rings = static_cast<int32>(OptNum(Options, TEXT("spread_rings"), 3));
	const double Falloff = OptNum(Options, TEXT("spread_falloff"), 0.7);

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
		TArray<double> Limit;
		const TArray<bool> Cand = PushCandidates(Geom, Source->GetRefSkeleton(), Options, Limit, Error);
		TArray<FVector3d> RestNormal;
		VertexNormals(Geom.BindPos, Geom.Tris, RestNormal);
		TArray<double> Push;
		Push.Init(0.0, NV);
		TArray<FVector3d> Bind = Geom.BindPos;
		TArray<TSharedPtr<FJsonValue>> PassReports;
		int32 PokingBefore = 0, PokingAfter = 0;
		double WorstBefore = 0.0, WorstAfter = 0.0;
		for (int32 Pass = 0; Pass <= Passes; ++Pass)
		{
			for (int32 V = 0; V < NV; ++V) { Bind[V] = Geom.BindPos[V] - RestNormal[V] * Push[V]; }
			const FPokeResult M = MeasurePoke(Geom, Source, Bind, Poses, Garments.All, Garments.Segments, Cand, FTransform::Identity, Reach, MaxDepth, FacingMin);
			int32 Poking = 0, Within = 0;
			double Worst = 0.0;
			TArray<double> Need;
			Need.Init(0.0, NV);
			for (int32 V = 0; V < NV; ++V)
			{
				if (M.Depth[V] > 0.0) { ++Poking; Worst = FMath::Max(Worst, M.Depth[V]); }
				if (M.Depth[V] > -Margin && Push[V] < Limit[V] - 1e-4) { ++Within; Need[V] = M.Depth[V] + Margin; }
			}
			TSharedPtr<FJsonObject> PR = VmhJson::Obj();
			PR->SetNumberField(TEXT("pass"), Pass);
			PR->SetNumberField(TEXT("outside_garment"), Poking);
			PR->SetNumberField(TEXT("within_margin"), Within);
			PR->SetNumberField(TEXT("worst_cm"), Worst);
			PassReports.Add(MakeShared<FJsonValueObject>(PR));
			if (Pass == 0) { PokingBefore = Poking; WorstBefore = Worst; }
			PokingAfter = Poking;
			WorstAfter = Worst;
			if (Within == 0 || Pass == Passes || bDryRun)
			{
				break;
			}
			// spread the push over a few rings of candidates so it leaves no crease; never less than a vertex needs
			TArray<double> Add = Need;
			for (int32 Ring = 0; Ring < Rings; ++Ring)
			{
				TArray<double> Next = Add;
				for (int32 V = 0; V < NV; ++V)
				{
					if (!Cand[V]) { continue; }
					for (const int32 Nb : Geom.Neighbors[V]) { Next[V] = FMath::Max(Next[V], Add[Nb] * Falloff); }
				}
				Add = MoveTemp(Next);
			}
			for (int32 V = 0; V < NV; ++V)
			{
				if (Cand[V]) { Push[V] = FMath::Min(Push[V] + Add[V], Limit[V]); }
			}
		}
		int32 Moved = 0;
		double MaxMove = 0.0;
		for (int32 V = 0; V < NV; ++V)
		{
			if (Push[V] > 1e-4) { ++Moved; MaxMove = FMath::Max(MaxMove, Push[V]); }
		}
		int32 NCand = 0;
		for (const bool B : Cand) { NCand += B ? 1 : 0; }
		R->SetNumberField(TEXT("vertices"), NV);
		R->SetNumberField(TEXT("candidates"), NCand);
		R->SetNumberField(TEXT("outside_before"), PokingBefore);
		R->SetNumberField(TEXT("worst_before_cm"), WorstBefore);
		R->SetNumberField(TEXT("outside_after"), bDryRun ? PokingBefore : PokingAfter);
		R->SetNumberField(TEXT("worst_after_cm"), bDryRun ? WorstBefore : WorstAfter);
		R->SetNumberField(TEXT("vertices_moved"), Moved);
		R->SetNumberField(TEXT("max_push_cm"), MaxMove);
		R->SetArrayField(TEXT("passes"), PassReports);
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
	Out->SetStringField(TEXT("source_body"), Source->GetPathName());
	Out->SetArrayField(TEXT("garments"), VmhJson::StringArray(Garments.Names));
	Out->SetArrayField(TEXT("poses"), PoseLabels(Poses));
	Out->SetBoolField(TEXT("dry_run"), bDryRun);
	Out->SetArrayField(TEXT("lods"), LodReports);
	if (!bDryRun && Edited.Num() > 0)
	{
		USkeletalMesh* Fitted = DuplicateMesh(Source, OutBodyMeshPath, Error);
		if (!Fitted || !CommitLods(Fitted, Edited, Error))
		{
			return VmhJson::Error(TEXT("COMMIT_FAILED"), Error, Out);
		}
		Out->SetStringField(TEXT("fitted_body"), Fitted->GetPathName());
		if (bAssignToBlueprint)
		{
			AssignMesh(Blueprint, BodyComp.Template, Fitted);
			Out->SetBoolField(TEXT("assigned"), true);
		}
	}
	Out->SetStringField(TEXT("hint"), TEXT("outside_after should be 0 over every pose the game shows; then look at it through capture_fit_views (seated and standing, a low camera for skirts). Save the fitted body (and the Blueprint when assigned)"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanWardrobeService::MeasurePokeThrough(const FString& BlueprintPath, const FString& GarmentComponentsJson, const FString& OptionsJson)
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
	USkeletalMesh* Body = VmhAssets::Load<USkeletalMesh>(OptStr(Options, TEXT("body_mesh"), BodyComp.Mesh->GetPathName()));
	if (!Body)
	{
		return VmhJson::Error(TEXT("MESH_NOT_FOUND"), TEXT("no body mesh"));
	}
	TArray<FPose> Poses;
	if (!ReadPoses(Options, BodyComp.Mesh, Poses, Error, Blueprint, TEXT("runtime")))
	{
		return VmhJson::Error(TEXT("BAD_POSE"), Error);
	}
	FPosedGarments Garments;
	if (!PoseGarments(Blueprint, BodyComp, GarmentComponentsJson, Poses, true, Garments, Error))
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), Error);
	}
	const double Reach = OptNum(Options, TEXT("reach"), 6.0);
	const double MaxDepth = OptNum(Options, TEXT("max_depth"), 2.0);
	const double FacingMin = OptNum(Options, TEXT("facing"), 0.3);
	TArray<TSharedPtr<FJsonValue>> LodReports;
	TArray<int32> Lods;
	for (const TSharedPtr<FJsonValue>& V : OptArr(Options, TEXT("lods"))) { Lods.AddUnique(static_cast<int32>(V->AsNumber())); }
	if (Lods.Num() == 0) { Lods.Add(0); }
	for (const int32 Lod : Lods)
	{
		TSharedPtr<FJsonObject> R = VmhJson::Obj();
		R->SetNumberField(TEXT("lod"), Lod);
		FMeshDescription Desc;
		FLodGeom Geom;
		if (!ReadLod(Body, Lod, Desc, Geom, Error))
		{
			R->SetStringField(TEXT("skipped"), Error);
			LodReports.Add(MakeShared<FJsonValueObject>(R));
			continue;
		}
		TArray<double> Limit;
		const TArray<bool> Cand = PushCandidates(Geom, Body->GetRefSkeleton(), Options, Limit, Error);
		TArray<TSharedPtr<FJsonValue>> PerGarment;
		for (int32 G = 0; G < Garments.Names.Num(); ++G)
		{
			TArray<FSoup> Soups;
			for (int32 P = 0; P < Poses.Num(); ++P) { Soups.Add(MoveTemp(Garments.Each[P][G])); }
			const FPokeResult M = MeasurePoke(Geom, Body, Geom.BindPos, Poses, Soups, Garments.Segments, Cand, FTransform::Identity, Reach, MaxDepth, FacingMin);
			for (int32 P = 0; P < Poses.Num(); ++P) { Garments.Each[P][G] = MoveTemp(Soups[P]); }
			TSharedPtr<FJsonObject> GR = VmhJson::Obj();
			GR->SetStringField(TEXT("garment"), Garments.Names[G]);
			double Worst = 0.0;
			int32 WorstVertex = INDEX_NONE, Outside = 0;
			for (int32 V = 0; V < M.Depth.Num(); ++V)
			{
				if (M.Depth[V] > 0.0) { ++Outside; }
				if (M.Depth[V] > Worst) { Worst = M.Depth[V]; WorstVertex = V; }
			}
			GR->SetNumberField(TEXT("vertices_outside"), Outside);
			GR->SetNumberField(TEXT("worst_cm"), Worst);
			TMap<FString, int32> Where;
			for (int32 V = 0; V < M.Depth.Num(); ++V)
			{
				if (M.Depth[V] <= 0.0) { continue; }
				int32 Dominant = INDEX_NONE; float Best = 0.f;
				for (const TPair<int32, float>& W : Geom.Weights[V]) { if (W.Value > Best) { Best = W.Value; Dominant = W.Key; } }
				if (Dominant != INDEX_NONE) { Where.FindOrAdd(Body->GetRefSkeleton().GetBoneName(Dominant).ToString())++; }
			}
			GR->SetArrayField(TEXT("outside_by_bone"), TopCounts(Where, 8));
			if (WorstVertex != INDEX_NONE)
			{
				GR->SetStringField(TEXT("worst_at_bind_cm"), Geom.BindPos[WorstVertex].ToString());
				GR->SetStringField(TEXT("worst_pose"), Poses[M.WorstPose[WorstVertex]].Label);
			}
			TArray<TSharedPtr<FJsonValue>> PerPose;
			for (int32 P = 0; P < Poses.Num(); ++P)
			{
				TSharedPtr<FJsonObject> PP = VmhJson::Obj();
				PP->SetStringField(TEXT("pose"), Poses[P].Label);
				PP->SetNumberField(TEXT("vertices_outside"), M.PoseCount[P]);
				PP->SetNumberField(TEXT("worst_cm"), FMath::Max(0.0, M.PoseMax[P]));
				PerPose.Add(MakeShared<FJsonValueObject>(PP));
			}
			GR->SetArrayField(TEXT("poses"), PerPose);
			PerGarment.Add(MakeShared<FJsonValueObject>(GR));
		}
		R->SetArrayField(TEXT("garments"), PerGarment);
		LodReports.Add(MakeShared<FJsonValueObject>(R));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("body"), Body->GetPathName());
	Out->SetArrayField(TEXT("poses"), PoseLabels(Poses));
	Out->SetArrayField(TEXT("lods"), LodReports);
	Out->SetStringField(TEXT("hint"), TEXT("worst_cm > 0: skin comes out through that garment in that pose; push_body_under_garments fixes it on a body copy (or hide_body_under_garments for a closed garment)"));
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// capture_fit_views / dump_runtime_poses / dump_actor_pose
// =====================================================================================================================

namespace
{
	struct FView { FString Name; double Yaw = 90.0; double Pitch = 0.0; };

	bool ParseViews(const FString& ViewsJson, TArray<FView>& Out, FString& OutError)
	{
		TArray<TSharedPtr<FJsonValue>> List;
		const FString Json = ViewsJson.IsEmpty() ? TEXT("[\"front\",\"three_quarter\",\"side\",\"back\"]") : ViewsJson;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Json), List))
		{
			OutError = TEXT("views: [\"front\",\"three_quarter\",\"side\",\"back\",\"left\",\"right\",\"low_front\"] or {\"name\":..,\"yaw\":deg,\"pitch\":deg}");
			return false;
		}
		// MetaHumans face +Y: yaw 90 looks at the face, 270 at the back; pitch < 0 puts the camera below
		auto Named = [](const FString& Name, FView& View) -> bool
		{
			View.Name = Name;
			if (Name == TEXT("front")) { View.Yaw = 90.0; }
			else if (Name == TEXT("back")) { View.Yaw = 270.0; }
			else if (Name == TEXT("side") || Name == TEXT("left")) { View.Yaw = 0.0; }
			else if (Name == TEXT("right")) { View.Yaw = 180.0; }
			else if (Name == TEXT("three_quarter") || Name == TEXT("q34")) { View.Yaw = 45.0; }
			else if (Name == TEXT("low_front")) { View.Yaw = 90.0; View.Pitch = -25.0; }
			else if (Name == TEXT("high_front")) { View.Yaw = 90.0; View.Pitch = 25.0; }
			else { return false; }
			return true;
		};
		for (const TSharedPtr<FJsonValue>& V : List)
		{
			FView View;
			if (V->Type == EJson::Object)
			{
				const TSharedPtr<FJsonObject> O = V->AsObject();
				View.Name = OptStr(O, TEXT("name"), FString::Printf(TEXT("view%d"), Out.Num()));
				View.Yaw = OptNum(O, TEXT("yaw"), 90.0);
				View.Pitch = OptNum(O, TEXT("pitch"), 0.0);
			}
			else if (!Named(V->AsString().ToLower(), View))
			{
				OutError = FString::Printf(TEXT("unknown view %s"), *V->AsString());
				return false;
			}
			Out.Add(View);
		}
		return Out.Num() > 0;
	}

	TArray<FName> FocusBones(const FString& Focus)
	{
		if (Focus == TEXT("head")) { return { TEXT("head"), TEXT("neck_02"), TEXT("FACIAL_C_FacialRoot") }; }
		if (Focus == TEXT("chest")) { return { TEXT("spine_03"), TEXT("spine_05"), TEXT("neck_01"), TEXT("clavicle_l"), TEXT("clavicle_r"), TEXT("upperarm_l"), TEXT("upperarm_r") }; }
		if (Focus == TEXT("legs")) { return { TEXT("pelvis"), TEXT("thigh_l"), TEXT("thigh_r"), TEXT("calf_l"), TEXT("calf_r"), TEXT("foot_l"), TEXT("foot_r") }; }
		if (Focus == TEXT("hands")) { return { TEXT("hand_l"), TEXT("hand_r"), TEXT("lowerarm_l"), TEXT("lowerarm_r") }; }
		return {};
	}
}

FString UMetaHumanWardrobeService::CaptureFitViews(const FString& BlueprintPath, const FString& AnimPath, float Time, const FString& ViewsJson, const FString& OutDir, const FString& OptionsJson)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint at %s"), *BlueprintPath));
	}
	FString Error;
	const TSharedPtr<FJsonObject> Options = VmhJson::Parse(OptionsJson, Error);
	if (!Options.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_OPTIONS"), Error);
	}
	TArray<FView> Views;
	if (!ParseViews(ViewsJson, Views, Error))
	{
		return VmhJson::Error(TEXT("BAD_VIEWS"), Error);
	}
	UAnimSequence* Anim = nullptr;
	if (!AnimPath.IsEmpty())
	{
		Anim = VmhAssets::Load<UAnimSequence>(AnimPath);
		if (!Anim)
		{
			return VmhJson::Error(TEXT("ANIM_NOT_FOUND"), FString::Printf(TEXT("No AnimSequence at %s"), *AnimPath));
		}
	}
	TArray<double> Times;
	for (const TSharedPtr<FJsonValue>& T : OptArr(Options, TEXT("times"))) { Times.Add(T->AsNumber()); }
	if (Times.Num() == 0) { Times.Add(Time); }

	VmhPreview::FCharacter Character;
	Character.BodyComponentName = OptStr(Options, TEXT("body_component"), TEXT("Body"));
	if (!Character.Spawn(Blueprint, Error))
	{
		return VmhJson::Error(TEXT("SPAWN_FAILED"), Error);
	}
	TArray<FString> Hidden;
	for (const TSharedPtr<FJsonValue>& H : OptArr(Options, TEXT("hide_components"))) { Hidden.Add(H->AsString()); }
	if (Hidden.Num() > 0)
	{
		TArray<UPrimitiveComponent*> Prims;
		Character.Actor()->GetComponents(Prims);
		for (UPrimitiveComponent* P : Prims) { if (Hidden.Contains(P->GetName())) { P->SetVisibility(false); } }
	}
	const int32 Lod = static_cast<int32>(OptNum(Options, TEXT("lod"), 0));
	const int32 Width = FMath::Clamp(static_cast<int32>(OptNum(Options, TEXT("width"), 768)), 64, 4096);
	const int32 Height = FMath::Clamp(static_cast<int32>(OptNum(Options, TEXT("height"), 1024)), 64, 4096);
	const float FOV = static_cast<float>(OptNum(Options, TEXT("fov"), 30.0));
	const float Exposure = static_cast<float>(OptNum(Options, TEXT("exposure_bias"), 1.5));
	const FString Focus = OptStr(Options, TEXT("focus"), TEXT("body")).ToLower();
	const FString Dir = OutDir.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("VibeMetaHumans") / TEXT("FitViews") : OutDir;
	FString Package, BpName;
	VmhAssets::SplitAssetPath(Blueprint->GetPathName(), Package, BpName);

	TArray<TSharedPtr<FJsonValue>> Files;
	for (const double T : Times)
	{
		if (!Character.Pose(Anim, T, Error))
		{
			return VmhJson::Error(TEXT("POSE_FAILED"), Error);
		}
		Character.ForceLod(Lod);
		FBox Box = Focus == TEXT("body") ? Character.Bounds() : Character.BoneBox(FocusBones(Focus), Focus == TEXT("head") ? 14.0 : 18.0);
		if (!Box.IsValid)
		{
			Box = Character.Bounds();
		}
		const FVector Center = Box.GetCenter();
		const double Radius = FMath::Max(10.0, Box.GetExtent().Size());
		const double HalfFovV = FMath::Atan(FMath::Tan(FMath::DegreesToRadians(FOV * 0.5)) * Height / Width);
		const double Distance = Radius / FMath::Sin(FMath::Min(HalfFovV, FMath::DegreesToRadians(FOV * 0.5))) * 0.92;
		for (const FView& View : Views)
		{
			const FVector Direction = FRotator(View.Pitch, View.Yaw, 0.0).Vector();
			const FVector Eye = Center + Direction * Distance;
			TArray<FColor> Pixels;
			if (!Character.Capture(Eye, Center, FOV, Width, Height, Exposure, Pixels, Error))
			{
				return VmhJson::Error(TEXT("CAPTURE_FAILED"), Error);
			}
			const FString Label = Anim ? FString::Printf(TEXT("%s_%05.2f"), *Anim->GetName(), T) : TEXT("rest");
			const FString File = Dir / FString::Printf(TEXT("%s_%s_LOD%d_%s.png"), *BpName, *Label, Lod, *View.Name);
			if (!VmhPreview::SavePng(File, Width, Height, Pixels, Error))
			{
				return VmhJson::Error(TEXT("WRITE_FAILED"), Error);
			}
			int64 Lit = 0;
			for (const FColor& C : Pixels) { Lit += (C.R + C.G + C.B) > 60 ? 1 : 0; }
			TSharedPtr<FJsonObject> F = VmhJson::Obj();
			F->SetStringField(TEXT("file"), File);
			F->SetStringField(TEXT("view"), View.Name);
			F->SetNumberField(TEXT("time"), T);
			F->SetNumberField(TEXT("non_dark_fraction"), static_cast<double>(Lit) / Pixels.Num());
			Files.Add(MakeShared<FJsonValueObject>(F));
		}
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	Out->SetStringField(TEXT("anim"), Anim ? Anim->GetPathName() : TEXT(""));
	Out->SetNumberField(TEXT("lod"), Lod);
	Out->SetStringField(TEXT("focus"), Focus);
	Out->SetArrayField(TEXT("files"), Files);
	Out->SetStringField(TEXT("hint"), TEXT("the Blueprint is posed in a private preview world with every anim graph evaluated (post-process correctives included), as in PIE; grooms may not draw in a scene capture: judge hair in PIE. Look at every file"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanWardrobeService::DumpRuntimePoses(const FString& BlueprintPath, const FString& PosesJson, const FString& OutFilePath)
{
	UBlueprint* Blueprint = VmhAssets::Load<UBlueprint>(BlueprintPath);
	if (!Blueprint || !Blueprint->SimpleConstructionScript)
	{
		return VmhJson::Error(TEXT("BLUEPRINT_NOT_FOUND"), FString::Printf(TEXT("No Blueprint with a construction script at %s"), *BlueprintPath));
	}
	FString Error;
	FComp BodyComp;
	if (!FindComp(Blueprint, TEXT("Body"), BodyComp, Error))
	{
		return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), Error);
	}
	TSharedPtr<FJsonObject> Options = VmhJson::Obj();
	TArray<TSharedPtr<FJsonValue>> List;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(PosesJson), List))
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("PosesJson: [{\"anim\": path, \"time\": s} | {\"anim\", \"times\": [...]} | {\"anim\", \"stride\": s}]"));
	}
	Options->SetArrayField(TEXT("poses"), List);
	TArray<FPose> Runtime, Asset;
	Options->SetStringField(TEXT("evaluate"), TEXT("runtime"));
	if (!ReadPoses(Options, BodyComp.Mesh, Runtime, Error, Blueprint))
	{
		return VmhJson::Error(TEXT("BAD_POSE"), Error);
	}
	Options->SetStringField(TEXT("evaluate"), TEXT("asset"));
	ReadPoses(Options, BodyComp.Mesh, Asset, Error, Blueprint);
	TArray<TSharedPtr<FJsonValue>> Poses, Diffs;
	for (int32 P = 1; P < Runtime.Num(); ++P) // [0] is the rest pose
	{
		Poses.Add(MakeShared<FJsonValueObject>(VmhPreview::PoseToJson(Runtime[P])));
		if (Asset.IsValidIndex(P))
		{
			double Max = 0.0;
			FName Worst;
			for (const TPair<FName, FTransform>& B : Runtime[P].BoneCS)
			{
				if (const FTransform* A = Asset[P].BoneCS.Find(B.Key))
				{
					const double D = FVector::Dist(A->GetLocation(), B.Value.GetLocation());
					if (D > Max) { Max = D; Worst = B.Key; }
				}
			}
			TSharedPtr<FJsonObject> D = VmhJson::Obj();
			D->SetStringField(TEXT("pose"), Runtime[P].Label);
			D->SetNumberField(TEXT("runtime_vs_asset_max_cm"), Max);
			D->SetStringField(TEXT("bone"), Worst.ToString());
			Diffs.Add(MakeShared<FJsonValueObject>(D));
		}
	}
	TSharedPtr<FJsonObject> File = VmhJson::Obj();
	File->SetStringField(TEXT("blueprint"), Blueprint->GetPathName());
	File->SetStringField(TEXT("body_mesh"), BodyComp.Mesh->GetPathName());
	File->SetStringField(TEXT("space"), TEXT("Body component space, cm"));
	File->SetArrayField(TEXT("poses"), Poses);
	const FString Path = OutFilePath.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("VibeMetaHumans") / TEXT("Poses") / (Blueprint->GetName() + TEXT("_poses.json")) : OutFilePath;
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveStringToFile(VmhJson::Stringify(File), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return VmhJson::Error(TEXT("WRITE_FAILED"), FString::Printf(TEXT("could not write %s"), *Path));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("file"), Path);
	Out->SetNumberField(TEXT("poses"), Poses.Num());
	Out->SetArrayField(TEXT("runtime_vs_asset"), Diffs);
	Out->SetStringField(TEXT("hint"), TEXT("pass {\"poses\": [{\"file\": <file>}]} to push_body_under_garments, measure_poke_through, hide_body_under_garments or push_garment_out_of_mesh"));
	return VmhJson::Ok(Out);
}

FString UMetaHumanWardrobeService::DumpActorPose(const FString& ActorLabel, const FString& ComponentName, const FString& OutFilePath, const FString& Label, bool bAppend)
{
	// PIE first: the pose the game really shows
	UWorld* World = (GEditor && GEditor->PlayWorld) ? GEditor->PlayWorld.Get() : VmhAssets::EditorWorld();
	AActor* Actor = nullptr;
	for (TActorIterator<AActor> It(World); It && !Actor; ++It)
	{
		if (It->GetActorLabel() == ActorLabel || It->GetName() == ActorLabel) { Actor = *It; }
	}
	if (!Actor)
	{
		return VmhJson::Error(TEXT("ACTOR_NOT_FOUND"), FString::Printf(TEXT("no actor %s in %s"), *ActorLabel, World ? *World->GetName() : TEXT("(no world)")));
	}
	USkeletalMeshComponent* Comp = nullptr;
	TArray<USkeletalMeshComponent*> Comps;
	Actor->GetComponents(Comps);
	for (USkeletalMeshComponent* C : Comps) { if (C->GetName() == (ComponentName.IsEmpty() ? TEXT("Body") : ComponentName)) { Comp = C; } }
	if (!Comp || !Comp->GetSkeletalMeshAsset())
	{
		return VmhJson::Error(TEXT("COMPONENT_NOT_FOUND"), FString::Printf(TEXT("%s has no skeletal mesh component %s"), *ActorLabel, *ComponentName));
	}
	FPose Pose;
	Pose.Label = Label.IsEmpty() ? FString::Printf(TEXT("%s %.2fs"), *ActorLabel, World->GetTimeSeconds()) : Label;
	const FReferenceSkeleton& Ref = Comp->GetSkeletalMeshAsset()->GetRefSkeleton();
	const TArray<FTransform>& CS = Comp->GetComponentSpaceTransforms();
	for (int32 B = 0; B < Ref.GetNum() && B < CS.Num(); ++B) { Pose.BoneCS.Add(Ref.GetBoneName(B), CS[B]); }
	const FString Path = OutFilePath.IsEmpty() ? FPaths::ProjectSavedDir() / TEXT("VibeMetaHumans") / TEXT("Poses") / (ActorLabel + TEXT("_pie.json")) : OutFilePath;
	TSharedPtr<FJsonObject> File;
	FString Existing, ParseError;
	if (bAppend && FFileHelper::LoadFileToString(Existing, *Path))
	{
		File = VmhJson::Parse(Existing, ParseError);
	}
	if (!File.IsValid())
	{
		File = VmhJson::Obj();
		File->SetStringField(TEXT("space"), TEXT("Body component space, cm"));
		File->SetStringField(TEXT("body_mesh"), Comp->GetSkeletalMeshAsset()->GetPathName());
	}
	TArray<TSharedPtr<FJsonValue>> Poses = OptArr(File, TEXT("poses"));
	Poses.Add(MakeShared<FJsonValueObject>(VmhPreview::PoseToJson(Pose)));
	File->SetArrayField(TEXT("poses"), Poses);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(Path), true);
	if (!FFileHelper::SaveStringToFile(VmhJson::Stringify(File), *Path, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM))
	{
		return VmhJson::Error(TEXT("WRITE_FAILED"), FString::Printf(TEXT("could not write %s"), *Path));
	}
	TSharedPtr<FJsonObject> Out = VmhJson::Obj();
	Out->SetStringField(TEXT("file"), Path);
	Out->SetStringField(TEXT("world"), World->GetName());
	Out->SetBoolField(TEXT("pie"), GEditor && GEditor->PlayWorld != nullptr);
	Out->SetNumberField(TEXT("poses_in_file"), Poses.Num());
	return VmhJson::Ok(Out);
}

// =====================================================================================================================
// GeometryScript-backed helpers (Content/Python/vmh_wardrobe.py): add_skinned_accessory, make_garment_variant,
// transfer_skin_weights
// =====================================================================================================================

namespace
{
	/** Run vmh_wardrobe.<Function>(**Kwargs) in the editor's Python and return its JSON (files in, files out: no quoting). */
	FString RunWardrobePython(const FString& Function, const TSharedPtr<FJsonObject>& Kwargs)
	{
		IPythonScriptPlugin* Python = IPythonScriptPlugin::Get();
		if (!Python || !Python->IsPythonAvailable())
		{
			return VmhJson::Error(TEXT("PYTHON_UNAVAILABLE"), TEXT("the Python Editor Script Plugin is not available"));
		}
		const FString Dir = FPaths::ConvertRelativePathToFull(FPaths::ProjectSavedDir() / TEXT("VibeMetaHumans") / TEXT("Py"));
		IFileManager::Get().MakeDirectory(*Dir, true);
		const FString Id = FGuid::NewGuid().ToString(EGuidFormats::Digits);
		const FString In = Dir / (Id + TEXT("_in.json"));
		const FString Out = Dir / (Id + TEXT("_out.json"));
		FFileHelper::SaveStringToFile(VmhJson::Stringify(Kwargs), *In, FFileHelper::EEncodingOptions::ForceUTF8WithoutBOM);
		FPythonCommandEx Command;
		Command.ExecutionMode = EPythonCommandExecutionMode::ExecuteFile;
		Command.FileExecutionScope = EPythonFileExecutionScope::Private;
		Command.Command = FString::Printf(TEXT("import vmh_wardrobe\nwith open(r'%s', encoding='utf-8') as _i:\n    _r = vmh_wardrobe.run_json('%s', _i.read())\nwith open(r'%s', 'w', encoding='utf-8') as _o:\n    _o.write(_r)\n"), *In, *Function, *Out);
		const bool bRan = Python->ExecPythonCommandEx(Command);
		FString Result;
		const bool bRead = FFileHelper::LoadFileToString(Result, *Out);
		IFileManager::Get().Delete(*In);
		IFileManager::Get().Delete(*Out);
		if (!bRan || !bRead)
		{
			return VmhJson::Error(TEXT("PYTHON_FAILED"), FString::Printf(TEXT("vmh_wardrobe.%s did not run: %s"), *Function, *Command.CommandResult.Left(1500)));
		}
		return Result;
	}
}

FString UMetaHumanWardrobeService::AddSkinnedAccessory(const FString& BlueprintPath, const FString& StaticMeshPath, const FString& ComponentName, const FString& Mode, const FString& SurfaceComponentsJson, const FString& Bone, const FString& RelativeJson, const FString& OutMeshPath)
{
	FString Error;
	TArray<TSharedPtr<FJsonValue>> Surfaces;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(SurfaceComponentsJson.IsEmpty() ? TEXT("[\"Body\"]") : SurfaceComponentsJson), Surfaces))
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), TEXT("SurfaceComponentsJson is a JSON array of component names"));
	}
	const TSharedPtr<FJsonObject> Relative = VmhJson::Parse(RelativeJson, Error);
	if (!Relative.IsValid())
	{
		return VmhJson::Error(TEXT("BAD_ARGS"), Error);
	}
	TSharedPtr<FJsonObject> K = VmhJson::Obj();
	K->SetStringField(TEXT("blueprint"), BlueprintPath);
	K->SetStringField(TEXT("static_mesh"), StaticMeshPath);
	K->SetStringField(TEXT("component"), ComponentName);
	K->SetStringField(TEXT("mode"), Mode.IsEmpty() ? TEXT("surface") : Mode.ToLower());
	K->SetArrayField(TEXT("surface_components"), Surfaces);
	K->SetStringField(TEXT("bone"), Bone.IsEmpty() ? TEXT("spine_05") : Bone);
	K->SetObjectField(TEXT("relative"), Relative);
	K->SetStringField(TEXT("out_path"), OutMeshPath);
	return RunWardrobePython(TEXT("add_skinned_accessory"), K);
}

FString UMetaHumanWardrobeService::MakeGarmentVariant(const FString& SourceMeshPath, const FString& DestFolder, const FString& Name, const FString& TextureVariant, int32 MaxTextureSize)
{
	TSharedPtr<FJsonObject> K = VmhJson::Obj();
	K->SetStringField(TEXT("source_mesh"), SourceMeshPath);
	K->SetStringField(TEXT("dst_folder"), DestFolder);
	K->SetStringField(TEXT("name"), Name);
	K->SetStringField(TEXT("texture_variant"), TextureVariant);
	K->SetNumberField(TEXT("max_texture_size"), MaxTextureSize);
	return RunWardrobePython(TEXT("make_garment_variant"), K);
}

FString UMetaHumanWardrobeService::TransferSkinWeights(const FString& TargetMeshPath, const FString& SourceMeshPath, const FString& OutMeshPath, const FString& Method)
{
	TSharedPtr<FJsonObject> K = VmhJson::Obj();
	K->SetStringField(TEXT("target_mesh"), TargetMeshPath);
	K->SetStringField(TEXT("source_mesh"), SourceMeshPath);
	K->SetStringField(TEXT("out_path"), OutMeshPath);
	K->SetStringField(TEXT("method"), Method.IsEmpty() ? TEXT("closest") : Method.ToLower());
	return RunWardrobePython(TEXT("transfer_skin_weights"), K);
}
