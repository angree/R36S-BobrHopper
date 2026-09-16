// Compares src/engine/math.h against vectors computed by three.js r182
// (tools/webref/gen_math_vectors.mjs -> tests/data/math_vectors.txt).
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "engine/math.h"

using namespace cr;

static int g_failures = 0;
static int g_checks = 0;

static bool near(float a, float b, float tol) { return std::fabs(a - b) <= tol * (1.0f + std::fabs(b)); }

static void expectMat(const char *what, int line, const Mat4 &got, const std::vector<float> &want, float tol = 2e-5f)
{
    g_checks++;
    for (int i = 0; i < 16; i++) {
        if (!near(got.e[i], want[size_t(i)], tol)) {
            std::printf("FAIL %s (line %d) element %d: got %.7f want %.7f\n", what, line, i, got.e[i], want[size_t(i)]);
            g_failures++;
            return;
        }
    }
}

static std::vector<float> floats(const std::string &s)
{
    std::vector<float> v;
    std::istringstream in(s);
    float x;
    while (in >> x) v.push_back(x);
    return v;
}

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/data/math_vectors.txt";
    std::ifstream in(path);
    if (!in) {
        std::printf("cannot open %s\n", path);
        return 2;
    }
    std::string text;
    int lineNo = 0;
    while (std::getline(in, text)) {
        lineNo++;
        std::vector<std::string> parts;
        size_t start = 0, bar;
        while ((bar = text.find('|', start)) != std::string::npos) {
            parts.push_back(text.substr(start, bar - start));
            start = bar + 1;
        }
        parts.push_back(text.substr(start));
        std::istringstream head(parts[0]);
        std::string kind;
        head >> kind;
        std::vector<float> a = floats(parts[0].substr(kind.size()));
        std::vector<float> b = floats(parts.size() > 1 ? parts[1] : "");

        if (kind == "compose") {
            Mat4 m = composeEuler({a[0], a[1], a[2]}, {a[3], a[4], a[5]}, {a[6], a[7], a[8]});
            expectMat("compose", lineNo, m, b);
        } else if (kind == "lookat") {
            Mat4 m = lookAtRotation({a[0], a[1], a[2]}, {a[3], a[4], a[5]}, {0, 1, 0});
            m.e[12] = a[0];
            m.e[13] = a[1];
            m.e[14] = a[2];
            expectMat("lookat", lineNo, m, b);
        } else if (kind == "ortho") {
            expectMat("ortho", lineNo, orthographic(a[0], a[1], a[2], a[3], a[4], a[5], a[6]), b);
        } else if (kind == "invert") {
            Mat4 m;
            std::memcpy(m.e, a.data(), sizeof m.e);
            expectMat("invert", lineNo, inverse(m), b, 2e-4f);
        } else if (kind == "camera") {
            float w = a[0], h = a[1], s = a[2];
            Mat4 proj = orthographic(-(w * s), w * s, h * s, -(h * s), -30, 30, 400);
            Mat4 world = lookAtRotation({-1, 2.8f, -2.9f}, {0, 0, 0}, {0, 1, 0});
            world.e[12] = -1;
            world.e[13] = 2.8f;
            world.e[14] = 1;
            expectMat("camera projection", lineNo, proj, b);
            expectMat("camera view", lineNo, inverseRigid(world), floats(parts[2]));
        }
    }
    std::printf("test_math: %d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
