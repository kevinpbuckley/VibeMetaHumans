---
name: metahuman-wardrobe
display_name: MetaHuman wardrobe (dress a built MetaHuman)
description: Dress a BUILT MetaHuman Blueprint in marketplace / Fab skeletal garments with unreal.MetaHumanWardrobeService (the default route for every step) and MetaHumanCharacterService.attach_skeletal_mesh_to_blueprint - garment intake (material variant, capped textures, weights onto a root-only Chaos render mesh), leader-pose garments, LOD streaks and cuff spikes, the full-topology body for dresses and open collars, pushing skin under a dress instead of deleting it, deleting skin under closed garments, trimming a shirt under a jacket, pushing a neckline out over the face mesh's neck skirt, checking the fit in the poses the game really plays (runtime poses, poke-through measure, fit-view captures), skinned accessories, surviving a Creator rebuild (snapshot/restore, post-build fix-up, placed-instance audit), groom LODs that go bald. Use for "put this jacket on the character", "the clothes poke through", "see-through dress", "holes in the body", "spikes from the sleeves", "the garment streaks at distance", "skin shows through the shirt", "the tie floats", "a rebuild lost my clothes / brought the default tee back", "the hair disappears at distance".
vibeue_classes:
  - MetaHumanWardrobeService
  - MetaHumanCharacterService
unreal_classes:
  - USkeletalMesh
  - USkeletalMeshComponent
  - UGroomAsset
keywords:
  - metahuman clothes
  - metahuman outfit
  - fab garment
  - leader pose
  - cuff spikes
  - lod streaks
  - poke through
  - clipping
  - see-through dress
  - neck skirt
  - hide body
  - push body
  - fit view
  - runtime pose
  - skinned accessory
  - rebuild lost components
  - placed instance override
  - groom lod
---

# Dressing a built MetaHuman (wardrobe)

```python
import json, unreal
mh, wd = unreal.MetaHumanCharacterService, unreal.MetaHumanWardrobeService
BP, BODY = "/Game/MetaHumans/Vale/BP_Vale", "/Game/MetaHumans/Vale/Body/SKM_MH_Vale_BodyMesh"
SEATED = "/Game/Anims/A_Idle_Seated"
```

Learned dressing six characters in Fab garments for a shipped game (2026-11). **Use the service methods below for
every step**: in that project the one-off scripts written before they existed were reused in 11 of 16 sessions and
re-solved problems the service already handled. Everything works on COPIES in your own folder: never edit the pack's
meshes, materials or textures (`/Game/Fab` is refused by the editing tools). Nothing is saved for you unless a method
says so: save what it reports by name.

## The default route

| Step | Method | Notes |
|---|---|---|
| Copy a pack garment, material variant, 1K textures | `wd.make_garment_variant(src, folder, name, variant, 1024)` | reports pack textures still referenced |
| Weights onto a root-only Chaos render mesh | `wd.transfer_skin_weights(target, skinned_source, out, "closest")` | new mesh with the source's skeleton |
| Look at a mesh's LODs, bones, sections, slots | `wd.describe_skeletal_mesh(mesh, BODY)` | per LOD: bones not evaluated, follower check |
| Cuff spikes and LOD streaks | `wd.fix_unevaluated_bone_weights(copy, BODY, dry)` | every garment LOD against every body LOD it is drawn at |
| Attach | `mh.attach_skeletal_mesh_to_blueprint(BP, "G_X", copy, "Body", True, material)` | leader pose + leader bounds; read `follower_check` |
| Skin under a dress / open garment | `wd.push_body_under_garments(BP, garments, out, opts, assign)` | the default: moves skin inward, deletes nothing |
| Skin under closed garments | `wd.hide_body_under_garments(BP, garments, out, opts, assign)` | trousers, shoes, long sleeves |
| Shirt flecks through a jacket | `wd.trim_inner_garment(BP, "G_Shirt", '["G_Jacket"]', out, opts, assign)` | drops the hidden triangles + stray islands |
| Neckline vs the face's neck skirt | `wd.push_garment_out_of_mesh(BP, "G_Sweater", "Face", out, opts)` | pass outer layers in `under` |
| Poses the game shows | `wd.dump_runtime_poses(BP, poses)` / `wd.dump_actor_pose(label)` (PIE) | feed `{"poses":[{"file": f}]}` to every fit tool |
| Did anything poke through? | `wd.measure_poke_through(BP, garments, opts)` | worst cm per garment and pose |
| Look at it | `wd.capture_fit_views(BP, clip, t, views, out_dir, opts)` | PNGs, no level change, no PIE |
| Accessory that follows the cloth | `wd.add_skinned_accessory(BP, static_mesh, "ACC_X", mode, surfaces, bone, rel)` | `surface` or `rigid` |
| Rebuild the character | `mh.build(c, ..., True)` then `wd.post_build_fixup(BP, opts)` | then re-run the body fit |
| Placed actors still wrong | `wd.audit_placed_instances(BP, clear, map)` | per-instance overrides vs a fresh spawn |
| Bald LODs, vanishing brows | `wd.fill_groom_lods(groom, True, False)` | also part of `post_build_fixup` |

