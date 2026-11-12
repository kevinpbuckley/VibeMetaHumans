---
name: metahuman
display_name: MetaHumans with VibeMetaHumans
description: Create, edit, rig, animate, dress and place MetaHumans in Unreal Engine 5.9 through the VibeMetaHumans services (unreal.MetaHumanCharacterService, MetaHumanWardrobeService, MetaHumanAnimatorService, MetaHumanIdentityService, MetaHumanLiveLinkService, MetaHumanSetupService, MetaHumanObjectService). Use for any MetaHuman task - new character, body/skin/eyes/hair changes, clothes on a built character, facial animation from webcam video, iPhone Live Link Face takes or audio, real-time Live Link, assembly into a Blueprint.
vibeue_classes:
  - MetaHumanSetupService
  - MetaHumanCharacterService
  - MetaHumanWardrobeService
  - MetaHumanAnimatorService
  - MetaHumanIdentityService
  - MetaHumanLiveLinkService
  - MetaHumanObjectService
keywords:
  - metahuman
  - metahuman creator
  - metahuman animator
  - live link face
  - facial animation
  - audio to face
  - mesh to metahuman
---

# MetaHumans with VibeMetaHumans

Every tool is a static method on an `unreal.<Service>` class (editor Python, or `call_tool` on the
MCP endpoint under `VibeMetaHumans.<Service>.<Tool>`). Every tool returns a JSON string with
`success`, so always `json.loads(...)` the result and branch on it. Asset arguments accept package
paths (`/Game/MetaHumans/MH_Hero`) or object paths.

## Start every MetaHuman session here

```python
import json, unreal
s = json.loads(unreal.MetaHumanSetupService.get_status())
```

- `plugins` must show `MetaHumanCharacter`, `MetaHuman`, `MetaHumanLiveLink`, `LiveLink`, `Takes` and
  `CaptureManagerEditor` as `enabled`. If not, stop and tell the user which plugin to enable
  (it needs an editor restart).
- `optional_content_installed == false` means the "MetaHuman Creator Core Data" launcher option is
  missing: no presets, no wardrobe library, no texture synthesis. Characters can still be created
  from the blank template and Animator / Live Link work normally. Tell the user; do not try to
  work around it.
- Cloud steps (`request_auto_rig`, `request_texture_sources`, identity auto-rig) need the editor to
  be signed in to the MetaHuman service with an Epic account. A failed/blocked rig request is the
  usual symptom; `MetaHumanIdentityService.log_in_to_service("")` opens the login.

## Routing table

| Task | Load this skill (`GetSkills`) | Service |
|---|---|---|
| Environment, what exists, enum names, open an asset editor | `/Skills/VibeMetaHumans/metahuman-setup` | `MetaHumanSetupService` |
| Make or change a character (body, skin, eyes, makeup, teeth, lashes, hair, Creator outfits), rig it, build the Blueprint, place it | `/Skills/VibeMetaHumans/metahuman-creator` | `MetaHumanCharacterService` |
| Dress a BUILT Blueprint in marketplace/Fab garments; poke-through, cuff spikes, necklines, keep it through a rebuild, groom LODs | `/Skills/VibeMetaHumans/metahuman-wardrobe` | `MetaHumanWardrobeService` (+ `attach_skeletal_mesh_to_blueprint`) |
| Facial animation from a video file (webcam/DSLR), an iPhone take, or audio; export AnimSequence/LevelSequence | `/Skills/VibeMetaHumans/metahuman-animator` | `MetaHumanAnimatorService` (+ `MetaHumanIdentityService` for iPhone depth takes) |
| Real-time face capture in the editor, calibrate, bind to a placed MetaHuman, Take Recorder | `/Skills/VibeMetaHumans/metahuman-live-link` | `MetaHumanLiveLinkService` |
| Anything not wrapped: read/patch properties, call a Blueprint library, describe a struct | (this pack, below) | `MetaHumanObjectService` |

## Rules of thumb

1. **One character at a time.** `MetaHumanCharacterService` opens the asset for editing on first use
   (texture-synthesis model load, a few seconds). Call `end_edit` when you are done with a character
   to free preview meshes, and `save` after meaningful edits.
2. **Rig before you animate.** A character shows `rig_state: "Rigged"` in `get_summary` only after
   `request_auto_rig`. Only a rigged character assembles with a working face rig for Animator and
   Live Link.
3. **Pick the Animator route by input.** Plain video → `MonoFootage` (no identity). iPhone take in
   MetaHuman Animator mode → `DepthFootage` + a prepared Identity (best fidelity). Audio file →
   `Audio`. Never promise an identity from a webcam video: identities need depth.
4. **Some calls are blocking.** `request_auto_rig(..., True)`, `request_texture_sources`
   and `build` return when done; a 30-second-timeout MCP client may abandon the call while the
   editor keeps working. Re-query (`get_status`, `get_summary`) instead of re-running. Animator
   processing is Epic's asynchronous `ProcessPerformance` (poll `MetaHumanAnimatorService.get_status`).
5. **Patch, don't rebuild.** Settings tools take a JSON patch; only the fields you pass change.
   When unsure of a shape: `MetaHumanObjectService.describe_struct("MetaHumanCharacterSkinSettings")`
   or `MetaHumanCharacterService.get_settings(c, "skin")`.
6. **Keep the user's content layout.** Default output folders are `/Game/MetaHumans` (characters,
   builds) and `/Game/MHA` (capture data, identities, performances); follow the project's own
   convention when one exists.

## MetaHumanObjectService (escape hatch)

```python
o = unreal.MetaHumanObjectService
o.get_properties(perf, ["InputType", "HeadMovementMode"])
o.set_properties(perf, '{"HeadMovementMode":"ControlRig","bSkipDiagnostics":true}')
o.list_functions("MetaHumanCharacterEditorSubsystem", "Commit")
# (Epic's CaptureManagerIngestTools.ScanForTakes is the AI-tool route to the same scan)
o.call_function("CaptureManagerIngestBlueprintLibrary", "FindTakeDirectories", '{"SearchDirectory":"D:/Takes","bRecursive":true}')
o.call_object_function(subject_settings_path, "SetSmoothing", '{"0": "/MetaHumanLiveLink/..."}')  # positional args by index
o.describe_struct("MetaHumanCharacterEyesSettings")
```

Enum values are passed by name (`"Cinematic"`, `"JointsOnly"`, `"MonoFootage"`); object properties by
asset path; nested structs as nested JSON objects.
