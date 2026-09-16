// Software renderer maths (src/sw/sw_math.*, CR_FIXED) against the same 16.16 matrices evaluated in double: screen
// positions, depth, subpixel rounding, the per-axis Lambert light and the colour packing. Prints the worst errors.
//   test_sw_math.exe
#include <algorithm>
#include <cmath>
#include <cstdio>

#include "game/settings.h"
#include "sw/sw_math.h"

using namespace cr;

static int checks = 0, failures = 0;

static void check(bool ok, const char *what)
{
    checks++;
    if (!ok) {
        failures++;
        std::printf("FAIL %s\n", what);
    }
}

static uint32_t seed = 987654321;
static double rnd01()
{
    seed = seed * 1664525u + 1013904223u;
    return double(seed >> 8) / 16777216.0;
}
static double rnd(double lo, double hi) { return lo + (hi - lo) * rnd01(); }

struct M4d {
    double e[16];
};
static M4d toDouble(const Mat4 &m)
{
    M4d r;
    for (int i = 0; i < 16; i++) r.e[i] = m.e[i].toDouble();
    return r;
}
static void apply(const M4d &m, const double in[3], double out[3])
{
    double t[3];
    for (int r = 0; r < 3; r++) t[r] = m.e[r] * in[0] + m.e[4 + r] * in[1] + m.e[8 + r] * in[2] + m.e[12 + r];
    out[0] = t[0], out[1] = t[1], out[2] = t[2];
}

