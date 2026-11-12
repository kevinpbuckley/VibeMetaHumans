---
name: metahuman-setup
display_name: MetaHuman Setup and Discovery
description: Check the MetaHuman environment (plugins, optional content, cloud login), look up enum option names with unreal.MetaHumanSetupService, and list MetaHuman assets (characters, presets, identities, performances, capture data, wardrobe items, built Blueprints) or open their editors through Epic AssetToolset.FindAssets / EditorAppToolset.OpenEditorForAsset. Use at the start of any MetaHuman task and whenever a tool says an asset or option was not found.
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

## Listing assets (Epic AssetToolset.FindAssets)

Asset discovery is Epic's `EditorToolset.AssetToolset.FindAssets` (this service no longer has `list_assets`).
Call it with a `filter` of `{folderPath, assetType, bRecursive: true, maxResults: 0}` (`maxResults` 0 = all; the
default caps at 100). It returns asset references you can pass straight to the other services. One call per kind:

| kind | `assetType` (class path) | default `folderPath` |
|---|---|---|
| character | `/Script/MetaHumanCharacter.MetaHumanCharacter` | `/Game` |
| preset | `/Script/MetaHumanCharacter.MetaHumanCharacter` | `/MetaHumanCharacter/Optional/Presets` |
| identity | `/Script/MetaHumanIdentity.MetaHumanIdentity` | `/Game` |
| performance | `/Script/MetaHumanPerformance.MetaHumanPerformance` | `/Game` |
| capture_data | `/Script/CaptureDataCore.CaptureData` | `/Game` |
| wardrobe | `/Script/MetaHumanCharacterPalette.MetaHumanWardrobeItem` | `/MetaHumanCharacter/Optional` |
| collection | `/Script/MetaHumanCharacterPalette.MetaHumanCollection` | `/Game` |
| blueprint (built MetaHumans) | `/Script/Engine.Blueprint` | `/Game/MetaHumans` |
| soundwave | `/Script/Engine.SoundWave` | `/Game` |
| animation | `/Script/Engine.AnimSequence` | `/Game` |
| level_sequence | `/Script/LevelSequence.LevelSequence` | `/Game` |

Class matching includes subclasses. An empty result under `/MetaHumanCharacter/Optional` usually means the
optional content is not installed (see `optional_content_installed` above). For groom slots,
`MetaHumanCharacterService.list_wardrobe_items(slot)` is the plugin-aware alternative.

Wardrobe item naming (optional content): `/MetaHumanCharacter/Optional/Grooms/Bindings/<Slot>/WI_<...>`
where `<Slot>` is `Hair`, `Eyebrows`, `Eyelashes`, `Mustaches`, `Beards`, `Peachfuzz`; clothing under
`/MetaHumanCharacter/Optional/Clothing/WI_<...>` (slot `Outfits`).

## get_enum_options()

Names accepted by the other services: `body_type` (`f_med_nrw` … `m_tal_ovw`, `BlendableBody`),
`rig_type` (`JointsOnly`, `JointsAndBlendShapes`), `pipeline_type` (`Cinematic`, `Optimized`, `UEFN`),
`quality_level` (`Low`, `Medium`, `High`, `Cinematic`), `performance_input_type` (`DepthFootage`,
`Audio`, `MonoFootage`), `head_movement_mode`, `audio_mood`, `audio_output_controls`, plus the
settings section names and wardrobe slot names.

## Opening an editor (Epic EditorAppToolset.OpenEditorForAsset)

`EditorToolset.EditorAppToolset.OpenEditorForAsset(assetPath)` opens the MetaHuman Character editor, Identity
editor, Performance editor, etc. Use it when the user wants to look at a result; it is not needed for any
scripted step.

## Typical opening sequence

```python
s = json.loads(svc.get_status())
assert s["plugins"]["MetaHumanCharacter"] == "enabled"
chars = await call_tool("EditorToolset.AssetToolset.FindAssets", filter={"folderPath": "/Game", "assetType": "/Script/MetaHumanCharacter.MetaHumanCharacter", "bRecursive": True, "maxResults": 0})
presets = (await call_tool("EditorToolset.AssetToolset.FindAssets", filter={"folderPath": "/MetaHumanCharacter/Optional/Presets", "assetType": "/Script/MetaHumanCharacter.MetaHumanCharacter", "bRecursive": True, "maxResults": 0})) if s["optional_content_installed"] else []
```
