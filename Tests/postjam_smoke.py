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
RUN = __import__("time").strftime("%H%M%S")   # the GeometryScript helpers save new meshes and never overwrite: unique names
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
    # ... unless that property is skipped (a value changed on purpose after the snapshot), and a dry run changes nothing
    s = J(wd.restore_blueprint_components(path, json.dumps(snap), json.dumps([victim + ".SkinnedAsset"]), True, True, False))
    d = J(wd.restore_blueprint_components(path, json.dumps(snap), "[]", True, True, True))
    kept = scs(unreal.load_asset(path))[victim].get_editor_property("skeletal_mesh_asset")
    check("snapshot.skip_and_dry_run", s["success"] and not s["applied"] and kept == other and [a["property"] for a in d["applied"]] == [victim + ".SkinnedAsset"],
          skipped=s.get("skipped"), applied=s.get("applied"), dry=[a["property"] for a in d.get("applied", [])])
    r = J(wd.restore_blueprint_components(path, json.dumps(snap), "[]", True, True, False))
    after = scs(unreal.load_asset(path))[victim].get_editor_property("skeletal_mesh_asset")
    check("snapshot.restore_clears", r["success"] and after is None, applied=r.get("applied"), failed=r.get("failed"))


def t_snapshot_zero():
    # values recorded as "nothing" (hidden, a cleared groom) must come back too: a real rebuild re-showed a hidden groom
    path = dup_bp("BP_SnapZero")
    bp = unreal.load_asset(path)
    nodes = scs(bp)
    grooms = [n for n, o in nodes.items() if isinstance(o, unreal.GroomComponent) and o.get_editor_property("groom_asset")]
    hide, clear = grooms[0], grooms[-1]
    asset = nodes[clear].get_editor_property("groom_asset")
    nodes[hide].set_editor_property("visible", False)
    nodes[clear].set_editor_property("groom_asset", None)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    snap = J(wd.snapshot_blueprint_components(path, ""))["snapshot"]
    nodes[hide].set_editor_property("visible", True)
    nodes[clear].set_editor_property("groom_asset", asset)
    unreal.BlueprintEditorLibrary.compile_blueprint(bp)
    r = J(wd.restore_blueprint_components(path, json.dumps(snap), "[]", True, True, False))
    after = scs(unreal.load_asset(path))
    check("snapshot.restores_hidden_and_cleared", r["success"] and not r["failed"] and after[hide].get_editor_property("visible") is False
          and after[clear].get_editor_property("groom_asset") is None, hidden=hide, cleared=clear,
          applied=[a["property"] for a in r.get("applied", [])], failed=r.get("failed"))


# ------------------------------------------------------------------ #4 post-build fix-up (dry run on the real BP)
def t_fixup():
    r = J(wd.post_build_fixup(BP, json.dumps({"brow_opacity": 2.0, "texture_caps": {"T_Body_*_VT": 1024, "T_Head_*_VT": 2048}}), True))
    plain = J(wd.post_build_fixup(BP, "{}", True))
    check("fixup.caps_opt_in", plain["success"] and plain.get("textures") == [], textures=len(plain.get("textures", [])))
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


# ------------------------------------------------------------------ #1 runtime poses, poke-through, push
def full_body(nodes):
    """the uncut body next to the built one (make_full_body_from_dna's default name), else the Body mesh"""
    body = body_mesh(nodes).split(".")[0]
    full = body[:body.index("BodyMesh") + len("BodyMesh")] + "_Full" if "BodyMesh" in body else body + "_Full"
    return full if EAL.does_asset_exist(full) else body


def visible_garments(nodes, gs):
    """the garment the seated test clip shows (Lila's seated gown variant), else the visible garments"""
    seated = [g for g in gs if "Seated" in g]
    return seated[:1] or [g for g in gs if nodes[g].get_editor_property("visible") and "Shoe" not in g] or gs[:1]


