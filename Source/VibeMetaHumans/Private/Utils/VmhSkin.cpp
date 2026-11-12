// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#include "Utils/VmhSkin.h"

#include "Animation/AnimData/IAnimationDataModel.h"
#include "Animation/AnimSequence.h"
#include "Animation/Skeleton.h"
#include "BoneIndices.h"
#include "BoneWeights.h"
#include "Distance/DistPoint3Triangle3.h"
#include "Dom/JsonObject.h"
#include "Dom/JsonValue.h"
#include "DynamicMesh/DynamicMesh3.h"
#include "DynamicMesh/DynamicMeshAABBTree3.h"
#include "Engine/SkeletalMesh.h"
#include "MeshQueries.h"
#include "RenderingThread.h"
#include "SkeletalMeshAttributes.h"
#include "Spatial/FastWinding.h"

namespace VmhSkin
{
	using namespace UE::Geometry;

	bool ReadLod(const USkeletalMesh* Mesh, int32 Lod, FMeshDescription& OutDesc, FLodGeom& G, FString& OutError)
	{
		if (!Mesh)
		{
			OutError = TEXT("no mesh");
			return false;
		}
		if (Lod < 0 || Lod >= Mesh->GetLODNum())
		{
			OutError = FString::Printf(TEXT("%s has %d LODs; LOD %d does not exist"), *Mesh->GetPathName(), Mesh->GetLODNum(), Lod);
			return false;
		}
		if (!Mesh->HasMeshDescription(Lod) || !Mesh->CloneMeshDescription(Lod, OutDesc))
		{
			OutError = FString::Printf(TEXT("%s LOD %d has no mesh description (an auto-generated LOD)"), *Mesh->GetPathName(), Lod);
			return false;
		}
		FSkeletalMeshAttributes Attr(OutDesc);
		TVertexAttributesRef<FVector3f> Positions = Attr.GetVertexPositions();
		FSkinWeightsVertexAttributesRef SkinWeights = Attr.GetVertexSkinWeights();
		if (!SkinWeights.IsValid())
		{
			OutError = FString::Printf(TEXT("%s LOD %d has no skin weights"), *Mesh->GetPathName(), Lod);
			return false;
		}

		G = FLodGeom();
		G.Lod = Lod;
		const int32 NumVerts = OutDesc.Vertices().Num();
		G.VertexIds.Reserve(NumVerts);
		G.BindPos.Reserve(NumVerts);
		G.Weights.Reserve(NumVerts);
		for (const FVertexID VertexId : OutDesc.Vertices().GetElementIDs())
		{
			const int32 Dense = G.VertexIds.Add(VertexId);
			G.DenseOf.Add(VertexId.GetValue(), Dense);
			G.BindPos.Add(FVector3d(Positions.Get(VertexId)));
			TArray<TPair<int32, float>>& W = G.Weights.AddDefaulted_GetRef();
			for (const UE::AnimationCore::FBoneWeight BoneWeight : SkinWeights.Get(VertexId))
			{
				W.Emplace(BoneWeight.GetBoneIndex(), BoneWeight.GetWeight());
			}
		}
		G.Neighbors.SetNum(G.VertexIds.Num());
		G.Tris.Reserve(OutDesc.Triangles().Num());
		G.TriIds.Reserve(OutDesc.Triangles().Num());
		for (const FTriangleID TriId : OutDesc.Triangles().GetElementIDs())
		{
			const TArrayView<const FVertexID> V = OutDesc.GetTriangleVertices(TriId);
			const int32 A = G.DenseOf.FindChecked(V[0].GetValue());
			const int32 B = G.DenseOf.FindChecked(V[1].GetValue());
			const int32 C = G.DenseOf.FindChecked(V[2].GetValue());
			G.Tris.Add(FIntVector3(A, B, C));
			G.TriIds.Add(TriId);
			G.Neighbors[A].AddUnique(B); G.Neighbors[A].AddUnique(C);
			G.Neighbors[B].AddUnique(A); G.Neighbors[B].AddUnique(C);
			G.Neighbors[C].AddUnique(A); G.Neighbors[C].AddUnique(B);
		}
		return true;
	}