`make_garment_variant`, `transfer_skin_weights` and `add_skinned_accessory` run `Content/Python/vmh_wardrobe.py`;
the last two need the GeometryScripting plugin enabled (error `GEOMETRY_SCRIPT_MISSING` otherwise).

## Pick the garment route

| | Creator `Outfits` slot (`add_wardrobe_item` + `build`) | Skeletal garment on the built Blueprint (leader pose) |
|---|---|---|
| Fits the body | yes: the outfit's Chaos resize graph refits it | no: keeps the size it was modelled for |
| Needs a rebuild | yes, and a rebuild regenerates `BP_<Name>` | no |
| What went wrong | one Chaos outfit refit from a single source size: the collar sat below the shoulders, ragged | cuff spikes, LOD streaks, skin poking through, the face mesh's neck skirt in the neckline (all fixable below) |

Skeletal garments (`*_Male_*` / `*_Female_*` meshes skinned to the MetaHuman body skeleton) are the jam-proof route.
A Chaos outfit made for a different body shape (it balloons as a leader-pose garment, collar over the face): give a
THROWAWAY duplicate of the character the outfit, build it into a scratch folder, and use the baked
`Clothing/<Name>_Outfits` mesh as a leader-pose garment on the real Blueprint (it is bound to that character's skeleton
and fits). A render mesh skinned to `root` only gets weights with `transfer_skin_weights` first.

## Workflow

```python
# 0. BEFORE the build that will show skin: no grey sports top baked into the body and the face's neck skirt
mh.set_settings(c, "skin", '{"Skin": {"bShowTopUnderwear": false}}')       # then build
# 1. copies: mesh + material variant + 1K textures in your folder
g = json.loads(wd.make_garment_variant("/Game/Fab/MetaHuman/Jacket_Female_01/Jacket_Female_01", "/Game/Wardrobe", "Jacket_Vale", "2", 1024))
JACKET = g["mesh"]
# 2. what the garment and body look like per LOD; then move weights off bones the body does not evaluate
print(json.loads(wd.describe_skeletal_mesh(JACKET, BODY))["lods"][-1]["as_follower"])
wd.fix_unevaluated_bone_weights(JACKET, BODY, False)
# 3. attach: leader pose + leader bounds + materials; follower_check covers every garment LOD
r = json.loads(mh.attach_skeletal_mesh_to_blueprint(BP, "G_Jacket", JACKET, "Body", True, ""))
assert r["follower_check"]["unevaluated_bone_vertices"] == 0, r["warnings"]
# 4. hide the built default outfit component (set its mesh to None or bVisible false on the template)
# 5. the poses the game uses (post-process correctives evaluated, as in PIE)
poses = json.loads(wd.dump_runtime_poses(BP, json.dumps([{"anim": SEATED, "stride": 0.5}, {"anim": "/Game/Anims/A_Idle", "time": 1.0}])))
P = {"poses": [{"file": poses["file"]}]}
# 6. the body under the clothes - push for open garments, delete for closed ones (both on copies of the UNCUT body)
full = json.loads(wd.make_full_body_from_dna(BODY, BODY + "_Full", "", "/Game/MetaHumans/MH_Vale", False))["full_body"]
print(json.loads(wd.push_body_under_garments(BP, '["G_Jacket","G_Shirt"]', BODY + "_Fit", json.dumps(dict(P, source_body=full)), True, False)))
print(json.loads(wd.hide_body_under_garments(BP, '["G_Pants","G_Shoes"]', BODY + "_Dressed",
      json.dumps(dict(P, source_body=BODY + "_Fit", delete_boxes=[[[-40, -40, 12], [40, 40, 85]]])), True, False)))
# 7. layers and necklines
wd.trim_inner_garment(BP, "G_Shirt", '["G_Jacket"]', "/Game/Wardrobe/SKM_Shirt_Vale", json.dumps(P), True, False)
wd.push_garment_out_of_mesh(BP, "G_Sweater", "Face", "/Game/Wardrobe/SKM_Sweater_Vale", json.dumps(dict(P, under=["G_Jacket"])), False)
# 8. check: numbers, then pictures
print(json.loads(wd.measure_poke_through(BP, '["G_Jacket","G_Shirt","G_Pants"]', json.dumps(dict(P, lods=[0, 1, 2])))))
print(json.loads(wd.capture_fit_views(BP, SEATED, 1.0, '["front","three_quarter","side","back","low_front"]', "", '{"lod":0}')))
# 9. save the copies, the bodies and the Blueprint by name
```

