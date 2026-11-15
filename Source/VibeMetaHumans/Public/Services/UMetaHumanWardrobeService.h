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
 *   3. push_body_under_garments      - under a dress or an open garment: skin that comes through moved inward (the
 *                                      default; deleting it showed holes). hide_body_under_garments deletes the skin
 *                                      closed garments cover (trousers, shoes); trim_inner_garment does it to a shirt
 *                                      under a jacket.
 *   4. push_garment_out_of_mesh      - the MetaHuman face mesh's neck skirt reaches the collarbones and pokes through
 *                                      necklines; push the garment out over it (or over the body).
 *   5. snapshot/restore_blueprint_components - a Creator rebuild regenerates BP_<Name> and drops all of the above;
 *                                      record the Blueprint first and put it back after (build(..., preserve) does both);
 *                                      then post_build_fixup, and audit_placed_instances on the level.
 *   6. fill_groom_lods                - Optimized builds leave groom LODs with no geometry (bald at LOD4/LOD6).
 * Checks: describe_skeletal_mesh, measure_poke_through, capture_fit_views. Intake: make_garment_variant,
 * transfer_skin_weights. Accessories: add_skinned_accessory.
 *
 * Geometry is computed from asset data (reference skeletons, mesh descriptions). Poses: the rest pose always, plus the
 * listed ones - {"anim": path, "time"|"times"|"stride"} sampled on the asset ("evaluate":"asset": the body's post-process
 * correctives stay at rest, up to 14 cm off in a seated pose) or played on the Blueprint in a private preview world
 * ("evaluate":"runtime": every anim graph evaluated, as in PIE), or {"file": dump} from dump_runtime_poses /
 * dump_actor_pose. No level, PIE or viewport is needed. Nothing is saved unless a method says so.
 */
