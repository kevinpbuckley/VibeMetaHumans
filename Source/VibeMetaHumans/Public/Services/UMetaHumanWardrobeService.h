// Copyright Buckley Builds LLC 2026 All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"
#include "ToolsetRegistry/ToolsetDefinition.h"
#include "UMetaHumanWardrobeService.generated.h"

class USkeletalMesh;

/**
 * MetaHuman Wardrobe Service - dress a BUILT MetaHuman Blueprint in skeletal garments (leader pose) and make it read
 * clean, without a Creator rebuild (2026-11-12, from dressing six characters in Fab garments).
 *
 * Mental model: a Fab/marketplace garment skinned to the MetaHuman body skeleton is added as a leader-pose component
 * under Body (MetaHumanCharacterService.attach_skeletal_mesh_to_blueprint). It keeps the shape it was modelled for; it
 * does not refit. Then, on COPIES (never the pack's own assets):
 *   1. fix_unevaluated_bone_weights  - the Optimized body does not evaluate ~144 hand helper joints; garment vertices
 *                                      weighted to them collapse to the actor origin (spikes from the cuffs to the floor).
 *   2. make_full_body_from_dna       - the built body has the faces under the Creator's default tee/shorts removed;
 *                                      anything showing more skin (a dress, an open collar) needs the full topology back.
 *   3. hide_body_under_garments      - a body copy with the skin the garments cover deleted (no poke-through).
 *   4. push_garment_out_of_mesh      - the MetaHuman face mesh's neck skirt reaches the collarbones and pokes through
 *                                      necklines; push the garment out over it (or over the body).
 *   5. snapshot/restore_blueprint_components - a Creator rebuild regenerates BP_<Name> and drops all of the above;
 *                                      record the Blueprint first and put it back after (build(..., preserve) does both).
 *   6. fill_groom_lods                - Optimized builds leave groom LODs with no geometry (bald at LOD4/LOD6).
 *
 * Everything is computed from asset data (reference skeletons, mesh descriptions, AnimSequences), so no level, actor,
 * viewport or PIE session is needed. Poses: the rest pose always, plus any {"anim": path, "time": s} you list (e.g. a
 * seated idle), retargeted onto the Body mesh. Body correctives driven by RigLogic are not evaluated in those poses.
 * Nothing is saved: save the touched packages afterwards.
 */
