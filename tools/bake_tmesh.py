#!/usr/bin/env python3
"""Textured meshes for the SF2000 render benchmark (src/sf2000/render_bench.cpp): the R36S .mesh (CRM1, the GLES
triangles) with its upstream texture, as integers. Only the benchmark reads them; the game draws .fmesh.

The MagicaVoxel textures are palettes blown up to several texels per voxel face: the texture is shrunk by the gcd of
its colour runs (one texel per voxel) and stored as palette indices; UVs are in texels of that small texture.

.tmesh (little endian):
  char[4] "CRTM"
  u32 vertex_count, u32 triangle_count
  u16 tex_w, u16 tex_h, u16 colour_count, u16 pad
  colour_count x { u8 r, g, b }
  tex_w * tex_h x u8 colour index, rows top first
  vertex_count x { i32 x, y, z (16.16), i32 s, t (texel coordinates, 16.16) }
  triangle_count x { u16 a, b, c, u8 axis, u8 pad }   axis 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z; sorted by axis

Usage: python tools/bake_tmesh.py --all <upstream_assets_dir> <r36s_data_dir> <sf2000_data_dir>
"""
import math
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from assets_manifest import MODELS, TEXTURES  # noqa: E402


def read_mesh(path):
    b = open(path, "rb").read()
    if b[:4] != b"CRM1":
        raise SystemExit(f"{path}: not a CRM1 mesh")
    vc, ic = struct.unpack_from("<II", b, 4)
    off = 12 + 24  # "CRM1", vertex and index counts, aabb
    verts = struct.unpack_from("<%df" % (vc * 8), b, off)
    off += vc * 32
    idx = struct.unpack_from("<%dH" % ic, b, off)
    return vc, verts, idx


def voxel_texels(data, w, h):
    g = 0
    for y in range(h):
        row = data[y * w * 3:(y + 1) * w * 3]
        run = 1
        for x in range(1, w):
            if row[x * 3:x * 3 + 3] == row[(x - 1) * 3:x * 3]:
                run += 1
            else:
                g, run = math.gcd(g, run), 1
        g = math.gcd(g, run)
    for x in range(w):
        run = 1
        for y in range(1, h):
            a = (y * w + x) * 3
            if data[a:a + 3] == data[a - w * 3:a - w * 3 + 3]:
                run += 1
            else:
                g, run = math.gcd(g, run), 1
        g = math.gcd(g, run)
    return max(g, 1)


def bake(mesh_path, png_path, dst):
    vc, verts, idx = read_mesh(mesh_path)
    im = Image.open(png_path).convert("RGB")
    w, h = im.size
    data = im.tobytes()
    g = voxel_texels(data, w, h)
    tw, th = (w + g - 1) // g, (h + g - 1) // g
    palette, index, tex = [], {}, bytearray()
    for ty in range(th):
        for tx in range(tw):
            a = (ty * g * w + tx * g) * 3
            c = bytes(data[a:a + 3])
            if c not in index:
                index[c] = len(palette)
                palette.append(c)
            tex.append(index[c])
    if len(palette) > 256:
        raise SystemExit(f"{png_path}: {len(palette)} colours do not fit u8 indices")
    out_verts = []
    for i in range(vc):
        px, py, pz, nx, ny, nz, u, v = verts[i * 8:i * 8 + 8]
        s = u * w / g
        t = v * h / g  # the .mesh already stores t = 1 - v (image row 0 at t = 0, tools/bake_models.py)
        out_verts.append((round(px * 65536), round(py * 65536), round(pz * 65536), round(s * 65536), round(t * 65536)))
    tris = []
    for i in range(0, len(idx), 3):
        a, b, c = idx[i], idx[i + 1], idx[i + 2]
        n = verts[a * 8 + 3:a * 8 + 6]
        k = max(range(3), key=lambda j: abs(n[j]))
        tris.append((a, b, c, 2 * k + (0 if n[k] >= 0 else 1)))
    tris.sort(key=lambda t: t[3])
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(b"CRTM")
        f.write(struct.pack("<IIHHHH", vc, len(tris), tw, th, len(palette), 0))
        for c in palette:
            f.write(c)
        f.write(bytes(tex))
        for v in out_verts:
            f.write(struct.pack("<5i", *v))
        for a, b, c, axis in tris:
            f.write(struct.pack("<3HBB", a, b, c, axis, 0))
    return len(tris), tw, th, g, len(palette)


def main(argv):
    if len(argv) == 4 and argv[0] == "--all":
        upstream, r36s, sf2000 = argv[1], argv[2], argv[3]
        total = 0
        for name, (mesh, texture) in sorted(MODELS.items()):
            mesh_path = os.path.join(r36s, "meshes", mesh + ".mesh")
            if not os.path.exists(mesh_path) or not TEXTURES.get(texture):
                print(f"tmesh {name}: skipped (no {mesh_path} or texture)")
                continue
            tris, tw, th, g, colours = bake(mesh_path, os.path.join(upstream, TEXTURES[texture]),
                                            os.path.join(sf2000, "meshes", name + ".tmesh"))
            total += tris
            print(f"tmesh {name:22s} tris {tris:5d}  texture {tw}x{th} (1/{g})  colours {colours}")
        print(f"tmesh total triangles {total}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
