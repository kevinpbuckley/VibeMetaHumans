---
name: metahuman-wardrobe
display_name: MetaHuman wardrobe (dress a built MetaHuman)
description: Dress a BUILT MetaHuman Blueprint in marketplace / Fab skeletal garments with unreal.MetaHumanWardrobeService and MetaHumanCharacterService.attach_skeletal_mesh_to_blueprint - leader-pose garments, the unevaluated-hand-bone cuff spikes, the full-topology body for dresses and open collars, deleting the skin under the clothes, pushing a neckline out over the face mesh's neck skirt, surviving a Creator rebuild (snapshot/restore, build preserve), groom LODs that go bald. Use for "put this jacket on the character", "the clothes poke through", "spikes from the sleeves", "skin shows through the shirt", "a rebuild lost my clothes", "the hair disappears at distance".
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
  - poke through
  - clipping
  - neck skirt
  - hide body
  - dress
  - rebuild lost components
  - groom lod
---

# Dressing a built MetaHuman (wardrobe)

```python
import json, unreal
mh, wd = unreal.MetaHumanCharacterService, unreal.MetaHumanWardrobeService
BP, BODY = "/Game/MetaHumans/Vale/BP_Vale", "/Game/MetaHumans/Vale/Body/SKM_MH_Vale_BodyMesh"
```

Learned dressing six characters in Fab garments (2026-11-12). Everything below works on COPIES in your own
folder: never edit the pack's meshes, materials or textures (`/Game/Fab` is refused by the editing tools).

## Pick the method

| | Creator `Outfits` slot (`add_wardrobe_item` + `build`) | Skeletal garment on the built Blueprint (leader pose) |
|---|---|---|
| Fits the body | yes: the outfit's Chaos resize graph refits it | no: keeps the size it was modelled for |
| Needs a rebuild | yes, and a rebuild regenerates `BP_<Name>` | no |
| What went wrong | one Chaos outfit refit from a single source size: the collar sat below the shoulders, ragged | cuff spikes, skin poking through, the face mesh's neck skirt in the neckline (all fixable below) |

