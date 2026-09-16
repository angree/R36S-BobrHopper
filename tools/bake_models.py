#!/usr/bin/env python3
"""OBJ -> .mesh baker.

Follows three.js OBJLoader semantics (faces fan-triangulated, 1-based indices, vn/vt per corner),
splits triangles at T-junctions (see fix_t_junctions), then deduplicates identical corners into an indexed mesh.

.mesh (little endian):
  char[4] "CRM1"
  u32 vertex_count, u32 index_count
  f32[3] aabb_min, f32[3] aabb_max       (same as three.js Box3.setFromObject on the untransformed mesh)
  vertex_count x { f32 px,py,pz, f32 nx,ny,nz, f32 u,t }   t = 1 - v  (image row 0 at t = 0, like flipY)
  index_count  x u16

Usage: python tools/bake_models.py <src_obj> <dst_mesh>
       python tools/bake_models.py --all <upstream_assets_dir> <data_dir>   (uses tools/assets_manifest.py)
"""
import os
import struct
import sys


def parse_obj(path):
    positions, uvs, normals, corners = [], [], [], []
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            p = line.split()
            if not p:
                continue
            tag = p[0]
            if tag == "v":
                positions.append(tuple(float(x) for x in p[1:4]))
            elif tag == "vt":
                uvs.append((float(p[1]), float(p[2]) if len(p) > 2 else 0.0))
            elif tag == "vn":
                normals.append(tuple(float(x) for x in p[1:4]))
            elif tag == "f":
                face = []
                for c in p[1:]:
                    s = c.split("/")

                    def ref(i, table):
                        if i >= len(s) or s[i] == "":
                            return None
                        k = int(s[i])
                        return k - 1 if k > 0 else len(table) + k

                    face.append((ref(0, positions), ref(1, uvs), ref(2, normals)))
                for k in range(1, len(face) - 1):
                    corners.extend((face[0], face[k], face[k + 1]))
    return positions, uvs, normals, corners


def face_normal(a, b, c):
    ux, uy, uz = b[0] - a[0], b[1] - a[1], b[2] - a[2]
    vx, vy, vz = c[0] - a[0], c[1] - a[1], c[2] - a[2]
    nx, ny, nz = uy * vz - uz * vy, uz * vx - ux * vz, ux * vy - uy * vx
    ln = (nx * nx + ny * ny + nz * nz) ** 0.5 or 1.0
    return (nx / ln, ny / ln, nz / ln)


EPS = 1e-5


def points_on_edge(a, b, points):
    """Positions strictly inside segment a-b, as (t, point) sorted along the edge."""
    d = (b[0] - a[0], b[1] - a[1], b[2] - a[2])
    ll = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]
    if ll < EPS * EPS:
        return []
    lo = [min(a[i], b[i]) - EPS for i in range(3)]
    hi = [max(a[i], b[i]) + EPS for i in range(3)]
    length = ll ** 0.5
    found = []
    for p in points:
        if not (lo[0] <= p[0] <= hi[0] and lo[1] <= p[1] <= hi[1] and lo[2] <= p[2] <= hi[2]):
            continue
        t = ((p[0] - a[0]) * d[0] + (p[1] - a[1]) * d[1] + (p[2] - a[2]) * d[2]) / ll
        # absolute distance from the endpoints: the chicken has vertices 1e-6 apart (y 0.399999 and 0.4), which
        # are the same point for the rasteriser; a relative test would treat them differently per edge length
        if t * length <= EPS or (1 - t) * length <= EPS:
            continue
        q = (a[0] + d[0] * t - p[0], a[1] + d[1] * t - p[1], a[2] + d[2] * t - p[2])
        if q[0] * q[0] + q[1] * q[1] + q[2] * q[2] < EPS * EPS:
            found.append((t, p))
    found.sort()
    return found


def lerp(x, y, t):
    return tuple(x[i] + (y[i] - x[i]) * t for i in range(len(x)))


def normalized(n):
    ln = (n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) ** 0.5 or 1.0
    return (n[0] / ln, n[1] / ln, n[2] / ln)


