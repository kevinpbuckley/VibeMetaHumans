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
4. Verify in the editor console: `VibeMetaHumans.ListToolsets` (expect 6 toolsets) and from the
   agent: `unreal.MetaHumanSetupService.get_status()`.

Skill packs under `Skills/` are discovered natively by UE 5.9 (`<Plugin>/Skills/**/SKILL.md`) and
served through `ToolsetRegistry.AgentSkillToolset` → `ListSkills` / `GetSkills` as
`/Skills/VibeMetaHumans/<pack>`, the same way VibeUE's are. The engine re-reads the files on every
call, so edits need no reload or restart.

---

## Quick tour (editor Python, or the engine's `execute_python` MCP tool)

```python
import json, unreal
setup, mh, anim, ident, ll = (unreal.MetaHumanSetupService, unreal.MetaHumanCharacterService,
                              unreal.MetaHumanAnimatorService, unreal.MetaHumanIdentityService,
                              unreal.MetaHumanLiveLinkService)

print(json.loads(setup.get_status()))                                   # plugins, optional content, counts

# 1. Create a MetaHuman (blank template, or pass a preset path from setup.list_assets("preset", ""))
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
cd = json.loads(anim.ingest_mono_video("D:/Takes/line01.mp4", "", "line01", 1))["capture_data"]
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_line01", "MonoFootage", cd, "", ""))["performance"]
anim.process(p, -1, -1)
anim.export_anim_sequence(p, "/Game/MHA/Anims", "AS_line01", "", True, True)

# 3. iPhone (Live Link Face, MetaHuman Animator mode) -> identity -> animation
cd = json.loads(anim.ingest_live_link_face_take("D:/Takes/Kevin_Take01"))["capture_data"]
i  = json.loads(ident.create_identity_from_footage("/Game/MHA", "ID_Kevin", cd, 120, 1, True, False))["identity"]
# ... poll ident.get_status(i) until face.has_dna, then:
ident.prepare_for_performance(i)
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_take01", "DepthFootage", cd, i, ""))["performance"]
anim.process(p, -1, -1)
anim.export_level_sequence(p, "/Game/MHA/Seq", "LS_take01", b["blueprints"][0], False, True, True, False)

# 4. Audio only
p = json.loads(anim.create_performance("/Game/MHA", "Perf_vo01", "Audio", "", "", "/Game/Audio/vo01"))["performance"]
anim.set_audio_mood(p, "Happiness", 0.6, "FullFace", "ControlRig")
anim.process(p, -1, -1)

# 5. Live
ll.add_video_source("", "Webcam", -1); ll.capture_neutrals("Webcam")
ll.bind_subject_to_actor("BP_MH_Hero", "Webcam", True)
ll.start_take_recording(["Webcam"]); ...; ll.stop_take_recording()
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
│   └── VibeMetaHumansModule.*           # reflection-based toolset registration, console commands
├── Content/Python/vibemetahumans.py     # agent helpers (status, toolset discovery)
└── Skills/<pack>/SKILL.md               # metahuman (router), -setup, -creator, -animator, -live-link (native 5.9 discovery)
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
