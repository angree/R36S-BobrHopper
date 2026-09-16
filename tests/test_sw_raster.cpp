// Software rasteriser (src/sw/raster.*): coverage against a brute-force edge-function reference with the top-left rule,
// watertight meshes (every pixel exactly once), guard band clipping of huge triangles, degenerate triangles, back-face
// culling and the depth test.
//   test_sw_raster.exe
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "sw/raster.h"

using namespace cr;
using namespace cr::sw;

static int checks = 0, failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what);
    }
}

static uint32_t seed = 24681357;
static uint32_t rnd32()
{
    seed = seed * 1664525u + 1013904223u;
    return seed;
}
static int32_t rndi(int32_t lo, int32_t hi) { return lo + int32_t(rnd32() % uint32_t(hi - lo + 1)); }

static ScreenVertex V(int32_t x, int32_t y, int32_t z = 0)
{
    ScreenVertex v;
    v.x = x, v.y = y, v.z = z;
    return v;
}

// centre (px, py) in subpixels inside triangle abc with the top-left rule (y down)
static bool refInside(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, int64_t px, int64_t py)
{
    const int64_t area = int64_t(b.x - a.x) * (c.y - a.y) - int64_t(c.x - a.x) * (b.y - a.y);
    if (area == 0) return false;
    const int64_t s = area > 0 ? 1 : -1;
    const ScreenVertex *e[3][2] = {{&a, &b}, {&b, &c}, {&c, &a}};
    for (auto &edge : e) {
        const ScreenVertex &p = *edge[0], &q = *edge[1];
        const int64_t E = ((int64_t(q.x) - p.x) * (py - p.y) - (int64_t(q.y) - p.y) * (px - p.x)) * s;
        const int64_t gx = -(int64_t(q.y) - p.y) * s, gy = (int64_t(q.x) - p.x) * s;
        const bool topLeft = gx > 0 || (gx == 0 && gy > 0);
        if (!(E > 0 || (E == 0 && topLeft))) return false;
    }
    return true;
}

// the distance of centre (px, py) from the nearest edge line, in subpixels
static double edgeDistance(const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c, double px, double py)
{
    double best = 1e30;
    const ScreenVertex *e[3][2] = {{&a, &b}, {&b, &c}, {&c, &a}};
    for (auto &edge : e) {
        const double dx = double(edge[1]->x) - edge[0]->x, dy = double(edge[1]->y) - edge[0]->y;
        const double len = std::sqrt(dx * dx + dy * dy);
        if (len == 0) continue;
        best = std::min(best, std::fabs(dx * (py - edge[0]->y) - dy * (px - edge[0]->x)) / len);
    }
    return best;
}

struct Counter {
    int w, h;
    std::vector<int> count;
    int badSpans = 0;
    Counter(int w_, int h_) : w(w_), h(h_), count(size_t(w_ * h_), 0) {}
    void operator()(int y, int x0, int x1, int32_t, int32_t)
    {
        if (y < 0 || y >= h || x0 < 0 || x1 > w || x0 >= x1) {
            badSpans++;
            return;
        }
        for (int x = x0; x < x1; x++) count[size_t(y * w + x)]++;
    }
    void reset() { std::fill(count.begin(), count.end(), 0); }
};

static RasterTarget target(int w, int h)
{
    RasterTarget t;
    t.width = w;
    t.height = h;
    return t;
}

// grid of shared, jittered vertices covering [x0, x1] x [y0, y1], two triangles per cell, GL front-facing
struct Tri {
    ScreenVertex v[3];
};