UCLASS(BlueprintType)
class VIBEMETAHUMANS_API UMetaHumanWardrobeService : public UToolsetDefinition
{
	GENERATED_BODY()

public:
	/**
	 * Move a garment's skin weights off the leader's unevaluated bones onto their nearest evaluated ancestor, on every
	 * LOD: what the body does itself for the joints its LOD settings remove. Garment LOD n is checked against leader LOD
	 * n, and the garment's LAST LOD against every remaining leader LOD (a one-LOD garment is drawn at body LOD 0, 1 and 2,
	 * where the body drops corrective and helper bones). GarmentMeshPath is edited IN PLACE (use a copy of a marketplace
	 * mesh); LeaderMeshPath is the Blueprint's Body mesh. bDryRun only counts. Reports per LOD the leader LODs checked,
	 * the vertices fixed and the bones they used.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString FixUnevaluatedBoneWeights(const FString& GarmentMeshPath, const FString& LeaderMeshPath, bool bDryRun = false);

	/**
	 * Read-only facts about a skeletal mesh, per LOD: screen size, vertices, triangles, sections (material index,
	 * triangles, disabled), required bones, the bones NOT evaluated at that LOD and those dropped since the previous LOD,
	 * the LOD settings' bones_to_remove; plus skeleton, physics asset, post-process AnimBP, LOD settings asset, material
	 * slots, sockets, morph target count and bounds. LeaderMeshPath (optional, the Body mesh): each LOD is also checked
	 * as that leader's follower (vertices weighted to bones the leader does not evaluate or lacks). Bone name lists are
	 * capped at 40 unless bListAllBones. Use it instead of guessing lod_info / get_lod_num / bones_to_remove in Python.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString DescribeSkeletalMesh(const FString& MeshPath, const FString& LeaderMeshPath = TEXT(""), bool bListAllBones = false);

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
	 * "winding":0.5,"pose_combine":"all","poses":[],"evaluate":"asset","lods":[],"keep_boxes":[],"delete_boxes":[],
	 * "body_component":"Body","source_body":"","target_component":"","min_fragment":0}. Re-runs start from the source
	 * body each time. target_component cuts another mesh than the body (an inner garment, skinned as the body's
	 * follower; see trim_inner_garment); min_fragment also deletes islands of that many triangles or fewer the cut leaves.
	 * bAssignToBlueprint points the cut component at the result. For closed garments only: under a dress or an open
	 * garment a deleted patch is a hole from some camera - use push_body_under_garments.
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
	 * The default under a dress, a gown, a slit, short sleeves, a bare back or an open collar: write OutBodyMeshPath, the
	 * source body (options.source_body - use the UNCUT full body from make_full_body_from_dna - else the Body
	 * component's mesh) with every skin vertex that comes out through the garments in any listed pose moved inward, at
	 * the bind pose, to `margin` cm under the cloth. Nothing is deleted, so no camera sees a hole (deleting skin under a
	 * dress showed a see-through crotch from a low camera and a black patch on an upper arm). Each LOD is solved on its
	 * own geometry. A vertex is "under" a garment when the nearest cloth point lies along the skin's normal and skin and
	 * cloth face the same way (an arm hanging beside a skirt is left alone); the cloth's outward side comes from the
	 * skeleton, never its winding. Poses default to "runtime" evaluation (the Blueprint in a preview world, post-process
	 * correctives included: asset-sampled seated poses were up to 14 cm off), or pass dumps: {"poses":[{"file": ...}]}.
	 * OptionsJson defaults: {"margin":0.6,"reach":6,"max_depth":2.0,"facing":0.3,"passes":4,"max_push":5,
	 * "spread_rings":3,"spread_falloff":0.7,"exclude_bones":["head","neck_*","FACIAL_*","hand_*","thumb_*","index_*",
	 * "middle_*","ring_*","pinky_*"] (dominant bone; a hand resting on the lap is not poke-through),"exclude_boxes":[],
	 * "only_boxes":[],"limits":[{"box":[[min],[max]],"max_push":0.8}],"poses":[],"evaluate":"runtime","lods":[],
	 * "body_component":"Body","source_body":""}. Reports per LOD and pass the vertices outside the cloth and the worst
	 * depth. bAssignToBlueprint points Body at the result. Deleting is still right for closed garments
	 * (hide_body_under_garments: legs in trousers, feet in shoes); run it on the pushed body when both apply.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString PushBodyUnderGarments(const FString& BlueprintPath, const FString& GarmentComponentsJson, const FString& OutBodyMeshPath, const FString& OptionsJson = TEXT("{}"), bool bAssignToBlueprint = false, bool bDryRun = false);

	/**
	 * Read-only poke-through check: for each garment component and each listed pose, how many body vertices come out
	 * through it and the worst depth in cm (with the bind position of the worst vertex and its pose). Same rules and
	 * options as push_body_under_garments (poses default to runtime evaluation; options.body_mesh measures another body
	 * than the Body component's; options.lods default [0]). Run it before and after a fit, in the poses the game uses.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString MeasurePokeThrough(const FString& BlueprintPath, const FString& GarmentComponentsJson, const FString& OptionsJson = TEXT("{}"));

	/**
	 * Drop an inner garment's triangles that the outer garments hide (a shirt under a jacket flecks through it), on a
	 * copy OutMeshPath, in every listed pose, and remove the islands of 40 triangles or fewer the cut leaves. It is
	 * hide_body_under_garments with target_component = InnerComponent and the jam's layer values (shell 0.6, winding
	 * 0.6, contract 2, ray_out 5, ray_in 3, min_fragment 40); OptionsJson overrides any of them (and takes poses,
	 * keep_boxes, delete_boxes, source_body). Pass the rest AND the seated pose. bAssignToBlueprint points the inner
	 * component at the copy.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString TrimInnerGarment(const FString& BlueprintPath, const FString& InnerComponent, const FString& OuterComponentsJson, const FString& OutMeshPath, const FString& OptionsJson = TEXT("{}"), bool bAssignToBlueprint = false, bool bDryRun = false);

	/**
	 * Render a dressed character for review without a level change or PIE: the Blueprint is spawned in a private
	 * preview world, the body plays AnimPath at Time (empty = the Blueprint's own pose), every anim graph is evaluated
	 * (the post-process AnimBP's correctives too) and each view is captured to a PNG. ViewsJson: names "front",
	 * "three_quarter", "side", "back", "right", "low_front", "high_front", or {"name","yaw","pitch"} (MetaHumans face
	 * +Y: yaw 90 is the front; pitch < 0 puts the camera below - use it for skirts). OutDir default
	 * Saved/VibeMetaHumans/FitViews. OptionsJson: {"times":[s...] (several poses of the clip), "focus":"body"|"head"|
	 * "chest"|"legs"|"hands", "lod":0, "width":768, "height":1024, "fov":30, "exposure_bias":1.5, "hide_components":[],
	 * "body_component":"Body"}. Returns the files (and the fraction of non-dark pixels, to catch an empty frame). Needs
	 * a rendering editor (not -nullrhi). Grooms may not draw in a scene capture: judge hair in PIE.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString CaptureFitViews(const FString& BlueprintPath, const FString& AnimPath = TEXT(""), float Time = 0.f, const FString& ViewsJson = TEXT("[\"front\",\"three_quarter\",\"side\",\"back\"]"), const FString& OutDir = TEXT(""), const FString& OptionsJson = TEXT("{}"));

	/**
	 * Evaluate body poses as the game shows them (the Blueprint in a preview world, every anim graph including the
	 * post-process AnimBP) and write them to OutFilePath (default Saved/VibeMetaHumans/Poses/<BP>_poses.json) as Body
	 * component-space bone transforms. PosesJson: [{"anim": path, "time": s} | {"anim", "times": [...]} |
	 * {"anim", "stride": s}]. Reports, per pose, how far the runtime pose is from sampling the clip on the asset (the
	 * jam measured up to 14 cm in seated poses). Feed the file to the fit tools: {"poses": [{"file": <file>}]}.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString DumpRuntimePoses(const FString& BlueprintPath, const FString& PosesJson, const FString& OutFilePath = TEXT(""));

	/**
	 * Append the current pose of a placed character's skeletal mesh component (default "Body") to a pose file, read from
	 * the running PIE world when there is one, else the editor world: the exact pose the game shows, for the fit tools'
	 * {"poses": [{"file": ...}]}. Call it at the moments that matter while the game plays.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString DumpActorPose(const FString& ActorLabel, const FString& ComponentName = TEXT("Body"), const FString& OutFilePath = TEXT(""), const FString& Label = TEXT(""), bool bAppend = true);

	/**
	 * A static accessory (tie, scarf, chain, brooch, pocket square) as a leader-pose SKINNED mesh under Body, so it follows
	 * the surface under it instead of floating or kinking on one bone. The static mesh is placed like a static mesh on
	 * Bone's socket with RelativeJson {"location":[x,y,z],"rotation":[roll,pitch,yaw],"scale":[x,y,z]} at Body's
	 * reference pose; the new skeletal mesh (OutMeshPath, default <static mesh folder>/SKM_<name>_Skinned) carries the
	 * body's skeleton. Mode "surface": weights by closest point from SurfaceComponentsJson as displayed at rest (the
	 * garment it lies on; default ["Body"]); "rigid": all weight on the dominant bone under it (stiff pieces: no kinks).
	 * Weights are then moved off bones the body drops at lower LODs, and the mesh is attached as ComponentName (leader
	 * pose, leader bounds). Runs Content/Python/vmh_wardrobe.py on GeometryScript (the GeometryScripting plugin must be
	 * enabled). Check it with capture_fit_views (focus "chest", seated and standing). Saves the new mesh only.
	 * Jam lesson: a piece along the body's centre line over an opening (a tie on an open collar) has no stable surface.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString AddSkinnedAccessory(const FString& BlueprintPath, const FString& StaticMeshPath, const FString& ComponentName, const FString& Mode = TEXT("surface"), const FString& SurfaceComponentsJson = TEXT("[\"Body\"]"), const FString& Bone = TEXT("spine_05"), const FString& RelativeJson = TEXT("{}"), const FString& OutMeshPath = TEXT(""));

	/**
	 * Garment intake: copy a marketplace garment into DestFolder as SKM_<Name> with its own material copies
	 * (Materials/<material>_<Name>) and capped texture copies (Textures/, MaxTextureSize, virtual texture streaming
	 * untouched), and point the mesh copy's slots at them, so the cook never pulls the pack's 4K material in through the
	 * mesh. TextureVariant "2" swaps T_X_1_* (or T_X_*) for the pack's T_X_2_* colourway where it exists. Reports any pack
	 * texture still referenced (a material instance copy keeps the pack parent's defaults). GeometryScript-free, but runs
	 * in Content/Python/vmh_wardrobe.py.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString MakeGarmentVariant(const FString& SourceMeshPath, const FString& DestFolder, const FString& Name, const FString& TextureVariant = TEXT(""), int32 MaxTextureSize = 1024);

	/**
	 * Garment intake: give a mesh with no usable skinning (a Chaos outfit's render mesh is skinned to root only and
	 * cannot be a leader-pose garment) the bones and weights of a skinned source in the same space (the body or outfit it
	 * was made on). Writes a NEW mesh (OutMeshPath, default <target>_Skinned) with the source's skeleton; Method
	 * "closest" (closest point on the source surface) or "inpaint" (smooth fill). Then fix_unevaluated_bone_weights and
	 * attach it. A Chaos outfit made for another body shape is refitted by a throwaway build instead (see the skill).
	 * Runs Content/Python/vmh_wardrobe.py on GeometryScript.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString TransferSkinWeights(const FString& TargetMeshPath, const FString& SourceMeshPath, const FString& OutMeshPath = TEXT(""), const FString& Method = TEXT("closest"));

	/**
	 * Redo the asset-level fixes a Creator rebuild throws away, on one built Blueprint (after build(..., preserve)):
	 * (1) fill_groom_lods on every groom component's groom (hidden LODs made visible: brows), (2) the brow groom's
	 * OpacityNear/OpacityFar (reported always; set when options.brow_opacity > 0 - pale brows were fixed with 2.0),
	 * (3) per-asset max_texture_size caps on the build's textures by name wildcard, only when options.texture_caps is
	 * given (the jam's web build used {"T_Body_*_VT":1024,"T_Head_*_VT":2048}; a cap only lowers, never raises; a
	 * device-profile cap does not reach virtual textures; virtual_texture_streaming is never touched), (4) the Face component's anim class back to options.face_anim_class (a rebuild restores the plugin's
	 * copy-pose-only ABP_Face). OptionsJson defaults: {"groom_lods":true,"show_hidden_groom_lods":true,"brow_opacity":0,
	 * "brow_component":"Eyebrows","texture_caps":{},"face_anim_class":"","face_component":"Face","folder":<BP folder>}.
	 * Only /Game assets outside /Game/Fab are edited. Body fits (hide/push) are NOT redone: re-run them on the new body.
	 * bDryRun reports what it would change. Saves nothing; returns the dirty packages.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString PostBuildFixup(const FString& BlueprintPath, const FString& OptionsJson = TEXT("{}"), bool bDryRun = false);

	/**
	 * List the instances of a Blueprint placed in the open editor level (or MapPath, loaded first) whose components
	 * carry per-instance overrides that differ from the Blueprint: an old Body mesh override on a placed actor kept the
	 * holes in every real shot while every fresh spawn was clean; a stale Face anim class override hid the face fix.
	 * Reports per actor every differing editable component property (instance value, Blueprint value) and components
	 * added on the instance only; the root's placement is ignored. bClearOverrides copies the Blueprint's values back
	 * for EVERY listed override (read the list first: an override may be meant), then save the level / its external
	 * actor packages. MapPath is refused (UNSAVED_LEVEL) while the open level has unsaved changes.
	 */
	UFUNCTION(BlueprintCallable, meta = (AICallable), Category = "VibeMetaHumans|Wardrobe")
	static FString AuditPlacedInstances(const FString& BlueprintPath, bool bClearOverrides = false, const FString& MapPath = TEXT(""));

	/**
	 * Not a tool: how a garment will behave as Leader's follower at GarmentLod (-1 = every LOD, totals plus a per-LOD
	 * list) - vertices weighted to bones Leader does not evaluate at the leader LODs that garment LOD is drawn with, and
	 * garment bones Leader lacks. Used by attach_skeletal_mesh_to_blueprint and describe_skeletal_mesh.
	 */
	static TSharedPtr<FJsonObject> CheckFollower(const USkeletalMesh* Garment, const USkeletalMesh* Leader, int32 GarmentLod = 0);
};
