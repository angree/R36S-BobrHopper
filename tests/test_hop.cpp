// Hop trajectory of the port vs the original, including the "second hop before the first finished" case.
// Reference: docs/reference/gsap_double_hop.json recorded by tools/webref/probe_gsap.mjs (per animation frame:
// t seconds since the first hop, z relative to the start, moving flag, finishedMovingAnimation calls).
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "engine/assets.h"
#include "game/game.h"

using namespace cr;

struct Sample {
    int f;
    double t, z;
    bool moving;
    int finished;
};

static std::vector<Sample> parseGap(const std::string &json, const std::string &key)
{
    std::vector<Sample> out;
    size_t at = json.find("\"" + key + "\"");
    if (at == std::string::npos) return out;
    size_t end = json.find(']', at);
    size_t pos = at;
    while (true) {
        pos = json.find('{', pos);
        if (pos == std::string::npos || pos > end) break;
        size_t close = json.find('}', pos);
        std::string obj = json.substr(pos, close - pos);
        auto num = [&obj](const char *name) {
            size_t p = obj.find(std::string("\"") + name + "\"");
            return p == std::string::npos ? 0.0 : std::atof(obj.c_str() + obj.find(':', p) + 1);
        };
        Sample s;
        s.f = int(num("f"));
        s.t = num("t");
        s.z = num("z");
        s.finished = int(num("finished"));
        s.moving = obj.find("\"moving\": true") != std::string::npos || obj.find("\"moving\":true") != std::string::npos;
        out.push_back(s);
        pos = close;
    }
    return out;
}

int main(int argc, char **argv)
{
    const char *refPath = argc > 1 ? argv[1] : "docs/reference/gsap_double_hop.json";
    std::vector<uint8_t> buf;
    if (!readFile(refPath, buf)) {
        std::printf("cannot read %s\n", refPath);
        return 2;
    }
    std::string json(buf.begin(), buf.end());

    Manifest manifest;
    if (!loadManifest(dataDir() + "manifest.txt", manifest)) return 2;
    ModelLibrary models;
    if (!models.load(manifest, dataDir())) return 2;

    int failures = 0, checks = 0;
    for (int gap : {2, 4, 8}) {
        std::vector<Sample> ref = parseGap(json, "gap_" + std::to_string(gap) + "_frames");
        if (ref.size() < 20) {
            std::printf("reference gap %d missing\n", gap);
            return 2;
        }
        double tSecond = ref[size_t(gap)].t;

        Game game(models, 1);
        game.setupGame("chicken");
        game.init();
        game.startPlaying(); // the probe called updateWithGameState('playing') and waited 600 ms
        for (int i = 0; i < 36; i++) {
            game.step();
            game.endFrame();
        }
        double z0 = game.hero().position().z;

        // port timeline at 60 Hz: first hop at t=0, second at the reference's t of frame `gap`
        int secondStep = int(std::lround(tSecond * 60));
        std::vector<double> portZ;
        std::vector<bool> portMoving;
        for (int s = 0; s <= 60; s++) {
            if (s == 0) game.moveWithDirection(Swipe::Up);
            if (s == secondStep && s != 0) game.moveWithDirection(Swipe::Up);
            if (s == secondStep && s == 0) game.moveWithDirection(Swipe::Up);
            portZ.push_back(game.hero().position().z - z0);
            portMoving.push_back(game.hero().moving);
            game.step();
            game.endFrame();
        }

        double worst = 0;
        int worstFrame = -1;
        for (const Sample &r : ref) {
            if (r.t > 0.95) break;
            // browser frames are irregular: compare against the port's value at the same time, allowing the
            // port to be up to one 60 Hz step early or late
            int s = int(std::floor(r.t * 60));
            double best = 1e9;
            for (int k = std::max(0, s - 1); k <= std::min(60, s + 2); k++) best = std::min(best, std::fabs(portZ[size_t(k)] - r.z));
            checks++;
            if (best > worst) {
                worst = best;
                worstFrame = r.f;
            }
            if (best > 0.12) failures++;
        }
        std::printf("gap %d frames (second hop at %.3f s = step %d): worst |dz| %.3f at ref frame %d, final port dz %.3f\n",
                    gap, tSecond, secondStep, worst, worstFrame, portZ.back());
        checks++;
        if (std::fabs(portZ.back() - 2.0) > 1e-3) {
            std::printf("FAIL gap %d: final dz %.4f, want 2\n", gap, portZ.back());
            failures++;
        }
    }
    std::printf("test_hop: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
