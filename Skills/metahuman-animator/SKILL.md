---
name: metahuman-animator
display_name: MetaHuman Animator (offline facial animation)
description: Turn footage or audio into MetaHuman facial animation with unreal.MetaHumanAnimatorService and unreal.MetaHumanIdentityService - ingest a webcam/DSLR video, an iPhone Live Link Face take or a take archive into Footage Capture Data (Epic CaptureManagerToolset), build and auto-rig an Identity from a neutral frame (Mesh to MetaHuman), create MonoFootage / DepthFootage / Audio Performances, process (Epic ProcessPerformance) and export an AnimSequence (Epic) or LevelSequence onto a MetaHuman. Use for "animate the face from this video/phone take/voice line".
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
# Epic AI tools (inside execute_python): await call_tool("<Toolset.Full.Name>.<Tool>", camelCaseParam=value)
CM   = "CaptureManagerToolset.CaptureManagerIngestTools"
PERF = "MetaHumanAnimatorToolset.MetaHumanAnimatorPerformanceTools"
IDT  = "MetaHumanAnimatorToolset.MetaHumanAnimatorIdentityTools"
INS  = "CaptureManagerToolset.CaptureManagerInspectionTools"
EXP  = "MetaHumanAnimatorToolset.MetaHumanAnimatorExportTools"
```

Ingest, processing, identity auto-rig / prepare and AnimSequence export are Epic's own tools now
(`CaptureManagerToolset`, `MetaHumanAnimatorToolset`, `EditorToolset`; the plugin declares them as optional
dependencies, and the project must enable them, e.g. via `AllToolsets` / `AllMetaHumanToolsets`).
This service keeps Performance creation, configuration, status, cancel and Level Sequence export.

## Choose the route by input

| Input | Capture data | Identity | Performance `InputType` |
|---|---|---|---|
| Video file from a webcam / DSLR / phone camera roll | `CM.IngestMonoVideo(videoFilePath, audioFilePath, slate, takeNumber)` | none | `MonoFootage` |
| iPhone Live Link Face take (**MetaHuman Animator** capture mode → has depth) | `CM.IngestLiveLinkFace(takeDirectoryPath)` | required, prepared | `DepthFootage` |
| Take archive (.zip from Live Link Hub / Capture Manager) | `CM.IngestTakeArchive(takeArchivePath)` (a directory containing the .cptake also works) | depends on contents | as above |
| Audio file (SoundWave asset) | none | none | `Audio` |

`CM.ScanForTakes(searchDirectory, bRecursive, maxResults=0)` lists what is on disk and says which ingest verb fits
(`bIsTakeArchive` / `bIsLiveLinkFace` / video files). `INS.InspectFootageCapture(captureData)` reports the
stream layout (image, depth, audio) of an ingested Footage Capture Data; a take without depth can only go
the MonoFootage route. ARKit-mode Live Link Face takes have no depth. Epic ingests are asynchronous and are
not retry-safe: do not re-issue an ingest that is still running.

## 1. Webcam / DSLR video → animation

```python
cd = (await call_tool(CM + ".IngestMonoVideo", videoFilePath="D:/Takes/line01.mp4", audioFilePath="", slate="line01", takeNumber=1))["captureDataAssetPath"]   # the result is a struct; a failed ingest raises or completes with an error, never a path
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_line01", "MonoFootage", cd, "", ""))["performance"]
anim.configure_performance(p, '{"HeadMovementMode":"ControlRig","bHeadStabilization":true}')   # optional
r  = await call_tool(PERF + ".ProcessPerformance", performance=p, params={})   # async; for a range pass params={"processingFrameRange":{"startFrame":0,"endFrame":120}} (end exclusive)
await call_tool("EditorToolset.AssetToolset.SaveAsset", assetPath=p)            # Epic does not save the performance
a  = await call_tool(EXP + ".ExportPerformanceAnimationSequence", performance=p,
        options={"assetPath": "/Game/MHA/Anims", "assetName": "AS_line01", "targetSkeletonOrSkeletalMesh": "", "bEnableHeadMovement": True, "exportRange": "ProcessingRange"})
```

Good footage: face filling a third of the frame, even light, 24-60 fps, performer facing the
camera. Processing needs a D3D12/Vulkan GPU and the NNE ORT runtime; `ProcessPerformance` refuses
with a classified error when inputs are incomplete (`get_status` shows `can_process`).

## 2. iPhone take → identity → animation (best fidelity)

```python
cd = (await call_tool(CM + ".IngestLiveLinkFace", takeDirectoryPath="D:/Takes/Kevin_Neutral"))["captureDataAssetPath"]
i  = json.loads(ident.create_identity_from_footage("/Game/MHA", "ID_Kevin", cd, 120, 1, True, False))["identity"]
# frame 120 = a frame where the performer holds a relaxed neutral face. Auto-rig is asynchronous:
st = json.loads(ident.get_status(i))                  # poll until not st["auto_rig_in_progress"] and st["face"]["has_dna"]
await call_tool(IDT + ".PrepareIdentityForPerformance", identity=i)   # trains predictive solvers (required for DepthFootage); awaits the result
await call_tool("EditorToolset.AssetToolset.SaveAsset", assetPath=i)
cd2 = (await call_tool(CM + ".IngestLiveLinkFace", takeDirectoryPath="D:/Takes/Kevin_Line01"))["captureDataAssetPath"]
p  = json.loads(anim.create_performance("/Game/MHA", "Perf_Kevin01", "DepthFootage", cd2, i, ""))["performance"]
await call_tool(PERF + ".ProcessPerformance", performance=p, params={})
```

The same identity can give the character its likeness:
`MetaHumanCharacterService.import_from_identity(character, i, True, True, False)`.
Identity creation needs the Epic login for auto-rig (`ident.log_in_to_service("")`). To (re)run the auto-rig later, sign in first, then `await call_tool(IDT + ".GenerateIdentityRig", identity=i)` (it refuses when not logged in and waits for the cloud result). Body type index
1-6 picks the preset body used for previews only.

## 3. Audio only

```python
p = json.loads(anim.create_performance("/Game/MHA", "Perf_vo01", "Audio", "", "", "/Game/Audio/vo01"))["performance"]
# mood / output controls / head movement: one ObjectToolset.SetProperties on the Performance. Moods: AutoDetect, Neutral, Happiness, Sadness, Disgust, Anger, Surprise, Fear, ...
await call_tool("EditorToolset.ObjectToolset.SetProperties", instance=p,
    values=json.dumps({"AudioDrivenAnimationSolveOverrides": {"Mood": "Happiness", "MoodIntensity": 0.6}, "AudioDrivenAnimationOutputControls": "FullFace", "HeadMovementMode": "ControlRig"}))