static void drawGrid(const RasterTarget &t, Counter &c, int nx, int ny, int32_t x0, int32_t y0, int32_t x1, int32_t y1,
                     int32_t jitter, bool cull, const DepthGradient *gradient = nullptr, std::vector<Tri> *drawn = nullptr)
{
    std::vector<ScreenVertex> g(size_t((nx + 1) * (ny + 1)));
    for (int j = 0; j <= ny; j++)
        for (int i = 0; i <= nx; i++) {
            int32_t x = int32_t(x0 + int64_t(x1 - x0) * i / nx), y = int32_t(y0 + int64_t(y1 - y0) * j / ny);
            if (i > 0 && i < nx) x += rndi(-jitter, jitter);
            if (j > 0 && j < ny) y += rndi(-jitter, jitter);
            // exactly on a pixel centre (floor by shift: C division would move negative coordinates up to 23 subpixels
            // and fold small cells)
            if ((rnd32() & 7) == 0) x = ((x >> 4) << 4) + 8, y = ((y >> 4) << 4) + 8;
            g[size_t(j * (nx + 1) + i)] = V(x, y);
        }
    for (int j = 0; j < ny; j++)
        for (int i = 0; i < nx; i++) {
            const ScreenVertex &p00 = g[size_t(j * (nx + 1) + i)], &p10 = g[size_t(j * (nx + 1) + i + 1)];
            const ScreenVertex &p01 = g[size_t((j + 1) * (nx + 1) + i)], &p11 = g[size_t((j + 1) * (nx + 1) + i + 1)];
            // y down: (p00, p01, p10) runs clockwise on screen = counter-clockwise in GL
            const Tri first = (i + j) & 1 ? Tri{{p00, p01, p10}} : Tri{{p00, p01, p11}};
            const Tri second = (i + j) & 1 ? Tri{{p10, p01, p11}} : Tri{{p00, p11, p10}};
            for (const Tri &tri : {first, second}) {
                rasterTriangle(t, tri.v[0], tri.v[1], tri.v[2], cull, c, gradient);
                if (drawn) drawn->push_back(tri);
            }
        }
}