def t_push():
    nodes, gs = garments(BP)
    d = J(wd.dump_runtime_poses(BP, json.dumps([{"anim": ANIM, "times": [0.0, 1.0]}]), os.path.join(OUT, "poses.json")))
    diffs = [x["runtime_vs_asset_max_cm"] for x in d.get("runtime_vs_asset", [])]
    check("push.dump_runtime_poses", d["success"] and d["poses"] == 2 and os.path.exists(d["file"]),
          runtime_vs_asset=d.get("runtime_vs_asset"))
    vis = visible_garments(nodes, gs)
    full = full_body(nodes)
    poses = {"poses": [{"file": d["file"]}]}
    m_fit = J(wd.measure_poke_through(BP, json.dumps(vis), json.dumps(poses)))
    m_full = J(wd.measure_poke_through(BP, json.dumps(vis), json.dumps(dict(poses, body_mesh=full))))
    w = lambda m: [(g["garment"], g["vertices_outside"], round(g["worst_cm"], 2), g.get("worst_at_bind_cm"), g.get("worst_pose"), g.get("outside_by_bone")) for g in m["lods"][0]["garments"]]  # noqa: E731
    check("push.measure", m_fit["success"] and m_full["success"], body=w(m_fit), full_body=w(m_full), full=full)
    path = dup_bp("BP_PushTest")
    r = J(wd.push_body_under_garments(path, json.dumps(vis), f"{TEST}/SKM_PushTest",
                                      json.dumps(dict(poses, source_body=full, lods=[0])), True, False))
    lod0 = r["lods"][0] if r.get("success") else {}
    after = J(wd.measure_poke_through(path, json.dumps(vis), json.dumps(poses)))
    check("push.fit", r["success"] and lod0.get("outside_after", 1) <= max(3, lod0.get("outside_before", 0) // 4),
          before=(lod0.get("outside_before"), lod0.get("worst_before_cm")), after=(lod0.get("outside_after"), lod0.get("worst_after_cm")),
          moved=lod0.get("vertices_moved"), max_push=lod0.get("max_push_cm"), passes=lod0.get("passes"),
          remeasured=w(after) if after.get("success") else after)


# ------------------------------------------------------------------ #6 inner garment trim (dry run)
def t_intake():
    for bp in (BP, "/Game/DirectorsCut/Characters/Theo/BP_Theo", "/Game/DirectorsCut/Characters/Benny/BP_Benny"):
        nodes, gs = garments(bp)
        inner = [g for g in gs if "Shirt" in g or "Sweater" in g or "Knit" in g]
        outer = [g for g in gs if "Jacket" in g or "Coat" in g]
        if inner and outer:
            r = J(wd.trim_inner_garment(bp, inner[0], json.dumps(outer), f"{TEST}/SKM_TrimTest", json.dumps({"lods": [0]}), False, True))
            check("intake.trim_inner", r["success"] and r["lods"][0]["deleted"] > 0, bp=bp, inner=inner[0], outer=outer,
                  lod0={k: r["lods"][0].get(k) for k in ("triangles", "deleted", "deleted_fragments", "covered_inside_or_shell", "covered_by_rays")} if r.get("success") else r)
            return
    check("intake.trim_inner", False, error="no Blueprint with a shirt under a jacket")


# ------------------------------------------------------------------ #2 fit-view capture (needs a rendering editor)
def t_capture():
    r = J(wd.capture_fit_views(BP, ANIM, 1.0, json.dumps(["front", "low_front"]), os.path.join(OUT, "fit"), json.dumps({"width": 512, "height": 768})))
    files = r.get("files", [])
    check("capture.fit_views", r["success"] and len(files) == 2 and all(os.path.exists(f["file"]) and f["non_dark_fraction"] > 0.01 for f in files),
          files=files, error=r.get("error"))
    h = J(wd.capture_fit_views(BP, "", 0.0, json.dumps(["front"]), os.path.join(OUT, "fit"), json.dumps({"focus": "head", "width": 512, "height": 512})))
    check("capture.head", h["success"] and h["files"][0]["non_dark_fraction"] > 0.05, files=h.get("files"), error=h.get("error"))


# ------------------------------------------------------------------ #8 blinks + face AnimBP
FACE_DIR = "/Game/DirectorsCut/Anims/Face"


def blink_peaks(seq, name="CTRL_expressions_eyeBlinkL"):
    names = {str(c).lower(): str(c) for c in unreal.AnimationLibrary.get_animation_curve_names(seq, unreal.RawCurveTrackTypes.RCT_FLOAT)}
    if name.lower() not in names:
        return []
    t, v = unreal.AnimationLibrary.get_float_keys(seq, names[name.lower()])
    return [round(t[i], 2) for i in range(len(v)) if v[i] >= 0.99 and (i == 0 or v[i - 1] < 0.99)]


def t_blinks():
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    src = sorted(str(a.package_name) for a in ar.get_assets_by_path(FACE_DIR, True) if str(a.asset_name).startswith("FA_"))[0]
    dst = f"{TEST}/FA_BlinkTest"
    if EAL.does_asset_exist(dst):
        EAL.delete_asset(dst)
    EAL.duplicate_asset(src, dst)
    seq = unreal.load_asset(dst)
    # strip the copy's blinks so the pass has work to do
    for c in ("CTRL_expressions_eyeBlinkL", "CTRL_expressions_eyeBlinkR"):
        names = {str(n).lower(): n for n in unreal.AnimationLibrary.get_animation_curve_names(seq, unreal.RawCurveTrackTypes.RCT_FLOAT)}
        if c.lower() in names:
            unreal.AnimationLibrary.remove_curve(seq, names[c.lower()], False)
    dry = J(an.add_blinks(dst, 2.8, 4.2, True, "{}"))
    wet = J(an.add_blinks(dst, 2.8, 4.2, False, "{}"))
    peaks = blink_peaks(unreal.load_asset(dst))
    again = J(an.add_blinks(dst, 2.8, 4.2, True, "{}"))
    planned = dry["report"][0]["added_at"]
    check("blinks.add", wet["success"] and len(planned) > 0 and len(peaks) == len(planned) and again["blinks_added"] == 0,
          length=dry["report"][0]["length_s"], planned=planned, peaks_after=peaks, rerun_adds=again.get("blinks_added"))


def t_faceabp():
    name = "ABP_FaceSlotTest"
    if EAL.does_asset_exist(f"{TEST}/{name}"):
        EAL.delete_asset(f"{TEST}/{name}")
    blink = "/Game/DirectorsCut/Anims/A_Face_BlinkLoop"
    r = J(an.create_face_anim_blueprint(TEST, name, "", blink if EAL.does_asset_exist(blink) else "", "DefaultSlot"))
    check("faceabp.create", r["success"] and r.get("compiled") and r.get("linked"), result=r)


# ------------------------------------------------------------------ #5 skinned accessory, #6 variant + weight transfer
def find_assets(root, cls):
    ar = unreal.AssetRegistryHelpers.get_asset_registry()
    return sorted(str(a.package_name) for a in ar.get_assets_by_path(root, True) if str(a.asset_class_path.asset_name) == cls)


def t_accessory():
    path = dup_bp("BP_AccTest")
    sms = find_assets("/Game/DirectorsCut/Characters/Accessories", "StaticMesh")
    sm = [s for s in sms if "Brooch" in s or "Pocket" in s or "Tags" in s][:1] or sms[:1]
    for mode in ("surface", "rigid"):
        out = f"{TEST}/SKM_AccTest_{mode}_{RUN}"
        if EAL.does_asset_exist(out):
            EAL.delete_asset(out)
        r = J(wd.add_skinned_accessory(path, sm[0], f"ACC_Test_{mode}", mode, '["Body"]', "spine_05",
                                       json.dumps({"location": [0, 12, 0]}), out))
        check(f"accessory.{mode}", r.get("success"), static_mesh=sm[0], mesh=r.get("mesh"), rigid_bone=r.get("rigid_bone"),
              top=r.get("top_bones"), lod_fix=r.get("lod_fix"), error=r.get("error"),
              follower=(r.get("attach") or {}).get("follower_check", {}).get("unevaluated_bone_vertices"))


def t_intake2():
    fab = [m for m in find_assets("/Game/Fab", "SkeletalMesh")][:1]
    if not fab:
        check("intake.variant", False, error="no skeletal mesh under /Game/Fab")
        return
    r = J(wd.make_garment_variant(fab[0], f"{TEST}/Wardrobe", "VariantTest", "", 1024))
    check("intake.variant", r.get("success") and all(not m["pack_textures_left"] for m in r.get("materials", [])) if r.get("success") else False,
          source=fab[0], result=r)
    nodes, _ = garments(BP)
    out = f"{TEST}/SKM_TransferTest_{RUN}"
    if EAL.does_asset_exist(out):
        EAL.delete_asset(out)
    t = J(wd.transfer_skin_weights(r.get("mesh", fab[0]), body_mesh(nodes), out, "closest"))
    check("intake.transfer", t.get("success"), result=t)


# ------------------------------------------------------------------ #7 face-fit capture
def t_landmarks():
    ch = BP.rsplit("/", 1)[0] + "/MH_" + BP.rsplit("/BP_", 1)[1]
    r = J(mh.capture_face_for_fit(ch, os.path.join(OUT, "facefit"), "test", 768, 30.0, False, True))
    ok = r.get("success") and os.path.exists(r.get("image", "")) and os.path.exists(r.get("meta", ""))
    check("landmarks.capture", ok, character=ch, image=r.get("image"), landmarks=r.get("landmarks"),
          contours=sorted((r.get("tracked_contours_px") or {}).keys())[:6], tracking_error=r.get("tracking_error"), error=r.get("error"))


run("accessory", t_accessory)
run("intake", t_intake2)
run("landmarks", t_landmarks)
run("blinks", t_blinks)
run("faceabp", t_faceabp)
run("describe", t_describe)
run("snapshot", t_snapshot)
run("snapshot", t_snapshot_zero)
run("fixup", t_fixup)
run("audit", t_audit)
run("push", t_push)
run("intake", t_intake)
run("capture", t_capture)

with open(os.path.join(OUT, "postjam.json"), "w") as f:
    json.dump(results, f, indent=1)
unreal.log("VMHTEST DONE " + json.dumps({k: v["ok"] for k, v in results.items()}))
unreal.SystemLibrary.quit_editor()
