#!/usr/bin/env python3
"""OBJ + texture -> .fmesh baker for the SF2000 software renderer (no textures in 3D).

The model textures are MagicaVoxel palettes: every textured triangle is cut along the texel borders where the
colour changes, so each output triangle has exactly one RGB colour (what GLES NEAREST sampling shows inside it).
Vertices are deduplicated by position only, snapped to 16.16, and T-junctions are split again after the cut
(the cut adds vertices in the middle of neighbouring triangles' edges; see bake_models.fix_t_junctions).

.fmesh (little endian):
  char[4] "CRFM"
  u32 vertex_count, u32 triangle_count, u32 colour_count
  f32[3] aabb_min, f32[3] aabb_max        (bit-identical to the .mesh of the same OBJ: the game logic uses them)
  colour_count   x { u8 r, g, b }          (sRGB, as in the texture)
  vertex_count   x { i32 x, y, z }         (16.16 fixed point)
  triangle_count x { u16 a, b, c, u8 colour, u8 axis }
                                           axis = model-space normal: 0 +X, 1 -X, 2 +Y, 3 -Y, 4 +Z, 5 -Z
Triangles keep the OBJ winding (counter-clockwise = front) and are sorted by (axis, colour).

Usage: python tools/bake_flat.py <src_obj> <src_png> <dst_fmesh>
       python tools/bake_flat.py --all <upstream_assets_dir> <data_dir>   (one .fmesh per manifest model)
"""
import bisect
import os
import struct
import sys

from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from bake_models import face_normal, parse_obj  # noqa: E402

ONE = 65536
EDGE_EPS = 1e-6      # texel units: a vertex this close to a cut line lies on it
SNAP = 2             # 16.16 units: points closer than this in every axis are one vertex
AXES = ((1, 0, 0), (-1, 0, 0), (0, 1, 0), (0, -1, 0), (0, 0, 1), (0, 0, -1))


class Texture:
    def __init__(self, path):
        im = Image.open(path).convert("RGB")
        self.w, self.h = im.size
        data = im.tobytes()
        self.runs = []  # per row: (starts, colours) with run i covering starts[i] .. starts[i+1]-1
        for y in range(self.h):
            row = data[y * self.w * 3:(y + 1) * self.w * 3]
            starts, colours = [], []
            prev = None
            for x in range(self.w):
                c = row[x * 3:x * 3 + 3]
                if c != prev:
                    starts.append(x)
                    colours.append(tuple(c))
                    prev = c
            self.runs.append((starts, colours))

    def colour(self, x, y):
        x = min(max(x, 0), self.w - 1)
        y = min(max(y, 0), self.h - 1)
        starts, colours = self.runs[y]
        return colours[bisect.bisect_right(starts, x) - 1]

    def row_colours(self, y, a, b):
        """Distinct colours of texels a..b (inclusive) in row y, and the colour borders inside (x = first texel)."""
        starts, colours = self.runs[y]
        i = bisect.bisect_right(starts, a) - 1
        found, borders = {colours[i]}, []
        i += 1
        while i < len(starts) and starts[i] <= b:
            found.add(colours[i])
            borders.append(starts[i])
            i += 1
        return found, borders


def area2(poly, i=0, j=1):
    """Twice the signed area of a polygon projected on coordinates i, j."""
    s = 0.0
    for k in range(len(poly)):
        p, q = poly[k], poly[(k + 1) % len(poly)]
        s += p[i] * q[j] - q[i] * p[j]
    return s


def split(poly, axis, k):
    """Sutherland-Hodgman with both halves in one pass, so the cut points are identical in the two pieces."""
    lo, hi = [], []
    n = len(poly)
    for idx in range(n):
        a, b = poly[idx], poly[(idx + 1) % n]
        da, db = a[axis] - k, b[axis] - k
        if abs(da) < EDGE_EPS:
            da = 0.0
        if abs(db) < EDGE_EPS:
            db = 0.0
        if da <= 0:
            lo.append(a)
        if da >= 0:
            hi.append(a)
        if (da < 0 < db) or (db < 0 < da):
            t = da / (da - db)
            p = tuple(a[i] + (b[i] - a[i]) * t for i in range(len(a)))
            p = p[:axis] + (float(k),) + p[axis + 1:]
            lo.append(p)
            hi.append(p)
    return lo, hi