UCLASS(BlueprintType)
class VIBEMETAHUMANS_API UMetaHumanWardrobeService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Move a garment's skin weights off the leader's unevaluated bones onto their nearest evaluated ancestor, on every
	 * LOD (garment LOD n is checked against leader LOD min(n, last)): what the body does itself for the joints its LOD
	 * settings remove. GarmentMeshPath is edited IN PLACE (use a copy of a marketplace mesh); LeaderMeshPath is the
	 * Blueprint's Body mesh. bDryRun only counts. Reports per LOD the vertices fixed and the bones they used.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString FixUnevaluatedBoneWeights(const FString& GarmentMeshPath, const FString& LeaderMeshPath, bool bDryRun = false);

	/**
	 * A copy of a built body (OutMeshPath, default <BodyMeshPath>_Full) whose LODs are replaced by the full-topology
	 * body from the character's own body DNA (keeps the built mesh's LOD settings, physics asset, DNA user data and
	 * materials). DnaMeshPath: the body DNA asset (or a skeletal mesh already generated from it); else CharacterPath's
	 * body DNA is exported as an asset into <OutName>_Dna. The mesh is generated next to OutMeshPath as
	 * <OutName>_DnaSource (reused when present; delete it and the _Dna folder when done). Generate from the DNA ASSET:
	 * a .dna FILE read with the legacy config lands in another space. Each built LOD is matched to the DNA LOD with the
	 * same vertices (built LOD n is often DNA LOD n or n+1).
	 * bDryRun reports the matching without writing the output mesh (the DNA source mesh is still generated).
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString MakeFullBodyFromDna(const FString& BodyMeshPath, const FString& OutMeshPath, const FString& DnaMeshPath, const FString& CharacterPath, bool bDryRun = false);

	/**
	 * Write OutBodyMeshPath: the Blueprint's body (or options.source_body) with the triangles the garments cover
	 * deleted, on every LOD. GarmentComponentsJson: ["G_Jacket","G_Pants"] or [{"component":"G_Pants","shell":3}].
	 * A triangle goes when all three of its vertices are covered in every pose (pose_combine "all"; "any" = in at
	 * least one): (v1) inside a garment (|winding| >= winding) or within `shell` cm of it, contracted by `contract`
	 * rings; plus (v2, use_rays) a garment within ray_out cm outside or ray_in cm inside along the skin normal,
	 * contracted by ray_contract rings. delete_boxes always go (fully covered zones such as trouser legs, where pocket
	 * openings let rays through); keep_boxes never go (skin inside a neckline). Boxes: [[min],[max]] cm, Body space.
	 * OptionsJson defaults: {"shell":2,"contract":2,"use_rays":true,"ray_out":6,"ray_in":5,"ray_contract":1,
	 * "winding":0.5,"pose_combine":"all","poses":[],"lods":[],"keep_boxes":[],"delete_boxes":[],"body_component":"Body",
	 * "source_body":""}. Re-runs start from the source body each time. bAssignToBlueprint points Body at the result.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString HideBodyUnderGarments(const FString& BlueprintPath, const FString& GarmentComponentsJson, const FString& OutBodyMeshPath, const FString& OptionsJson = TEXT("{}"), bool bAssignToBlueprint = false, bool bDryRun = false);

	/**
	 * Push a garment out over an obstacle mesh of the same Blueprint (default "Face": its neck skirt pokes through
	 * necklines; "Body" for skin poking through a dress) wherever, in any listed pose, the garment lies behind the
	 * obstacle's skin or less than `margin` cm in front of it (only within `reach` cm). The outward direction comes from
	 * the skeleton (away from the nearest bone), never from the triangle winding (the face skirt's winding points INTO the
	 * skin: a push along it tore a jacket). Each LOD is pushed on its own; the push is diffused over the garment's edges
	 * (never below what a vertex needs) and repeated up to `passes` times. OutMeshPath: the garment copy to write (made
	 * from the component's mesh if missing, then assigned to the component); empty = edit the component's mesh in place
	 * (refused for meshes outside /Game or under /Game/Fab). OptionsJson defaults: {"margin":0.5,"reach":3,"passes":3,
	 * "smooth_iterations":6,"region":null,"spread":3,"under":[],"under_gap":0.3,"poses":[],"lods":[],
	 * "body_component":"Body"}; region = one box (Body space, rest pose) limiting which garment vertices move (e.g. the
	 * front of a V neck); under = outer garment components the push must stay inside of (a knit under a jacket: without
	 * it the knit came out through the jacket's shoulders), capped at their surface minus under_gap cm. A violation an
	 * outer garment leaves no room for is reported as blocked (it is hidden by that garment anyway).
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString PushGarmentOutOfMesh(const FString& BlueprintPath, const FString& GarmentComponent, const FString& ObstacleComponent = TEXT("Face"), const FString& OutMeshPath = TEXT(""), const FString& OptionsJson = TEXT("{}"), bool bDryRun = false);

	/**
	 * Record a Blueprint's construction-script components (class, parent, socket, every editable property that differs
	 * from the class default) and class defaults, as JSON; also written to OutFilePath when given. Take it before a
	 * MetaHuman rebuild, restore it after.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString SnapshotBlueprintComponents(const FString& BlueprintPath, const FString& OutFilePath = TEXT(""));

	/**
	 * Put a snapshot back: re-create the components the Blueprint lacks (bAddMissing) and re-apply recorded property
	 * values that differ now (bApplyToExisting, plus class defaults). Never deletes. Snapshot: the JSON text, or a path
	 * to the file snapshot_blueprint_components wrote. SkipPatternsJson: ["LODSync.*", "*.RelativeLocation"] wildcards
	 * on "Component.Property". bDryRun lists what would change. Compiles; does not save.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString RestoreBlueprintComponents(const FString& BlueprintPath, const FString& Snapshot, const FString& SkipPatternsJson = TEXT("[]"), bool bAddMissing = true, bool bApplyToExisting = true, bool bDryRun = false);

	/**
	 * Give every groom LOD geometry: a Cards LOD without a cards description gets a copy of the nearest LOD's cards
	 * (mesh + textures), a Meshes LOD without a mesh gets the nearest helmet mesh (or the cards when the groom has no
	 * meshes). bMakeHiddenLodsVisible also turns hidden LODs (brows at LOD5-7) into visible cards. Rebuilds the groom's
	 * derived data; its bindings rebuild on their own (about a minute). bDryRun lists the gaps only.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString FillGroomLods(const FString& GroomPath, bool bMakeHiddenLodsVisible = false, bool bDryRun = false);

	/**
	 * Not a tool: how a garment will behave as Leader's follower at GarmentLod - vertices weighted to bones Leader does
	 * not evaluate, and garment bones Leader lacks. Used by attach_skeletal_mesh_to_blueprint.
	 */
	static TSharedPtr<FJsonObject> CheckFollower(const USkeletalMesh* Garment, const USkeletalMesh* Leader, int32 GarmentLod = 0);
};