int main()
{
    const Vec3 light = normalize({settings::lightX, settings::lightY, settings::lightZ});
    const Mat4 camRot = lookAtRotation({Fixed(-1), Fixed(2.8), Fixed(-2.9)}, {0, 0, 0}, {0, 1, 0});

    double worstPx = 0, worstSub = 0, worstDepth = 0;
    int depthOrderChecked = 0, depthOrderWrong = 0;
    const int sizes[][2] = {{320, 240}, {320, 240}, {640, 480}};
    const double scales[] = {1.5, 3.0, 6.0};
    for (int cam = 0; cam < 3; cam++) {
        const int W = sizes[cam][0], H = sizes[cam][1];
        const real w = real(W) * Fixed(scales[cam]), h = real(H) * Fixed(scales[cam]);
        Mat4 projection = orthographic(-w, w, h, -h, settings::cameraNear, settings::cameraFar, settings::cameraZoom);
        Mat4 shift = Mat4::identity();
        shift.e[13] = Fixed(0.15);
        projection = shift * projection;
        const Mat4 view = inverseRigid(camRot); // rotation only: nodes are drawn relative to the camera position
        const sw::Mat34 screen = sw::screenMatrix(projection, view, W, H);
        const M4d pd = toDouble(projection), vd = toDouble(view);

        for (int node = 0; node < 400; node++) {
            const Vec3 pos{Fixed(rnd(-20, 20)), Fixed(rnd(-4, 4)), Fixed(rnd(-20, 20))};
            const Vec3 rot{Fixed(rnd(-3.2, 3.2)), Fixed(rnd(-3.2, 3.2)), Fixed(rnd(-3.2, 3.2))};
            const Vec3 scl{Fixed(rnd(0.3, 2.0)), Fixed(rnd(0.3, 2.0)), Fixed(rnd(0.3, 2.0))};
            const Mat4 world = composeEuler(pos, rot, scl);
            const sw::Mat34 t = sw::modelToScreen(screen, world);
            const M4d wd = toDouble(world);
            double prevDist = 0, prevDepth = 0;
            for (int v = 0; v < 50; v++) {
                const int32_t p[3] = {int32_t(rnd(-3, 3) * 65536), int32_t(rnd(-1, 3) * 65536), int32_t(rnd(-3, 3) * 65536)};
                const double pdbl[3] = {p[0] / 65536.0, p[1] / 65536.0, p[2] / 65536.0};
                double inWorld[3], inView[3], ndc[3];
                apply(wd, pdbl, inWorld);
                apply(vd, inWorld, inView);
                apply(pd, inView, ndc);
                const double px = (ndc[0] + 1) * W / 2, py = (1 - ndc[1]) * H / 2, dist = -inView[2];

                const sw::PointQ32 q = sw::transformQ32(t, p[0], p[1], p[2]);
                const double qx = double(q.x) / 4294967296.0, qy = double(q.y) / 4294967296.0;
                worstPx = std::max({worstPx, std::fabs(qx - px), std::fabs(qy - py)});
                const sw::ScreenVertex sv = sw::toScreen(q);
                worstSub = std::max({worstSub, std::fabs(sv.x / 16.0 - px), std::fabs(sv.y / 16.0 - py)});
                const double depth = (dist + sw::kDepthOffset) * 1024;
                if (depth > 1 && depth < sw::kDepthMax - 1) {
                    worstDepth = std::max(worstDepth, std::fabs(sv.z - depth));
                    if (v > 0 && std::fabs(depth - prevDepth) > 2 && prevDepth > 1 && prevDepth < sw::kDepthMax - 1) {
                        depthOrderChecked++;
                        const int32_t prevZ = int32_t(prevDepth); // compare orders through the reference sign
                        if ((depth > prevDepth) != (sv.z > prevZ)) depthOrderWrong++;
                    }
                }
                prevDist = dist;
                prevDepth = depth;
            }
            (void)prevDist;
        }
    }
    std::printf("worst screen error: Q32 %.6f px (1/%.0f), 28.4 %.4f px, depth %.3f steps (1/1024 unit)\n", worstPx,
                1 / worstPx, worstSub, worstDepth);
    check(worstPx < 1.0 / 256, "screen position within 1/256 px before subpixel rounding");
    check(worstSub <= 1.0 / 32 + 1.0 / 256, "28.4 rounding within 1/32 px");
    // the depth is truncated to whole steps (up to 1) plus the 16.16 rounding of the matrices (~0.04 steps)
    check(worstDepth <= 1.1, "depth within one step (1/1024 unit) plus matrix rounding");
    check(depthOrderChecked > 1000 && depthOrderWrong == 0, "depth order kept for points 2 steps apart");

    // light of the six axis normals vs the shader: normalize(inverse-transpose * axis), pow((1.8 + max(d, 0)) / PI, 1/2.2)
    double worstLight = 0;
    const double l[3] = {light.x.toDouble(), light.y.toDouble(), light.z.toDouble()};
    const double ll = std::sqrt(20.0 * 20 + 30.0 * 30 + 0.05 * 0.05);
    const double lexact[3] = {20 / ll, 30 / ll, 0.05 / ll};
    for (int node = 0; node < 20000; node++) {
        const Vec3 rot{Fixed(rnd(-3.2, 3.2)), Fixed(rnd(-3.2, 3.2)), Fixed(rnd(-3.2, 3.2))};
        const Vec3 scl{Fixed(rnd(0.05, 3.0)), Fixed(rnd(0.05, 3.0)), Fixed(rnd(0.05, 3.0))};
        const Mat4 world = composeEuler({0, 0, 0}, node < 100 ? Vec3{0, 0, 0} : rot, scl);
        int32_t b[6];
        sw::axisBrightness(world, light, b);
        const M4d wd = toDouble(world);
        for (int k = 0; k < 3; k++) {
            // compose() = R * S: the inverse transpose is R * S^-1, whose column k points along R e_k like column k
            const double sk = k == 0 ? scl.x.toDouble() : k == 1 ? scl.y.toDouble() : scl.z.toDouble();
            double n[3] = {wd.e[k * 4] / (sk * sk), wd.e[k * 4 + 1] / (sk * sk), wd.e[k * 4 + 2] / (sk * sk)};
            const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            const double d = (n[0] * lexact[0] + n[1] * lexact[1] + n[2] * lexact[2]) / len;
            for (int sign = 0; sign < 2; sign++) {
                const double dd = std::max(sign ? -d : d, 0.0);
                const double expect = std::pow((1.8 + dd) / 3.141592653589793, 1 / 2.2);
                worstLight = std::max(worstLight, std::fabs(b[k * 2 + sign] / 65536.0 - expect));
            }
        }
    }
    (void)l;
    std::printf("worst light error: %.6f (%.3f/255)\n", worstLight, worstLight * 255);
    check(worstLight * 255 < 0.25, "axis light within 0.25/255 of the shader");
    check(sw::lambertQ16(-5000) == sw::lambertQ16(0), "light clamps n.l at 0");

    // shading and packing
    bool shadeOk = true;
    for (int c = 0; c < 256; c++)
        for (int32_t lq = 50000; lq <= 63000; lq += 37)
            if (sw::shade8(uint8_t(c), lq) != int(std::floor(c * (lq / 65536.0) + 0.5))) shadeOk = false;
    check(shadeOk, "shade8 = round(colour * light)");
    check(sw::rgb565(255, 255, 255) == 0xffff && sw::rgb565(255, 0, 0) == 0xf800 && sw::rgb565(0, 255, 0) == 0x07e0 &&
              sw::rgb565(0, 0, 255) == 0x001f,
          "rgb565 packing");

    std::printf("test_sw_math: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