def clean(poly):
    out = []
    for p in poly:
        if not out or max(abs(p[i] - out[-1][i]) for i in range(5)) > 1e-12:
            out.append(p)
    if len(out) > 1 and max(abs(out[0][i] - out[-1][i]) for i in range(5)) <= 1e-12:
        out.pop()
    return out


def coverage(poly, tex):
    """Per texel row the inclusive texel column range the polygon's interior touches: {y: (a, b)}."""
    rs = [p[1] for p in poly]
    y0 = max(int(min(rs) + EDGE_EPS), 0)
    y1 = min(int(-(-(max(rs) - EDGE_EPS) // 1)) - 1, tex.h - 1)
    rows = {}
    rest = poly
    for y in range(y0, y1 + 1):
        strip, rest = split(rest, 1, y + 1) if y < y1 else (rest, [])
        strip = clean(strip)
        if len(strip) >= 3 and abs(area2(strip)) > 1e-9:
            ss = [p[0] for p in strip]
            a = max(int(min(ss) + EDGE_EPS), 0)
            b = min(int(-(-(max(ss) - EDGE_EPS) // 1)) - 1, tex.w - 1)
            if b >= a:
                rows[y] = (a, b)
        if len(rest) < 3:
            break
    return rows


def cut_by_colour(poly, tex, out, depth=0):
    """poly: convex polygon of (s, r, x, y, z) in texel units. Appends (polygon, colour) pieces to out."""
    rows = coverage(poly, tex)
    if not rows:
        return
    colours, vertical, horizontal = set(), {}, {}
    per_row = {}
    for y, (a, b) in rows.items():
        found, borders = tex.row_colours(y, a, b)
        colours |= found
        per_row[y] = found
        for x in borders:
            vertical[x] = vertical.get(x, 0) + 1
    if len(colours) == 1:
        out.append((poly, colours.pop()))
        return
    if depth > 64:
        raise SystemExit("bake_flat: colour cut does not converge")
    for y, (a, b) in rows.items():
        if y - 1 not in rows:
            continue
        pa, pb = rows[y - 1]
        lo, hi = max(a, pa), min(b, pb)
        diff = 0
        if lo <= hi:
            x = lo
            while x <= hi:
                if tex.colour(x, y) != tex.colour(x, y - 1):
                    diff += 1
                x += 1
        elif per_row[y] != per_row[y - 1]:
            diff = 1
        if diff:
            horizontal[y] = horizontal.get(y, 0) + diff
    cs = [p[0] for p in poly]
    rs = [p[1] for p in poly]
    mid = ((min(cs) + max(cs)) / 2, (min(rs) + max(rs)) / 2)
    candidates = [(-w, abs(x - mid[0]), 0, x) for x, w in vertical.items()]
    candidates += [(-w, abs(y - mid[1]), 1, y) for y, w in horizontal.items()]
    if not candidates:
        raise SystemExit("bake_flat: several colours but no border to cut along")
    _, _, axis, k = min(candidates)
    for half in split(poly, axis, k):
        half = clean(half)
        if len(half) >= 3 and abs(area2(half)) > 1e-9:
            cut_by_colour(half, tex, out, depth + 1)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def cross(u, v):
    return (u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0])


def norm2(u):
    return u[0] * u[0] + u[1] * u[1] + u[2] * u[2]


def ear_clip(ring, position, collinear):
    """A convex ring of vertices (some may sit in the middle of edges) -> len(ring) - 2 triangles, no new vertex.
    An ear (prev, v, next) at a true corner is valid when the ring left after cutting it still has a true corner (or is
    that last triangle): in a convex polygon the diagonal prev-next can only hold other vertices when all of them lie on
    it. Of the valid ears the largest is cut, which keeps slivers out while real corners remain.
    Ring [A, p, B, C] with p on AB: the ear at C is the largest but would leave A, p, B on one line (a T-junction at p)."""
    def degenerate(r):
        return all(collinear(position(r[i - 1]), position(r[i]), position(r[(i + 1) % len(r)])) for i in range(len(r)))

    ring = list(ring)
    out = []
    # the ring's orientation (Newell normal): snapping can leave a vertex a unit or two outside a straight edge, and an
    # ear at such a reflex vertex would overlap its neighbours
    normal = [0.0, 0.0, 0.0]
    for i in range(len(ring)):
        c = cross(position(ring[i - 1]), position(ring[i]))
        normal = [normal[k] + c[k] for k in range(3)]
    while len(ring) >= 3:
        n = len(ring)
        ears = []
        for i in range(n):
            p, v, q = position(ring[i - 1]), position(ring[i]), position(ring[(i + 1) % n])
            if collinear(p, v, q):
                continue
            c = cross(sub(v, p), sub(q, v))
            if c[0] * normal[0] + c[1] * normal[1] + c[2] * normal[2] <= 0:
                continue
            ears.append((norm2(c), i))
        chosen = -1
        for _, i in sorted(ears, reverse=True):
            rest = ring[:i] + ring[i + 1:]
            if len(rest) < 3 or not degenerate(rest):
                chosen = i
                break
        if chosen < 0:
            break
        out.append((ring[chosen - 1], ring[chosen], ring[(chosen + 1) % n]))
        del ring[chosen]
    return out


def collinear_float(p, v, q):
    c = cross(sub(v, p), sub(q, v))
    return norm2(c) <= 1e-18 * max(norm2(sub(v, p)) * norm2(sub(q, v)), 1e-30)


def triangulate(poly3):
    """Convex planar polygon (may have vertices in the middle of its edges) -> triangles with no zero-area ones."""
    return ear_clip(poly3, lambda p: p, collinear_float)


class Points:
    """Snaps float positions to 16.16 and merges points within SNAP units."""

    def __init__(self):
        self.cells = {}
        self.list = []

    def add(self, p):
        q = tuple(int(round(c * ONE)) for c in p)
        key = tuple(c // (SNAP * 2) for c in q)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for dz in (-1, 0, 1):
                    for idx in self.cells.get((key[0] + dx, key[1] + dy, key[2] + dz), ()):
                        o = self.list[idx]
                        if all(abs(o[i] - q[i]) <= SNAP for i in range(3)):
                            return idx
        self.cells.setdefault(key, []).append(len(self.list))
        self.list.append(q)
        return len(self.list) - 1


def points_on_segment(a, b, pts, xs):
    """Indices of points strictly inside segment a-b (integer 16.16 coordinates), sorted along it."""
    lo_x, hi_x = min(a[0], b[0]) - SNAP, max(a[0], b[0]) + SNAP
    d = sub(b, a)
    ll = norm2(d)
    if ll == 0:
        return []
    found = []
    for k in range(bisect.bisect_left(xs, (lo_x, -1)), bisect.bisect_right(xs, (hi_x, len(pts)))):
        idx = xs[k][1]
        p = pts[idx]
        if not (min(a[1], b[1]) - SNAP <= p[1] <= max(a[1], b[1]) + SNAP and
                min(a[2], b[2]) - SNAP <= p[2] <= max(a[2], b[2]) + SNAP):
            continue
        ap = sub(p, a)
        t = (ap[0] * d[0] + ap[1] * d[1] + ap[2] * d[2]) / ll
        length = ll ** 0.5
        if t * length <= SNAP or (1 - t) * length <= SNAP:
            continue
        # distance from the line, squared, in 16.16 units
        if norm2(cross(ap, d)) / ll <= SNAP * SNAP:
            found.append((t, idx))
    found.sort()
    return [idx for _, idx in found]


def collinear_int(p, v, q):
    """Three 16.16 points on one line within the snapping distance."""
    d = sub(q, p)
    ll = norm2(d)
    return ll == 0 or norm2(cross(sub(v, p), d)) / ll <= SNAP * SNAP


def fix_t_junctions(tris, points, ear=True):
    """tris: [(i, j, k, colour, axis)]. A triangle with other vertices on its edges is re-triangulated over its whole
    outline by ear clipping (len(outline) - 2 triangles, no new vertex; bake_models.py's centroid fan needs
    len(outline) triangles and a vertex)."""
    pts = points.list
    xs = sorted((p[0], i) for i, p in enumerate(pts))
    out, junctions = [], 0
    for tri in tris:
        # a zero-area triangle covers nothing, but its edges lie on its neighbours' and every fan made from it puts new
        # vertices on them: train_middle split without end until these were dropped here instead of after the passes
        if collinear_int(pts[tri[0]], pts[tri[1]], pts[tri[2]]):
            continue
        ring = []
        for e in range(3):
            a, b = tri[e], tri[(e + 1) % 3]
            ring.append(a)
            inner = points_on_segment(pts[a], pts[b], pts, xs)
            junctions += len(inner)
            ring.extend(inner)
        if len(ring) == 3:
            out.append(tri)
            continue
        n = len(ring)
        clipped = ear_clip(ring, lambda i: pts[i], collinear_int) if ear else []
        # the outline's own pieces hold no further vertex (points_on_segment just listed them all): only the new
        # diagonals need the test
        boundary = {(ring[e], ring[(e + 1) % n]) for e in range(n)}
        diagonals = {(t[e], t[(e + 1) % 3]) for t in clipped for e in range(3)} - boundary
        if len(clipped) != n - 2 or any(points_on_segment(pts[a], pts[b], pts, xs) for a, b in diagonals):
            # near-collinear rings the snapping tolerance cannot decide: the centroid fan of bake_models.py, which
            # always closes them
            centre = tuple(sum(pts[i][k] for i in tri[:3]) / 3 / ONE for k in range(3))
            c = points.add(centre)
            clipped = [(c, ring[e], ring[(e + 1) % len(ring)]) for e in range(len(ring))
                       if not collinear_int(pts[c], pts[ring[e]], pts[ring[(e + 1) % len(ring)]])]
        out.extend((t[0], t[1], t[2], tri[3], tri[4]) for t in clipped)
    return out, junctions




PLANE_AXES = ((1, 2), (2, 0), (0, 1))  # the in-plane axes (u, v) of a face of axis k, right-handed: u x v = +k


def shoelace(poly):
    return sum(poly[i][0] * poly[(i + 1) % len(poly)][1] - poly[(i + 1) % len(poly)][0] * poly[i][1]
               for i in range(len(poly))) / 2


def clip_rect(poly, x0, x1, y0, y1):
    """A convex 2D polygon clipped to the rectangle [x0, x1] x [y0, y1]."""
    for axis, bound, keep_below in ((0, x0, False), (0, x1, True), (1, y0, False), (1, y1, True)):
        out = []
        for i in range(len(poly)):
            p, q = poly[i], poly[(i + 1) % len(poly)]
            pin = p[axis] <= bound if keep_below else p[axis] >= bound
            qin = q[axis] <= bound if keep_below else q[axis] >= bound
            if pin:
                out.append(p)
            if pin != qin:
                t = (bound - p[axis]) / (q[axis] - p[axis])
                out.append((p[0] + (q[0] - p[0]) * t, p[1] + (q[1] - p[1]) * t))
        poly = out
        if len(poly) < 3:
            return []
    return poly


def grid_unit(values):
    """The spacing of a 1D grid the sorted coordinates (16.16 units) sit on, within 4 units, or None."""
    diffs = [b - a for a, b in zip(values, values[1:]) if b - a > 16]
    if not diffs:
        return None
    for divisor in range(1, 9):
        unit = min(diffs) / divisor
        if unit < 256:
            break
        if all(abs((x - values[0]) / unit - round((x - values[0]) / unit)) * unit <= 4 for x in values):
            return unit
    return None


def merge_group(axis, polys):
    """Coplanar convex pieces of one colour and axis -> the maximal rectangles of the grid their vertices sit on
    (greedy meshing), or None when the pieces are not a union of whole grid cells."""
    k = axis // 2
    ui, vi = PLANE_AXES[k]
    plane = sum(p[k] for poly in polys for p in poly) / sum(len(poly) for poly in polys)
    flat = [[(p[ui] * ONE, p[vi] * ONE) for p in poly] for poly in polys]
    us = sorted({round(x) for poly in flat for x, _ in poly})
    vs = sorted({round(y) for poly in flat for _, y in poly})
    unit_u, unit_v = grid_unit(us), grid_unit(vs)
    if not unit_u or not unit_v:
        return None
    u0, v0 = us[0], vs[0]
    nu, nv = round((us[-1] - u0) / unit_u), round((vs[-1] - v0) / unit_v)
    if nu <= 0 or nv <= 0 or nu * nv > 250000:
        return None
    # the exact area of every cell the pieces cover (they never overlap: they partition the source faces); only whole
    # or empty cells can be merged - a piece cut along a diagonal through cells keeps the group's own triangulation
    cover = {}
    for poly in flat:
        i0 = max(0, int((min(x for x, _ in poly) - u0) / unit_u) - 1)
        i1 = min(nu, int((max(x for x, _ in poly) - u0) / unit_u) + 2)
        j0 = max(0, int((min(y for _, y in poly) - v0) / unit_v) - 1)
        j1 = min(nv, int((max(y for _, y in poly) - v0) / unit_v) + 2)
        for j in range(j0, j1):
            for i in range(i0, i1):
                part = clip_rect(poly, u0 + i * unit_u, u0 + (i + 1) * unit_u, v0 + j * unit_v, v0 + (j + 1) * unit_v)
                if part:
                    cover[j * nu + i] = cover.get(j * nu + i, 0.0) + abs(shoelace(part))
    cell_area = unit_u * unit_v
    cells = bytearray(nu * nv)
    for index, a in cover.items():
        if a > cell_area * 0.999:
            cells[index] = 1
        elif a > cell_area * 0.001:
            return None
    done = bytearray(nu * nv)
    tris = []

    def on_grid(values, x):
        # the source coordinate the grid line stands for (grid_unit allows 4 units of noise), so a rectangle corner is
        # the very vertex of the neighbouring faces
        at = bisect.bisect_left(values, x)
        best = min(values[max(0, at - 1):at + 1], key=lambda v: abs(v - x))
        return best if abs(best - x) <= 4 else x

    def corner(i, j):
        p = [0.0, 0.0, 0.0]
        p[k] = plane
        p[ui] = on_grid(us, u0 + i * unit_u) / ONE
        p[vi] = on_grid(vs, v0 + j * unit_v) / ONE
        return tuple(p)

    for j in range(nv):
        i = 0
        while i < nu:
            if not cells[j * nu + i] or done[j * nu + i]:
                i += 1
                continue
            w = 1
            while i + w < nu and cells[j * nu + i + w] and not done[j * nu + i + w]:
                w += 1
            h = 1
            while j + h < nv and all(cells[(j + h) * nu + x] and not done[(j + h) * nu + x] for x in range(i, i + w)):
                h += 1
            for y in range(j, j + h):
                for x in range(i, i + w):
                    done[y * nu + x] = 1
            a, b, c, d = corner(i, j), corner(i + w, j), corner(i + w, j + h), corner(i, j + h)
            # counter-clockwise around +k in (u, v); faces of -k run the other way
            tris += [(a, b, c), (a, c, d)] if axis % 2 == 0 else [(a, c, b), (a, d, c)]
            i += w
    return tris


def merge_plane_groups(pieces, report):
    """pieces: (axis, colour, convex planar polygon). Coplanar pieces of one colour become grid rectangles."""
    groups = {}
    for axis, colour, poly in pieces:
        k = axis // 2
        plane = round(sum(p[k] for p in poly) / len(poly) * ONE / 8)
        groups.setdefault((axis, plane, colour), []).append(poly)
    out = []
    for (axis, _, colour), polys in groups.items():
        tris = merge_group(axis, polys)
        if tris is None:
            report["merge_fallback"] += 1
            tris = [t for poly in polys for t in triangulate(poly)]
        out.extend((axis, colour, t) for t in tris)
    return out


def axis_of(n):
    k = max(range(3), key=lambda i: abs(n[i]))
    return k * 2 + (0 if n[k] >= 0 else 1)


def bake(src_obj, src_png, dst, merge=True):
    positions, uvs, normals, corners = parse_obj(src_obj)
    tex = Texture(src_png)
    report = {"source": len(corners) // 3, "nonaxial": 0, "flipped": 0, "degenerate_uv": 0, "cut": 0,
              "dropped": 0, "merge_fallback": 0, "unmerged": 0, "flipped_out": 0}
    points = Points()
    palette, colour_index = [], {}
    tris = []
    flat_pieces = []
    for i in range(0, len(corners), 3):
        tri = corners[i:i + 3]
        pos = [positions[c[0]] for c in tri]
        ns = [normals[c[2]] for c in tri if c[2] is not None]
        geo = face_normal(*pos)
        n = tuple(sum(v[k] for v in ns) for k in range(3)) if ns else geo
        axis = axis_of(n)
        ln = norm2(n) ** 0.5 or 1.0
        if abs(n[axis // 2]) / ln < 0.999:
            report["nonaxial"] += 1
        if sum(geo[k] * AXES[axis][k] for k in range(3)) < 0:
            report["flipped"] += 1
        uv = [uvs[c[1]] if c[1] is not None else (0.0, 0.0) for c in tri]
        poly = [(u * tex.w, (1.0 - v) * tex.h) + p for (u, v), p in zip(uv, pos)]
        if abs(area2(poly)) < 1e-9:
            report["degenerate_uv"] += 1
            s = sum(p[0] for p in poly) / 3
            r = sum(p[1] for p in poly) / 3
            pieces = [(poly, tex.colour(int(s), int(r)))]
        else:
            pieces = []
            cut_by_colour(poly, tex, pieces)
            if len(pieces) > 1:
                report["cut"] += 1
        for piece, colour in pieces:
            if colour not in colour_index:
                colour_index[colour] = len(palette)
                palette.append(colour)
            flat_pieces.append((axis, colour_index[colour], [p[2:] for p in piece]))
    report["unmerged"] = sum(len(triangulate(poly)) for _, _, poly in flat_pieces)
    if merge:
        merged = merge_plane_groups(flat_pieces, report)
    else:
        merged = [(axis, colour, t) for axis, colour, poly in flat_pieces for t in triangulate(poly)]
    for axis, colour, t in merged:
        tris.append((points.add(t[0]), points.add(t[1]), points.add(t[2]), colour, axis))
    total_junctions = 0
    history = []
    for attempt in range(12):
        # ear clipping on the first pass only: its diagonals can meet vertices of overlapping source faces
        # (train_middle grew without bound); the centroid fans of later passes always converge
        tris, junctions = fix_t_junctions(tris, points, ear=attempt == 0)
        history.append(junctions)
        total_junctions += junctions
        if len(tris) > 40 * max(report["source"], 1):
            raise SystemExit(f"{src_obj}: T-junction splitting runs away (per pass: {history})")
        if not junctions:
            break
    else:
        raise SystemExit(f"{src_obj}: T-junctions left after 12 passes (per pass: {history})")

    # drop triangles that snapping made degenerate (zero area in 16.16)
    pts = points.list
    kept = []
    for t in tris:
        if len({t[0], t[1], t[2]}) < 3 or norm2(cross(sub(pts[t[1]], pts[t[0]]), sub(pts[t[2]], pts[t[0]]))) == 0:
            report["dropped"] += 1
            continue
        kept.append(t)
    for t in kept:
        if sum(cross(sub(pts[t[1]], pts[t[0]]), sub(pts[t[2]], pts[t[0]]))[i] * AXES[t[4]][i] for i in range(3)) < 0:
            report["flipped_out"] += 1
    kept.sort(key=lambda t: (t[4], t[3]))
    used = sorted({i for t in kept for i in t[:3]})
    remap = {old: new for new, old in enumerate(used)}
    if len(used) > 65535:
        raise SystemExit(f"{src_obj}: {len(used)} vertices do not fit u16 indices")

    used_src = {c[0] for c in corners}
    aabb = tuple(f(positions[i][k] for i in used_src) for f in (min, max) for k in range(3))

    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(b"CRFM")
        f.write(struct.pack("<III", len(used), len(kept), len(palette)))
        f.write(struct.pack("<6f", *aabb))
        for c in palette:
            f.write(bytes(c))
        for i in used:
            f.write(struct.pack("<3i", *pts[i]))
        for a, b, c, colour, axis in kept:
            f.write(struct.pack("<3HBB", remap[a], remap[b], remap[c], colour, axis))
    report.update(vertices=len(used), triangles=len(kept), colours=len(palette), junctions=total_junctions)
    return report


def main(argv):
    merge = "--no-merge" not in argv
    argv = [a for a in argv if a != "--no-merge"]
    if len(argv) == 3 and argv[0] == "--all":
        from assets_manifest import MESHES, MODELS, TEXTURES
        totals = [0, 0]
        for name, (mesh, texture) in sorted(MODELS.items()):
            r = bake(os.path.join(argv[1], MESHES[mesh]), os.path.join(argv[1], TEXTURES[texture]),
                     os.path.join(argv[2], "meshes", name + ".fmesh"), merge)
            totals[0] += r["source"]
            totals[1] += r["triangles"]
            print(f"fmesh {name:22s} tris {r['source']:5d} -> {r['triangles']:5d} (pieces {r['unmerged']:5d})  "
                  f"verts {r['vertices']:5d}  colours {r['colours']:2d}  cut {r['cut']:4d}  "
                  f"T-junctions {r['junctions']:4d}  flatUV {r['degenerate_uv']:4d}  nonaxial {r['nonaxial']}  "
                  f"flipped {r['flipped']}/{r['flipped_out']}  dropped {r['dropped']}  "
                  f"merge fallback {r['merge_fallback']}")
        print(f"fmesh total triangles {totals[0]} -> {totals[1]}")
        return 0
    if len(argv) == 3:
        print(bake(*argv))
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