## Check in the poses the game uses

- **Asset-sampled poses are wrong.** The body's post-process AnimBP (`ABP_Body_PostProcess`) drives about 60
  corrective bones (thigh fwd/bck/in/out, knee, latissimus) from the pose. Sampling a clip on the asset leaves them at
  their reference offset: a seated pose was **14 cm** off at `thigh_bck_l` (measured again by `dump_runtime_poses`, which
  reports `runtime_vs_asset_max_cm` per pose). A fit solved on asset poses is wrong at thighs, knees and armpits.
- `push_body_under_garments` and `measure_poke_through` evaluate clips at runtime by default (the Blueprint in a
  private preview world, every anim graph evaluated). The older tools (`hide_body_under_garments`,
  `push_garment_out_of_mesh`) sample on the asset unless you pass `"evaluate": "runtime"` or a pose file.
- The exact game pose: in PIE, `wd.dump_actor_pose("BP_Vale", "Body", "", "seated line 3")` at each moment that matters
  appends to a pose file; pass that file.
- Use every clip the character plays (`{"anim": clip, "stride": 0.5}`), then look: `capture_fit_views` seated and
  standing, front, three-quarter, back and a low camera for skirts, at LOD 0, 1 and 2 (`{"lod": 1}`). A capture is no
  substitute for the game's own cameras on the placed actors (section Rebuilds).

## The body under the clothes: push or delete

| Garment | Do | Why |
|---|---|---|
| Open or loose (dress, gown, slit, open collar, short sleeves, bare back) | `push_body_under_garments` on the uncut full body | any camera that sees under or into the garment sees a hole: a see-through crotch from a low camera, a black patch on an upper arm where a trim took 16 triangles too many |
| Closed, always covering (trousers, long sleeves under a jacket, shoes) | `hide_body_under_garments` | cheap; pocket openings let skin through every other test |

`push_body_under_garments` moves every skin vertex that comes out through the cloth in any pose inward (at the bind
pose) to `margin` cm under it, spread over a few rings so no crease shows; nothing is deleted. Limit where skin shows
(`limits: [{"box": ..., "max_push": 0.8}]` for leg skin in a slit) and keep regions out (`exclude_boxes`,
`exclude_bones`; head, neck and hands are excluded by default: a hand on the lap is not poke-through). Measured in the
jam: a slip dress needed 551 / 249 / 98 vertices moved at LOD 0 / 1 / 2, a floor-length gown 1892 / 744 / 306, at most
4 cm. Report: per LOD and pass, `outside_garment` and `worst_cm`; aim for 0 outside in every pose, then look.

