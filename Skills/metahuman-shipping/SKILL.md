---
name: metahuman-shipping
display_name: Shipping MetaHumans (packaged game, web and mobile path, runtime)
description: What happens to MetaHumans when the game ships, which the authoring packs do not cover - face clips that never play at run time (no Slot in the plugin's face AnimBP), the head snap, blinks, meshes that vanish under root motion, groom LODs that go bald and brows that vanish, hair cvars that remove cards too, plugin content cooked for nothing (realtime solver and tracker models: 318 MB web, 556 MB Windows) and how to never-cook it, virtual-texture caps, the teeth and wet-skin fixes for the mobile shading path (web, low end), the face-tracking UDP listener in Shipping builds, the editor crash from deleting a duplicated plugin AnimBP, and a pre-ship checklist. Use when a MetaHuman goes into a packaged, web or mobile build, and on "the face does not move in game", "lip sync plays in Sequencer but not in game", "the head jumps when a line starts", "they never blink", "hair or eyebrows disappear at distance", "teeth glow white", "skin looks wet", "the character vanishes mid-scene", "Windows Firewall asks about the game", "the build is huge", "Sampler type is Virtual Color, should be Color", "TEXTURE STREAMING POOL OVER BUDGET".
vibeue_classes:
  - MetaHumanAnimatorService
  - MetaHumanWardrobeService
unreal_classes:
  - USkeletalMeshComponent
  - UAnimInstance
  - UGroomComponent
  - UTexture
keywords:
  - metahuman packaged game
  - cook size
  - never cook
  - web build
  - mobile renderer
  - face anim blueprint slot
  - lip sync in game
  - blinks
  - head snap
  - groom lod
  - hair cards
  - virtual texture cap
  - teeth white
  - wet skin
  - live link firewall
  - OnlyTickPoseWhenRendered
---

# Shipping MetaHumans

Learned shipping six MetaHumans (Optimized Medium builds, card hair, baked lip sync) to Windows and to the browser in a
48-hour project (UE 5.9, 2026-11). Numbers are measured there; engine facts were read in the 5.9 install. Authoring
lives in the other packs: `metahuman-creator` (build), `metahuman-wardrobe` (clothes, rebuilds, groom LODs),
`metahuman-animator` (bakes, the face AnimBP, blinks).

## 1. Face animation at run time

- **The plugin's face AnimBP cannot play a clip.** `/MetaHumanCharacter/Face/ABP_Face` is Copy Pose From Mesh only, with
  no Slot node: a face AnimSequence played on a slot (`PlaySlotAnimationAsDynamicMontage`, a Sequencer slot track) is
  silently ignored - the voice plays, the face is still. Make one that can:
  `MetaHumanAnimatorService.create_face_anim_blueprint("/Game/Anim", "ABP_Face_Slot")` (copy pose, the slot blended in
  below `FACIAL_C_FacialRoot` with curves overriding, `HeadControlSwitch` x 0). It is created in your project, not
  shipped as plugin content: VibeMetaHumans is an editor plugin, so a Face component pointing at its content would lose
  the AnimBP in a packaged game.
- Set it as the Face component's anim class on every built Blueprint, and again after every rebuild (a rebuild puts
  `ABP_Face` back): `MetaHumanWardrobeService.post_build_fixup(bp, '{"face_anim_class": "/Game/Anim/ABP_Face_Slot"}')`.
- **The head snap.** A clip exported with head movement drives `HeadControlSwitch`, which gates head IK in
  `ABP_Face_PostProcess`; on means a jump to the archetype head pose (about 4.4 cm forward and down on one character).
  Scaling the head yaw, pitch or translation curves does not remove it; the AnimBP above zeroes the switch.
- **Play lines** with `PlaySlotAnimationAsDynamicMontage(clip, "DefaultSlot", ...)` on the Face's anim instance and start
  the voice with it. Log an error when there is no anim instance. Never fall back to `PlayAnimation` on a face that has
  an AnimBP: single-node mode destroys the AnimBP instance for the line and a seated character's head drops off the neck.
- **Bake lip sync offline, do not solve at run time.** Epic's `GenerateAnimationFromAudio` + `ExportPerformanceAnimationSequence`
  against `/MetaHumanCharacter/Face/Face_Archetype_Skeleton` (`metahuman-animator`). 132 clips cooked to 3.22 MB (232 MB of
  source curves in the editor). Keep the Performance assets in a never-cooked editor folder (277 MB for 132 lines).
  Run-time solving needs the realtime solver models (section 4), a capture or audio source and Live Link; a browser has
  none of that path.
