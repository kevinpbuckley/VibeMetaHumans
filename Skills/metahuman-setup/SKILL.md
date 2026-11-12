---
name: metahuman-setup
display_name: MetaHuman Setup and Discovery
description: Check the MetaHuman environment (plugins, optional content, cloud login), list MetaHuman assets (characters, presets, identities, performances, capture data, wardrobe items, built Blueprints), look up enum option names and open MetaHuman asset editors with unreal.MetaHumanSetupService. Use at the start of any MetaHuman task and whenever a tool says an asset or option was not found.
vibeue_classes:
  - MetaHumanSetupService
keywords:
  - metahuman status
  - optional content
  - metahuman creator core data
  - list metahumans
  - presets
  - wardrobe items
---

# MetaHuman Setup and Discovery

```python
import json, unreal
svc = unreal.MetaHumanSetupService
```

## get_status()

Returns one JSON object. Fields that drive decisions:

| Field | Meaning | If bad |
|---|---|---|
| `plugins.<Name>` | `enabled` / `disabled` / `missing` for every MetaHuman-related plugin | Ask the user to enable it in Edit → Plugins (or the `.uproject`) and restart; nothing else fixes this |
| `optional_content_installed` | the "MetaHuman Creator Core Data" launcher option (`<Engine>/Plugins/MetaHuman/MetaHumanCharacter/Content/Optional`) | Presets, wardrobe items and texture synthesis are unavailable. Creator still works from the blank template; Animator and Live Link are unaffected. Tell the user how to install it (Epic Games Launcher → Library → Engine 5.8 → Options) |
| `character_editor_subsystem` | MetaHuman Creator's editor subsystem exists | Creator plugin not loaded |
| `texture_synthesis_enabled` | the texture-synthesis model loaded | needs the optional content |
| `asset_counts` | characters, presets, identities, performances, capture data, wardrobe items, built MetaHuman Blueprints | zero presets with optional content installed means the asset registry has not scanned yet; call again |
| `live_link_sources` / `live_link_subjects` | current Live Link state | – |

## list_assets(kind, search_root)

`kind`: `character`, `preset`, `identity`, `performance`, `capture_data`, `wardrobe`, `collection`,
`blueprint` (built MetaHumans under `/Game/MetaHumans`), `soundwave`, `animation`, `level_sequence`.
`search_root` empty uses the kind's default (`/Game`, or the plugin's `Optional` folder for presets
and wardrobe). Returns object paths you can pass straight to the other services.

Wardrobe item naming (optional content): `/MetaHumanCharacter/Optional/Grooms/Bindings/<Slot>/WI_<...>`
where `<Slot>` is `Hair`, `Eyebrows`, `Eyelashes`, `Mustaches`, `Beards`, `Peachfuzz`; clothing under
`/MetaHumanCharacter/Optional/Clothing/WI_<...>` (slot `Outfits`).

## get_enum_options()

Names accepted by the other services: `body_type` (`f_med_nrw` … `m_tal_ovw`, `BlendableBody`),
`rig_type` (`JointsOnly`, `JointsAndBlendShapes`), `pipeline_type` (`Cinematic`, `Optimized`, `UEFN`),
`quality_level` (`Low`, `Medium`, `High`, `Cinematic`), `performance_input_type` (`DepthFootage`,
`Audio`, `MonoFootage`), `head_movement_mode`, `audio_mood`, `audio_output_controls`, plus the
settings section names and wardrobe slot names.

## open_in_editor(asset_path)

Opens the MetaHuman Character editor, Identity editor, Performance editor, etc. Use it when the user
wants to look at a result; it is not needed for any scripted step.

## Typical opening sequence

```python
s = json.loads(svc.get_status())
assert s["plugins"]["MetaHumanCharacter"] == "enabled"
chars = json.loads(svc.list_assets("character", "/Game"))["assets"]
presets = json.loads(svc.list_assets("preset", ""))["assets"] if s["optional_content_installed"] else []
```