A dress, a bare back or an open collar shows skin the BUILT body no longer has (the Creator removes the faces under its
default tee and shorts): start from `make_full_body_from_dna`. It matches each built LOD to the DNA LOD with the same
vertices (built LOD n was DNA LOD n for one character and n+1 for another) and reports `matched_percent` /
`max_offset_cm` per LOD. Pass the body DNA ASSET as `DnaMeshPath` when you have one; never generate from an exported
`.dna` FILE (a fixed legacy coordinate config: 0.4% of the vertices matched). Keep the uncut body and name each variant
for its outfit, so a costume change is a pointer swap.

## LODs: garments and the body

- The body's LOD settings drop bones: an Optimized body does not evaluate ~144 hand helper joints even at LOD0, and
  drops corrective and helper bones at LOD1-2 (`describe_skeletal_mesh(BODY)`: `bones_dropped_since_previous_lod`).
  Garment vertices weighted to a bone the body does not evaluate collapse (cuff spikes to the floor) or follow a bone
  that stopped updating (streaks at distance).
- A garment LOD is drawn at more than one body LOD: garment LOD n at body LOD n, and its LAST LOD at every remaining
  body LOD (a one-LOD dress is drawn at body LOD 0, 1 and 2). A LOD generated by reduction rides on its base LOD's
  weights, so the base LOD is checked against every body LOD its generated LODs are drawn at. `fix_unevaluated_bone_weights`
  and the attach check do exactly that (a 4-LOD boot with generated LOD1-3 had 4106 LOD0 vertices on bones the body
  drops by LOD2). Fixing a base LOD against lower body LODs moves weights to coarser bones at LOD0 too; give the
  garment real LODs if that shows.
- Give garments real LODs: a baked Chaos outfit comes as one LOD (30.9k triangles at every distance in one case).
- Force LOD 1 and LOD 2 in a capture (`capture_fit_views` `{"lod": 2}`): the only way to see streaks before a player.

## Rebuilding a dressed character

A Creator rebuild regenerates `BP_<Name>` (added components vanish, edited ones reset: Body mesh, Face anim class,
override materials, tags) and rewrites the built body, grooms and materials.

```python
mh.build(c, "Optimized", "Medium", "/Game/MetaHumans", "", True)          # True = preserve: snapshot + restore
print(json.loads(wd.post_build_fixup(BP, json.dumps({"brow_opacity": 2.0, "face_anim_class": "/Game/Anim/ABP_Face_Slot"}), False)))
print(json.loads(wd.audit_placed_instances(BP, False, "/Game/Maps/L_Main")))   # then True to clear what should not differ
# by hand: wd.snapshot_blueprint_components(BP, path) ... build ... wd.restore_blueprint_components(BP, path, '["LODSync.*"]', True, True, True)
```

- The snapshot records every component property that differs from the class default **and**, whatever its value, the
  mesh (`SkinnedAsset`, `StaticMesh`, `GroomAsset`, `BindingAsset`), `AnimClass`, `OverrideMaterials`, `bVisible` and
  `bHiddenInGame`: a cleared default-outfit mesh equals the class default, and before this was recorded the rebuild's
  default tee came back. The restore re-creates missing components, re-applies differing values and never deletes.
  Read its `applied` list: a stale recorded value (old brows, old override materials) comes back too, and a slot that
  was empty before the build is emptied again (hair or an outfit newly chosen in the Creator): set those back, or
  restore by hand with skip patterns (`'["Hair.GroomAsset"]'`).
- `post_build_fixup` redoes what a rebuild throws away at asset level: groom LOD fill on every groom (brows made
  visible), the brow groom's `OpacityNear/Far` (pale brows were fixed with 2.0), per-asset texture caps
  when you pass them (`"texture_caps": {"T_Body_*_VT": 1024, "T_Head_*_VT": 2048}` for a web or low-end target; a cap
  only lowers; never flips virtual texture streaming), the Face anim class.
  It does **not** redo body fits: run `push/hide_body_under_garments` again on the new body.
