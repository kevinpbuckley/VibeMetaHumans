---
name: metahuman-live-link
display_name: MetaHuman Live Link (real-time face capture)
description: Drive a MetaHuman's face live in the editor with unreal.MetaHumanLiveLinkService - add a MetaHuman Video source from a webcam, a MetaHuman Audio source from a microphone, or connect the Live Link Face app on an iPhone; calibrate neutrals, head tracking and smoothing; bind the subject to a placed MetaHuman actor (face AnimBP); record with Take Recorder. Use for "hook my webcam/phone up to the MetaHuman", "it is not moving", "record a take".
vibeue_classes:
  - MetaHumanLiveLinkService
  - MetaHumanObjectService
unreal_classes:
  - UMetaHumanLocalLiveLinkSourceBlueprint
  - ULiveLinkFaceSourceBlueprint
  - UMetaHumanLiveLinkSubjectSettings
  - UTakeRecorderBlueprintLibrary
keywords:
  - live link
  - live link face
  - webcam
  - real time facial animation
  - take recorder
  - calibrate neutral
---

# MetaHuman Live Link (real-time face capture)

```python
import json, unreal
ll = unreal.MetaHumanLiveLinkService
```

## Sources

| Source | Tool | Needs |
|---|---|---|
| Webcam / capture card (local MetaHuman Animator real-time solve) | `add_video_source(device_substring, subject, format_index)` | a camera; the MetaHuman Live Link plugin's real-time models (installed with the engine) |
| Microphone (audio-to-face) | `add_audio_source(device_substring, subject)` | a mic |
| iPhone / iPad running Live Link Face | `add_live_link_face_source(phone_ip, subject, 14785)` | app open, **MetaHuman Animator** mode, same network |

`list_video_devices(True)` shows cameras with their formats (index, width, height, fps); leave
`format_index` at -1 for an automatic 720p-1080p / 24-60 fps choice. Device matching is a substring of
the name. Source creation blocks a few seconds while the device starts.

`list_subjects()` reports `state` per subject: `Connected` is good; `Unresponsive` after a phone
connect means the app is not streaming (wrong mode or IP); `Disconnected` means the device went away.
`remove_source(guid)` / `remove_source("all")` cleans up.

## Calibrate

With the performer relaxed and facing the camera: `capture_neutrals(subject)` (neutral frame + head
pose). Head output: `set_head_tracking(subject, orientation, translation, stabilization)`. Everything
else on the subject settings object is reachable with `subject_settings(subject, json_patch)` (empty
patch = read): smoothing params, calibration alpha, mood for audio subjects.

## Bind to a character

```python
r = json.loads(ll.bind_subject_to_actor("BP_MH_Hero", "Webcam", True))
print(r["properties_set"], r["anim_blueprint_switched_on"])
```

Works on a placed, **built** MetaHuman Blueprint: it sets the Blueprint's Live Link subject
properties, any `LiveLinkComponentController`, and the Face component's `LLink_Face_Subj` variable
(switching the Face to `ABP_MH_LiveLink` when needed). `NO_LIVE_LINK_INPUT` means the actor is not a
MetaHuman Blueprint (the Creator preview actor does not take Live Link); build the character first
(`MetaHumanCharacterService.build`) and place `BP_<Name>`.

## Record

```python
ll.start_take_recording(["Webcam"])     # opens Take Recorder, adds a Live Link source per subject, starts
...
r = json.loads(ll.stop_take_recording()) # r["last_recorded_sequence"]
ll.get_take_recorder_status()
```

Take Recorder writes a Level Sequence under `/Game/Cinematics/Takes/<date>/` with an animation track
per subject. For a clean AnimSequence of the character, record the actor itself in Take Recorder
too, or process the same take offline with `MetaHumanAnimatorService` (higher quality than the
real-time solve).

## Gotchas

- Subjects persist for the editor session only; re-add after a restart (or save a Live Link preset).
- The real-time video solve needs the GPU; expect a few seconds of warm-up on the first frames.
- The phone must show the same IP you pass; port 14785 is the Live Link Face control port.
- `capture_neutrals` again whenever the performer or camera moves.
- Engine-level reference: UnrealEngineSkills `ue-metahuman-live-link`.