- **Blinks.** Audio-driven bakes barely blink (blink curves peaked at 0.05; many clips had no blink key):
  `MetaHumanAnimatorService.add_blinks(folder, 2.8, 4.2, False, '{"name_prefix":"FA_"}')`. For silence between lines,
  pass a looping blink-only clip to `create_face_anim_blueprint` (blended by max curve value). A still or reference
  sheet can now catch a character mid-blink: look, and render again.
- **Never duplicate a plugin AnimBP into `/Game` as a test fixture** (`ABP_Face`, `ABP_MH_LiveLink`). Deleting the copy
  runs ForceDeleteObjects, which cannot unload a package that references MetaHuman Control Rig classes, and the editor
  fatals on a `RigVMHost` cast. Build fixtures from a minimal AnimBP you create.

## 2. Meshes that vanish or stop moving

- **Root motion far from the actor origin:** set Body and Face to `AlwaysTickPoseAndRefreshBones`
  (`VisibilityBasedAnimTickOption`) on the placed actors. With `OnlyTickPoseWhenRendered` the bounds went stale once a
  clip carried a character metres from its origin; the mesh stopped updating and dropped out of the shot. Garments can
  stay on tick-when-rendered: they take the leader's bounds (`bUseBoundsFromLeaderPoseComponent`, set by
  `attach_skeletal_mesh_to_blueprint`).
- **`LODSyncComponent` drives every part's LOD.** Add each garment to it. For a close-up capture force LOD 0.
- **Check the placed actors**, not fresh spawns: an old per-instance override (a `Body` mesh, a Face anim class) hides
  every Blueprint fix. `MetaHumanWardrobeService.audit_placed_instances(bp, False, map)`.

## 3. LODs and hair

- **Count the LODs you have.** An Optimized Medium build gave 3 skeletal LODs for body and face and 4 for outfits, not
  8. `r.SkeletalMeshLODBias=2` would put every face on its last LOD in close-ups; all skeletal meshes of six characters
  were 7.8 MB cooked, so no LOD bias was applied. `describe_skeletal_mesh` lists the LODs and the bones each drops.
- **Grooms with cards at only some LODs go bald at the others.** An Optimized build: strands at LOD 0-1, cards only at
  LOD 3, helmets at LOD 5 and 7, brows hidden at LOD 5-7. With strands off, LOD 4 and 6 drew no hair and brows vanished
  from LOD 4 (screen size 0.0625: any wide shot). `fill_groom_lods` / `post_build_fixup` give every empty LOD the nearest
  cards or helmet (+4 MB for 15 grooms). Check: `r.HairStrands.Strands 0`, then `r.HairStrands.MinLOD 0`..`7`.
- **Cards only is `r.HairStrands.Strands=0`.** `r.HairStrands.Enable=0` disables the whole hair system - "This affects
  all geometric representations (i.e., strands, cards, and meshes)" (`GroomVisualizationData.cpp`): every character loses
  hair, brows and beard. Also `r.HairStrands.Simulation=0`. The mobile web tiers cannot draw strands, so strand LODs fall
  back to cards there anyway.
- A scene capture may draw no hair (five of six characters in one session, untouched assets): judge hair in PIE or the
  packaged build.

## 4. Cook size

- **Plugins always-cook content the game never reads.** `MetaHumanLiveLink`'s own `Config/Game.ini` has
  `+DirectoriesToAlwaysCook=(Path="/MetaHumanCoreTech")`. With baked face animation nothing reads:
  - the realtime face solver models: **318.6 MB** in a web build (HeadPoseTracker 119.4, GenericRigSolver 87.9,
    StereoHMCRigSolver 87.9, FaceDetector 23.4);
  - `/MetaHuman/GenericTracker`, MetaHuman Animator's footage trackers: **556 MB** in a Windows build.
- Never-cook the subfolders in `Config/DefaultGame.ini`. **A plugin root cannot be never-cook** (the cook fails):
  ```ini
  [/Script/UnrealEd.ProjectPackagingSettings]
  +DirectoriesToNeverCook=(Path="/MetaHumanCoreTech/RealtimeMono")
  +DirectoriesToNeverCook=(Path="/MetaHumanCoreTech/GenericTracker")
  +DirectoriesToNeverCook=(Path="/MetaHuman/GenericTracker")
  ```
  Check the folder names against your install (`Engine/Plugins/MetaHuman/*/Content`); then read the cook's size report
  for any solver or tracker model left.