- **Check the placed actors, not a fresh spawn.** A level instance kept an old per-instance `Body` mesh override: holes
  in the chest and shoulders in every real shot while every fresh-spawn check passed; another kept a stale Face anim
  class. `audit_placed_instances` lists components whose values differ from a fresh spawn of the Blueprint
  (construction-script values are not counted) and clears them with `bClearOverrides`.
- A build leaves a few dirty `T_PreBakedGroom_*` packages never written to disk (`build` lists them): leave them unsaved.

## Accessories

| Where it sits | Attach |
|---|---|
| On the head (glasses, earrings) | static mesh on the Face component's `head` socket (`attach_static_mesh_to_blueprint`), pivot at the bone origin, rotation undoing the bone's rest rotation (pitch -90) |
| Round the neck (choker) | static mesh on Body's `neck_01` socket |
| On cloth, stiff (brooch, pocket square) | `add_skinned_accessory(..., "rigid", '["G_Jacket"]', ...)`: one bone, no kinks, rides the lapel |
| Hanging on cloth (chain, scarf) | `add_skinned_accessory(..., "surface", '["G_Shirt"]', ...)`: weights by closest point from the cloth |

`RelativeJson` places the static mesh exactly as it would sit on that bone's socket. The new mesh's weights are moved
off LOD-dropped bones and it is attached with leader pose. A piece along the body's centre line over an opening (a tie
on an open collar) has no stable surface: three rounds of fixes in the jam and the owner had every tie removed. Check
every accessory in the real close-up frame before building it (earrings hidden by hair, a pocket square below frame).

## Groom LODs

Optimized builds leave groom LODs with nothing to draw: cards only at LOD3, helmets only at LOD5/7, so hair went bald
at LOD4 and LOD6 and brows vanished from LOD4 on (screen size 0.0625: wide shots).
`wd.fill_groom_lods(groom, True, False)` copies the nearest LOD's cards or helmet into every empty LOD (`True` also turns
hidden LODs into visible cards: the brows); `post_build_fixup` runs it on every groom of the Blueprint. The bindings
rebuild by themselves in about a minute. Judge hair in PIE: a scene capture may not draw grooms.

## Gotchas

- **Frustum culling**: a leader-pose garment uses its OWN bounds unless `bUseBoundsFromLeaderPoseComponent`, and those
  sit at the rest pose: it vanished in close-ups. The attach tool sets it.
- **The face mesh's neck skirt** reaches the collarbones and pokes through V-necks, collars and jacket backs. The face
  cannot be edited (DNA): push the garment. Outward comes from the skeleton, never from triangle winding (the skirt's
  winding points INTO the skin on 179 of 186 triangles; a winding-based push tore a jacket). Pushing a crew neck can
  tear the collar: limit it with `region`, and always pass the outer layers in `under`. With bare shoulders the skirt's
  edge shows as a faint shading line no garment fix removes: plan a neckline or a necklace over it.
- **The default grey sports top** is baked into the body and the face's neck skirt textures unless
  `bShowTopUnderwear` is off BEFORE the build (step 0). Turning it off, with the face and body skin textures of one
  preset, removed both the grey band and a skin-tone step at the neck. Inpainting baked textures is the fallback for a
  build you cannot redo.
- **Garment skeleton vs bind pose**: some packs share the generic `metahuman_base_skel`, whose reference pose is 4-8 cm
  off the garment's own bind pose. The wardrobe tools skin from each MESH's reference skeleton; your own Python must too.
- **Layered garments**: drop the inner garment's hidden triangles in the rest AND seated pose (`trim_inner_garment`),
  or its colour flecks through the outer one. A knit hem over a belt: pull the hem inside the waistband instead.
- **Script-spawned actors do not pose in the editor**: `override_animation_data` leaves the bones at rest. Use
  `capture_fit_views` (preview world) or PIE; never judge a fit on an editor-level actor.
- **Sitting in a long skirt**: a skirt weighted to the legs cannot wrap crossed legs. What shipped: a second mesh
  variant draped over the seated pose, switched by the Blueprint from its thigh angles.
- Nothing is saved by the editing tools. Save the copies, the bodies and the Blueprint by name.
