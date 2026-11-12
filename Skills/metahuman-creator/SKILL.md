---
name: metahuman-creator
display_name: MetaHuman Creator (characters)
description: Create and edit MetaHuman Character assets with unreal.MetaHumanCharacterService - blank template or preset, body shape and body type, skin tone and skin properties, eyes, makeup, teeth and eyelashes, face coefficients, sculpt-landmark fitting to a reference picture, Mesh to MetaHuman imports (Identity, DNA, template meshes), cloud auto-rig and high-res textures, wardrobe items and their per-item settings (groom colour, ombre, highlights), custom grooms as Creator Hair wardrobe items (Legacy02 binding, copied WI, WardrobePaths folder), built-Blueprint hair/groom tools, non-blocking build and save, build to a Blueprint, spawn in the level, export DNA. Use for "make a MetaHuman", "change her hair/skin/body", "colour his beard", "rig and build the character", "add hair to the built character", "make my groom selectable in the Creator".
vibeue_classes:
  - MetaHumanCharacterService
  - MetaHumanSetupService
  - MetaHumanObjectService
unreal_classes:
  - UMetaHumanCharacter
  - UMetaHumanCharacterEditorSubsystem
  - UMetaHumanCollection
  - UMetaHumanWardrobeItem
  - UGroomAsset
  - UGroomBindingAsset
keywords:
  - metahuman creator
  - create metahuman
  - body shape
  - skin tone
  - eye color
  - makeup
  - hair groom
  - hair color
  - groom color
  - face landmarks
  - add hair
  - custom groom
  - custom hair
  - groom binding
  - wardrobe path
  - card hair
  - alembic groom
  - groom turntable
  - auto rig
  - assemble
  - build metahuman
  - deferred save
---

# MetaHuman Creator (characters)

```python
import json, unreal
mh = unreal.MetaHumanCharacterService
```

## Mental model

A **MetaHuman Character** asset (`/Game/MetaHumans/MH_Name`) stores face + body state, the look
settings (skin, eyes, makeup, head model = eyelashes/teeth, face evaluation = head scale) and an
internal wardrobe **collection** (slots → items). Edits go through the Creator editor subsystem; every
service tool opens the asset for edit on demand, commits the change and refreshes the preview. The
character becomes usable for animation after a cloud **auto-rig** (`rig_state: "Rigged"`), and
becomes a placeable actor after **build** (assembly) writes `BP_<Name>` plus meshes, materials and
grooms under `/Game/MetaHumans/<Name>`.

## Workflow

1. `create_character(package_path, name, preset_path)` — `preset_path` empty = blank template.
   Presets live in `/MetaHumanCharacter/Optional/Presets` (optional content) or can be any existing
   character; the new asset copies its state.
2. Shape the body: `set_body_shape(c, masc_fem, fat, muscularity, height_cm)` (0..1 each, negative
   = unchanged; height 135..220). Finer control: `get_body_constraints` → `set_body_constraints(c,
   '{"Height":182,"Upper Arm Length":0.6}')`. Fixed legacy proportions: `set_body_type(c, "m_tal_nrw")`;
   back to parametric: `set_body_type(c, "BlendableBody")`.
