"""VibeMetaHumans wardrobe helpers that run on GeometryScript (2026-11-14, from the Dead Reel jam's one-off scripts).

Called by the C++ tools MetaHumanWardrobeService.add_skinned_accessory / make_garment_variant / transfer_skin_weights
(each returns this module's JSON); also usable directly in editor Python. Needs the GeometryScripting plugin enabled
(the C++ tools report GEOMETRY_SCRIPT_MISSING otherwise). Every function writes NEW assets in /Game (never /Game/Fab)
and saves nothing it did not create; results are JSON-able dicts with "success".
"""
import json

import unreal

EAL = unreal.EditorAssetLibrary


def _gs():
    if not hasattr(unreal, "GeometryScript_AssetUtils") or not hasattr(unreal, "GeometryScript_BoneWeights"):
        raise RuntimeError("GEOMETRY_SCRIPT_MISSING: enable the GeometryScripting plugin (Edit > Plugins) and restart")
    return unreal.GeometryScript_AssetUtils, unreal.GeometryScript_BoneWeights


def _editable(path):
    return path.startswith("/Game/") and not path.startswith("/Game/Fab/")


def _split(path):
    path = path.split(".")[0]
    return path.rsplit("/", 1)


def _read_sk(sk, lod=0, render=True):
    gs, _ = _gs()
    o = unreal.GeometryScriptCopyMeshFromAssetOptions()
    rl = unreal.GeometryScriptMeshReadLOD()
    rl.set_editor_property("lod_index", lod)
    rl.set_editor_property("lod_type", unreal.GeometryScriptLODType.RENDER_DATA if render else unreal.GeometryScriptLODType.SOURCE_MODEL)
    dm, oc = gs.copy_mesh_from_skeletal_mesh(sk, unreal.DynamicMesh(), o, rl)
    if oc != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError(f"could not read {sk.get_path_name()} LOD {lod}")
    return dm


def _read_sm(sm):
    gs, _ = _gs()
    o = unreal.GeometryScriptCopyMeshFromAssetOptions()
    o.set_editor_property("apply_build_settings", False)
    rl = unreal.GeometryScriptMeshReadLOD()
    rl.set_editor_property("lod_index", 0)
    rl.set_editor_property("lod_type", unreal.GeometryScriptLODType.SOURCE_MODEL)
    dm, oc = gs.copy_mesh_from_static_mesh(sm, unreal.DynamicMesh(), o, rl)
    if oc != unreal.GeometryScriptOutcomePins.SUCCESS:
        raise RuntimeError(f"could not read {sm.get_path_name()}")
    return dm


def _bones(dm):
    _, gsb = _gs()
    res = gsb.get_all_bones_info(dm)
    infos = [x for x in (res if isinstance(res, (list, tuple)) else [res]) if isinstance(x, (list, tuple, unreal.Array))]
    infos = infos[0] if infos else res
    return [(str(b.get_editor_property("name")), b.get_editor_property("world_transform")) for b in infos]