int main()
{
    const int W = 64, H = 48;
    RasterTarget t = target(W, H);
    Counter c(W, H);

    // 1. coverage vs reference, random and tie-heavy coordinates, both windings
    int mismatches = 0, drawnPixels = 0;
    for (int n = 0; n < 40000; n++) {
        ScreenVertex v[3];
        for (auto &p : v) {
            if (n % 2) p = V(rndi(-300, W * 16 + 300), rndi(-300, H * 16 + 300));
            else p = V(rndi(-4, W + 4) * 8, rndi(-4, H + 4) * 8); // on pixel centres and pixel borders
        }
        // every other triangle with a depth gradient: small ones then take the half-space loop
        if (n % 4 >= 2) {
            // small triangles too, so the half-space loop sees plenty of them
            if (n % 8 >= 6)
                for (int k = 1; k < 3; k++) v[k] = V(v[0].x + rndi(-120, 120), v[0].y + rndi(-120, 120));
        }
        const DepthGradient zeroGradient{0, 0};
        c.reset();
        rasterTriangle(t, v[0], v[1], v[2], false, c, n % 4 >= 2 ? &zeroGradient : nullptr);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int got = c.count[size_t(y * W + x)];
                const int want = refInside(v[0], v[1], v[2], x * 16 + 8, y * 16 + 8) ? 1 : 0;
                if (got != want) mismatches++;
                drawnPixels += got;
            }
    }
    std::printf("coverage: 40000 triangles (half with a depth gradient), %d pixels, %d mismatches, %d bad spans\n",
                drawnPixels, mismatches, c.badSpans);
    check(mismatches == 0 && c.badSpans == 0 && drawnPixels > 100000, "coverage equals the top-left reference");

    // 2. back-face culling: GL counter-clockwise (screen clockwise) triangles only
    {
        c.reset();
        rasterTriangle(t, V(16, 16), V(16, 400), V(400, 16), true, c); // screen clockwise: front
        int front = 0;
        for (int v : c.count) front += v;
        c.reset();
        rasterTriangle(t, V(16, 16), V(400, 16), V(16, 400), true, c); // back
        int back = 0;
        for (int v : c.count) back += v;
        check(front > 0 && back == 0, "back faces culled, front faces drawn");
    }

    // 3. watertight meshes: every pixel exactly once
    int overdraw = 0, holes = 0;
    const DepthGradient gridGradient{0, 0};
    int diagnostics = 0, coarseBad = 0, fineBad = 0, fanBad = 0;
    for (int n = 0; n < 600; n++) {
        c.reset();
        // n >= 300: fine grids of small cells with a depth gradient (the half-space loop next to the scanline one);
        // cells stay over 40 subpixels, so jitter (6) plus the snap to a pixel centre (8) never folds two cells
        const bool fine = n >= 300;
        const bool cull = n % 2 == 0;
        std::vector<Tri> tris;
        drawGrid(t, c, fine ? rndi(8, 24) : rndi(1, 12), fine ? rndi(6, 18) : rndi(1, 9), -rndi(0, 200), -rndi(0, 200),
                 W * 16 + rndi(0, 200), H * 16 + rndi(0, 200), fine ? rndi(0, 6) : rndi(0, 20), cull,
                 fine ? &gridGradient : nullptr, &tris);
        int bad = 0;
        for (int v : c.count) overdraw += v > 1, holes += v == 0, bad += v != 1;
        (fine ? fineBad : coarseBad) += bad;
        // on a failure: which triangle disagrees with the reference, and where
        for (const Tri &tri : tris) {
            if (!bad || diagnostics >= 4) break;
            Counter one(W, H);
            rasterTriangle(t, tri.v[0], tri.v[1], tri.v[2], cull, one, fine ? &gridGradient : nullptr);
            const int64_t area = int64_t(tri.v[1].x - tri.v[0].x) * (tri.v[2].y - tri.v[0].y) -
                                 int64_t(tri.v[2].x - tri.v[0].x) * (tri.v[1].y - tri.v[0].y);
            for (int y = 0; y < H && diagnostics < 4; y++)
                for (int x = 0; x < W; x++) {
                    const bool want = (!cull || area < 0) && refInside(tri.v[0], tri.v[1], tri.v[2], x * 16 + 8, y * 16 + 8);
                    if ((one.count[size_t(y * W + x)] != 0) == want) continue;
                    std::printf("  mismatch: triangle (%d,%d) (%d,%d) (%d,%d) pixel (%d,%d) got %d want %d\n",
                                tri.v[0].x, tri.v[0].y, tri.v[1].x, tri.v[1].y, tri.v[2].x, tri.v[2].y, x, y,
                                one.count[size_t(y * W + x)], want ? 1 : 0);
                    diagnostics++;
                    break;
                }
        }
    }
    // a fan around a centre on a pixel centre, rim partly off screen
    for (int n = 0; n < 100; n++) {
        c.reset();
        const ScreenVertex centre = V(rndi(0, W) * 16 + 8, rndi(0, H) * 16 + 8);
        const int spokes = rndi(3, 40);
        std::vector<ScreenVertex> rim;
        for (int k = 0; k < spokes; k++) {
            const double a = -2 * 3.141592653589793 * k / spokes; // clockwise on screen
            rim.push_back(V(centre.x + int32_t(std::lround(std::cos(a) * 3000)), centre.y + int32_t(std::lround(std::sin(a) * 3000))));
        }
        // angles fall with k and y points down, so (centre, rim[k], rim[k + 1]) runs clockwise on screen: front
        for (int k = 0; k < spokes; k++) rasterTriangle(t, centre, rim[size_t(k)], rim[size_t((k + 1) % spokes)], true, c);
        int bad = 0;
        for (int v : c.count) overdraw += v > 1, holes += v == 0, bad += v != 1;
        fanBad += bad;
        for (int y = 0; y < H && bad && diagnostics < 8; y++)
            for (int x = 0; x < W && diagnostics < 8; x++) {
                if (c.count[size_t(y * W + x)] == 1) continue;
                std::printf("  fan: centre (%d,%d) spokes %d pixel (%d,%d) drawn %d times; covering triangles:",
                            centre.x, centre.y, spokes, x, y, c.count[size_t(y * W + x)]);
                for (int k = 0; k < spokes; k++) {
                    const ScreenVertex &p = rim[size_t(k)], &q = rim[size_t((k + 1) % spokes)];
                    const int64_t area = int64_t(p.x - centre.x) * (q.y - centre.y) - int64_t(q.x - centre.x) * (p.y - centre.y);
                    if (refInside(centre, p, q, x * 16 + 8, y * 16 + 8))
                        std::printf(" k%d area %lld (%d,%d)-(%d,%d)", k, (long long)area, p.x, p.y, q.x, q.y);
                }
                std::printf("\n");
                diagnostics++;
            }
    }
    std::printf("watertight: %d pixels drawn twice, %d holes (bad pixels: coarse grids %d, fine grids %d, fans %d)\n",
                overdraw, holes, coarseBad, fineBad, fanBad);
    check(overdraw == 0 && holes == 0, "meshes and fans cover every pixel exactly once");

    // 4. huge triangles through the guard band: still watertight, and only centres next to an edge may differ
    int hugeOverdraw = 0, hugeHoles = 0, hugeMismatch = 0, farMismatch = 0;
    for (int n = 0; n < 300; n++) {
        c.reset();
        drawGrid(t, c, rndi(1, 4), rndi(1, 4), -rndi(20000, 400000), -rndi(20000, 400000), rndi(20000, 400000),
                 rndi(20000, 400000), 15000, false);
        for (int v : c.count) hugeOverdraw += v > 1, hugeHoles += v == 0;
    }
    for (int n = 0; n < 3000; n++) {
        ScreenVertex v[3];
        for (auto &p : v) p = V(rndi(-300000, 300000), rndi(-300000, 300000));
        c.reset();
        rasterTriangle(t, v[0], v[1], v[2], false, c);
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                const int got = c.count[size_t(y * W + x)];
                const int want = refInside(v[0], v[1], v[2], x * 16 + 8, y * 16 + 8) ? 1 : 0;
                if (got != want) {
                    hugeMismatch++;
                    if (edgeDistance(v[0], v[1], v[2], x * 16 + 8, y * 16 + 8) > 1.0) farMismatch++;
                }
            }
    }
    std::printf("guard band: %d drawn twice, %d holes; %d mismatches vs reference (%d farther than 1/16 px from an edge)\n",
                hugeOverdraw, hugeHoles, hugeMismatch, farMismatch);
    check(hugeOverdraw == 0 && hugeHoles == 0, "clipped huge meshes stay watertight");
    check(farMismatch == 0, "clipping moves coverage only at the edges");

    // 5. degenerate triangles draw nothing
    {
        c.reset();
        rasterTriangle(t, V(100, 100), V(100, 100), V(100, 100), false, c);
        rasterTriangle(t, V(0, 0), V(400, 300), V(800, 600), false, c);
        rasterTriangle(t, V(8, 8), V(500, 8), V(900, 8), false, c);
        rasterTriangle(t, V(-5000000, 8), V(5000000, 8), V(0, 8), false, c);
        int drawn = 0;
        for (int v : c.count) drawn += v;
        check(drawn == 0 && c.badSpans == 0, "degenerate triangles draw nothing");
    }

    // 6. depth: nearer wins in either order, equal depth passes (LEQUAL), plane interpolation within a step
    {
        std::vector<uint16_t> color(size_t(W * H)), depth(size_t(W * H));
        t.color = color.data();
        t.depth = depth.data();
        const int flags = kDepthTest | kDepthWrite;
        clearTarget(t, 0);
        drawTriangle(t, V(-100, -100, 1000), V(-100, 2000, 1000), V(2000, -100, 1000), 0x1111, flags);
        drawTriangle(t, V(-100, -100, 2000), V(-100, 2000, 2000), V(2000, -100, 2000), 0x2222, flags);
        const bool nearKept = color[0] == 0x1111;
        drawTriangle(t, V(-100, -100, 1000), V(-100, 2000, 1000), V(2000, -100, 1000), 0x3333, flags);
        const bool equalPasses = color[0] == 0x3333;
        drawTriangle(t, V(-100, -100, 500), V(-100, 2000, 500), V(2000, -100, 500), 0x4444, kDepthTest);
        const bool noWrite = color[0] == 0x4444 && depth[0] == 1000;
        check(nearKept && equalPasses && noWrite, "depth test less-or-equal, depth write flag");

        // plane mode: the expected depth is z0 + ax * px + ay * py (pixels); otherwise through the three vertices
        struct Probe {
            double worst = 0;
            ScreenVertex a, b, c;
            bool plane = false;
            double z0 = 0, ax = 0, ay = 0;
            void operator()(int y, int x0, int x1, int32_t z, int32_t dz)
            {
                for (int x = x0; x < x1; x++, z += dz) {
                    const double px = x * 16 + 8, py = y * 16 + 8;
                    double expect;
                    if (plane) {
                        expect = z0 + ax * px / 16 + ay * py / 16;
                    } else {
                        const double area = double(b.x - a.x) * (c.y - a.y) - double(c.x - a.x) * (b.y - a.y);
                        const double wb = ((px - a.x) * (c.y - a.y) - (c.x - a.x) * (py - a.y)) / area;
                        const double wc = ((b.x - a.x) * (py - a.y) - (px - a.x) * (b.y - a.y)) / area;
                        expect = a.z + wb * (b.z - a.z) + wc * (c.z - a.z);
                    }
                    worst = std::max(worst, std::fabs(z / 256.0 - expect));
                }
            }
        } probe;
        auto minAltitude = [](const ScreenVertex &a, const ScreenVertex &b, const ScreenVertex &c) {
            const double area = std::fabs(double(b.x - a.x) * (c.y - a.y) - double(c.x - a.x) * (b.y - a.y));
            double longest = 0;
            const ScreenVertex *e[3][2] = {{&a, &b}, {&b, &c}, {&c, &a}};
            for (auto &edge : e)
                longest = std::max(longest, std::hypot(double(edge[1]->x - edge[0]->x), double(edge[1]->y - edge[0]->y)));
            return longest > 0 ? area / longest : 0.0; // subpixels
        };
        int wellShaped = 0;
        for (int n = 0; n < 20000; n++) {
            probe.a = V(rndi(-200, 1200), rndi(-200, 900), rndi(0, 65535));
            probe.b = V(rndi(-200, 1200), rndi(-200, 900), rndi(0, 65535));
            probe.c = V(rndi(-200, 1200), rndi(-200, 900), rndi(0, 65535));
            if (minAltitude(probe.a, probe.b, probe.c) < 32) continue; // slivers: see the plane case below
            wellShaped++;
            rasterTriangle(t, probe.a, probe.b, probe.c, false, probe);
        }
        std::printf("depth from vertices: %d triangles at least 2 px thick, worst %.3f steps\n", wellShaped, probe.worst);
        check(wellShaped > 1000 && probe.worst < 1.5, "vertex depth plane within 1.5 steps in well-shaped triangles");

        // slivers with the face's exact plane: the renderer's case
        probe.worst = 0;
        probe.plane = true;
        int slivers = 0;
        for (int n = 0; n < 20000; n++) {
            probe.ax = rndi(-3000, 3000) / 10.0;
            probe.ay = rndi(-3000, 3000) / 10.0;
            probe.z0 = 30000;
            auto onPlane = [&](int32_t x, int32_t y) {
                return V(x, y, int32_t(std::lround(probe.z0 + probe.ax * x / 16.0 + probe.ay * y / 16.0)));
            };
            const int32_t x0 = rndi(-200, 1200), y0 = rndi(-200, 900), x1 = rndi(-200, 1200), y1 = rndi(-200, 900);
            const int32_t off = rndi(-6, 6); // the third vertex a fraction of a pixel off the first edge
            probe.a = onPlane(x0, y0);
            probe.b = onPlane(x1, y1);
            probe.c = onPlane((x0 + x1) / 2 + off, (y0 + y1) / 2 - off);
            if (minAltitude(probe.a, probe.b, probe.c) >= 16) continue;
            slivers++;
            const DepthGradient g{int32_t(std::lround(probe.ax * 256)), int32_t(std::lround(probe.ay * 256))};
            rasterTriangle(t, probe.a, probe.b, probe.c, false, probe, &g);
        }
        std::printf("depth with the face plane: %d slivers, worst %.3f steps\n", slivers, probe.worst);
        check(slivers > 1000 && probe.worst < 1.5, "slivers with a given gradient within 1.5 steps");
    }

    std::printf("test_sw_raster: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
