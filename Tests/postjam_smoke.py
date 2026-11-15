# VibeMetaHumans post-jam smoke tests (2026-11-14). Headless, in a host project that has built MetaHumans:
#   UnrealEditor.exe <host>.uproject -ExecCmds="py <this file> <groups> <bp> <anim>" -unattended -RenderOffscreen
# groups: "+"-joined list of describe snapshot fixup audit push capture accessory blinks faceabp intake landmarks, or all
# (not commas: -ExecCmds splits commands on commas)
# bp: a built MetaHuman Blueprint with leader-pose garments (default /Game/DirectorsCut/Characters/Lila/BP_Lila)
# anim: a body AnimSequence for the pose tests (default /Game/DirectorsCut/Animation/Body/A_Idle_Seated_Lila)
# Writes Saved/VibeMetaHumans/tests/postjam.json (one entry per check: ok + details) and quits the editor.
# Everything it creates goes under /Game/VmhTest (duplicates; the source assets are never edited) and is not saved.
import json
import os
import sys
import traceback

import unreal

ARGS = [a for a in sys.argv[1:]]
GROUPS = set((ARGS[0] if ARGS else "all").replace(",", "+").split("+"))   # -ExecCmds splits on commas: use +
BP = ARGS[1] if len(ARGS) > 1 else "/Game/DirectorsCut/Characters/Lila/BP_Lila"
ANIM = ARGS[2] if len(ARGS) > 2 else "/Game/DirectorsCut/Animation/Body/A_Idle_Seated_Lila"
TEST = "/Game/VmhTest"
OUT = os.path.join(unreal.Paths.project_saved_dir(), "VibeMetaHumans", "tests")
os.makedirs(OUT, exist_ok=True)
EAL = unreal.EditorAssetLibrary
wd, mh, an = unreal.MetaHumanWardrobeService, unreal.MetaHumanCharacterService, unreal.MetaHumanAnimatorService
results = {}


def J(s):
    return json.loads(s)


def check(name, ok, **details):
    results[name] = dict(ok=bool(ok), **details)
    unreal.log(f"VMHTEST {name}: {'PASS' if ok else 'FAIL'} {json.dumps(details)[:600]}")


def run(group, fn):
    if "all" not in GROUPS and group not in GROUPS:
        return
    try:
        fn()
    except Exception as e:  # noqa: BLE001
        check(group + ".exception", False, error=str(e), trace=traceback.format_exc()[-1500:])


def scs(bp):
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    out = {}
    for h in sds.k2_gather_subobject_data_for_blueprint(bp):
        d = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h)
        o = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(d)
        if o is not None:
            out.setdefault(str(unreal.SubobjectDataBlueprintFunctionLibrary.get_variable_name(d)), o)
    return out


def dup_bp(name):
    dst = f"{TEST}/{name}"
    if EAL.does_asset_exist(dst):
        EAL.delete_asset(dst)
    EAL.duplicate_asset(BP, dst)
    return dst


def garments(bp_path):
    """leader-pose skeletal garment components (parented under Body, not Face/Body themselves)"""
    nodes = scs(unreal.load_asset(bp_path))
    out = []
    for n, o in nodes.items():
        if isinstance(o, unreal.SkeletalMeshComponent) and n not in ("Body", "Face") and o.get_editor_property("skeletal_mesh_asset"):
            out.append(n)
    return nodes, out


def body_mesh(nodes):
    return nodes["Body"].get_editor_property("skeletal_mesh_asset").get_path_name()


# ------------------------------------------------------------------ #3 describe + LOD check
def t_describe():
    nodes, gs = garments(BP)
    body = body_mesh(nodes)
    r = J(wd.describe_skeletal_mesh(body, "", False))
    check("describe.body", r["success"] and r["lod_count"] >= 1 and "bones_not_evaluated" in r["lods"][0],
          lods=r.get("lod_count"), not_eval=[l.get("bones_not_evaluated") for l in r.get("lods", [])],
          dropped_lod1=(r["lods"][1].get("bones_dropped_since_previous_lod", [])[:5] if r.get("lod_count", 0) > 1 else None))
    for g in gs[:4]:
        mesh = nodes[g].get_editor_property("skeletal_mesh_asset").get_path_name()
        d = J(wd.describe_skeletal_mesh(mesh, body, False))
        last = d["lods"][-1]["as_follower"]
        f = J(wd.fix_unevaluated_bone_weights(mesh, body, True))
        ranges = [l.get("leader_lods") for l in f.get("lods", [])]
        # every body LOD the garment is drawn at must be checked: the last readable garment LOD (generated LODs ride on
        # their base LOD's weights) reaches the body's last LOD
        tops = [int(x.split("-")[-1]) for x in ranges if x]
        check(f"describe.{g}", d["success"] and f["success"] and tops and max(tops) == r["lod_count"] - 1,
              garment_lods=d["lod_count"], leader_ranges=ranges, body_lods=r["lod_count"],
              skipped=[l.get("skipped", "")[-90:] for l in f.get("lods", []) if l.get("skipped")][:2],
              last_lod_unevaluated=last.get("unevaluated_bone_vertices"), would_fix=f.get("vertices_fixed"))