def _top_bones(sk, n=8):
    wm = unreal.SkinWeightModifier()
    wm.set_skeletal_mesh(sk)
    acc = {}
    nv = wm.get_num_vertices()
    for v in range(0, nv, max(1, nv // 2000)):
        for k, x in wm.get_vertex_weights(v).items():
            acc[str(k)] = acc.get(str(k), 0.0) + x
    return [b for b, _ in sorted(acc.items(), key=lambda t: -t[1])[:n]]


def _new_skeletal(dm, skeleton, path, slot_names, materials):
    """create_new_skeletal_mesh_asset_from_mesh with bone proportions from the mesh and the given slots."""
    no = unreal.GeometryScriptCreateNewSkeletalMeshAssetOptions()
    no.set_editor_property("use_mesh_bone_proportions", True)
    no.set_editor_property("enable_recompute_normals", False)
    no.set_editor_property("enable_recompute_tangents", False)
    mm = unreal.Map(unreal.Name, unreal.MaterialInterface)
    for sn, m in zip(slot_names, materials):
        if m is not None:
            mm[unreal.Name(sn)] = m
    no.set_editor_property("materials", mm)
    if EAL.does_asset_exist(path):
        # writing a GeometryScript mesh over an existing asset scrambled its vertices in the jam: always a new asset
        raise RuntimeError(f"{path} exists: pass another name (or delete it first)")
    sk, oc = unreal.GeometryScript_NewAssetUtils.create_new_skeletal_mesh_asset_from_mesh(dm, skeleton, path, no)
    if sk is None:
        raise RuntimeError(f"create_new_skeletal_mesh_asset_from_mesh failed ({oc})")
    cur = sk.get_editor_property("materials")
    new = []
    for i, m in enumerate(cur):
        if i < len(materials) and materials[i] is not None:
            m.set_editor_property("material_interface", materials[i])
            m.set_editor_property("material_slot_name", slot_names[i])
        new.append(m)
    sk.set_editor_property("materials", new)
    return sk


def _scs(bp):
    sds = unreal.get_engine_subsystem(unreal.SubobjectDataSubsystem)
    out = {}
    for h in sds.k2_gather_subobject_data_for_blueprint(bp):
        d = unreal.SubobjectDataBlueprintFunctionLibrary.get_data(h)
        o = unreal.SubobjectDataBlueprintFunctionLibrary.get_object(d)
        if o is not None:
            out.setdefault(str(unreal.SubobjectDataBlueprintFunctionLibrary.get_variable_name(d)), o)
    return out


def _display_positions(garment_sk, body_sk):
    """a leader-pose garment's LOD0 as Body's follower in Body's reference pose, from the assets (leader-pose LBS with
    each mesh's own bind pose: some packs' skeletons sit 4-8 cm off the garment's own bind pose)."""
    _, gsb = _gs()
    dm = _read_sk(garment_sk)
    G = _bones(dm)
    B = dict(_bones(_read_sk(body_sk)))
    K = [g.inverse() * B[n] if n in B else unreal.Transform() for n, g in G]
    _d, pl, _g = unreal.GeometryScript_MeshQueries.get_all_vertex_positions(dm, False)
    pos = unreal.GeometryScript_List.convert_vector_list_to_array(pl)
    out = []
    for v, p in enumerate(pos):
        res = gsb.get_vertex_bone_weights(dm, v)
        bw = [x for x in res if isinstance(x, (list, tuple, unreal.Array))][0]
        acc, tw = unreal.Vector(0, 0, 0), 0.0
        for w in bw:
            x = w.get_editor_property("weight")
            acc = acc + K[w.get_editor_property("bone_index")].transform_location(p) * x
            tw += x
        out.append(acc * (1.0 / tw) if tw > 0 else p)
    dm = unreal.GeometryScript_MeshEdits.set_all_mesh_vertex_positions(dm, unreal.GeometryScript_List.convert_array_to_vector_list(out))
    return dm


# ---------------------------------------------------------------------------------------------------------------------
def add_skinned_accessory(blueprint, static_mesh, component, mode="surface", surface_components=("Body",), bone="spine_05",
                          relative=None, out_path="", materials=None):
    """A static accessory as a leader-pose skeletal mesh under Body that follows the surface under it.

    The static mesh is placed in Body component space by `relative` (a dict {"location":[x,y,z],"rotation":[roll,pitch,yaw],
    "scale":[...]}) relative to `bone` at Body's reference pose - the same numbers as a static mesh attached to that bone
    socket. mode "surface": weights by closest point from surface_components as displayed at rest (the garment it lies
    on: a scarf, a chain); "rigid": every vertex weighted 1.0 to the surface's dominant bone under the accessory's centre
    (a brooch, a pocket square - no kinks). The new mesh carries the body's skeleton; its weights are then moved off
    bones the body drops at lower LODs (fix_unevaluated_bone_weights) and it is attached with
    attach_skeletal_mesh_to_blueprint. Returns the mesh, the attach result and the dominant bones.
    """
    _, gsb = _gs()
    bp = unreal.load_asset(blueprint)
    sm = unreal.load_asset(static_mesh)
    if bp is None or sm is None:
        raise RuntimeError("need a Blueprint and a static mesh")
    nodes = _scs(bp)
    if "Body" not in nodes:
        raise RuntimeError("the Blueprint has no Body component")
    if component in nodes:
        raise RuntimeError(f"{blueprint} already has a component {component}: remove it first (remove_component_from_blueprint)")
    if mode not in ("surface", "rigid"):
        raise RuntimeError('mode is "surface" or "rigid"')
    body_sk = nodes["Body"].get_editor_property("skeletal_mesh_asset")
    bdm = _read_sk(body_sk)
    bone_t = dict(_bones(bdm)).get(bone)
    if bone_t is None:
        raise RuntimeError(f"Body has no bone {bone}")
    rel = relative or {}
    r = rel.get("rotation", [0, 0, 0])
    T = unreal.Transform(location=unreal.Vector(*rel.get("location", [0, 0, 0])),
                         rotation=unreal.Rotator(roll=r[0], pitch=r[1], yaw=r[2]),
                         scale=unreal.Vector(*rel.get("scale", [1, 1, 1]))) * bone_t
    dm = unreal.GeometryScript_MeshTransforms.transform_mesh(_read_sm(sm), T)
    dm = gsb.copy_bones_from_mesh(bdm, dm, unreal.GeometryScriptCopyBonesFromMeshOptions())
    src = None
    for name in surface_components:
        if name not in nodes:
            raise RuntimeError(f"no component {name}")
        sk = nodes[name].get_editor_property("skeletal_mesh_asset")
        part = bdm if name == "Body" else _display_positions(sk, body_sk)
        src = part if src is None else unreal.GeometryScript_MeshEdits.append_mesh(src, part, unreal.Transform())
    o = unreal.GeometryScriptTransferBoneWeightsOptions()
    o.set_editor_property("transfer_method", unreal.TransferBoneWeightsMethod.CLOSEST_POINT_ON_SURFACE)
    o.set_editor_property("output_target_mesh_bones", unreal.OutputTargetMeshBones.TARGET_BONES)
    dm = gsb.transfer_bone_weights_from_mesh(src, dm, o)
    rigid_bone = None
    if mode == "rigid":
        # collapse to the dominant bone over all vertices: the piece moves with the cloth point under it, unbent
        names = [n for n, _ in _bones(dm)]
        acc = {}
        nv = dm.get_vertex_count()
        for v in range(nv):
            _, bw, ok = dm.get_vertex_bone_weights(v)
            for w in (bw if ok else []):
                acc[w.get_editor_property("bone_index")] = acc.get(w.get_editor_property("bone_index"), 0.0) + w.get_editor_property("weight")
        top = max(acc, key=acc.get)
        rigid_bone = names[top]
        one = unreal.GeometryScriptBoneWeight()
        one.set_editor_property("bone_index", top)
        one.set_editor_property("weight", 1.0)
        for v in range(nv):
            dm.set_vertex_bone_weights(v, [one])
    slots = [str(s.material_slot_name) for s in sm.static_materials]
    mats = [unreal.load_asset(m) if isinstance(m, str) else m for m in (materials or [s.material_interface for s in sm.static_materials])]
    folder, name = _split(sm.get_path_name())
    path = out_path or f"{folder}/SKM_{name}_Skinned"
    if not _editable(path):
        raise RuntimeError("out_path must be under /Game and not /Game/Fab")
    sk = _new_skeletal(dm, body_sk.skeleton, path, slots, mats)
    fix = json.loads(unreal.MetaHumanWardrobeService.fix_unevaluated_bone_weights(sk.get_path_name(), body_sk.get_path_name(), False))
    EAL.save_asset(sk.get_path_name(), only_if_is_dirty=False)
    att = json.loads(unreal.MetaHumanCharacterService.attach_skeletal_mesh_to_blueprint(blueprint, component, sk.get_path_name(), "Body", True, ""))
    return {"success": bool(att.get("success")), "mesh": sk.get_path_name(), "mode": mode, "rigid_bone": rigid_bone,
            "top_bones": _top_bones(sk), "lod_fix": fix.get("vertices_fixed"), "attach": att,
            "hint": "save the Blueprint; look at it seated and standing: capture_fit_views(bp, clip, t, views, '', '{\"focus\":\"chest\"}')"}


def make_garment_variant(source_mesh, dst_folder, name, texture_variant="", max_texture_size=1024):
    """Copy a pack garment into your folder with its own material copy and capped texture copies, so the cook never
    pulls the pack's 4K material in through the mesh. texture_variant "2": a texture named T_X_1_BaseColor (or T_X_BaseColor)
    is replaced by T_X_2_BaseColor from the same folder when that exists (Fab packs ship colourways that way).
    Every texture a copied material samples is copied to <dst_folder>/Textures and capped at max_texture_size (never
    flips virtual texture streaming). Reports any pack texture still referenced."""
    src = unreal.load_asset(source_mesh)
    if src is None:
        raise RuntimeError(f"no skeletal mesh at {source_mesh}")
    if not _editable(dst_folder + "/x"):
        raise RuntimeError("dst_folder must be under /Game and not /Game/Fab")
    MEL = unreal.MaterialEditingLibrary
    mesh_path = f"{dst_folder}/SKM_{name}"
    if not EAL.does_asset_exist(mesh_path):
        EAL.duplicate_asset(src.get_path_name().split(".")[0], mesh_path)
    mesh = unreal.load_asset(mesh_path)

    def tex_copy(t):
        tf, tn = _split(t.get_path_name())
        if texture_variant:
            for cand in (tn.replace("_1_", f"_{texture_variant}_"),):
                if cand != tn and EAL.does_asset_exist(f"{tf}/{cand}"):
                    tn, t = cand, unreal.load_asset(f"{tf}/{cand}")
        dst = f"{dst_folder}/Textures/{tn}"
        if not EAL.does_asset_exist(dst):
            EAL.duplicate_asset(t.get_path_name().split(".")[0], dst)
        c = unreal.load_asset(dst)
        if c.get_editor_property("max_texture_size") != max_texture_size:
            c.set_editor_property("max_texture_size", max_texture_size)
        EAL.save_asset(dst, only_if_is_dirty=True)
        return c

    done, report = {}, []
    slots = mesh.get_editor_property("materials")
    for s in slots:
        m = s.get_editor_property("material_interface")
        if m is None:
            continue
        key = m.get_path_name()
        if key not in done:
            mf, mn = _split(key)
            dst = f"{dst_folder}/Materials/{mn}_{name}"
            if not EAL.does_asset_exist(dst):
                EAL.duplicate_asset(key.split(".")[0], dst)
            cm = unreal.load_asset(dst)
            swapped = []
            if isinstance(cm, unreal.MaterialInstanceConstant):
                for tp in cm.get_editor_property("texture_parameter_values"):
                    t = tp.get_editor_property("parameter_value")
                    if t is not None and not _editable(t.get_path_name()):
                        nt = tex_copy(t)
                        MEL.set_material_instance_texture_parameter_value(cm, tp.get_editor_property("parameter_info").get_editor_property("name"), nt)
                        swapped.append((t.get_name(), nt.get_name()))
                MEL.update_material_instance(cm)
            elif isinstance(cm, unreal.Material):
                for e in MEL.get_material_expressions(cm) if hasattr(MEL, "get_material_expressions") else []:
                    if isinstance(e, unreal.MaterialExpressionTextureBase):
                        t = e.get_editor_property("texture")
                        if t is not None and not _editable(t.get_path_name()):
                            nt = tex_copy(t)
                            e.set_editor_property("texture", nt)
                            swapped.append((t.get_name(), nt.get_name()))
                MEL.recompile_material(cm)
            EAL.save_asset(dst, only_if_is_dirty=False)
            used = [t.get_path_name() for t in MEL.get_used_textures(cm)] if hasattr(MEL, "get_used_textures") else []
            done[key] = cm
            report.append({"material": dst, "swapped": swapped, "pack_textures_left": [u for u in used if u.startswith("/Game/Fab")]})
        s.set_editor_property("material_interface", done[key])
    mesh.set_editor_property("materials", slots)
    EAL.save_asset(mesh_path, only_if_is_dirty=False)
    return {"success": True, "mesh": mesh_path, "materials": report,
            "hint": "a material INSTANCE copy still has the pack material as parent: its default textures cook too unless every texture parameter is overridden (pack_textures_left lists what is still referenced)"}


def transfer_skin_weights(target_mesh, source_mesh, out_path="", method="closest"):
    """Weights (and bones) from a skinned source onto a target that has none worth keeping - a Chaos outfit's render
    mesh skinned to root only cannot be a leader-pose garment. Writes a NEW mesh (out_path, default <target>_Skinned)
    with the source's skeleton and bones, weights by closest point on the source surface ("inpaint" fills gaps
    smoothly). Both meshes must be in the same space (the source body/garment the outfit was built on)."""
    _, gsb = _gs()
    tgt = unreal.load_asset(target_mesh)
    src = unreal.load_asset(source_mesh)
    if tgt is None or src is None:
        raise RuntimeError("need the target and source skeletal meshes")
    tdm = _read_sk(tgt, render=False)
    sdm = _read_sk(src)
    tdm = gsb.copy_bones_from_mesh(sdm, tdm, unreal.GeometryScriptCopyBonesFromMeshOptions())
    o = unreal.GeometryScriptTransferBoneWeightsOptions()
    o.set_editor_property("transfer_method", unreal.TransferBoneWeightsMethod.INPAINT_WEIGHTS if method == "inpaint"
                          else unreal.TransferBoneWeightsMethod.CLOSEST_POINT_ON_SURFACE)
    o.set_editor_property("output_target_mesh_bones", unreal.OutputTargetMeshBones.TARGET_BONES)
    tdm = gsb.transfer_bone_weights_from_mesh(sdm, tdm, o)
    folder, name = _split(tgt.get_path_name())
    path = out_path or f"{folder}/{name}_Skinned"
    if not _editable(path):
        raise RuntimeError("out_path must be under /Game and not /Game/Fab")
    slots = [str(m.get_editor_property("material_slot_name")) for m in tgt.get_editor_property("materials")]
    mats = [m.get_editor_property("material_interface") for m in tgt.get_editor_property("materials")]
    sk = _new_skeletal(tdm, src.skeleton, path, slots, mats)
    EAL.save_asset(sk.get_path_name(), only_if_is_dirty=False)
    return {"success": True, "mesh": sk.get_path_name(), "skeleton": src.skeleton.get_path_name(), "top_bones": _top_bones(sk),
            "hint": "run fix_unevaluated_bone_weights on it against the Body mesh, then attach_skeletal_mesh_to_blueprint"}


def run_json(fn_name, kwargs_json):
    """Entry point for the C++ tools: returns a JSON string, errors as {"success": false, ...}."""
    try:
        return json.dumps(globals()[fn_name](**json.loads(kwargs_json)))
    except Exception as e:  # noqa: BLE001
        msg = str(e)
        code = "GEOMETRY_SCRIPT_MISSING" if msg.startswith("GEOMETRY_SCRIPT_MISSING") else "FAILED"
        return json.dumps({"success": False, "error_code": code, "error": msg})