3. Shape the face: `get_face_coefficients` / `set_face_coefficients` (PCA vector; small deltas),
   `set_settings(c, "face_evaluation", '{"HeadScale":1.05}')`, `reset_face` to start over.
   Likeness from a scan or footage: `import_from_identity` (an Identity from
   `MetaHumanIdentityService`), `import_from_face_dna`, `import_from_template`.
   Likeness from a single reference PICTURE (no 3D scan, 2026-10-07): `get_face_landmarks(c)` reads the
   Creator's own sculpt landmarks (`{"landmarks":{"0":[x,y,z],...}, "likely_ears":[...], "likely_neck":[...],
   "likely_skull":[...]}`, centimetres, edit frame) - match them to a MediaPipe measurement of a front capture
   of the preview actor (a face-fit script does this), then
   `move_face_landmarks(c, '{"23":[0.3,0,-0.1],"41":[-0.2,0,0.15]}')` moves just those indices (deltas; pass
   `bAbsolute=true` for absolute positions) and holds every OTHER landmark in place by default
   (`HoldJson="auto"`; `"none"` lets everything else drift, a JSON array of indices holds exactly those),
   repeating translate+commit until every residual is under `ToleranceCm` (default 0.06) or
   `MaxIterations` (12) runs out - it returns the per-iteration residuals so you can see it converge.
   It REFUSES while the character wears any groom (spawning the Creator preview with grooms on hung the
   editor for minutes, 2026-10-06) and while the face rig is on (`remove_face_rig` first), and refuses any
   target landmark the position heuristic calls an ear unless `bAllowEars=true` - moving ear landmarks
   crumpled one character's ears (2026-10-07; `likely_ears` is read off positions each call, never a fixed
   index list, because the landmark SET differs per preset - 79 on one preset, 125 on another).
4. Look: `set_skin_tone(c, u, v)` (u = lightness 0 light → 1 dark, v = redness 0 → 1),
   `set_eye_color(c, u, v, "both")` (u = temperature cool → warm, v = brightness dark → light), and
   JSON patches for everything else:
   - `set_settings(c, "skin", '{"Skin":{"Roughness":1.0,"FaceTextureIndex":3},"Freckles":{"Density":0.6,"Mask":"Type2"}}')`
   - `set_settings(c, "eyes", '{"EyeLeft":{"Iris":{"IrisPattern":"Iris004"},"Pupil":{"Dilation":1.0}}}')`
   - `set_settings(c, "makeup", '{"Lips":{"Type":"Hollywood","Color":{"R":0.4,"G":0.02,"B":0.03,"A":1}},"Eyes":{"Type":"CatEye"}}')`
   - `set_settings(c, "head", '{"Eyelashes":{"Type":"LongCurl"},"Teeth":{"ToothLength":0.3,"Overbite":0.1}}')`
   Shapes: `get_settings(c, section)` or `MetaHumanObjectService.describe_struct("MetaHumanCharacterMakeupSettings")`.
5. Wardrobe: `get_wardrobe(c)` lists slots, items in the collection and the current selection. Slots and the
   principal asset each one's items wrap: `Hair, Eyebrows, Eyelashes, Mustache, Beard, Peachfuzz`
   (`GroomBindingAsset`), `Outfits` with its user-facing `Top Garment` / `Bottom Garment` (`ChaosOutfitAsset`),
   `SkeletalMesh` (`SkeletalMesh`, skeletal clothing, multi-select). `add_wardrobe_item(c, "Hair",
   "/MetaHumanCharacter/Optional/Grooms/Bindings/Hair/WI_Hair_L_StraightBangs", True)` adds and selects;
   `select_wardrobe_item(c, "Hair", "")` clears a slot. Selecting REPLACES the slot's selection
   (`SetSingleSlotSelection`), so layering several `SkeletalMesh` pieces needs the instance's
   `try_add_slot_selection` (no service tool yet). Items come from
   `MetaHumanSetupService.list_assets("wardrobe", "")` or `list_wardrobe_items("Hair")` (groom slots only;
   see **Hair on a built MetaHuman** below for a SECOND groom layered onto a built Blueprint, and
   **Custom hair as a Creator wardrobe item** for making your own groom wearable here).
   Per-item settings - groom colour (Melanin, Redness, Roughness, Whiteness, Lightness, DyeColor, plus the Ombre,
   Regions and Highlights groups when the item's groom pipeline enables them: `bSupportsOmbre` / `bSupportsRegions`
   / `bSupportsHightlights`, Epic's spelling, all on by default) and any outfit parameters - live on the character, the same values the Creator's
   Hair / Wardrobe panels edit: `get_wardrobe_item_parameters(c, "Hair")` (empty slot = every worn item) returns
   each item's `parameters`, `types`, `defaults` and `overridden` names;
   `set_wardrobe_item_parameters(c, "Hair", '{"Melanin":0.48,"Redness":0.53,"DyeColor":[1,0.2,0.2]}')` sets them
   (`ItemName` picks one of several items in a slot; `bClearOverrides=true` resets to the item's defaults first).
   Colour grooms HERE, never on a built Blueprint's `MI_WI_*` materials: every rebuild regenerates those, while
   the character's values go into every build. Overrides belong to the worn item - select the groom first, then
   set its colours.
   While the character is open for editing (the Creator's tab, or `begin_edit`), the Creator works on a PREVIEW
   copy of the collection and copies it back over the asset after each of its own edits. The wardrobe tools read
   and write that live copy and commit it, so they are safe beside an open Creator tab and the two never
   overwrite each other (before 2026-10-07 they wrote the asset directly, and the Creator's next edit reverted
   them: a Blueprint built with one set of grooms while the MetaHuman showed another).
6. Rig. Presets and edited faces arrive **Unrigged**; the rig comes from Epic's cloud auto-rig:
   `request_auto_rig(c, "JointsOnly", False)` (`JointsAndBlendShapes` for the heavier cinematic rig),
   then poll `get_summary(c)["rig_state"]` (or the shaped `get_auto_rig_status(c)`: `running|done|
   failed_or_not_requested`) until `Rigged`. The first request opens an Epic account
   sign-in page in the browser (device code, expires after ~10 min) - a human must complete it.
   Never pass `bBlocking=True` unless someone is at the machine: a blocking request parks the whole
   editor (and this MCP) until the login finishes or expires (`Login Failed`).
   Optional `request_texture_sources(c, True)` downloads 2k-8k skin sources (cloud, same login).
   **Offline fallback (no login):** import the engine's archetype head rig as a whole rig -
   `import_from_face_dna(c, "<UE>/Engine/Plugins/MetaHuman/MetaHumanCoreTechLib/Content/ArchetypeDNA/SKM_Face.dna", True, False)`
   gives `rig_state: Rigged` with a generic (archetype) face shape; skin, eyes, makeup, hair and
   body stay. Then clear the texture gate the same way Epic's own pipeline tests do:
   `MetaHumanObjectService.set_properties(c, '{"bHasHighResolutionTextures": true}')` (the build uses
   the locally synthesized textures; wrinkle/animated maps are skipped). Re-rig the real face later
   with `remove_face_rig(c)` + `request_auto_rig(...)` once signed in.
7. Build: `can_build(c)` then `build(c, "Cinematic", "Cinematic", "/Game/MetaHumans", "")`. Pipeline
   `Optimized` + quality `Low|Medium|High` for games; `UEFN` for Fortnite. Result lists the
   Blueprints. `spawn_in_level(c, loc, rot, False)` places the live preview actor instead (it
   follows further edits).
   A build can take 4-5 minutes (texture bakes, groom bindings) and a client that waits on one tool
   call that long can time out and drop its Unreal tools (2026-10-06): `build_async(c, ...)` queues it
   on a later editor tick and returns at once (`status: "queued"`), then poll `get_build_status(c)`
   (`queued|running|done|failed`, `seconds`, `blueprints`) rather than waiting on `build` itself. The
   build still blocks the game thread once it starts - this only frees the RPC call, not the editor.
8. `save(c)` and `end_edit(c)`. **Never call `save` (or `MetaHumanCharacterService.save`) on a
   character that is still in an edit session and expect it to return quickly: it now ROUTES to
   `save_deferred` automatically and says so (`routed`), because a save made in the same call that
   changed a Creator character deadlocked the editor four times (2026-10-06/07) - a game-thread
   sleep-poll inside `UPackage::Save` for async texture-synthesis work that only the game thread
   advances. `save_deferred(c, delay_seconds=15, min_frames=45)` ends the edit session now and saves
   on a tick at least that far away; poll `get_save_status(c)` (`pending|skipped|saved|failed`) for the
   result - do not assume it saved just because the call returned. A newer `save_deferred` on the same
   character re-arms the timer; if it is back in an edit session when the tick fires, that save is
   skipped (call `save_deferred` again once you are done).**

## Hair on a built MetaHuman (2026-10-07)

A groom from `add_wardrobe_item` (step 5) is baked into the FIRST `build()`. To add, swap or layer a
groom on an ALREADY-BUILT Blueprint without rebuilding the whole character (a working recipe:
a second "spiky crown" groom layered over the base fringe, keeping every other polish round's work):

```python
b = json.loads(mh.build(c, "Cinematic", "Cinematic", "/Game/MetaHumans", ""))
bp = b["blueprints"][0]                                                  # BP_<Name>
items = json.loads(mh.list_wardrobe_items("Hair"))["items"]              # path, name, slot per groom
r = json.loads(mh.add_groom_to_blueprint(bp, "HairTop", items[0]["path"], "Face", "Hair", True))
#   -> groom, binding, binding_duplicated, binding_saved, material_slots, materials_copied, compiled
```

`GroomAssetPath` takes a groom Wardrobe Item (`WI_Hair_...`, as above), the plugin's own
`GroomBindingAsset` directly, or a bare `GroomAsset` (e.g. from `import_groom`, which has no plugin
binding to retarget - the new component comes back unbound, no RBF transfer onto this face).
For a wardrobe item or plugin binding, the plugin's binding is DUPLICATED into
`<Blueprint's folder>/Accessories/<ComponentName>/` and retargeted onto the Blueprint's BUILT face
mesh - the same thing `build()`'s own groom pipeline does internally (`DuplicateObject`,
`SetTargetSkeletalMesh`, which kicks an ASYNC RBF rebuild). `MaterialFromComponent` ("Hair" above)
copies an existing groom component's override materials onto the new one's matching slots (by name,
then by position) so a dyed/tinted look carries over. `bSaveBinding=True` (the default) saves the
duplicated binding; pass `False` and save it yourself **at least a minute later** if you are about to
make several such calls in a row - saving a binding mid-build can hang the editor the same way a
Creator save can. `remove_component_from_blueprint(bp, "HairTop")` takes it back off.