# ------------------------------------------------------------------ #4 snapshot keeps explicit clears
def t_snapshot():
    path = dup_bp("BP_SnapTest")
    bp = unreal.load_asset(path)
    nodes, gs = garments(path)
    victim = gs[0]
    other = nodes[victim].get_editor_property("skeletal_mesh_asset")
    nodes[victim].set_editor_property("skeletal_mesh_asset", None)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    snap = J(wd.snapshot_blueprint_components(path, ""))["snapshot"]
    rec = [c for c in snap["components"] if c["name"] == victim][0]["properties"]
    check("snapshot.records_clear", rec.get("SkinnedAsset", "missing") in ("None", ""), recorded=rec.get("SkinnedAsset"), keys=sorted(rec)[:30])
    # a "rebuild" puts the mesh back; the restore must clear it again
    nodes[victim].set_editor_property("skeletal_mesh_asset", other)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    r = J(wd.restore_blueprint_components(path, json.dumps(snap), "[]", True, True, False))
    after = scs(unreal.load_asset(path))[victim].get_editor_property("skeletal_mesh_asset")
    check("snapshot.restore_clears", r["success"] and after is None, applied=r.get("applied"), failed=r.get("failed"))


# ------------------------------------------------------------------ #4 post-build fix-up (dry run on the real BP)
def t_fixup():
    r = J(wd.post_build_fixup(BP, json.dumps({"brow_opacity": 2.0}), True))
    check("fixup.dry_run", r["success"] and "grooms" in r and "textures" in r,
          grooms=[(g["component"], len(g.get("changes", [])), len(g.get("gaps", []))) for g in r.get("grooms", [])],
          brows=r.get("brow_materials"), textures=len(r.get("textures", [])), face=r.get("face_anim_class_before"))


# ------------------------------------------------------------------ #4 placed-instance audit
def t_audit():
    path = dup_bp("BP_AuditTest")
    les = unreal.get_editor_subsystem(unreal.LevelEditorSubsystem)
    les.new_level("/Temp/VmhAudit")
    eas = unreal.get_editor_subsystem(unreal.EditorActorSubsystem)
    actor = eas.spawn_actor_from_class(unreal.load_asset(path).generated_class(), unreal.Vector(0, 0, 0))
    clean = J(wd.audit_placed_instances(path, False, ""))
    body = [c for c in actor.get_components_by_class(unreal.SkeletalMeshComponent) if c.get_name() == "Body"][0]
    face = [c for c in actor.get_components_by_class(unreal.SkeletalMeshComponent) if c.get_name() == "Face"][0]
    body.set_skeletal_mesh_asset(face.get_skeletal_mesh_asset())   # a per-instance override, as the jam's level had
    dirty = J(wd.audit_placed_instances(path, False, ""))
    props = [(o["component"], o["property"]) for a in dirty["actors"] for o in a["overrides"]]
    cleared = J(wd.audit_placed_instances(path, True, ""))
    again = J(wd.audit_placed_instances(path, False, ""))
    check("audit.finds_and_clears", dirty["instances"] == 1 and ("Body", "SkinnedAsset") in props and again["overrides"] == 0,
          clean=clean.get("overrides"), found=props[:10], cleared=cleared.get("cleared"), after=again.get("overrides"),
          clean_list=[(o["component"], o["property"]) for a in clean.get("actors", []) for o in a["overrides"]][:10])


run("describe", t_describe)
run("snapshot", t_snapshot)
run("fixup", t_fixup)
run("audit", t_audit)

with open(os.path.join(OUT, "postjam.json"), "w") as f:
    json.dump(results, f, indent=1)
unreal.log("VMHTEST DONE " + json.dumps({k: v["ok"] for k, v in results.items()}))
unreal.SystemLibrary.quit_editor()