	int32 NumLods(const USkeletalMesh* Mesh)
	{
		int32 Count = 0;
		for (int32 Lod = 0; Mesh && Lod < Mesh->GetLODNum(); ++Lod)
		{
			Count += Mesh->HasMeshDescription(Lod) ? 1 : 0;
		}
		return Count;
	}

	void RefPoseCS(const FReferenceSkeleton& Ref, TArray<FTransform>& OutCS)
	{
		const TArray<FTransform>& Local = Ref.GetRefBonePose();
		OutCS.SetNum(Local.Num());
		for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
		{
			const int32 Parent = Ref.GetParentIndex(Bone);
			OutCS[Bone] = Parent == INDEX_NONE ? Local[Bone] : Local[Bone] * OutCS[Parent];
		}
	}

	FPose RefPose(const FReferenceSkeleton& LeaderRef)
	{
		FPose Pose;
		Pose.Label = TEXT("rest");
		TArray<FTransform> CS;
		RefPoseCS(LeaderRef, CS);
		for (int32 Bone = 0; Bone < CS.Num(); ++Bone)
		{
			Pose.BoneCS.Add(LeaderRef.GetBoneName(Bone), CS[Bone]);
		}
		return Pose;
	}

	bool AnimPose(const FReferenceSkeleton& LeaderRef, const UAnimSequence* Anim, double Time, FPose& OutPose, FString& OutError)
	{
		if (!Anim || !Anim->GetSkeleton())
		{
			OutError = TEXT("the animation has no skeleton");
			return false;
		}
		const USkeleton* Skeleton = Anim->GetSkeleton();
		const FReferenceSkeleton& SkelRef = Skeleton->GetReferenceSkeleton();
		const TArray<FTransform>& SkelLocal = SkelRef.GetRefBonePose();
		const TArray<FTransform>& MeshLocal = LeaderRef.GetRefBonePose();
		const IAnimationDataModel* Model = Anim->GetDataModel();
		const FAnimExtractContext Context(Time, false);

		TArray<FTransform> CS;
		CS.SetNum(MeshLocal.Num());
		int32 Animated = 0;
		for (int32 Bone = 0; Bone < MeshLocal.Num(); ++Bone)
		{
			const FName Name = LeaderRef.GetBoneName(Bone);
			FTransform Local = MeshLocal[Bone];
			const int32 SkelBone = SkelRef.FindBoneIndex(Name);
			if (SkelBone != INDEX_NONE && Model && Model->IsValidBoneTrackName(Name))
			{
				FTransform AnimLocal;
				Anim->GetBoneTransform(AnimLocal, FSkeletonPoseBoneIndex(SkelBone), Context, true);
				FVector Translation = AnimLocal.GetTranslation();
				switch (Skeleton->GetBoneTranslationRetargetingMode(SkelBone))
				{
				case EBoneTranslationRetargetingMode::Skeleton:
					Translation = MeshLocal[Bone].GetTranslation();
					break;
				case EBoneTranslationRetargetingMode::AnimationScaled:
				case EBoneTranslationRetargetingMode::OrientAndScale:
				{
					const double SkelLen = SkelLocal.IsValidIndex(SkelBone) ? SkelLocal[SkelBone].GetTranslation().Size() : 0.0;
					if (SkelLen > UE_KINDA_SMALL_NUMBER)
					{
						Translation *= MeshLocal[Bone].GetTranslation().Size() / SkelLen;
					}
					break;
				}
				case EBoneTranslationRetargetingMode::AnimationRelative:
					if (SkelLocal.IsValidIndex(SkelBone))
					{
						Translation = MeshLocal[Bone].GetTranslation() + (AnimLocal.GetTranslation() - SkelLocal[SkelBone].GetTranslation());
					}
					break;
				default:
					break;
				}
				Local = FTransform(AnimLocal.GetRotation(), Translation, AnimLocal.GetScale3D());
				++Animated;
			}
			const int32 Parent = LeaderRef.GetParentIndex(Bone);
			CS[Bone] = Parent == INDEX_NONE ? Local : Local * CS[Parent];
		}
		if (Animated == 0)
		{
			OutError = FString::Printf(TEXT("%s animates none of the leader's bones"), *Anim->GetPathName());
			return false;
		}
		OutPose = FPose();
		OutPose.Label = FString::Printf(TEXT("%s@%.2f"), *Anim->GetName(), Time);
		for (int32 Bone = 0; Bone < CS.Num(); ++Bone)
		{
			OutPose.BoneCS.Add(LeaderRef.GetBoneName(Bone), CS[Bone]);
		}
		return true;
	}

