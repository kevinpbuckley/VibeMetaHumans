"""VibeMetaHumans face-fit kit, step 4: sculpt-landmark deltas that move the MetaHuman front render's FaceMesh toward
the reference's (from the 2026-11 jam). Runs OUTSIDE the editor (opencv-python, numpy).

usage: python -I fit.py <ref.json> <mh_front_measure.json> <mh_front.json (capture meta)> <mh_front.png> <out_deltas.json> [gain]

- ref.json / mh_front_measure.json: measure.py output; mh_front.json: the meta file capture_face_for_fit writes (cm_per_px,
  skip, landmarks_cm, landmarks_px). The reference is aligned onto the render by a similarity transform over the 468
  mesh points; each sculpt landmark near the tracked mesh moves by the weighted displacement of its 4 nearest points,
  times gain (0.7), capped at 0.8 cm a step, then symmetrised (MetaHuman faces are symmetric; photos have yaw).
- out_deltas.json {"deltas": {index: [dx, 0, dz] cm}, "mean_err_ipd": ...} feeds
  MetaHumanCharacterService.move_face_landmarks(character, json.dumps(d["deltas"])) - relative moves, the rest held.
- Repeat capture -> measure -> fit -> move until mean_err_ipd stops falling (two or three rounds in the jam).
"""
import sys, json, cv2, numpy as np
ref, mhm, meta, img_path, out = sys.argv[1:6]
gain = float(sys.argv[6]) if len(sys.argv) > 6 else 0.7
R = np.array(json.load(open(ref))["points"])[:, :2]
M = np.array(json.load(open(mhm))["points"])[:, :2]
meta = json.load(open(meta))
cmpp = meta["cm_per_px"]; skip = set(meta["skip"])
# similarity (Procrustes) align R onto M over the first 468 mesh points
A, B = R[:468], M[:468]
ma, mb = A.mean(0), B.mean(0)
A0, B0 = A - ma, B - mb
U, S, Vt = np.linalg.svd(A0.T @ B0)
D = np.eye(2); D[1, 1] = np.sign(np.linalg.det(U @ Vt))
Rot = U @ D @ Vt
s = (S * np.diag(D)).sum() / (A0 ** 2).sum()
Ra = (R - ma) @ Rot * s + mb
ipd = np.linalg.norm(M[468] - M[473])
err = np.linalg.norm(Ra[:468] - M[:468], axis=1).mean() / ipd
deltas, used = {}, []
for k, p in meta["landmarks_px"].items():
    k_i = int(k)
    if k_i in skip:
        continue
    p = np.array(p)
    d = np.linalg.norm(M[:468] - p, axis=1)
    near = np.argsort(d)[:4]
    if d[near[0]] > 18:  # landmark off the tracked mesh (side of head, under hair): leave to the hold
        continue
    w = 1.0 / (d[near] + 2.0)
    disp = ((Ra[near] - M[near]) * w[:, None]).sum(0) / w.sum()
    dx, dz = disp[0] * cmpp * meta["x_sign"] * gain, -disp[1] * cmpp * gain
    mag = (dx * dx + dz * dz) ** 0.5
    if mag > 0.8:
        dx, dz = dx * 0.8 / mag, dz * 0.8 / mag
    if mag < 0.03:
        continue
    deltas[k] = [round(dx, 4), 0.0, round(dz, 4)]
    used.append((p, disp))
# symmetrize: MetaHuman faces are symmetric, the photo has slight yaw/perspective
cm = {int(k): np.array(v) for k, v in meta["landmarks_cm"].items()}
sym = {}
for k, v in deltas.items():
    c = cm[int(k)]
    if abs(c[0]) < 0.3:
        sym[k] = [0.0, 0.0, v[2]]; continue
    mir = min(cm, key=lambda j: np.linalg.norm(cm[j] - np.array([-c[0], c[1], c[2]])))
    if np.linalg.norm(cm[mir] - np.array([-c[0], c[1], c[2]])) > 0.5 or str(mir) not in deltas:
        sym[k] = [round(v[0] * 0.5, 4), 0.0, round(v[2] * 0.5, 4)]; continue
    o = deltas[str(mir)]
    sym[k] = [round((v[0] - o[0]) / 2, 4), 0.0, round((v[2] + o[2]) / 2, 4)]
deltas = {k: v for k, v in sym.items() if abs(v[0]) + abs(v[2]) >= 0.03}
json.dump({"deltas": deltas, "mean_err_ipd": err, "scale": s}, open(out, "w"), indent=1)
img = cv2.imread(img_path)
for q in M[:468]: cv2.circle(img, (int(q[0]), int(q[1])), 1, (0, 255, 0), -1)
for q in Ra[:468]: cv2.circle(img, (int(q[0]), int(q[1])), 1, (0, 0, 255), -1)
for k, p in meta["landmarks_px"].items():
    cv2.circle(img, (int(p[0]), int(p[1])), 4, (255, 255, 0) if k in deltas else (128, 128, 128), 1)
for p, disp in used:
    cv2.arrowedLine(img, (int(p[0]), int(p[1])), (int(p[0] + disp[0] * 3), int(p[1] + disp[1] * 3)), (255, 0, 255), 1)
cv2.imwrite(out.replace(".json", "_overlay.png"), img)
print("mean err (IPD units) %.4f, %d deltas, max %.3f cm" % (err, len(deltas), max([abs(v[0]) + abs(v[2]) for v in deltas.values()] or [0])))
