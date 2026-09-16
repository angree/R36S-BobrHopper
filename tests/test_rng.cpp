// src/game/rng.h must be bit-identical to the JavaScript mulberry32 of the reference build.
// Expected values were printed by node with the exact function used in tools/webref/capture.mjs.
#include <cstdio>

#include "game/rng.h"

using namespace cr;

int main()
{
    struct Case {
        uint32_t seed;
        double values[6];
    };
    const Case cases[] = {
        {1u, {0.62707394058816135, 0.0027357211802154779, 0.52744703995995224, 0.98105096747167408,
              0.96837789821438491, 0.28110350295901299}},
        {12345u, {0.97972826776094735, 0.30675226449966431, 0.48420542152598500, 0.81793441250920296,
                  0.50942836934700608, 0.34747186047025025}},
        {2654435769u, {0.35888998024165630, 0.10590326134115458, 0.67529047932475805, 0.91793455881997943,
                       0.10157715040259063, 0.30100292386487126}},
    };
    int fail = 0, checks = 0;
    for (const Case &c : cases) {
        Rng r(c.seed);
        for (double want : c.values) {
            double got = r.next();
            checks++;
            if (got != want) {
                std::printf("FAIL seed %u: got %.17f want %.17f\n", c.seed, got, want);
                fail++;
            }
        }
    }
    // GameRng::seed(1).fx must equal mulberry32(1 ^ 0x9e3779b9)
    GameRng g;
    g.seed(1);
    Rng fx(1u ^ 0x9E3779B9u);
    checks++;
    if (g.fx.next() != fx.next()) {
        std::printf("FAIL fx stream seed derivation\n");
        fail++;
    }
    std::printf("test_rng: %d checks, %d failures\n", checks, fail);
    return fail ? 1 : 0;
}
