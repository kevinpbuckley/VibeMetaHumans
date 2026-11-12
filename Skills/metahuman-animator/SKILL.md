---
name: metahuman-animator
display_name: MetaHuman Animator (offline facial animation)
description: Turn footage or audio into MetaHuman facial animation with unreal.MetaHumanAnimatorService and unreal.MetaHumanIdentityService - ingest a webcam/DSLR video, an iPhone Live Link Face take or a take archive into Footage Capture Data, build and auto-rig an Identity from a neutral frame (Mesh to MetaHuman), create MonoFootage / DepthFootage / Audio Performances, set audio mood, process, and export an AnimSequence or LevelSequence onto a MetaHuman. Use for "animate the face from this video/phone take/voice line".
vibeue_classes:
  - MetaHumanAnimatorService
  - MetaHumanIdentityService
  - MetaHumanObjectService
unreal_classes:
  - UMetaHumanPerformance
  - UMetaHumanIdentity
  - UFootageCaptureData
  - UMetaHumanPerformanceExportUtils
keywords:
  - metahuman animator
  - facial animation
  - webcam
  - live link face take
  - identity
  - mesh to metahuman
  - audio driven animation
  - lip sync
  - export anim sequence
---

# MetaHuman Animator (offline facial animation)

```python
import json, unreal
anim, ident = unreal.MetaHumanAnimatorService, unreal.MetaHumanIdentityService
```

## Choose the route by input

| Input | Capture data | Identity | Performance `InputType` |
|---|---|---|---|
| Video file from a webcam / DSLR / phone camera roll | `ingest_mono_video(video, audio_or_empty, slate, take)` | none | `MonoFootage` |
| iPhone Live Link Face take (**MetaHuman Animator** capture mode → has depth) | `ingest_live_link_face_take(take_dir)` | required, prepared | `DepthFootage` |
| Take archive (.zip from Live Link Hub / Capture Manager) | `ingest_take_archive(path)` | depends on contents | as above |
| Audio file (SoundWave asset) | none | none | `Audio` |

`find_take_directories(dir, True)` lists what is on disk. Ingest results report `has_depth`; a
take without depth can only go the MonoFootage route. ARKit-mode Live Link Face takes have no depth.

## 1. Webcam / DSLR video → animation

```python
cd = json.loads(anim.ingest_mono_video("D:/Takes/line01.mp4", "", "line01", 1))["capture_data"]
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_line01", "MonoFootage", cd, "", ""))["performance"]
anim.configure_performance(p, '{"HeadMovementMode":"ControlRig","bHeadStabilization":true}')   # optional
r  = json.loads(anim.process(p, -1, -1))              # blocking; r["processed_frames"]
a  = json.loads(anim.export_anim_sequence(p, "/Game/MHA/Anims", "AS_line01", "", True, True))
```

Good footage: face filling a third of the frame, even light, 24-60 fps, performer facing the
camera. Processing needs a D3D12/Vulkan GPU and the NNE ORT runtime; `process` returns
`CANNOT_PROCESS` with the status when inputs are incomplete.

## 2. iPhone take → identity → animation (best fidelity)

```python
cd = json.loads(anim.ingest_live_link_face_take("D:/Takes/Kevin_Neutral"))["capture_data"]
i  = json.loads(ident.create_identity_from_footage("/Game/MHA", "ID_Kevin", cd, 120, 1, True, False))["identity"]
# frame 120 = a frame where the performer holds a relaxed neutral face. Auto-rig is asynchronous:
st = json.loads(ident.get_status(i))                  # poll until not st["auto_rig_in_progress"] and st["face"]["has_dna"]
ident.prepare_for_performance(i)                       # trains predictive solvers (required for DepthFootage)
cd2 = json.loads(anim.ingest_live_link_face_take("D:/Takes/Kevin_Line01"))["capture_data"]
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_Kevin01", "DepthFootage", cd2, i, ""))["performance"]
anim.process(p, -1, -1)
```

The same identity can give the character its likeness:
`MetaHumanCharacterService.import_from_identity(character, i, True, True, False)`.
Identity creation needs the Epic login for auto-rig (`ident.log_in_to_service("")`). Body type index
1-6 picks the preset body used for previews only.

## 3. Audio only

```python
p = json.loads(anim.create_performance("/Game/MHA", "Perf_vo01", "Audio", "", "", "/Game/Audio/vo01"))["performance"]
anim.set_audio_mood(p, "Happiness", 0.6, "FullFace", "ControlRig")   # moods: AutoDetect, Neutral, Happiness, Sadness, Disgust, Anger, Surprise, Fear, ...
anim.process(p, -1, -1)
anim.export_anim_sequence(p, "/Game/MHA/Anims", "AS_vo01", "", True, True)
```

Audio processing runs on CPU and needs no GPU; `MouthOnly` output controls keep the upper face free
for hand animation. Import the wav first (any importer) so a SoundWave asset exists.

## Export and apply

- `export_anim_sequence(p, pkg, name, target_skeleton, head_movement, processing_range_only)`:
  target empty = performer skeleton; pass `/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton`
  (exists once a MetaHuman has been built) to play on any MetaHuman face. Play it on the built
  Blueprint's Face component (Sequencer animation track, or the Face AnimBP slot).
- `export_level_sequence(p, pkg, name, target_metahuman_bp, identity, camera, audio, video)`: with a
  built MetaHuman Blueprint path the sequence drives that character with the Face control rig track,
  head movement, and audio. Open it with `MetaHumanSetupService.open_in_editor`.
- `get_status(p)` anytime: inputs, `can_process`, `processed_frames`, `diagnostics_warning`.
- `configure_performance(p, json)` for anything else (`SolveType`, `bSkipDiagnostics`,
  `StartFrameToProcess`/`EndFrameToProcess`, `FocalLength`, `UserExcludedFrames`).

## Gotchas

- Capture data assets land under `/Game/MHA` style folders chosen by Capture Manager settings; the
  ingest result gives the real path, use it.
- One performance processes at a time. `process` blocks the game thread; MCP clients with short
  timeouts should poll `get_status` rather than re-run.
- An identity must be prepared (`face.has_predictive_solvers`) before a DepthFootage performance
  reports `can_process`.
- Epic's own sample scripts use mood names like `HAPPY`; the 5.8 enum names are `Happiness`,
  `Sadness`, etc. The service accepts enum names case-insensitively.
- Engine-level reference: UnrealEngineSkills `ue-metahuman-animator`.