await call_tool(PERF + ".ProcessPerformance", performance=p, params={})
await call_tool("EditorToolset.AssetToolset.SaveAsset", assetPath=p)
await call_tool(EXP + ".ExportPerformanceAnimationSequence", performance=p,
    options={"assetPath": "/Game/MHA/Anims", "assetName": "AS_vo01", "targetSkeletonOrSkeletalMesh": "/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton", "bEnableHeadMovement": True, "exportRange": "ProcessingRange"})
# for a NEW audio performance, PERF + ".GenerateAnimationFromAudio" takes mood, moodIntensity, outputControls and a head-movement option directly
```

Audio processing runs on CPU and needs no GPU; `MouthOnly` output controls keep the upper face free
for hand animation. Import the wav first (any importer) so a SoundWave asset exists.

## Export and apply

- `EXP.ExportPerformanceAnimationSequence(performance, options={assetPath, assetName, targetSkeletonOrSkeletalMesh, bEnableHeadMovement, exportRange})`
  (`exportRange`: `ProcessingRange` | `WholeSequence`). Epic validates the target skeleton, saves the result and
  refuses to overwrite an existing asset; with an empty target it binds `Face_Archetype_Skeleton` for a face-only
  export. Epic object parameters need full `Package.Object` paths (a bare package path is rejected). Pass `/Game/MetaHumans/Common/Face/Face_Archetype_Skeleton.Face_Archetype_Skeleton` explicitly (exists once a MetaHuman has
  been built) to play on any MetaHuman face. Play it on the built
  Blueprint's Face component (Sequencer animation track, or the Face AnimBP slot).
- `export_level_sequence(p, pkg, name, target_metahuman_bp, identity, camera, audio, video)`: with a
  built MetaHuman Blueprint path the sequence drives that character with the Face control rig track,
  head movement, and audio. Open it with `EditorToolset.EditorAppToolset.OpenEditorForAsset(assetPath)`.
- `get_status(p)` anytime: inputs, `can_process`, `processed_frames`, `diagnostics_warning`.
- `configure_performance(p, json)` for anything else (`SolveType`, `bSkipDiagnostics`,
  `StartFrameToProcess`/`EndFrameToProcess`, `FocalLength`, `UserExcludedFrames`).

## Gotchas

- Capture data assets land under `/Game/MHA` style folders chosen by Capture Manager settings; the
  ingest result gives the real path, use it.
- One performance processes at a time. `ProcessPerformance` is asynchronous and leaves the editor responsive;
  poll `get_status` (`is_processing`) and use `cancel_processing` to stop it. Do not re-issue it while running.
- An identity must be prepared (`face.has_predictive_solvers`) before a DepthFootage performance
  reports `can_process`.
- Epic's own sample scripts use mood names like `HAPPY`; the 5.8 enum names are `Happiness`,
  `Sadness`, etc. The service accepts enum names case-insensitively.
- **Face clips played as montages do not show on the plugin's face AnimBP.** `/MetaHumanCharacter/Face/ABP_Face`
  is CopyPoseFromMesh only, with no Slot node, so a face AnimSequence played on a slot (`PlaySlotAnimationAsDynamicMontage`,
  a sequencer slot track) is ignored. Use your own face AnimBP in the project: copy pose from the body, then a layered
  blend with a `DefaultSlot`, and set it as the Face component's anim class on the built Blueprint (2026-11-12).
- **Exported head movement snaps the head.** In `ABP_Face_PostProcess`, `HeadControlSwitch` is an on/off gate for
  the head-movement IK (0.35 = off, no blend); when on, it snaps the head to the archetype pose (about 4.4 cm forward
  and down on one character). Scaling HeadYaw/Pitch/Translation does not remove that offset: multiply
  `HeadControlSwitch` by 0 in your face AnimBP, or export without head movement.
- An AnimGraph node's `show_pin_for_properties` cannot be applied from Python (no ReconstructNode; set_editor_property
  does not fire the PostEditChange). Use node property maps such as ModifyCurve's `curve_map`.
- **Never duplicate an engine-plugin AnimBP into /Game as a test fixture** (e.g. `ABP_MH_LiveLink`, `ABP_Face`).
  Deleting the copy runs ForceDeleteObjects, which cannot unload a package that references MetaHuman Control Rig
  classes, and the editor then fatals on a `RigVMHost` cast. Build fixtures from a minimal AnimBP you create.
- Engine-level reference: UnrealEngineSkills `ue-metahuman-animator`.
