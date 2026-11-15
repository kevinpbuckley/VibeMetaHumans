"""VibeMetaHumans face-fit kit, step 1 and 3: MediaPipe FaceMesh measurement (from the 2026-11 jam, used on five characters).

Runs OUTSIDE the editor, in any Python with opencv-python, numpy and mediapipe (the editor's Python has none of them):
    python -I measure.py crop <reference_photo.jpg> <ref_crop.png>   # biggest frontal face, cropped square, 768 px
    python -I measure.py measure <ref_crop.png> <ref.json>          # 478 FaceMesh points (+ _overlay.png)
    python -I measure.py measure <mh_front.png> <mh_front_measure.json>
The MetaHuman render comes from unreal.MetaHumanCharacterService.capture_face_for_fit (see ue_capture.py); fit.py turns
the two measurements into sculpt-landmark moves for move_face_landmarks. Measure lit renders (bBaseColor=False): the
tracker found faces on lit renders more reliably.
"""
import sys, json, cv2, numpy as np
import mediapipe as mp

def measure(img_bgr, max_faces=1):
    fm = mp.solutions.face_mesh.FaceMesh(static_image_mode=True, max_num_faces=max_faces, refine_landmarks=True, min_detection_confidence=0.3)
    h, w = img_bgr.shape[:2]
    r = fm.process(cv2.cvtColor(img_bgr, cv2.COLOR_BGR2RGB))
    out = []
    for f in (r.multi_face_landmarks or []):
        pts = np.array([[p.x * w, p.y * h, p.z * w] for p in f.landmark])
        out.append(pts)
    return out

def yaw_proxy(p):
    # nose tip x relative to outer eye corners
    l, rr, n = p[33], p[263], p[1]
    return float((n[0] - (l[0] + rr[0]) / 2) / abs(rr[0] - l[0]))

if __name__ == "__main__":
    cmd = sys.argv[1]
    if cmd == "crop":
        src, dst = sys.argv[2], sys.argv[3]
        img = cv2.imread(src)
        faces = measure(img, 8)
        info = []
        for p in faces:
            ipd = np.linalg.norm(p[468, :2] - p[473, :2])
            info.append((abs(yaw_proxy(p)), -ipd, p))
            print("face at", p[1, :2].round(), "yaw", round(yaw_proxy(p), 3), "ipd", round(ipd, 1))
        # biggest frontal face: among |yaw|<0.05 pick largest ipd
        cands = [i for i in info if i[0] < 0.06] or info
        cands.sort(key=lambda t: t[1])
        p = cands[0][2]
        cx, cy = (p[468, 0] + p[473, 0]) / 2, (p[468, 1] + p[473, 1]) / 2
        ipd = np.linalg.norm(p[468, :2] - p[473, :2])
        half = ipd * 2.4
        x0, y0 = int(cx - half), int(cy - half * 0.95)
        x1, y1 = int(cx + half), int(cy + half * 1.05)
        x0, y0 = max(0, x0), max(0, y0)
        crop = img[y0:y1, x0:x1]
        crop = cv2.resize(crop, (768, 768), interpolation=cv2.INTER_AREA)
        cv2.imwrite(dst, crop)
        print("crop", x0, y0, x1, y1, "->", dst)
    elif cmd == "measure":
        src, dst = sys.argv[2], sys.argv[3]
        img = cv2.imread(src)
        faces = measure(img, 1)
        if not faces:
            json.dump({"found": False}, open(dst, "w")); print("NO FACE"); sys.exit(2)
        p = faces[0]
        json.dump({"found": True, "size": [img.shape[1], img.shape[0]], "points": p.round(3).tolist(), "yaw": yaw_proxy(p)}, open(dst, "w"))
        # overlay
        ov = img.copy()
        for q in p: cv2.circle(ov, (int(q[0]), int(q[1])), 1, (0, 255, 0), -1)
        cv2.imwrite(dst.replace(".json", "_overlay.png"), ov)
        print("found yaw", round(yaw_proxy(p), 3), "ipd", round(float(np.linalg.norm(p[468,:2]-p[473,:2])),1))
