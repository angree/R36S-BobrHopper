// The 16.16 game (CR_FIXED) drawn by the SF2000 software renderer on the PC: scenario screenshots to compare with the
// GLES build (build/sw_scene_compare.sh for the 3D scene, build/ui_shots.sh sw for the screens).
//   sw_game.exe --scenario car_cross_s26 --frames 90 --shots 60,90 --shot-prefix car_cross_s26 \
//               --shot-dir out/check/sw_scene/port [--size 320x240] [--view-scale 6] [--view-shift -0.15] [--data data_sf2000]
//   --hud [--ui-data data] [--pause-at N] [--settings-at N]: HUD and screens (fonts and images from --ui-data); the menus
//   open on step N the way the pad would open them
// Numbered shots are <prefix>_t<step>.png after that logic step, like bobrhopper --shots; script `shot:name` tokens
// write <name>.png.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "engine/assets.h"
#include "engine/input.h"
#include "engine/log.h"
#include "engine/png_write.h"
#include "engine/renderer.h"
#include "engine/text.h"
#include "game/game.h"
#include "game/scene_render.h"
#include "game/script.h"
#include "game/smoke_bot.h"
#include "ui/hud.h"
#include "ui/screens.h"

using namespace cr;

static uint64_t hostMicros()
{
    return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

int main(int argc, char **argv)
{
    uint32_t seed = 1;
    long frames = 0, pauseAt = -1, settingsAt = -1;
    int bench = 0; // --bench N: draw every shot frame N times and print the cost per frame and stage
    bool modelStats = false; // --model-stats: per shot, visible nodes per model and their front-facing triangles (O3.7)
    // --dump-nodes: every visible node with a model, by name and world position, at each shot. It exists so the
    // Amiga sprite port can be compared object by object instead of pixel by pixel - a percentage of differing
    // pixels cannot say WHICH object moved, and reading it as "a whole class is missing" was wrong twice.
    bool dumpNodes = false;
    bool depthBuffer = false; // --depth: the depth-buffered renderer of v003-v008 instead of the painter's order (O5.3)
    bool rideFix = true;      // --no-ride-fix: O16 off, so a test can render one frame with and without it
    bool originalBehaviour = false; // --original: the original's behaviour (GameContext::originalBehaviour), e.g. its map
    long scanFrames = 0;            // --scan N [--scan-out file]: per-frame cost of the bot's game (see the scan block)
    std::string scanOut = "scan.txt";
    // --fly: no bot; the hero stays in the start state (collisions end nothing there) and glides forward one row a
    // second, so the camera and the map pass every kind of row through the whole screen
    bool scanFly = false;
    // --scan-detail: the renderer's own clock too (mesh setup vs pixels per draw; its clock calls add their own cost)
    bool scanDetail = false;
    int width = 320, height = 240;
    double viewScale = 6, viewShift = -0.15;
    std::string scenario, script, data = "data_sf2000", uiData = "data", shotDir = ".", shotPrefix = "shot";
    std::vector<long> shots;
    ShadowMode shadows = ShadowMode::Full;
    bool hud = false;
    int uiScale = 1; // --ui-scale 2: the screens in logical pixels twice the size, half-size fonts (the SF2000 core)
    int level = 0;   // --level N: play Progression level N (0 = Classic), for the finish line's screenshots
    // --character ID: the hero model (the game's own default is the beaver; the older shot comparisons were all taken
    // with the chicken, so that stays the default here)
    std::string character = "chicken";
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--scenario") scenario = next();
        else if (a == "--seed") seed = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--frames") frames = std::atol(next().c_str());
        else if (a == "--script") script = next();
        else if (a == "--level") level = std::atoi(next().c_str());
        else if (a == "--character") character = next();
        else if (a == "--data") data = next();
        else if (a == "--ui-data") uiData = next();
        else if (a == "--hud") hud = true;
        else if (a == "--ui-scale") uiScale = std::atoi(next().c_str());
        else if (a == "--bench") bench = std::atoi(next().c_str());
        else if (a == "--model-stats") modelStats = true;
        else if (a == "--dump-nodes") dumpNodes = true;
        else if (a == "--depth") depthBuffer = true;
        else if (a == "--no-ride-fix") rideFix = false;
        else if (a == "--original") originalBehaviour = true;
        else if (a == "--scan") scanFrames = std::atol(next().c_str());
        else if (a == "--scan-out") scanOut = next();
        else if (a == "--fly") scanFly = true;
        else if (a == "--scan-detail") scanDetail = true;
        else if (a == "--paint-key") sw::gPaintKey = std::atoi(next().c_str());
        else if (a == "--small-raster") sw::detail::gSmallTrianglePixels = std::atoi(next().c_str());
        else if (a == "--pause-at") pauseAt = std::atol(next().c_str());
        else if (a == "--settings-at") settingsAt = std::atol(next().c_str());
        else if (a == "--shot-dir") shotDir = next();
        else if (a == "--shot-prefix") shotPrefix = next();
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &width, &height);
        else if (a == "--view-scale") viewScale = std::atof(next().c_str());
        else if (a == "--view-shift") viewShift = std::atof(next().c_str());
        else if (a == "--shadows") {
            const std::string m = next();
            shadows = m == "off" ? ShadowMode::Off : m == "simple" ? ShadowMode::Simple : ShadowMode::Full;
        } else if (a == "--shots") {
            std::istringstream list(next());
            std::string item;
            while (std::getline(list, item, ','))
                if (!item.empty()) shots.push_back(std::atol(item.c_str()));
        } else {
            std::fprintf(stderr, "sw_game: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    if (!scenario.empty()) {
        FILE *f = std::fopen("build/trace_scenarios.txt", "rb");
        bool found = false;
        char buf[4096];
        while (f && !found && std::fgets(buf, sizeof buf, f)) {
            std::string line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            const size_t p1 = line.find('|'), p2 = line.find('|', p1 + 1), p3 = line.find('|', p2 + 1);
            if (line.empty() || line[0] == '#' || p3 == std::string::npos || line.substr(0, p1) != scenario) continue;
            seed = uint32_t(std::strtoul(line.substr(p1 + 1, p2 - p1 - 1).c_str(), nullptr, 10));
            if (!frames) frames = std::atol(line.substr(p2 + 1, p3 - p2 - 1).c_str());
            script = line.substr(p3 + 1);
            found = true;
        }
        if (f) std::fclose(f);
        if (!found) {
            std::fprintf(stderr, "sw_game: unknown scenario %s (run from the repo root)\n", scenario.c_str());
            return 2;
        }
    }
    data += "/";
    uiData += "/";

    Manifest manifest;
    if (!loadManifest(data + "manifest.txt", manifest)) {
        std::fprintf(stderr, "sw_game: no manifest in %s\n", data.c_str());
        return 3;
    }
    ModelLibrary models;
    if (!models.load(manifest, data)) return 3;
    Renderer renderer;
    renderer.init(width, height);
    SceneRenderer scene;
    if (!scene.init(renderer, models, manifest, data)) return 4;
    scene.shadowMode = shadows;
    scene.depthBuffer = depthBuffer;
    scene.rideFix = rideFix;
    renderer.depthBuffer = depthBuffer;
    scene.viewShift = Fixed(viewShift);
    TextRenderer text;
    Screens screens;
    UserSettings userSettings;
    screens.settings = &userSettings;
    text.glyphScale = uiScale;
    if (hud && (!text.load(renderer, uiData) || !screens.load(renderer, uiData))) {
        std::fprintf(stderr, "sw_game: no fonts/images in %s\n", uiData.c_str());
        return 5;
    }

    Game game(models, seed);
    game.context().originalBehaviour = originalBehaviour;
    game.setupGame(character);
    game.init();

    // The same two diagnostic lines the Amiga port prints, in the same format. This build is CR_FIXED, so it
    // is the RIGHT reference for the Amiga - apps/trace.cpp is a PC build and compares a different arithmetic
    // path. The Amiga's row types diverge from the first randomised row while its RNG sequence is bit-identical
    // to mulberry32, which points at a different NUMBER of draws rather than different numbers.
    // Per-row stream state, so the FIRST row whose number disagrees with the Amiga names the function that
    // draws a different number of values. The first nine rows are forced grass (rowCount < 10), so a
    // divergence there is in obstacle generation, before any row-type draw happens at all.
    std::printf("birth: rng-state map=%lu fx=%lu\n", (unsigned long)game.rng().map.state(),
                (unsigned long)game.rng().fx.state());
    std::printf("birth: rows");
    for (int rz = 1; rz <= 23; rz++) {
        const RowRef *r = game.map().getRow(cr::real(rz));
        const char *kind = "none";
        if (r) {
            switch (r->type) {
            case RowType::Grass: kind = "grass"; break;
            case RowType::Road: kind = "road"; break;
            case RowType::Water: kind = "water"; break;
            case RowType::RailRoad: kind = "railroad"; break;
            default: kind = "?"; break;
            }
        }
        std::printf(" %d:%s", rz, kind);
    }
    std::printf("\n");
    // O11.3: --level N plays a Progression level (the finish line and the HUD counters)
    if (level > 0) game.setLevel(level);
    game.tickEngineOnly(); // as bobrhopper.cpp: GameEngine.unpause() ticks once before the frame loop
    game.takeSounds();
    InputScript input;
    input.parse(script);

    int failed = 0;
    uint64_t stepMicros = 0;
    long stepCount = 0;
    // one whole frame as the core will draw it; returns the stats of the 3D scene alone
    auto drawFrame = [&]() {
        renderer.bindTarget(nullptr);
        renderer.clear(Fixed(0x87 / 255.0), Fixed(0xC6 / 255.0), Fixed(0xFF / 255.0));
        renderer.resetStats();
        scene.render(renderer, game, width, height, Fixed(viewScale));
        const RenderStats sceneStats = renderer.stats;
        if (hud) {
            screens.drawSceneFade(renderer, width * uiScale, height * uiScale);
            drawHud(renderer, text, game, width * uiScale, height * uiScale);
            screens.draw(renderer, text, game, width * uiScale, height * uiScale);
        }
        return sceneStats;
    };
    auto save = [&](const std::string &path) {
        if (modelStats) {
            // the triangles each model drew in this frame (after node culling and faces turned away)
            std::vector<std::pair<const Model *, int>> log;
            scene.drawLog = &log;
            drawFrame();
            scene.drawLog = nullptr;
            std::vector<std::pair<std::string, std::pair<int, long>>> byModel;
            for (const auto &d : log) {
                bool found = false;
                for (auto &e : byModel)
                    if (e.first == d.first->name) e.second.first++, e.second.second += d.second, found = true;
                if (!found) byModel.push_back({d.first->name, {1, long(d.second)}});
            }
            std::sort(byModel.begin(), byModel.end(),
                      [](const auto &x, const auto &y) { return x.second.second > y.second.second; });
            long total = 0;
            for (const auto &e : byModel) total += e.second.second;
            std::printf("models %s: %ld triangles drawn\n", path.c_str(), total);
            for (const auto &e : byModel)
                std::printf("  %-22s nodes %3d  tris %6ld  %4.1f%%\n", e.first.c_str(), e.second.first, e.second.second,
                            100.0 * double(e.second.second) / double(total ? total : 1));
        }
        if (bench > 0) {
            renderer.profileClock = hostMicros;
            std::vector<double> ms;
            int64_t transform = 0, raster = 0, shadows = 0, overlay = 0;
            for (int i = 0; i < bench; i++) {
                const uint64_t t0 = hostMicros();
                drawFrame();
                ms.push_back(double(hostMicros() - t0) / 1000.0);
                transform += renderer.stats.usTransform;
                raster += renderer.stats.usRaster;
                shadows += renderer.stats.usShadows;
                overlay += renderer.stats.usOverlay;
            }
            renderer.profileClock = nullptr;
            const uint64_t w0 = hostMicros();
            for (int i = 0; i < bench; i++) updateWorld(game.sceneRoot());
            const double worldMs = double(hostMicros() - w0) / 1000.0 / bench;
            std::sort(ms.begin(), ms.end());
            double sum = 0;
            for (double v : ms) sum += v;
            const RenderStats s = drawFrame();
            std::printf("bench %s: frame avg %.3f min %.3f median %.3f ms | transform %.3f raster %.3f shadows %.3f "
                        "overlay %.3f ms | updateWorld %.3f ms | draws %d tris %d drawn %d spans %d pixels %lld\n",
                        path.c_str(), sum / bench, ms.front(), ms[ms.size() / 2], transform / 1000.0 / bench,
                        raster / 1000.0 / bench, shadows / 1000.0 / bench, overlay / 1000.0 / bench, worldMs,
                        s.drawCalls, s.triangles, s.trianglesDrawn, renderer.stats.spans,
                        (long long)renderer.stats.pixels);
        }
        const RenderStats sceneStats = drawFrame();
        std::vector<uint8_t> rgba;
        renderer.readPixels(width, height, rgba);
        const bool ok = png::writeRGBA(path, width, height, rgba.data());
        failed += ok ? 0 : 1;
        // ride=<frames on a log>,<of those, frames whose log sorted over the hero> (O16, build/log_ride_check.sh)
        std::printf("sw_game: %s draws=%d tris=%d drawn=%d culled=%d casters=%d heroY=%.2f ahead=%d behind=%d "
                    "ride=%d,%d %s\n",
                    path.c_str(), sceneStats.drawCalls, sceneStats.triangles, sceneStats.trianglesDrawn, scene.culled,
                    scene.shadowCasters, rd(scene.framing.heroScreenY), scene.framing.rowsAhead,
                    scene.framing.rowsBehind, scene.rideFrames, scene.ridePushed, ok ? "ok" : "FAILED");
        if (dumpNodes) {
            // Positions relative to the camera, which is what the Amiga port prints: the scene is drawn around
            // the camera there so the 16.16 numbers stay small, and only the same frame of reference compares.
            const cr::Vec3 cam = game.cameraPosition();
            std::vector<const cr::Node *> stack(1, game.sceneRoot());
            while (!stack.empty()) {
                const cr::Node *n = stack.back();
                stack.pop_back();
                if (!n || !n->visible) continue;
                if (n->model)
                    std::printf("hostnode: %-20s rel=(%.4f,%.4f)\n", n->model->name.c_str(),
                                rd(n->world.e[12] - cam.x), rd(n->world.e[14] - cam.z));
                for (size_t i = 0; i < n->children.size(); i++) stack.push_back(n->children[i]);
            }
        }
    };

    if (scanFrames > 0) {
        // O7: the SF2000 core's game loop with the smoke bot, 2 logic steps per frame (game30), one line per frame with
        // the logic and render microseconds by part, triangles, pixels, live root animations and the rows in view
        // (g grass, w water with lily pads, W water with logs, r road, t railroad; the hero's row is the 3rd). Under
        // qemu the times follow the MIPS instructions, 64-bit divisions included.
        SmokeBot bot(seed);
        Input in;
        FILE *out = std::fopen(scanOut.c_str(), "w");
        if (!out) return 6;
        game.profileClock = hostMicros;
        scene.profileClock = hostMicros;
        if (scanDetail) renderer.profileClock = hostMicros;
        static const struct {
            Action act;
            Swipe dir;
        } dirs[] = {{ActUp, Swipe::Up}, {ActDown, Swipe::Down}, {ActLeft, Swipe::Left}, {ActRight, Swipe::Right},
                    {ActA, Swipe::Up}};
        for (long f = 1; f <= scanFrames; f++) {
            game.stepProfile = Game::StepProfile();
            const uint64_t l0 = hostMicros();
            for (int s = 0; s < 2; s++) {
                if (scanFly) {
                    // no hop ever clears a ride here: a log or pad left behind is freed when its row is regenerated
                    game.hero().ridingOn = nullptr;
                    game.hero().hitBy = nullptr;
                    game.hero().position().x = 0;
                    game.hero().position().z += real(1) / real(60);
                    game.step();
                    game.endFrame();
                    game.takeSounds();
                    continue;
                }
                in.setSynthetic(bot.next(game.state()));
                in.step();
                switch (game.state()) {
                case GameState::None:
                    if (in.released(ActUp) || in.released(ActA)) game.startPlaying();
                    break;
                case GameState::Playing:
                    for (const auto &d : dirs) {
                        if (in.pressed(d.act)) game.beginMoveWithDirection();
                        if (in.released(d.act)) game.moveWithDirection(d.dir);
                    }
                    break;
                case GameState::GameOver:
                    if (in.released(ActA)) game.restart();
                    break;
                default:
                    break;
                }
                game.step();
                game.endFrame();
                game.takeSounds();
            }
            const uint64_t l1 = hostMicros();
            const RenderStats st = drawFrame();
            const uint64_t r1 = hostMicros();
            const SceneRenderer::Profile &p = scene.profile;
            for (long s : shots)
                if (s == f) save(shotDir + "/" + shotPrefix + "_f" + std::to_string(f) + ".png");
            std::string rows;
            const int hz = int(jsRound(game.hero().position().z));
            for (int z = hz - 2; z <= hz + 9; z++) {
                const RowRef *row = game.map().getRow(real(z));
                char c = '.';
                if (row && row->type == RowType::Grass) c = 'g';
                else if (row && row->type == RowType::Water) c = row->water->lilyPadPositions.empty() ? 'W' : 'w';
                else if (row && row->type == RowType::Road) c = 'r';
                else if (row && row->type == RowType::RailRoad) c = 't';
                rows += c;
            }
            std::fprintf(out,
                         "f=%ld logic=%lld gsap=%lld map=%lld render=%lld world=%lld trav=%lld sort=%lld floors=%lld "
                         "shadows=%lld objects=%lld tris=%d drawn=%d pixels=%lld anims=%d z=%d state=%d rows=%s\n",
                         f, (long long)(l1 - l0), (long long)game.stepProfile.gsap, (long long)game.stepProfile.map,
                         (long long)(r1 - l1), (long long)p.updateWorld, (long long)p.traverse, (long long)p.sort,
                         (long long)p.floors, (long long)p.shadows, (long long)p.objects, st.triangles, st.trianglesDrawn,
                         (long long)renderer.stats.pixels, game.gsapEngine().liveAnimations(), hz, int(game.state()),
                         rows.c_str());
            if (scanDetail)
                std::fprintf(out, "  detail f=%ld xform=%lld raster=%lld shadowsR=%lld\n", f,
                             (long long)renderer.stats.usTransform, (long long)renderer.stats.usRaster,
                             (long long)renderer.stats.usShadows);
            std::fflush(out);
        }
        std::fclose(out);
        return 0;
    }

    for (long t = 1; t <= frames; t++) {
        // bobrhopper.cpp's doStep order: scripted input, menus, the game step unless a menu pauses it, screens
        const std::vector<std::string> named = input.apply(game);
        if (t == pauseAt) screens.openPause();
        if (t == settingsAt) screens.openSettings(false);
        if (!screens.pausesGame()) {
            const uint64_t s0 = bench > 0 ? hostMicros() : 0;
            game.step();
            game.endFrame();
            if (bench > 0) stepMicros += hostMicros() - s0, stepCount++;
        }
        screens.update(game);
        game.takeSounds();
        for (long s : shots)
            if (s == t) save(shotDir + "/" + shotPrefix + "_t" + std::to_string(t) + ".png");
        for (const std::string &name : named) save(shotDir + "/" + name + ".png");
    }
    // the SF2000 ran a logic step in 1.24-1.55 ms in v002: a second host-to-device ratio next to updateWorld's
    if (bench > 0 && stepCount > 0)
        std::printf("bench logic: %ld steps, avg %.4f ms per step\n", stepCount, double(stepMicros) / 1000.0 / double(stepCount));
    return failed ? 1 : 0;
}
