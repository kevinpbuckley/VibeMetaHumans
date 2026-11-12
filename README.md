> [!WARNING]
> **Unreal Engine 5.9 build: not fully tested yet.** This `5-9` branch builds and loads in UE 5.9, but
> it has not been tested end to end. Expect rough edges and report problems as issues. For production
> work, use the stable `main` branch.

# VibeMetaHumans — AI Editor Toolset for MetaHumans (Unreal Engine 5.9)

VibeMetaHumans is a sibling of [VibeUE](https://github.com/kevinpbuckley/VibeUE) that focuses on one
thing: letting an MCP agent (Claude Code, Cursor, Copilot, …) **create, edit, rig, animate and
place MetaHumans** inside the Unreal Editor. It plugs into Unreal 5.9's native
`ToolsetRegistry` + `ModelContextProtocol` server exactly like VibeUE does: no separate server,
no API key, no in-editor chat. Every tool is also a plain `unreal.<Service>.<tool>()` call in
editor Python.

It covers the four workflows of the in-editor MetaHuman pipeline:

| Workflow | Service | What it does |
|---|---|---|
| Create & edit a character (MetaHuman Creator) | `MetaHumanCharacterService` | create from the blank template or a preset, body shape/type (`set_body_constraints` activates the measurements you name - `only_named` to switch off the rest - and returns before/after/delta for every one), skin tone, eyes, makeup, teeth/eyelashes, face coefficients, sculpt-landmark fitting to a reference picture (`get_face_landmarks` / `move_face_landmarks`, with ear/neck/skull detection and groom/rig refusals), Mesh-to-MetaHuman imports (Identity / DNA / template meshes), cloud auto-rig + high-res textures (`get_auto_rig_status`), wardrobe (hair, brows, lashes, beard, outfits), non-blocking build and save (`build_async`/`get_build_status`, `save_deferred`/`get_save_status` - `save` routes to it automatically while an edit session is open), spawn in the level, DNA export |
| **Custom mesh to MetaHuman** (UE 5.9's Import > from custom mesh, headless) | `MetaHumanCharacterService` | `import_from_custom_mesh` (face landmarks tracked on a front-on render, auto solve), `save_posed_dna` (the solved body in the mesh's pose: the baking target), `commit_a_pose`, `generate_skeletal_mesh_from_dna`; round trips with a DCC: `export_fbx`, `import_texture` (verifies it actually built, not a 32x32 placeholder), `reimport_skeletal_mesh` (replace in place, materials restored by slot name), `set_material_texture`, `attach_skeletal_mesh_to_blueprint` / `attach_static_mesh_to_blueprint` / `remove_component_from_blueprint` |
| **Hair on a built MetaHuman** (2026-10-07) | `MetaHumanCharacterService` | `list_wardrobe_items` (every groom Wardrobe Item by slot), `add_groom_to_blueprint` (a GroomComponent on a built Blueprint, the plugin's binding duplicated and retargeted onto the built face, material overrides copied from an existing groom component), `import_groom` (a custom Alembic groom) |
| **Dress a built MetaHuman** (2026-11-12) | `MetaHumanWardrobeService` | marketplace/Fab skeletal garments on a built Blueprint without a rebuild: `fix_unevaluated_bone_weights` (the cuff spikes), `make_full_body_from_dna` (the built body's missing skin back, for dresses and open collars), `hide_body_under_garments` (skin under the clothes deleted, rest + seated poses), `push_garment_out_of_mesh` (a neckline out over the face mesh's neck skirt), `snapshot_blueprint_components` / `restore_blueprint_components` (survive a rebuild; also `build(..., True)`), `fill_groom_lods` (no bald LODs). All computed from asset data: no level, actor or PIE |
| Facial animation from **webcam / DSLR video** | `MetaHumanAnimatorService` | ingest a video file → Footage Capture Data → MonoFootage Performance → process → AnimSequence / LevelSequence |
| Facial animation from an **iPhone** (Live Link Face) | `MetaHumanIdentityService` + `MetaHumanAnimatorService` | ingest the take, build and auto-rig an Identity from a neutral frame, process a DepthFootage Performance |
| Facial animation from **audio only** | `MetaHumanAnimatorService` | SoundWave → Audio Performance with mood controls → process → export |
| **Real-time** face capture | `MetaHumanLiveLinkService` | webcam / microphone / Live Link Face sources, neutral calibration, head tracking, bind a subject to a placed MetaHuman, Take Recorder |
| Environment & discovery | `MetaHumanSetupService` | plugin + optional-content status, asset listing (characters, presets, identities, performances, capture data, wardrobe), enum options, open editors |
| Escape hatch | `MetaHumanObjectService` | read/patch any object's properties by JSON, call any static or instance UFUNCTION (reaches private-header Blueprint libraries such as Capture Manager ingest) |

Every tool returns a JSON string: `{"success": true, ...}` or
`{"success": false, "error_code": "...", "error": "..."}`.

> **Optional content.** MetaHuman Creator needs the **"MetaHuman Creator Core Data"** install
> option from the Epic Games Launcher (Library → Unreal Engine 5.9 → Options). Without it there are
> no presets, no texture synthesis and no wardrobe library; `get_status()` reports
> `optional_content_installed`. Cloud steps (auto-rig, high-resolution textures) need an Epic account
> signed in to the MetaHuman service.

---

## Install

1. Enable Unreal's MCP first (Edit → Plugins → **Unreal MCP**, which pulls in Toolset Registry) and
   turn on *Model Context Protocol → Auto Start Server* in Editor Preferences. Default endpoint:
   `http://127.0.0.1:8000/mcp`.
2. Enable the MetaHuman plugins the toolset drives: **MetaHuman Creator** (`MetaHumanCharacter`),
   **MetaHuman Animator** (`MetaHuman`), `MetaHumanSDK`, `MetaHumanCoreTech`, `MetaHumanLiveLink`,
   `LiveLink`, `Takes`, and `CaptureManagerEditor` (for footage ingest). The `.uplugin` lists them
   as dependencies so enabling VibeMetaHumans enables them too.
3. Clone this repo into `YourProject/Plugins/VibeMetaHumans`, add
   `{ "Name": "VibeMetaHumans", "Enabled": true, "TargetAllowList": ["Editor"] }` to the
   project's `.uproject`, and build the editor target (in Proteus: `BuildAndLaunchGame.ps1`, which is
   also copied here). The plugin is `EnabledByDefault: false` so an unbuilt checkout never
   triggers the "missing modules" dialog.
4. Verify in the editor console: `VibeMetaHumans.ListToolsets` (expect 7 toolsets) and from the
   agent: `unreal.MetaHumanSetupService.get_status()`.

Skill packs under `Skills/` are discovered natively by UE 5.9 (`<Plugin>/Skills/**/SKILL.md`) and
served through `ToolsetRegistry.AgentSkillToolset` → `ListSkills` / `GetSkills` as
`/Skills/VibeMetaHumans/<pack>`, the same way VibeUE's are. The engine re-reads the files on every
call, so edits need no reload or restart.

---

## Quick tour (the engine's `execute_python` MCP tool)

Ingest, performance processing, identity preparation and AnimSequence export are Epic's own 5.9 tools
(`CaptureManagerToolset`, `MetaHumanAnimatorToolset`), and generic asset work (save, find, open) is
`EditorToolset`. VibeMetaHumans keeps everything those do not cover, and the tour mixes both:
`unreal.<Service>.*` for VibeMetaHumans, `await call_tool(...)` for Epic.

```python
import json, unreal
setup, mh, anim, ident, ll = (unreal.MetaHumanSetupService, unreal.MetaHumanCharacterService,
                              unreal.MetaHumanAnimatorService, unreal.MetaHumanIdentityService,
                              unreal.MetaHumanLiveLinkService)
CM   = "CaptureManagerToolset.CaptureManagerIngestTools"
PERF = "MetaHumanAnimatorToolset.MetaHumanAnimatorPerformanceTools"
IDT  = "MetaHumanAnimatorToolset.MetaHumanAnimatorIdentityTools"
EXP  = "MetaHumanAnimatorToolset.MetaHumanAnimatorExportTools"

print(json.loads(setup.get_status()))                                   # plugins, optional content, counts

# 1. Create a MetaHuman (blank template, or a preset found with
#    await call_tool("EditorToolset.AssetToolset.FindAssets", filter={"assetType": {"refPath": "/Script/MetaHumanCharacter.MetaHumanCharacter"}, "folderPath": "/", "maxResults": 0}))
r = json.loads(mh.create_character("/Game/MetaHumans", "MH_Hero", ""))
c = r["character"]
mh.set_body_shape(c, 0.2, 0.3, 0.7, 182)                                 # masc/fem, fat, muscle, cm
mh.set_skin_tone(c, 0.55, 0.45)
mh.set_eye_color(c, 0.2, 0.8, "both")
mh.set_settings(c, "makeup", '{"Lips":{"Type":"Natural","Color":{"R":0.3,"G":0.05,"B":0.05,"A":1}}}')
mh.add_wardrobe_item(c, "Hair", "/MetaHumanCharacter/Optional/Grooms/Bindings/Hair/WI_Hair_L_StraightBangs", True)
mh.request_auto_rig(c, "JointsOnly", True)                               # cloud, blocking
b = json.loads(mh.build(c, "Cinematic", "Cinematic", "/Game/MetaHumans", ""))
print(b["blueprints"])                                                   # BP_MH_Hero ...
mh.spawn_in_level(c, unreal.Vector(0, 0, 0), unreal.Rotator(0, 0, 0), False)

# 2. Webcam / DSLR video -> animation (no identity needed)
cd = (await call_tool(CM + ".IngestMonoVideo", videoFilePath="D:/Takes/line01.mp4", audioFilePath="", slate="line01", takeNumber=1))["captureDataAssetPath"]
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_line01", "MonoFootage", cd, "", ""))["performance"]
await call_tool(PERF + ".ProcessPerformance", performance=p, params={})   # asynchronous; the await returns when it finishes
await call_tool("EditorToolset.AssetToolset.SaveAsset", assetPath=p)
await call_tool(EXP + ".ExportPerformanceAnimationSequence", performance=p,
                options={"assetPath": "/Game/MHA/Anims", "assetName": "AS_line01", "bEnableHeadMovement": True, "exportRange": "ProcessingRange"})

# 3. iPhone (Live Link Face, MetaHuman Animator mode) -> identity -> animation
cd = (await call_tool(CM + ".IngestLiveLinkFace", takeDirectoryPath="D:/Takes/Kevin_Take01"))["captureDataAssetPath"]
i  = json.loads(ident.create_identity_from_footage("/Game/MHA", "ID_Kevin", cd, 120, 1, True, False))["identity"]
# ... poll ident.get_status(i) until face.has_dna, then:
await call_tool(IDT + ".PrepareIdentityForPerformance", identity=i)
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_take01", "DepthFootage", cd, i, ""))["performance"]
await call_tool(PERF + ".ProcessPerformance", performance=p, params={})
anim.export_level_sequence(p, "/Game/MHA/Seq", "LS_take01", b["blueprints"][0], False, True, True, False)

# 4. Audio only: mood / output controls / head movement are plain Performance properties
p = json.loads(anim.create_performance("/Game/MHA", "Perf_vo01", "Audio", "", "", "/Game/Audio/vo01"))["performance"]
await call_tool("EditorToolset.ObjectToolset.SetProperties", instance=p, values=json.dumps(
    {"AudioDrivenAnimationSolveOverrides": {"Mood": "Happiness", "MoodIntensity": 0.6},
     "AudioDrivenAnimationOutputControls": "FullFace", "HeadMovementMode": "ControlRig"}))
await call_tool(PERF + ".ProcessPerformance", performance=p, params={})

# 5. Live
ll.add_video_source("", "Webcam", -1); ll.capture_neutrals("Webcam")
ll.bind_subject_to_actor("BP_MH_Hero", "Webcam", True)
ll.start_take_recording(["Webcam"]); ...; ll.stop_take_recording()

# 6. A Fab jacket on the built character (on copies; see the metahuman-wardrobe skill)
wd, BP = unreal.MetaHumanWardrobeService, b["blueprints"][0]
body = "/Game/MetaHumans/MH_Hero/Body/SKM_MH_Hero_BodyMesh"
wd.fix_unevaluated_bone_weights("/Game/Wardrobe/SKM_Jacket", body, False)            # no spikes from the cuffs
r = json.loads(mh.attach_skeletal_mesh_to_blueprint(BP, "G_Jacket", "/Game/Wardrobe/SKM_Jacket", "Body", True, "/Game/Wardrobe/M_Jacket"))
wd.hide_body_under_garments(BP, '["G_Jacket"]', body + "_Dressed", "{}", True, False) # no skin through the cloth
mh.build(c, "Cinematic", "Cinematic", "/Game/MetaHumans", "", True)                  # rebuild keeps the jacket
```

Through the MCP endpoint the same tools appear as `VibeMetaHumans.MetaHumanCharacterService.CreateCharacter`
etc. (`list_toolsets` / `describe_tools` / `call_tool`).

---

## Layout

```
VibeMetaHumans/
├── VibeMetaHumans.uplugin
├── Source/VibeMetaHumans/
│   ├── Public/Services/U*Service.h      # one UToolsetDefinition per domain, static AICallable UFUNCTIONs
│   ├── Private/Services/U*Service.cpp
│   ├── Public|Private/Utils/VmhJson.*   # JSON results, struct<->JSON, property patching, reflection calls
│   ├── Public|Private/Utils/VmhAssets.* # load/create/save assets, find actors
│   ├── Public|Private/Utils/VmhSkin.*   # CPU skinning from mesh descriptions, AnimSequence poses, AABB/winding queries
│   ├── Public|Private/Utils/VmhBlueprintSnapshot.* # record / restore a Blueprint's components around a rebuild
│   └── VibeMetaHumansModule.*           # reflection-based toolset registration, console commands
├── Content/Python/vibemetahumans.py     # agent helpers (status, toolset discovery)
└── Skills/<pack>/SKILL.md               # metahuman (router), -setup, -creator, -wardrobe, -animator, -live-link (native 5.9 discovery)
```

Design notes:

- Services only call APIs the MetaHuman plugins export. Where a Blueprint library lives in a
  private header (Capture Manager ingest, Take Recorder's Live Link source) the service goes through
  reflection (`VmhJson::CallStaticFunction` / `CallObjectFunction`), which is also what
  `MetaHumanObjectService.call_function` exposes to agents.
- Settings edits are JSON patches onto the real `USTRUCT`s (`FMetaHumanCharacterSkinSettings` …) so
  the whole surface of the MetaHuman Creator is reachable without a wrapper per field; use
  `MetaHumanObjectService.describe_struct("MetaHumanCharacterEyesSettings")` to see a shape.
- Long operations (auto-rig, texture download, performance processing) run blocking on the game
  thread so one tool call = one finished step; Epic's MCP endpoint has no request timeout.
- Nothing here needs the VibeUE plugin. The engine's own `execute_python` MCP tool is the easiest
  way to drive these services, and `SceneCaptureToolset.CaptureActiveEditorView` (or
  `CaptureGameViewport` in PIE) shows the result.

## Status

First cut, written against UE 5.8.3 headers. Not yet compiled or exercised in the editor in this
repository's history; see the commit log / issues for what has been validated.

## License

MIT — see `LICENSE`.
