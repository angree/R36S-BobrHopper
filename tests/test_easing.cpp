// src/ui/easing.h against values computed by react-native-web's own Easing (tests/data/easing_vectors.txt,
// from tools/webref/gen_easing_vectors.mjs).
//   test_easing.exe tests/data/easing_vectors.txt
#include <cmath>
#include <cstdio>
#include <cstring>

#include "ui/easing.h"

using namespace cr;

int main(int argc, char **argv)
{
    const char *path = argc > 1 ? argv[1] : "tests/data/easing_vectors.txt";
    FILE *f = std::fopen(path, "rb");
    if (!f) {
        std::printf("test_easing: cannot open %s\n", path);
        return 2;
    }
    int checks = 0, failures = 0;
    double worst = 0;
    char name[64];
    double t, expected;
    char line[256];
    while (std::fgets(line, sizeof line, f)) {
        if (line[0] == '#' || std::sscanf(line, "%63s %lf %lf", name, &t, &expected) != 3) continue;
        double got;
        if (!std::strcmp(name, "ease")) got = easing::ease(t);
        else if (!std::strcmp(name, "inOutEase")) got = easing::inOutEase(t);
        else if (!std::strcmp(name, "elastic")) got = easing::elastic(t);
        else if (!std::strcmp(name, "bezier_25_1_25_1")) got = easing::bezier(0.25, 0.1, 0.25, 1, t);
        else continue;
        checks++;
        double d = std::fabs(got - expected);
        if (d > worst) worst = d;
        if (d > 1e-9) {
            failures++;
            if (failures <= 10) std::printf("FAIL %s(%g) = %.17g, JS %.17g\n", name, t, got, expected);
        }
    }
    std::fclose(f);
    std::printf("test_easing: %d checks, %d failures, worst difference %.3g\n", checks, failures, worst);
    return (failures || !checks) ? 1 : 0;
}
