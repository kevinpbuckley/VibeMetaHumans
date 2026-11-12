# Copyright Buckley Builds LLC 2026 All Rights Reserved.
"""Agent-ergonomics helpers for VibeMetaHumans.

Usage from the engine's execute_python MCP tool or any editor Python:

    import unreal, vibemetahumans as vmh

    print(vmh.status())                       # plugins, optional content, cloud login, counts
    for name in vmh.list_toolsets(): print(name)
    print(vmh.describe("VibeMetaHumans.MetaHumanCharacterService"))

The services themselves are plain Unreal toolsets: call them directly as
``unreal.MetaHumanCharacterService.<snake_case_tool>(...)`` etc. This module only adds discovery,
JSON decoding and a couple of asset helpers that are awkward to express as static UFUNCTIONs.
"""

import json

import unreal

TOOLSET_PREFIX = "VibeMetaHumans."


def _decode(value):
    """Decode nested JSON strings the way the engine's execute_tool sometimes returns them."""
    for _ in range(4):
        if not isinstance(value, str):
            return value
        try:
            value = json.loads(value)
        except ValueError:
            return value
    return value


def list_toolsets():
    """Names of every registered toolset that belongs to VibeMetaHumans."""
    raw = unreal.ToolsetRegistry.get_all_toolset_json_schemas()
    data = _decode(raw)
    names = []
    if isinstance(data, dict):
        items = data.get("toolsets", data)
        names = list(items.keys()) if isinstance(items, dict) else [i.get("name", "") for i in items]
    elif isinstance(data, list):
        names = [i.get("name", "") if isinstance(i, dict) else str(i) for i in data]
    return sorted(n for n in names if n.startswith(TOOLSET_PREFIX))


def describe(toolset_name):
    """The JSON schema (tools + parameters) of one toolset, by its namespaced name."""
    raw = unreal.ToolsetRegistry.get_all_toolset_json_schemas()
    data = _decode(raw)
    items = data.get("toolsets", data) if isinstance(data, dict) else data
    if isinstance(items, dict):
        return _decode(items.get(toolset_name))
    for item in items or []:
        if isinstance(item, dict) and item.get("name") == toolset_name:
            return item
    return None


def status():
    """One-call environment summary: which MetaHuman pieces are usable right now."""
    svc = getattr(unreal, "MetaHumanSetupService", None)
    if svc is None:
        return {"error": "VibeMetaHumans is not loaded (enable the plugin and restart the editor)."}
    return _decode(svc.get_status())


def find_characters(search_root="/Game"):
    """Package paths of every MetaHumanCharacter asset under search_root."""
    registry = unreal.AssetRegistryHelpers.get_asset_registry()
    flt = unreal.ARFilter(
        package_paths=[search_root],
        recursive_paths=True,
        class_paths=[unreal.TopLevelAssetPath("/Script/MetaHumanCharacter", "MetaHumanCharacter")],
    )
    return [str(a.package_name) for a in registry.get_assets(flt)]


def load(path):
    """Load an asset by package or object path and return it (None when missing)."""
    return unreal.EditorAssetLibrary.load_asset(path)
