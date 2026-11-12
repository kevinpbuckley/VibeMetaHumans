// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "MeshDescription.h"

class USkeletalMesh;
class UAnimSequence;
struct FReferenceSkeleton;

/**
 * CPU skinning and geometry helpers for the wardrobe tools (2026-11-12).
 *
 * Everything works on asset data (a LOD's mesh description and the meshes' reference skeletons), never on a spawned
 * actor, so the tools are deterministic and need no world, no viewport and no PIE. A "pose" is the leader (Body)
 * component-space transform of each bone by name; a follower (garment, Face) is skinned the way leader pose /
 * copy-pose does it: bone b's matrix is inverse(follower bind CS of b) * leader CS of b.
 */
namespace VmhSkin
{
	/** One LOD's geometry, densely indexed. */
	struct VIBEMETAHUMANS_API FLodGeom
	{
		int32 Lod = 0;
		TArray<FVertexID> VertexIds;                       // dense index -> mesh description vertex
		TMap<int32, int32> DenseOf;                        // FVertexID value -> dense index
		TArray<FVector3d> BindPos;                         // mesh (bind) space, cm
		TArray<FIntVector3> Tris;                          // dense vertex indices
		TArray<FTriangleID> TriIds;
		TArray<TArray<TPair<int32, float>>> Weights;       // per vertex: (mesh ref-skeleton bone index, weight)
		TArray<TArray<int32>> Neighbors;                   // per vertex: edge neighbours (dense)
	};

	/**
	 * Clone the LOD's mesh description into OutDesc and read it into OutGeom. False (with OutError) when the LOD has
	 * no mesh description (an auto-generated LOD) or no skin weights.
	 */
	VIBEMETAHUMANS_API bool ReadLod(const USkeletalMesh* Mesh, int32 Lod, FMeshDescription& OutDesc, FLodGeom& OutGeom, FString& OutError);

	/** Number of LODs with an editable mesh description. */
	VIBEMETAHUMANS_API int32 NumLods(const USkeletalMesh* Mesh);

	/** Component-space reference pose (the mesh's own bind pose) of a reference skeleton. */
	VIBEMETAHUMANS_API void RefPoseCS(const FReferenceSkeleton& Ref, TArray<FTransform>& OutCS);

	/** A leader pose: component-space transform of every leader bone by name. */
	struct VIBEMETAHUMANS_API FPose
	{
		FString Label;
		TMap<FName, FTransform> BoneCS;
	};

	/** The leader's reference pose. */
	VIBEMETAHUMANS_API FPose RefPose(const FReferenceSkeleton& LeaderRef);

	/**
	 * The leader posed by an AnimSequence at Time, retargeted onto the leader mesh the way the engine does it (rotation
	 * from the animation; translation by the skeleton's per-bone retargeting mode). RigLogic-driven correctives and
	 * post-process AnimBP work are NOT evaluated: helper joints keep their reference offset from their parent.
	 */
	VIBEMETAHUMANS_API bool AnimPose(const FReferenceSkeleton& LeaderRef, const UAnimSequence* Anim, double Time, FPose& OutPose, FString& OutError);

	/**
	 * Follower bone matrices for Pose: bones the leader has take its transform; the others compose their reference
	 * local transform onto their (posed) parent. OutMissing counts follower bones the leader lacks.
	 */
	VIBEMETAHUMANS_API void FollowerSkinMatrices(const FReferenceSkeleton& FollowerRef, const FPose& Pose, TArray<FMatrix44d>& OutBoneMatrices, int32& OutMissing);

	/**
	 * Skin the LOD into the leader's component space. ComponentToLeader is the follower component's transform relative
	 * to the leader component (identity for a garment parented to Body at the origin). OutBlend (optional) receives each
	 * vertex's blended matrix, used to carry a component-space delta back into bind space.
	 */
	VIBEMETAHUMANS_API void Skin(const FLodGeom& Geom, const TArray<FMatrix44d>& BoneMatrices, const FTransform& ComponentToLeader, TArray<FVector3d>& OutPos, TArray<FMatrix44d>* OutBlend = nullptr, const TArray<FVector3d>* BindPosOverride = nullptr);

	/** Delete triangles (and the edges, vertex instances and vertices they orphan) but keep every polygon group, so material sections never shift; then compact. Returns the number deleted. */
	VIBEMETAHUMANS_API int32 DeleteTriangles(FMeshDescription& Desc, const TArray<FTriangleID>& Triangles);

	/** Area-weighted vertex normals. */
	VIBEMETAHUMANS_API void VertexNormals(const TArray<FVector3d>& Pos, const TArray<FIntVector3>& Tris, TArray<FVector3d>& OutNormals);

	/** A triangle soup with nearest-point, ray and winding-number queries. */
	class VIBEMETAHUMANS_API FSoup
	{
	public:
		FSoup();
		~FSoup();
		FSoup(FSoup&&);
		FSoup& operator=(FSoup&&);

		void Build(const TArray<FVector3d>& Pos, const TArray<FIntVector3>& Tris, bool bWinding);
		bool IsEmpty() const;

		/** Nearest point within MaxDist: true with OutPoint, OutNormal (unnormalised triangle normal is normalised) and OutTri. */
		bool Nearest(const FVector3d& P, double MaxDist, FVector3d& OutPoint, FVector3d& OutNormal, int32& OutTri, double& OutDist) const;

		/** Distance along Dir (unit) to the first hit within MaxT, or -1. */
		double Ray(const FVector3d& Origin, const FVector3d& Dir, double MaxT) const;

		/** Fast winding number (needs bWinding at Build). */
		double Winding(const FVector3d& P) const;

	private:
		struct FImpl;
		TUniquePtr<FImpl> Impl;
	};

	/** Shrink a triangle selection by Rings: a selected triangle touching an unselected one is dropped, Rings times. */
	VIBEMETAHUMANS_API void ContractSelection(const FLodGeom& Geom, TArray<bool>& InOutSelected, int32 Rings);

	/** Parse [[minx,miny,minz],[maxx,maxy,maxz]] boxes (or one {"min":[...],"max":[...]} object) from a JSON value list. */
	VIBEMETAHUMANS_API bool ParseBoxes(const TArray<TSharedPtr<class FJsonValue>>& Values, TArray<FBox3d>& OutBoxes, FString& OutError);

	/** Write edited mesh descriptions back to their LODs and rebuild the mesh once. */
	VIBEMETAHUMANS_API bool CommitLods(USkeletalMesh* Mesh, TMap<int32, FMeshDescription>& Edited, FString& OutError);
}