	void FollowerSkinMatrices(const FReferenceSkeleton& FollowerRef, const FPose& Pose, TArray<FMatrix44d>& OutBoneMatrices, int32& OutMissing)
	{
		TArray<FTransform> Bind;
		RefPoseCS(FollowerRef, Bind);
		const TArray<FTransform>& Local = FollowerRef.GetRefBonePose();
		TArray<FTransform> CS;
		CS.SetNum(Local.Num());
		OutBoneMatrices.SetNum(Local.Num());
		OutMissing = 0;
		for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
		{
			if (const FTransform* Leader = Pose.BoneCS.Find(FollowerRef.GetBoneName(Bone)))
			{
				CS[Bone] = *Leader;
			}
			else
			{
				++OutMissing;
				const int32 Parent = FollowerRef.GetParentIndex(Bone);
				CS[Bone] = Parent == INDEX_NONE ? Local[Bone] : Local[Bone] * CS[Parent];
			}
			OutBoneMatrices[Bone] = Bind[Bone].ToMatrixWithScale().Inverse() * CS[Bone].ToMatrixWithScale();
		}
	}

	int32 DeleteTriangles(FMeshDescription& Desc, const TArray<FTriangleID>& Triangles)
	{
		TArray<FEdgeID> Edges;
		TArray<FVertexInstanceID> Instances;
		TArray<FPolygonGroupID> Groups; // collected and deliberately kept
		int32 Deleted = 0;
		for (const FTriangleID Tri : Triangles)
		{
			if (Desc.IsTriangleValid(Tri))
			{
				Desc.DeleteTriangle(Tri, &Edges, &Instances, &Groups);
				++Deleted;
			}
		}
		TArray<FVertexID> Vertices;
		for (const FVertexInstanceID Instance : TSet<FVertexInstanceID>(Instances))
		{
			if (Desc.IsVertexInstanceValid(Instance) && Desc.GetVertexInstanceConnectedTriangleIDs(Instance).Num() == 0)
			{
				Desc.DeleteVertexInstance(Instance, &Vertices);
			}
		}
		for (const FEdgeID Edge : TSet<FEdgeID>(Edges))
		{
			if (Desc.IsEdgeValid(Edge) && Desc.GetEdgeConnectedTriangleIDs(Edge).Num() == 0)
			{
				Desc.DeleteEdge(Edge, &Vertices);
			}
		}
		for (const FVertexID Vertex : TSet<FVertexID>(Vertices))
		{
			if (Desc.IsVertexValid(Vertex) && Desc.GetVertexVertexInstanceIDs(Vertex).Num() == 0 && Desc.GetVertexConnectedEdgeIDs(Vertex).Num() == 0)
			{
				Desc.DeleteVertex(Vertex);
			}
		}
		FElementIDRemappings Remappings;
		Desc.Compact(Remappings);
		return Deleted;
	}

	void Skin(const FLodGeom& Geom, const TArray<FMatrix44d>& BoneMatrices, const FTransform& ComponentToLeader, TArray<FVector3d>& OutPos, TArray<FMatrix44d>* OutBlend, const TArray<FVector3d>* BindPosOverride)
	{
		const FMatrix44d ToLeader = ComponentToLeader.ToMatrixWithScale();
		const TArray<FVector3d>& Bind = BindPosOverride ? *BindPosOverride : Geom.BindPos;
		const int32 Num = Geom.BindPos.Num();
		OutPos.SetNum(Num);
		if (OutBlend)
		{
			OutBlend->SetNum(Num);
		}
		for (int32 V = 0; V < Num; ++V)
		{
			FMatrix44d M(ForceInitToZero);
			double Total = 0.0;
			for (const TPair<int32, float>& W : Geom.Weights[V])
			{
				if (BoneMatrices.IsValidIndex(W.Key) && W.Value > 0.f)
				{
					M += BoneMatrices[W.Key] * static_cast<double>(W.Value);
					Total += W.Value;
				}
			}
			M = Total > 0.0 ? M * (1.0 / Total) : FMatrix44d::Identity;
			M = M * ToLeader;
			OutPos[V] = M.TransformPosition(Bind[V]);
			if (OutBlend)
			{
				(*OutBlend)[V] = M;
			}
		}
	}