def fix_t_junctions(tris):
    """MagicaVoxel-style meshes merge coplanar faces, leaving vertices in the middle of neighbouring triangles'
    edges. Without antialiasing the rasteriser shows single-pixel cracks there (the original's canvas is
    antialiased and hides them). Every triangle with such points on its edges becomes a fan around its
    centroid over the full outline, so all shared edges end at identical vertices.
    tris: list of 3-tuples of corners (pos, uv, n). Returns (new tris, number of junction points)."""
    points = sorted({c[0] for tri in tris for c in tri})
    out, junctions = [], 0
    for tri in tris:
        ring = []
        for k in range(3):
            a, b = tri[k], tri[(k + 1) % 3]
            ring.append(a)
            for t, p in points_on_edge(a[0], b[0], points):
                junctions += 1
                ring.append((p, lerp(a[1], b[1], t), normalized(lerp(a[2], b[2], t))))
        if len(ring) == 3:
            out.append(tri)
            continue
        centre = (lerp(lerp(tri[0][0], tri[1][0], 0.5), tri[2][0], 1 / 3),
                  tuple(sum(c[1][i] for c in tri) / 3 for i in range(2)),
                  normalized(tuple(sum(c[2][i] for c in tri) for i in range(3))))
        for k in range(len(ring)):
            out.append((centre, ring[k], ring[(k + 1) % len(ring)]))
    return out, junctions


def count_t_junctions(tris):
    points = sorted({c[0] for tri in tris for c in tri})
    return sum(len(points_on_edge(tri[k][0], tri[(k + 1) % 3][0], points)) for tri in tris for k in range(3))


def bake(src, dst):
    positions, uvs, normals, corners = parse_obj(src)
    tris = []
    for i in range(0, len(corners), 3):
        tri = corners[i:i + 3]
        fn = None
        out = []
        for pi, ti, ni in tri:
            pos = positions[pi]
            uv = uvs[ti] if ti is not None else (0.0, 0.0)
            if ni is not None:
                n = normals[ni]
            else:
                fn = fn or face_normal(*(positions[c[0]] for c in tri))
                n = fn
            out.append((pos, (uv[0], 1.0 - uv[1]), n))
        tris.append(tuple(out))
    tris, junctions = fix_t_junctions(tris)
    left = count_t_junctions(tris)
    if left:
        raise SystemExit(f"{src}: {left} T-junction points left after the split")

    verts, index_of, indices = [], {}, []
    for tri in tris:
        for key in tri:
            if key not in index_of:
                index_of[key] = len(verts)
                verts.append(key)
            indices.append(index_of[key])
    if len(verts) > 65535:
        raise SystemExit(f"{src}: {len(verts)} vertices do not fit u16 indices")

    xs = [p[0] for p in positions]
    ys = [p[1] for p in positions]
    zs = [p[2] for p in positions]
    used = {c[0] for c in corners}
    xs = [positions[i][0] for i in used]
    ys = [positions[i][1] for i in used]
    zs = [positions[i][2] for i in used]
    aabb = (min(xs), min(ys), min(zs), max(xs), max(ys), max(zs))

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(b"CRM1")
        f.write(struct.pack("<II", len(verts), len(indices)))
        f.write(struct.pack("<6f", *aabb))
        for pos, uv, n in verts:
            f.write(struct.pack("<8f", *pos, *n, *uv))
        f.write(struct.pack(f"<{len(indices)}H", *indices))
    return len(verts), len(indices) // 3, aabb, junctions


def main(argv):
    if len(argv) == 3 and argv[0] != "--all":
        v, t, aabb, j = bake(argv[0], argv[1])
        print(f"{argv[1]}: {v} vertices, {t} triangles, {j} T-junction points split, aabb {aabb}")
        return 0
    if len(argv) == 3 and argv[0] == "--all":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from assets_manifest import MESHES
        for name, rel in MESHES.items():
            v, t, _, j = bake(os.path.join(argv[1], rel), os.path.join(argv[2], "meshes", name + ".mesh"))
            print(f"mesh {name:28s} {v:5d} v {t:5d} tris {j:4d} T-junctions split")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