- **Virtual textures need per-asset caps.** The body uses 8K virtual textures (`T_Body_BC_VT`, `_N_VT`, `_Scatter_VT`,
  `_SRMF_VT`); a device-profile texture-group cap does **not** reach them. Cap the body's at 1024 (it sits under clothes)
  and the face's `T_Head_*_VT` at 2048 with the per-asset max texture size, after each build:
  `post_build_fixup(bp, '{"texture_caps": {"T_Body_*_VT": 1024, "T_Head_*_VT": 2048}}')` (caps are opt-in and only lower).
  One 2K character added about 168 MB of cooked container and overflowed a 192 MB texture pool by 21 MB; six character
  folders went 255.1 to 175.6 MB.
- **Never flip `virtual_texture_streaming` from Python**: the materials then fail with "Sampler type is Virtual Color,
  should be Color" and the body falls back to the default material.
- **Plugin and pack content rides along.** The default garments under `/MetaHumanCharacter/Optional/Clothing` (4K/8K)
  were 123.2 MB until texture-group caps of 1K took them to 20.7; groom atlases 28.1 to 13.9; teeth 11.5 to 3.6.
  Marketplace garments are 4K: copy what you use and cap the copies (`make_garment_variant`), or the cook pulls the pack's
  material in through the mesh.
- Set once, never during a cook: `r.GPUSkin.Support16BitBoneIndex=True` and `r.GPUSkin.UnlimitedBoneInfluences=True`
  (`[/Script/Engine.RendererSettings]`).

## 5. The look on the mobile shading path (web, low end)

Found in the first full web playthrough; PIE and Win64 with Lumen looked right.

- **Teeth draw as a flat white band**: the teeth material's value and occlusion parameters never reach its low-quality
  path. At run time give the teeth slot (slot name starts with `teeth`) a simple lit material: the character's own baked
  teeth texture (parameter `Basecolor Baked` of the slot's material) times a warm grey (0.5, 0.465, 0.4), roughness 0.55,
  specular 0.15. It is loaded by path: always-cook its folder.
- **Skin reads wet**: on the skin slots (names start with `head_`) set a dynamic instance's
  `Roughness Global Offset Post-Bake` +0.2 and `Specular Global Multiply Post-Bake` 0.55.
- Apply both only when Lumen is not running (feature level below SM5, or `r.DynamicGlobalIlluminationMethod` not 1), to
  every face at start and to faces spawned later. Tune in the real target: the first values (+0.15, x0.6) did not hold
  up; the shipped ones came from a console command changed live in a Development web build.

## 6. Shipping safety

`BaseEngine.ini` ships `bEnableLiveLinkForFaceTracking=true` (`[/Script/AppleARKit.AppleARKitSettings]`,
`LiveLinkPublishingPort=11111`). With `AppleARKitFaceSupport` enabled the **Shipping** game opened a UDP listener and
Windows Firewall prompted on first launch. In `Config/DefaultEngine.ini`:

```ini
[/Script/AppleARKit.AppleARKitSettings]
bEnableLiveLinkForFaceTracking=False
```

The repackaged Shipping build then had no UDP or TCP endpoints and booted with no prompt.

## 7. Before a build that shows skin

Turn `bShowTopUnderwear` off (`set_settings(c, "skin", '{"Skin": {"bShowTopUnderwear": false}}')`, default true in
`MetaHumanCharacterSkin.h`) or the grey sports top is baked into the body and the face's neck-skirt textures, and choose
the face and body skin textures from one preset (a tone step at the neck otherwise). After any rebuild:
`post_build_fixup`, the body fits again, `audit_placed_instances`.

## 8. Pre-ship checklist

1. Every built Blueprint's Face anim class is your slot AnimBP; one voiced line per character shows lips in PIE.
2. No head jump when a line starts; blinks during a 10 s hold.
3. Hair and brows at every groom LOD (`r.HairStrands.Strands 0`, `r.HairStrands.MinLOD 0..7`).
4. Characters stay visible through every root-motion move; garments do not pop out in close-ups.
5. Each scene through the game's cameras, placed actors, seated and standing, LOD 0-2: no holes, no skin through cloth,
   no streaks, hands on surfaces, accessories in frame (`capture_fit_views` first, then PIE).
6. Never-cook lists in place; the size report shows no solver or tracker models and no pack 4K textures.
7. Virtual textures capped per asset; materials compile (no sampler-type errors, no default grey bodies).
8. On the real low-end or web target: teeth, skin sheen, exposure, hair.
9. Windows Shipping on first launch: no firewall prompt.
10. Editor-only folders (Performances, fit tests, backups) are never-cook.

Not verified in the project: exporting face clips without head movement instead of zeroing `HeadControlSwitch`;
per-character phase offsets for a looping blink; strand hair at run time (it shipped cards only).