Skeletal garments (`*_Male_*` / `*_Female_*` meshes skinned to the MetaHuman body skeleton) are the jam-proof
route. A Chaos outfit made for a different body shape can still be used: give a THROWAWAY duplicate of the
character the outfit, build it into a scratch folder, and use the baked `Clothing/<Name>_Outfits` mesh as a
leader-pose garment on the real Blueprint (it is bound to that character's skeleton and fits).

## Workflow

```python
# 1. copies: mesh -> /Game/.../Wardrobe/Meshes, material variant + 1K texture copies -> your folder
# 2. fix the cuff spikes on the COPY (garment vertices weighted to hand helper joints the body does not evaluate)
print(json.loads(wd.fix_unevaluated_bone_weights("/Game/Wardrobe/SKM_Jacket", BODY, True)))   # dry run: counts
wd.fix_unevaluated_bone_weights("/Game/Wardrobe/SKM_Jacket", BODY, False)
# 3. attach: leader pose + leader bounds + tick-when-rendered + materials; read follower_check / warnings
r = json.loads(mh.attach_skeletal_mesh_to_blueprint(BP, "G_Jacket", "/Game/Wardrobe/SKM_Jacket", "Body", True,
                                                    "/Game/Wardrobe/M_Jacket_v1"))
assert r["follower_check"]["unevaluated_bone_vertices"] == 0, r["warnings"]
# 4. hide the built default outfit component (set_properties on the template, or the Blueprint editor)
# 5. delete the skin under the clothes, on a body COPY, rest + seated pose, then point Body at it
SEATED = {"anim": "/Game/Anims/A_Idle_Seated", "time": 1.0}
opts = {"poses": [SEATED], "delete_boxes": [[[-40, -40, 12], [40, 40, 85]]]}      # trouser legs, Body space cm
print(json.loads(wd.hide_body_under_garments(BP, '["G_Jacket","G_Sweater","G_Pants","G_Shoes"]',
                                            BODY + "_Dressed", json.dumps(opts), True, False)))
# 6. necklines: push the garment out over the face mesh's neck skirt (copy written and assigned); "under" keeps a
#    knit inside the jacket (unrestricted, the push came out through the jacket's shoulders); "region" limits it further
print(json.loads(wd.push_garment_out_of_mesh(BP, "G_Sweater", "Face", "/Game/Wardrobe/SKM_Sweater_Vale",
                                            json.dumps({"poses": [SEATED], "under": ["G_Jacket"]}), False)))
# -> per LOD: violations_before/after, blocked_by_outer (skirt hidden under the jacket anyway), max_move_cm
# 7. look at it standing AND seated (PIE), then save the copies and the Blueprint
```

A dress, a bare back or an open collar shows skin the BUILT body no longer has (the Creator removes the faces
under its default tee and shorts). Get the full body back first, then hide from it:

```python
full = json.loads(wd.make_full_body_from_dna(BODY, BODY + "_Full", "", "/Game/MetaHumans/MH_Vale", False))
opts["source_body"] = full["full_body"]
wd.hide_body_under_garments(BP, '["G_Dress","G_Shoes"]', BODY + "_Dress", json.dumps(opts), True, False)
```

`make_full_body_from_dna` matches each built LOD to the DNA LOD with the same vertices (built LOD n was DNA LOD n
for one character and n+1 for another) and reports `matched_percent` / `max_offset_cm` per LOD. Pass the body DNA
ASSET as `DnaMeshPath` when you have one; with `CharacterPath` it exports one. Never generate from an exported
`.dna` FILE for this: the file route reads with a fixed legacy coordinate config and the mesh lands in another
space (0.4% of the vertices matched).

## Rebuilding a dressed character

A Creator rebuild regenerates `BP_<Name>`: added components vanish and edited ones reset (Body mesh, Face anim
class, override materials, tags).

```python
mh.build(c, "Optimized", "Medium", "/Game/MetaHumans", "", True)          # True = preserve: snapshot + restore
# or by hand:
wd.snapshot_blueprint_components(BP, "C:/tmp/BP_Vale_before.json")
# ... build ...
print(json.loads(wd.restore_blueprint_components(BP, "C:/tmp/BP_Vale_before.json", '["LODSync.*"]', True, True, True)))  # dry run
```

The restore re-creates missing components and re-applies differing recorded values; it never deletes. Asset-level
edits are NOT restored: a rebuild rewrites the built body, grooms and materials, so re-run
`hide_body_under_garments` (it reads the fresh body) and `fill_groom_lods`, and re-apply groom colour through
`set_wardrobe_item_parameters` on the character (never on the built `MI_WI_*`).

## Groom LODs

Optimized builds leave groom LODs with nothing to draw: cards only at LOD3, helmets only at LOD5/7, so hair went
bald at LOD4 and LOD6 and brows vanished from LOD4 on (screen size 0.0625: wide shots).
`wd.fill_groom_lods("/Game/MetaHumans/Vale/Grooms/Hair_S_SlickBack", True, False)` copies the nearest LOD's cards
or helmet into every empty LOD (`True` also turns hidden LODs into visible cards: the brows). Run it on each of
the build's grooms; the bindings rebuild by themselves in about a minute.

## Gotchas

- **Cuff spikes**: the Optimized body's LOD settings drop ~144 hand helper joints (metacarpal slides, finger
  bulge/side/palm). They sit at the component origin, so garment vertices weighted to them stretch to the floor.
  `attach_skeletal_mesh_to_blueprint` reports them (`follower_check`); `fix_unevaluated_bone_weights` moves them to
  the nearest evaluated ancestor on every LOD, like the body does. A Fab jacket had 133-193 such vertices.
- **Frustum culling**: a leader-pose garment uses its OWN bounds unless `bUseBoundsFromLeaderPoseComponent`, and
  those sit at the rest pose: it vanished in close-ups. The attach tool now sets it.
- **The face mesh's neck skirt** reaches the collarbones (and the trapezius) and pokes through V-necks, collars and
  jacket backs. The face cannot be edited (DNA), so push the garment instead. Do not trust triangle winding for
  "outward": a Python push through a GeometryScript read got normals pointing INTO the skin on 179 of 186 skirt
  triangles and tore a jacket. The tool takes outward from the skeleton (away from the nearest bone) and reports how
  many triangle normals it had to flip. Pushing a crew neck can tear the collar; limit it with `region`, and always
  pass the outer layers in `under`.
- **Baked skin carries the default tee**: the Creator paints its grey sports top into the baked body and the face's
  neck skirt textures. An open neckline shows a grey band; inpaint copies of `T_Body_*_VT` / `T_Head_LOD*` and set
  them as override materials on the Face component (slots 6 and 7 on an Optimized face).
- **Garment skeleton vs bind pose**: some packs share the generic `metahuman_base_skel`, whose reference pose is
  4-8 cm off the garment's own bind pose. The wardrobe tools skin from each MESH's reference skeleton; your own
  Python LBS must too.
- **A Chaos outfit's render mesh** (`SKM_*` inside an `OA_` pack) is often skinned to `root` only: it cannot be a
  leader-pose garment until weights are transferred from the pack's source body.
- **Seated poses**: `override_animation_data` on a script-spawned actor does not evaluate in the editor world
  (bones stay at rest). The wardrobe tools evaluate AnimSequences directly (`poses`); for pictures use PIE.
  RigLogic correctives are not evaluated in those poses.
- **Layered garments** (shirt under jacket): drop the inner garment's hidden triangles on a per-character copy, in
  the rest AND seated pose, or its colour flecks through the outer one.
- Nothing is saved by these tools. Save the copies, the dressed body and the Blueprint by name.