	void VertexNormals(const TArray<FVector3d>& Pos, const TArray<FIntVector3>& Tris, TArray<FVector3d>& OutNormals)
	{
		OutNormals.Init(FVector3d::ZeroVector, Pos.Num());
		for (const FIntVector3& T : Tris)
		{
			const FVector3d N = (Pos[T.Y] - Pos[T.X]) ^ (Pos[T.Z] - Pos[T.X]);
			OutNormals[T.X] += N;
			OutNormals[T.Y] += N;
			OutNormals[T.Z] += N;
		}
		for (FVector3d& N : OutNormals)
		{
			N = N.GetSafeNormal();
		}
	}

	struct FSoup::FImpl
	{
		FDynamicMesh3 Mesh;
		TUniquePtr<FDynamicMeshAABBTree3> Tree;
		TUniquePtr<TFastWindingTree<FDynamicMesh3>> Wind;
	};

	FSoup::FSoup() = default;
	FSoup::~FSoup() = default;
	FSoup::FSoup(FSoup&&) = default;
	FSoup& FSoup::operator=(FSoup&&) = default;

	void FSoup::Build(const TArray<FVector3d>& Pos, const TArray<FIntVector3>& Tris, bool bWinding)
	{
		Impl = MakeUnique<FImpl>();
		// a soup (unshared vertices) so non-manifold garment triangles never get dropped by AppendTriangle
		for (const FIntVector3& T : Tris)
		{
			const int32 A = Impl->Mesh.AppendVertex(Pos[T.X]);
			const int32 B = Impl->Mesh.AppendVertex(Pos[T.Y]);
			const int32 C = Impl->Mesh.AppendVertex(Pos[T.Z]);
			Impl->Mesh.AppendTriangle(A, B, C);
		}
		Impl->Tree = MakeUnique<FDynamicMeshAABBTree3>(&Impl->Mesh, true);
		if (bWinding)
		{
			Impl->Wind = MakeUnique<TFastWindingTree<FDynamicMesh3>>(Impl->Tree.Get(), true);
		}
	}

	bool FSoup::IsEmpty() const
	{
		return !Impl.IsValid() || Impl->Mesh.TriangleCount() == 0;
	}

	bool FSoup::Nearest(const FVector3d& P, double MaxDist, FVector3d& OutPoint, FVector3d& OutNormal, int32& OutTri, double& OutDist) const
	{
		if (IsEmpty())
		{
			return false;
		}
		IMeshSpatial::FQueryOptions Options;
		Options.MaxDistance = MaxDist;
		double DistSqr = 0.0;
		const int32 Tri = Impl->Tree->FindNearestTriangle(P, DistSqr, Options);
		if (Tri == IndexConstants::InvalidID)
		{
			return false;
		}
		FDistPoint3Triangle3d Query = TMeshQueries<FDynamicMesh3>::TriangleDistance(Impl->Mesh, Tri, P);
		OutPoint = Query.ClosestTrianglePoint;
		OutNormal = Impl->Mesh.GetTriNormal(Tri);
		OutTri = Tri;
		OutDist = FMath::Sqrt(Query.GetSquared());
		return true;
	}

	double FSoup::Ray(const FVector3d& Origin, const FVector3d& Dir, double MaxT) const
	{
		if (IsEmpty())
		{
			return -1.0;
		}
		const FRay3d Ray(Origin, Dir, true);
		double T = 0.0;
		int Tri = IndexConstants::InvalidID;
		if (Impl->Tree->FindNearestHitTriangle(Ray, T, Tri) && Tri != IndexConstants::InvalidID && T >= 0.0 && T <= MaxT)
		{
			return T;
		}
		return -1.0;
	}

	double FSoup::Winding(const FVector3d& P) const
	{
		return (Impl.IsValid() && Impl->Wind.IsValid()) ? Impl->Wind->FastWindingNumber(P) : 0.0;
	}