Full signatures: `add_groom_to_blueprint(bp, component, groom_or_item, target_component="Face",
material_from_component="", save_binding=True, socket_name="", inverse_socket_transform=False,
relative_location=(0,0,0), relative_rotation=(0,0,0))` and `import_groom(abc, folder, name, rigid_binding=True,
hair_width_cm=0.013, tip_scale=0.6)` (width <= 0 keeps the file's widths; nothing is saved - save in a later call).
A groom made outside Unreal (guides/strands only; the result reports each group's curve/guide counts) goes on a
built Blueprint RIGIDLY on the head bone - the three-argument `import_groom` already sets Rigid binding, a
0.013 cm width and tip 0.6:

```python
g = json.loads(mh.import_groom("E:/hair/Custom.abc", "/Game/MetaHumans/Dryad/Accessories/Hair", "Groom_Custom"))
# later call: save the groom, then
mh.add_groom_to_blueprint(bp, "HairCustom", "/Game/MetaHumans/Dryad/Accessories/Hair/Groom_Custom", "Body", "", False, "head", True)
```

`socket_name="head"` with `inverse_socket_transform=True` cancels the head bone's reference-pose transform, so a
groom authored in the mesh's component space lands where it was authored and follows the head. Hide the stock
`Hair` component yourself if the custom groom replaces it. Do not bind a dense custom groom to the face mesh
instead: an RBF bind of ~40k strands kept 12 cores busy for 16+ minutes.

**Never** spawn a Creator preview (`spawn_meta_human_actor` / any front-capture helper) on a character
that still wears grooms in its wardrobe collection - that is a DIFFERENT thing from the built
Blueprint's own grooms above, and doing it hung the shared editor for about 9 minutes (2026-10-06):
`move_face_landmarks` already refuses for this reason; clear the Creator's groom slots (step 5),
finish sculpting, then add grooms back before building.

## Custom hair as a Creator wardrobe item

Any custom groom can become a choice in the Creator's Hair slot, worn like library hair: it follows face
edits, takes the Hair colour settings and goes into every build (unlike the built-Blueprint path above).
The Hair slot lists only Wardrobe Items (`WI_*`) whose Principal Asset is a `GroomBindingAsset`; a dropped
`GroomAsset` does nothing. Library grooms are bound to one of the template heads in
`/MetaHumanCharacter/Optional/Grooms/GroomMesh` (`SKM_Groom_Head_Legacy01`, `SKM_Groom_Head_Legacy02` or
`MH_Groom_Head`; in 5.8 the Hair bindings split 18 Legacy02 / 17 Legacy01 / 1 MH_Groom_Head), and the Creator's
groom pipeline duplicates that binding and retargets it onto each character's face. The head you author on must
match the binding you copy: `Hair_S_Clean_Binding` (used below) is bound to Legacy02. So: author in Legacy02's
space, copy that binding and its item, point them at the custom groom, and put the item where the Creator looks.

1. **Author** on `SKM_Groom_Head_Legacy02`, in its component space (centimetres, face toward +Y). Each curve's
   FIRST point is its root; at most 255 points per curve (more are trimmed); roots on the scalp (MetaHuman groom
   verification warns past 4 mm).
2. **Import** `import_groom(abc, folder, name, False, 0.011, 0.5)`. `bRigidBinding=False` keeps the import
   default, **Skinning**, which Creator hair needs (it is skinned through its binding; Rigid is only for the
   unbound built-Blueprint path above). The two numbers turn the width override on (with no widths in the file
   every strand gets 0.01 cm, and `hair_width` does nothing unless `hair_width_override` is on) at root scale 1,
   tip 0.5. The default conversion places each Alembic point unchanged (1 unit = 1 cm, no axis swap), but
   `GroomImportOptions` is a per-user config object: a rotation or scale once confirmed in the import dialog is
   reused by every later import, scripted ones included. A groom that lands turned or scaled -> import with the
   raw task below and set `conversion_settings` yourself. An `.abc` that contains any polygon mesh, or a project
   without the "Alembic Groom Importer" plugin, goes to the geometry Alembic importer instead.
3. **Match the library** (observed in 5.8 on the library hair: width ~0.011 cm, root 1, tip 0.4-0.65, binding
   type Skinning). Copy the library groom's `hair_groups_materials` (`MI_Hair`, `MI_Hair_Cards`, `MI_Hair_Helmet`):
   a group whose slot name matches none falls back to slot 0, `MI_Hair`.
4. **Binding**: duplicate `Hair_S_Clean_Binding`, set `b.groom = g` (the property's BlueprintSetter `SetGroom`,
   which also moves the binding's change subscription to the new groom) and build it with
   `b.build(unreal.OnGroomBindingAssetBuildComplete())` (asynchronous; the Python overload needs the delegate).
   `set_editor_property("groom", g)` also rebuilds, but leaves the binding subscribed to the library groom. A
   binding is accepted only for the same groom with the same group count.
5. **Item**: duplicate `WI_Hair_S_Clean` - it keeps the library's groom pipeline (a `WI_` without a pipeline,
   including one the Creator auto-creates, fails validation and Wear skips it) - and repoint `principal_asset`.
   The copy inherits the library item's pipeline options. `WI_Hair_S_Clean` sets no `BakedGroomTexture`, so the
   copy gets no hair baked into the face at far LODs (from `GroomTextureMinLOD`, 5); copying an item that has one
   (e.g. `WI_Hair_L_Straight`, `WI_Hair_S_Messy`) bakes THAT style onto the face - replace it or clear it. It also
   inherits the colour-group flags (`WI_Hair_S_Clean` stores a non-default `bSupportsRegions`, so its copy may have
   no Regions group): read them with `w.get_editor_property("pipeline").get_editor_property("supports_regions")`
   (likewise `supports_ombre`, `supports_hightlights`). And it shows the Short Clean thumbnail until you set
   `thumbnail_image` / `thumbnail_name`.
6. **Save in a LATER call** (groom, binding, item), not in the call that made them.
7. **Register the folder** so the Creator's Hair panel lists it. Folders are scanned NON-recursively, and for every
   binding directly in a watched folder with no `WI_` pointing at it the Creator creates a pipeline-less one that
   fails validation - so keep only the `WI_` files in the watched folder and each groom + binding in a subfolder.
   `Config/DefaultMetaHumanCharacter.ini` (read at editor start; in a running editor add the same entry to the
   "New Wardrobe Default Asset Paths" project setting, which refreshes the wardrobe at once; Python cannot build
   the entry struct):
   ```ini
   [/Script/MetaHumanCharacterEditor.MetaHumanCharacterEditorSettings]
   +WardrobePaths=(ContentDirectoryToMonitor=(Path="/Game/Characters/Wardrobe/Hair"),SlotName="Hair",ClassesToFilter=("/Script/HairStrandsCore.GroomBindingAsset"),bPureVirtual=False)
   ```
   Other slots register the same way (`Outfits` + `/Script/ChaosOutfitAssetEngine.ChaosOutfitAsset`, `SkeletalMesh`
   + `/Script/Engine.SkeletalMesh`), but there is no library item to copy for them (the library `SkeletalMesh`
   section is pure virtual), and a mesh or outfit dropped straight into a watched folder gets an auto-created `WI_`
   with NO pipeline, which fails validation. Give such a `WI_` a pipeline, an instance of the Blueprint class
   `DefaultSkeletalMeshPipeline` / `DefaultOutfitPipeline` in `/MetaHumanCharacter/BuildPipeline/DefaultWardobePipeline/`
   (Epic's spelling): `w.set_editor_property("pipeline", unreal.new_object(cls, outer=w))` with `cls` the loaded
   `..._C` class; setting `pipeline` runs `SetPipeline`, which adds the default editor pipeline (per source, not
   tested). Skeletal clothing must be skinned to the MetaHuman body
   skeleton (MetaHuman verification checks it) and is not refit to the body; Chaos outfits are, and warn without a
   `BodyHiddenFaceMapTexture`. Per source the Creator's "Individual Assets" drop box wraps a dropped asset with the
   fallback pipeline from the settings' default asset pipelines - not tested.
8. **Wear and colour** with step 5's tools: `add_wardrobe_item(c, "Hair", WI, True)` (works by path, registered or
   not), then `set_wardrobe_item_parameters` - a new groom wears the default auburn until coloured; which of the
   Ombre / Regions / Highlights groups it offers follows the copied pipeline's flags (step 5).
   `list_wardrobe_items` reads only the plugin's optional content; find project items with
   `MetaHumanSetupService.list_assets("wardrobe", "/Game/...")`. Wardrobe changes do not need the rig removed.
9. **Strands only, so far.** A custom Alembic groom has no card or mesh LODs and no baked texture, while the copied
   pipeline keeps the library's LOD transition (strands -> cards -> ...). Only Cinematic builds and the Creator
   preview were checked: Optimized / Low builds and far LODs may show no hair. Generate cards in the groom (Hair
   Cards) or check a low-quality build before relying on it.

```python
eal = unreal.EditorAssetLibrary
LIB = "/MetaHumanCharacter/Optional/Grooms/Bindings/Hair/"
ROOT = "/Game/Characters/Wardrobe/Hair"                     # watched folder: WI_ files only
SUB, NAME = ROOT + "/Custom01", "GR_Hair_Custom01"           # groom + binding in a subfolder
GROOM, BIND, WI = SUB + "/" + NAME, SUB + "/Hair_Custom01_Binding", ROOT + "/WI_Hair_Custom01"
# call 1
mh.import_groom("E:/hair/Custom01.abc", SUB, NAME, False, 0.011, 0.5)
g = unreal.load_asset(GROOM)
lib_groom = unreal.load_asset(LIB + "Hair_S_Clean_Binding").get_editor_property("groom")
g.set_editor_property("hair_groups_materials", lib_groom.get_editor_property("hair_groups_materials"))
b = eal.load_asset(BIND) if eal.does_asset_exist(BIND) else eal.duplicate_asset(LIB + "Hair_S_Clean_Binding", BIND)
b.groom = g                                                     # SetGroom
b.build(unreal.OnGroomBindingAssetBuildComplete())              # async rebuild
w = eal.load_asset(WI) if eal.does_asset_exist(WI) else eal.duplicate_asset(LIB + "WI_Hair_S_Clean", WI)
pa, p = w.get_editor_property("principal_asset"), b.get_path_name()
pa.import_text('(Asset="%s",AssetIdentifier="%s")' % (p, p))   # EditorOnlyAssetReference
w.set_editor_property("principal_asset", pa)
assert w.get_editor_property("pipeline")                        # the copied groom pipeline
# call 2, later: save
for a in (GROOM, BIND, WI): eal.save_asset(a, only_if_is_dirty=False)
# then: mh.add_wardrobe_item(c, "Hair", WI, True); mh.set_wardrobe_item_parameters(c, "Hair", '{"Melanin":0.3}')
```

Raw import (explicit conversion) and the groom-struct rule - read the array with `get_editor_property`, edit nested
structs with `set_editor_property`, and WRITE THE WHOLE ARRAY BACK with `set_editor_property` (that is what rebuilds
and dirties the groom; edits to `g.hair_groups_rendering`, an attribute read, are a lost copy):

```python
ABC, SUB, NAME = "E:/hair/Custom01.abc", "/Game/Characters/Wardrobe/Hair/Custom01", "GR_Hair_Custom01"
GROOM = SUB + "/" + NAME
o = unreal.GroomImportOptions()
cs = o.get_editor_property("conversion_settings")
cs.set_editor_property("rotation", unreal.Vector(0, 0, 0)); cs.set_editor_property("scale", unreal.Vector(1, 1, 1))
o.set_editor_property("conversion_settings", cs)
t = unreal.AssetImportTask()
for k, v in (("filename", ABC), ("destination_path", SUB), ("destination_name", NAME), ("automated", True),
             ("replace_existing", True), ("save", False), ("factory", unreal.HairStrandsFactory()), ("options", o)):
    t.set_editor_property(k, v)                                  # automated: no dialog (and nothing saved to config)
unreal.AssetToolsHelpers.get_asset_tools().import_asset_tasks([t])
g, rend = unreal.load_asset(GROOM), []
for r in g.get_editor_property("hair_groups_rendering"):
    gs = r.get_editor_property("geometry_settings")
    for k, v in (("hair_width_override", True), ("hair_width", 0.011), ("hair_root_scale", 1.0), ("hair_tip_scale", 0.5)):
        gs.set_editor_property(k, v)
    r.set_editor_property("geometry_settings", gs); rend.append(r)
g.set_editor_property("hair_groups_rendering", rend)
```

Hair that thins or vanishes in small views (portraits, captures) is Auto LOD scaling the curve count by screen
size, not the LOD `screen_size` (with the single imported LOD that value picks nothing):
`g.set_editor_property("lod_mode", unreal.GroomLODMode.MANUAL)` or raise `auto_lod_bias`.

**From card hair.** A card-hair pack (polygon cards + alpha and root textures) can be converted to strands outside
Unreal: fit the pack's head onto Legacy02 (width/depth by the cranium, height separately by crown-to-nose so
hairlines stay off the brows); take each UV island as one card and grow strands along it only where the alpha is
painted (sample V flipped), root end from the root map; where the map is flat, one style-wide geometric vote (which
V end lies nearer the scalp, area-weighted over all cards). Never decide per card by the nearer end: the outer locks
flip upside down. End strands at alpha gaps; thin fade/undercut cards, cut strands that enter the face, push the
rest out of the scalp, join floating roots; wind dreadlock strands as helices inside each painted column.
Blender (Z-up, -Y forward) to Legacy02: write each point as (x, -z, -y) and export with `global_scale=100`, which
lands at (x, -y, z) cm in Legacy02 component space after UE's default Alembic conversion (observed). Observed with
Blender 5.2: its Alembic export of a Curves object writes positions only (no widths, root UVs or groups) - set width
on import, one file per group.

**Reviewing a groom from several angles.** Observed in 5.8: the Creator viewport is no place for it - Head Sculpt
shows no grooms and Python cannot spawn actors into the Creator's preview world. Stage it in the level instead:
a transient `SkeletalMeshActor` with Legacy02 and a `GroomActor` whose `groom_component` (`gc`) is attached to it,
set with `gc.set_groom_asset(groom)` then `gc.set_binding_asset(binding)` (Python has only the one-argument
`set_groom_asset`; the two-argument C++ overload is not exposed, and `gc.binding_asset = b` fails as an attribute,
though `gc.set_editor_property("binding_asset", b)` before `set_groom_asset` works too); a `SceneCapture2D` (fixed exposure - observed,
a black backdrop drives auto exposure up) with key and fill lights orbiting with the camera; front, three-quarters,
sides, back and top. Drive it from `unreal.register_slate_post_tick_callback`: wait several hundred ticks before the
first shot (bindings build asynchronously) and a few dozen per angle; observed, the first groom's first angles can
still come out bald, so capture it again at the end. `RenderingLibrary.export_render_target` has the file written
when it returns - a blank or bald frame means the groom was not ready. Keep the editor in front, or turn off the
background throttle that stops viewports and ticks:
`unreal.get_default_object(unreal.EditorPerformanceSettings).set_editor_property("throttle_cpu_when_not_foreground", False)`.
Keep the Python handles of the transient actors (`get_all_level_actors` skips them) and destroy them afterwards.

## Custom mesh to MetaHuman (UE 5.8, headless)

The Mesh Import tool's "Import > from custom mesh > Auto Solve" without its viewport (added 2026-10-04 for Master
Smith's characters). The mesh is a static or skeletal mesh already imported into the project: one combined body+head
(or a body and a separate head), centimetres, feet on z=0, facing **+Y** (the MetaHuman frame), A-pose with the arms
clear of the body and the fingers apart, bald, no eyelashes, nothing worn above the skin. A GLB exported from Blender
with the character facing -Y lands facing +Y in Unreal.

```python
c = json.loads(mh.create_character("/Game/MetaHumans", "MH_Dryad", ""))["character"]
r = json.loads(mh.import_from_custom_mesh(c, "/Game/Characters/Dryad/SM_Dryad_conform", "", True, True, 1024))
#   -> solved, seconds, facing, height_cm, face_tracking {curves, points}, warnings (feet off the floor, metres, facing)
#      1-3 minutes blocking for a 60k-vertex mesh; the character now holds the body in the MESH'S pose
mh.save_posed_dna(c, "/Game/Characters/Dryad/SM_Dryad_conform", "", "/Game/Characters/Dryad", "E:/out/Dryad/delivery/metahuman/in", "Dryad_Posed")
#   -> the UI's "Save Pose": one combined DNA still in that pose = the texture-baking target
mh.generate_skeletal_mesh_from_dna("/Game/Characters/Dryad/Dryad_Posed", "/Game/Characters/Dryad", "SKM_Dryad_Posed", "body")
mh.export_fbx("/Game/Characters/Dryad/SKM_Dryad_Posed", "E:/out/Dryad/delivery/metahuman/in/Dryad_Posed.fbx")
mh.commit_a_pose(c, "/Game/Characters/Dryad/SM_Dryad_conform", "")     # back into the MetaHuman A-pose
mh.request_auto_rig(c, "JointsOnly", True); mh.request_texture_sources(c, True)
b = json.loads(mh.build(c, "Cinematic", "Cinematic", "/Game/MetaHumans", ""))
# textures baked in a DCC onto the posed mesh come back onto the built materials:
t = json.loads(mh.import_texture("E:/out/Dryad/delivery/metahuman/T_Dryad_Body_N.png", "/Game/MetaHumans/Dryad/Body/Baked", "T_Dryad_Body_N", "normal", True))["texture"]
mh.set_material_texture("/Game/MetaHumans/Dryad/Body/Materials/MI_Body_Baked", "Normal Baked", t)
# an accessory rigged to the MetaHuman skeleton (SK FBX imported onto metahuman_base_skel) onto the Blueprint:
mh.attach_skeletal_mesh_to_blueprint("/Game/MetaHumans/Dryad/BP_Dryad", "Hair", "/Game/MetaHumans/Dryad/Accessories/SK_Dryad_Hair", "Body", True)
# a rigid accessory rides a bone socket instead (a headset on "head"): relative transform in the socket's frame, centimetres
mh.attach_static_mesh_to_blueprint("/Game/MetaHumans/Dryad/BP_Dryad", "Headset", "/Game/MetaHumans/Dryad/Accessories/SM_Headset", "Body", "head", unreal.Vector(-161.5, 1.5, 0.1), unreal.Rotator(roll=0, pitch=-90, yaw=0))
# (unreal.Rotator takes roll, pitch, yaw in that order - name them; the built head sits about 23 cm behind the
#  conform mesh's head and 5 cm to the side: measure the built face mesh's bounds against the conform mesh's)
```

What the solver gets wrong is in the mesh (fused fingers, a closed armpit, hair conformed into the skull): fix the
mesh and run again; `import_from_custom_mesh` returns `warnings` for the feet, the units and the facing. The face
tracking renders the top 15% of the mesh (or the separate head) front-on, lit from the camera, in a private preview
scene and writes the picture to `Saved/VibeMetaHumans/FaceTrack_<mesh>.png` (`face_tracking.render`): look at it
when the tracker finds no face. A head with empty eye sockets or no eyes fails the tracker (the MetaHuman topology
template did, 2026-10-04; a MetaHuman face with its eyes gave 16 curves / 358 points); then the solve still runs
on the body and the face stays the archetype's. `export_fbx` fills empty
material slots with the default material for the export (an empty slot crashed the engine's exporter).
Two solves for one character (the AINavigator, 2026-10-05): the solve WITH face landmarks stood the posed body 19 cm
behind the mesh (and 2% taller), so a bake against its posed DNA missed; the solve WITHOUT (`bTrackFace=False`) sat
on the mesh to the millimetre but keeps the archetype face. Bake from a scratch body-only character's posed DNA
(then delete it), and keep the face-tracked character for the rig and the build; the textures carry over
because every MetaHuman shares the UV layout. Conform once per fresh character: a second
`import_from_custom_mesh` after `commit_a_pose` gave a twisted posed DNA.
Built materials and their texture parameters: `MI_Body_Baked` ("Basecolor Baked", "Normal Baked", "SRMF Baked",
"Scatter Baked"), `MI_Face_Skin_Baked_LOD3` / `_LOD5to7` (the same plus "Normal LOD Baked"), `MI_EyeL/R_Baked` ("Iris
Basecolor Baked", "Iris Normal Baked", "Sclera ..."), `MI_Teeth_Baked`.

`import_texture` finishes the texture's compilation before returning and REFUSES (`IMPORT_INCOMPLETE`,
nothing marked dirty) when it built as a 32x32 placeholder with no miplevels - a texture-pipeline
stall seen after long editor sessions (2026-10-07; the Output Log says "contains no miplevels"). It
rendered a face white, and the next save of ANY material instance that uses that texture hangs in
`SavePackage`. Restart the editor before importing again rather than retrying in the same session.
Replacing a garment or accessory mesh IN PLACE (same asset path, same skeleton, after it was fit or
shrunk outside Unreal) is `reimport_skeletal_mesh(asset_path, fbx_path, bKeepMaterials=True)`: the
Blueprint's component keeps pointing at the same asset, slot materials are restored by slot name
afterwards (an import with materials off can leave `WorldGridMaterial` on them), and it refuses
(`SKELETON_MISMATCH`) rather than silently retargeting the mesh onto a different skeleton.

## Example: a specific character

```python
c = json.loads(mh.create_character("/Game/MetaHumans", "MH_Marina", ""))["character"]
mh.set_body_shape(c, 0.85, 0.25, 0.45, 168)
mh.set_skin_tone(c, 0.35, 0.55)
mh.set_eye_color(c, 0.15, 0.75, "both")
mh.set_settings(c, "head", '{"Eyelashes":{"Type":"LongSlightCurl"}}')
mh.add_wardrobe_item(c, "Hair", "/MetaHumanCharacter/Optional/Grooms/Bindings/Hair/WI_Hair_L_StraightBangs", True)
mh.add_wardrobe_item(c, "Eyebrows", "/MetaHumanCharacter/Optional/Grooms/Bindings/Eyebrows/WI_Eyebrows_Thin", True)
mh.request_auto_rig(c, "JointsOnly", False)   # then poll get_summary(c)["rig_state"] == "Rigged" (needs the Epic sign-in)
out = json.loads(mh.build(c, "Cinematic", "Cinematic", "/Game/MetaHumans", ""))
mh.save(c); mh.end_edit(c)
```

## Gotchas

- `get_summary(c)` is the cheap read: rig state, DNA/texture flags, body type, skin/eye tone,
  active constraints, wardrobe selections.
- Body constraints are rejected on a fixed body type (`FIXED_BODY`); switch to `BlendableBody` first.
- `set_body_constraints(c, values, normalized, only_named=False)` activates the measurements you name;
  the others keep their active flag, so a later body-shape or height call does not switch off a neck an
  earlier call widened. Pass `only_named=True` to leave only the named ones plus `Height` active: pinning
  every constraint active blocked a neck widen entirely (it did nothing until only `Height` + `Neck` +
  `Neck Base` were active, 2026-10-07). When going through the raw subsystem, the values must still be
  `.copy()`'d and passed back as a whole array in Python - editing `get_body_constraints`' items in
  place and expecting the subsystem to see it does nothing. The result's `measurements` array gives
  `name/active/before/after/delta` for EVERY measurement, not just the ones you named, because a body
  change often moves others as a side effect (a neck widen moved Shoulder Height +1.4 cm too).
- `can_build` reasons: `Character is not rigged.` -> step 6; `missing textures, use Download Texture
  Sources` -> `request_texture_sources` (cloud) or the `bHasHighResolutionTextures` flag above.
- The whole-rig DNA import makes the body a fixed type (`fixed_body_type: true`), so body-shape tools
  are rejected afterwards; set body shape first.
- Wardrobe item paths must be Wardrobe Item assets (`WI_*`), not groom or cloth assets directly
  (**Custom hair as a Creator wardrobe item** wraps a custom groom in one).
- Without the optional content `add_wardrobe_item` has nothing to add and `create_character` with a
  preset path fails with `PRESET_NOT_FOUND`; create from the blank template and say why.
- A build can take a minute or more (texture bakes, groom bindings). Do not call it in a loop.
- The Live Link / Animator inputs live on the **built** Blueprint, not on the preview actor.
- Read the engine-level reference skill `ue-metahuman-creator` (UnrealEngineSkills) when you need
  struct field semantics, pipeline internals or C++, and `ue-hair-and-grooms` for groom assets, Alembic
  import, bindings and Creator hair internals.