	void ContractSelection(const FLodGeom& Geom, TArray<bool>& Selected, int32 Rings)
	{
		if (Rings <= 0)
		{
			return;
		}
		TArray<TArray<int32>> VertTris;
		VertTris.SetNum(Geom.BindPos.Num());
		for (int32 T = 0; T < Geom.Tris.Num(); ++T)
		{
			VertTris[Geom.Tris[T].X].Add(T);
			VertTris[Geom.Tris[T].Y].Add(T);
			VertTris[Geom.Tris[T].Z].Add(T);
		}
		for (int32 Ring = 0; Ring < Rings; ++Ring)
		{
			TArray<bool> Border;
			Border.Init(false, Geom.BindPos.Num());
			for (int32 V = 0; V < VertTris.Num(); ++V)
			{
				for (const int32 T : VertTris[V])
				{
					if (!Selected[T])
					{
						Border[V] = true;
						break;
					}
				}
			}
			TArray<bool> Next = Selected;
			for (int32 T = 0; T < Geom.Tris.Num(); ++T)
			{
				if (Selected[T] && (Border[Geom.Tris[T].X] || Border[Geom.Tris[T].Y] || Border[Geom.Tris[T].Z]))
				{
					Next[T] = false;
				}
			}
			Selected = MoveTemp(Next);
		}
	}

	static bool ReadVec(const TSharedPtr<FJsonValue>& Value, FVector3d& Out)
	{
		const TArray<TSharedPtr<FJsonValue>>* Arr = nullptr;
		if (!Value.IsValid() || !Value->TryGetArray(Arr) || Arr->Num() != 3)
		{
			return false;
		}
		Out = FVector3d((*Arr)[0]->AsNumber(), (*Arr)[1]->AsNumber(), (*Arr)[2]->AsNumber());
		return true;
	}

	bool ParseBoxes(const TArray<TSharedPtr<FJsonValue>>& Values, TArray<FBox3d>& OutBoxes, FString& OutError)
	{
		for (const TSharedPtr<FJsonValue>& Value : Values)
		{
			FVector3d Min, Max;
			const TSharedPtr<FJsonObject>* Obj = nullptr;
			const TArray<TSharedPtr<FJsonValue>>* Pair = nullptr;
			bool bOk = false;
			if (Value.IsValid() && Value->TryGetObject(Obj))
			{
				bOk = ReadVec((*Obj)->TryGetField(TEXT("min")), Min) && ReadVec((*Obj)->TryGetField(TEXT("max")), Max);
			}
			else if (Value.IsValid() && Value->TryGetArray(Pair) && Pair->Num() == 2)
			{
				bOk = ReadVec((*Pair)[0], Min) && ReadVec((*Pair)[1], Max);
			}
			if (!bOk)
			{
				OutError = TEXT("a box is [[minx,miny,minz],[maxx,maxy,maxz]] or {\"min\":[x,y,z],\"max\":[x,y,z]} (cm, Body component space)");
				return false;
			}
			OutBoxes.Add(FBox3d(Min.ComponentMin(Max), Min.ComponentMax(Max)));
		}
		return true;
	}

	bool CommitLods(USkeletalMesh* Mesh, TMap<int32, FMeshDescription>& Edited, FString& OutError)
	{
		if (!Mesh)
		{
			OutError = TEXT("no mesh");
			return false;
		}
		// a component may be drawing this mesh: let the render thread finish with it before the LODs are rebuilt
		FlushRenderingCommands();
		Mesh->Modify();
		for (TPair<int32, FMeshDescription>& It : Edited)
		{
			if (!Mesh->CreateMeshDescription(It.Key, MoveTemp(It.Value)))
			{
				OutError = FString::Printf(TEXT("could not store LOD %d of %s"), It.Key, *Mesh->GetPathName());
				return false;
			}
			Mesh->ModifyMeshDescription(It.Key);
			if (!Mesh->CommitMeshDescription(It.Key))
			{
				OutError = FString::Printf(TEXT("could not commit LOD %d of %s"), It.Key, *Mesh->GetPathName());
				return false;
			}
		}
		Mesh->PostEditChange();
		Mesh->MarkPackageDirty();
		return true;
	}
}
