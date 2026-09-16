// Headless run of the port printing the same per-tick state as tools/webref/trace.mjs.
//   trace.exe --seed 3 --steps 400 --script "w20 s w40 u w30 u" --out out/trace_port/name.txt
// Built with CR_FIXED (build/build_host.sh pc-trace / mipsel-trace) it traces the 16.16 SF2000 logic; the values are
// printed through rd(), which only this host-side tool uses.
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "engine/assets.h"
#include "engine/log.h"
#include "game/game.h"
#include "game/difficulty.h"
#include "game/script.h"
#include "game/settings.h"

using namespace cr;

// particle systems as a digest, same fields and summation order as tools/webref/trace.mjs
static std::string particles(const ParticleSystem &sys)
{
    int n = 0;
    double sx = 0, sy = 0, sz = 0, ss = 0;
    for (const Node *p : sys.parts) {
        if (p->visible) n++;
        sx += rd(p->position.x);
        sy += rd(p->position.y);
        sz += rd(p->position.z);
        ss += rd(p->scale.x);
    }
    const Vec3 &m = sys.mesh->position;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d:%.3f,%.3f,%.3f,%.3f@%.3f,%.3f,%.3f", n, sx, sy, sz, ss, rd(m.x), rd(m.y), rd(m.z));
    return buf;
}

static const char *rowTypeName(RowType t)
{
    return t == RowType::Grass ? "grass" : t == RowType::Water ? "water" : t == RowType::Road ? "road" : "railroad";
}

// --path-check: can the hero walk forward through the generated rows? Decided from the rows' own content with the
// game's collision tests (trees, lily pads), independently of GameMap's RowRef::reach. Returns the reachable columns
// (bit x + 4) of row z for a hero arriving on `arrival`.
static int reachOfRow(const GameMap &map, int z, int arrival)
{
    const RowRef *row = map.getRow(real(z));
    int reach = 0;
    if (!row) return 0;
    if (row->type == RowType::Grass) {
        int free = 0;
        for (int x = -4; x <= 4; x++)
            if (!map.treeCollision(Vec3{real(x), 0, real(z)})) free |= 1 << (x + 4);
        reach = arrival & free;
        for (int i = 0; i < 9; i++) reach |= ((reach << 1) | (reach >> 1)) & free;
    } else if (row->type == RowType::Water && !row->water->entities.empty() && row->water->entities[0]->speed == 0) {
        for (int x = -4; x <= 4; x++)
            if ((arrival & (1 << (x + 4))) && row->water->getRidableForPosition(Vec3{real(x), 0, real(z)}))
                reach |= 1 << (x + 4);
    } else if (row->type == RowType::Water && !row->water->entities.empty()) {
        // logs carry the hero with the current: a column counts when some arrival column lies at or before it upstream
        const bool right = row->water->entities[0]->speed > 0;
        for (int x = -4; x <= 4; x++)
            for (int c = -4; c <= 4; c++)
                if ((arrival & (1 << (c + 4))) && (right ? x >= c : x <= c)) reach |= 1 << (x + 4);
    } else if (arrival) {
        reach = 0x1ff; // roads and railroads: every column
    }
    return reach;
}

static int pathCheck(Game &game, uint32_t seed, int rows, int &checked, int &printed)
{
    GameMap &map = game.map();
    int blocked = 0, reach = 1 << 4; // the hero starts on column 0 of the starting row
    for (int z = settings::startingRow; z < settings::startingRow + rows; z++) {
        while (map.rowCount <= z) map.newRow(game.context());
        const RowRef *row = map.getRow(real(z));
        const Node *object = !row ? nullptr : row->grass ? row->grass->object : row->water ? row->water->object
                                            : row->road ? row->road->object : row->railRoad->object;
        if (!object || object->position.z != real(z)) break; // its pool row was reused already (init only)
        int next = reachOfRow(map, z, reach);
        checked++;
        if (!next) {
            blocked++;
            if (printed++ < 1000) {
                const RowRef *prev = map.getRow(real(z - 1));
                std::printf("blocked seed=%u z=%d %s after %s\n", seed, z, rowTypeName(row->type),
                            prev ? rowTypeName(prev->type) : "-");
            }
            next = 0x1ff; // count the next closed row too
        }
        reach = next;
    }
    return blocked;
}

// --difficulty-check: the O8 limits (game/difficulty.h) on the generated rows, judged from the rows' content - a
// dangerous row is a road, a railroad or water whose entities move (logs); speeds are the cars' and logs' own
static int difficultyCheck(Game &game, uint32_t seed, int rows, int &checked, int &printed)
{
    GameMap &map = game.map();
    int bad = 0, dangerRun = 0;
    for (int z = 1; z < settings::startingRow + rows; z++) {
        while (map.rowCount <= z) map.newRow(game.context());
        const RowRef *row = map.getRow(real(z));
        if (!row) {
            dangerRun = 0;
            continue;
        }
        const Node *object = row->grass ? row->grass->object : row->water ? row->water->object
                             : row->road ? row->road->object : row->railRoad->object;
        if (object->position.z != real(z)) break; // its pool row was reused already (init only)
        const int score = difficulty::scoreOfRow(z);
        const bool logs = row->type == RowType::Water && !row->water->entities.empty() && row->water->entities[0]->speed != 0;
        const bool danger = row->type == RowType::Road || row->type == RowType::RailRoad || logs;
        dangerRun = danger ? dangerRun + 1 : 0;
        const auto fail = [&](const char *what) {
            bad++;
            if (printed++ < 20) std::printf("difficulty seed=%u z=%d score=%d %s\n", seed, z, score, what);
        };
        if (dangerRun > difficulty::dangerousRowsInARow(score)) fail("too many dangerous rows in a row");
        if (row->type == RowType::RailRoad) {
            int rails = 0;
            for (int k = z - 9; k <= z; k++) {
                const RowRef *r = k >= 0 ? map.getRow(real(k)) : nullptr;
                if (r && r->type == RowType::RailRoad) rails++;
            }
            if (rails > difficulty::railroadsPerTenRows(score)) fail("too many railroads in 10 rows");
        }
        if (row->type == RowType::Road && !row->road->cars.empty()) {
            const int open = difficulty::carSpeedBaskets(score);
            const real top = (open >= 4 ? real(0.06) : real(0.015) * open) + real(0.02);
            if (rabs(row->road->cars[0]->speed) > top) fail("car too fast");
        }
        if (logs) {
            const int open = difficulty::logSpeedBaskets(score);
            const real top = (open >= 4 ? real(0.05) : real(0.0125) * open) + real(0.02);
            if (rabs(row->water->entities[0]->speed) > top) fail("log too fast");
        }
        checked++;
    }
    return bad;
}

static const char *stateName(GameState s)
{
    switch (s) {
    case GameState::None: return "none";
    case GameState::Playing: return "playing";
    case GameState::Paused: return "paused";
    case GameState::GameOver: return "gameOver";
    }
    return "?";
}

int main(int argc, char **argv)
{
    uint32_t seed = 1;
    int steps = 300;
    int preTicks = 0; // ticks the original runs before its animation-frame loop (see trace.mjs)
    int debugFrom = -1, debugTo = -1; // --gsap-debug A,B: GSAP state after ticks A..B to <out>.gsap.txt
    std::string script, out = "out/trace_port/trace.txt";
    bool gameSounds = false; // --game (or --game-sounds): the game's behaviour instead of the original's (context.h)
    bool rowTypes = false;   // --row-types: a "rows z:type ..." line after pre_ticks (tests; absent otherwise)
    int pathSeeds = 0;       // --path-check N: the way-forward check over N seeds instead of a trace (pathCheck)
    int difficultySeeds = 0; // --difficulty-check N: the O8 limits over N seeds (difficultyCheck)
    bool rails = false;      // --rails: " train=x,box" of the hero's railroad on every line (tests; absent otherwise)
    bool water = false;      // --water: " water=<row is water>,<something to stand on under the hero>" (tests)
    std::string character = "chicken"; // --character ID: the hero model (sw_game --character takes the same ids)
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seed") seed = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--steps") steps = std::atoi(next().c_str());
        else if (a == "--script") script = next();
        else if (a == "--out") out = next();
        else if (a == "--pre") preTicks = std::atoi(next().c_str());
        else if (a == "--gsap-debug") std::sscanf(next().c_str(), "%d,%d", &debugFrom, &debugTo);
        else if (a == "--game-sounds" || a == "--game") gameSounds = true;
        else if (a == "--row-types") rowTypes = true;
        else if (a == "--rails") rails = true;
        else if (a == "--water") water = true;
        else if (a == "--character") character = next();
        else if (a == "--path-check") pathSeeds = std::atoi(next().c_str());
        else if (a == "--difficulty-check") difficultySeeds = std::atoi(next().c_str());
    }
    Manifest manifest;
    if (!loadManifest(dataDir() + "manifest.txt", manifest)) return 2;
    ModelLibrary models;
    if (!models.load(manifest, dataDir())) return 3;

    if (difficultySeeds > 0) {
        // --difficulty-check N [--steps rows] [--game]: seeds --seed .. --seed+N-1
        int checked = 0, printed = 0, bad = 0, seedsBad = 0;
        for (int i = 0; i < difficultySeeds; i++) {
            Game game(models, seed + uint32_t(i));
            game.context().originalBehaviour = !gameSounds;
            game.setupGame("chicken");
            game.init();
            int b = difficultyCheck(game, seed + uint32_t(i), steps, checked, printed);
            bad += b;
            if (b) seedsBad++;
        }
        std::printf("difficulty_check mode=%s seeds=%d rows=%d violations=%d seeds_with_violations=%d\n",
                    gameSounds ? "game" : "original", difficultySeeds, checked, bad, seedsBad);
        return 0;
    }

    if (pathSeeds > 0) {
        // --path-check N [--steps rows] [--game]: seeds --seed .. --seed+N-1, `rows` rows each from the starting row
        int checked = 0, printed = 0, blocked = 0, seedsBlocked = 0;
        for (int i = 0; i < pathSeeds; i++) {
            Game game(models, seed + uint32_t(i));
            game.context().originalBehaviour = !gameSounds;
            game.setupGame("chicken");
            game.init();
            int b = pathCheck(game, seed + uint32_t(i), steps, checked, printed);
            blocked += b;
            if (b) seedsBlocked++;
        }
        std::printf("path_check mode=%s seeds=%d rows=%d blocked=%d seeds_blocked=%d\n", gameSounds ? "game" : "original",
                    pathSeeds, checked, blocked, seedsBlocked);
        return 0;
    }

    Game game(models, seed);
    game.context().originalBehaviour = !gameSounds;
    // the hero's width feeds every collision box, so a trace and a screenshot only line up when both play the same
    // character (O16); the original's traces were all taken with the chicken, so that stays the default
    game.setupGame(character);
    game.init();
    InputScript input;
    input.parse(script);

    FILE *f = std::fopen(out.c_str(), "w");
    if (!f) {
        logf("cannot write %s", out.c_str());
        return 4;
    }
    {
        const Node *n = game.hero().node;
        const Vec3 &mn = n->model->aabbMin, &mx = n->model->aabbMax;
        std::fprintf(f, "node scale=%.4f,%.4f,%.4f pos=%.4f,%.4f,%.4f rot=%.4f,%.4f,%.4f geom=%.4f,%.4f,%.4f,%.4f,%.4f,%.4f children=1\n",
                     rd(n->scale.x), rd(n->scale.y), rd(n->scale.z), rd(n->position.x), rd(n->position.y),
                     rd(n->position.z), rd(n->rotation.x), rd(n->rotation.y), rd(n->rotation.z), rd(mn.x), rd(mn.y),
                     rd(mn.z), rd(mx.x), rd(mx.y), rd(mx.z));
    }
    for (int i = 0; i < preTicks; i++) {
        game.tickEngineOnly();
        game.takeSounds();
    }
    std::fprintf(f, "pre_ticks %d\n", preTicks);
    if (rowTypes) {
        std::string rows = "rows";
        for (int z = 0; z < game.map().rowCount; z++) {
            const RowRef *row = game.map().getRow(real(z));
            if (!row) continue;
            const char *type = row->type == RowType::Grass ? "grass" : row->type == RowType::Water ? "water"
                               : row->type == RowType::Road ? "road" : "railroad";
            rows += " " + std::to_string(z) + ":" + type;
        }
        std::fprintf(f, "%s\n", rows.c_str());
    }
    for (int t = 1; t <= steps; t++) {
        input.apply(game);
        game.step();
        std::string snd;
        for (const std::string &s : game.takeSounds()) snd += (snd.empty() ? "" : "+") + s;
        if (snd.empty()) snd = "-";
        Player &h = game.hero();
        Game::HeroSnapshot hs{h.position(), h.rotation(), h.scale(), h.isAlive, h.moving, h.ridingOn != nullptr,
                              h.hitBy != nullptr};
        if (const Game::HeroSnapshot *old = game.heroReplacedThisStep()) hs = *old;
        const Vec3 &p = hs.position, &r = hs.rotation, &s = hs.scale;
        const Vec3 &w = game.world()->position;
        const Vec3 &sp = game.sceneRoot()->position;
        std::fprintf(f,
                     "t=%d state=%s alive=%d moving=%d riding=%d hit=%d hero=%.3f,%.3f,%.3f rot=%.3f,%.3f "
                     "scale=%.3f,%.3f,%.3f world=%.3f,%.3f rows=%d score=%d scene=%.3f,%.3f,%.3f fth=%s wat=%s snd=%s",
                     t, stateName(game.state()), hs.isAlive ? 1 : 0, hs.moving ? 1 : 0, hs.riding ? 1 : 0,
                     hs.hit ? 1 : 0, rd(p.x), rd(p.y), rd(p.z), rd(r.y), rd(r.z), rd(s.x), rd(s.y), rd(s.z), rd(w.x),
                     rd(w.z), game.map().rowCount, game.score(), rd(sp.x), rd(sp.y), rd(sp.z),
                     particles(game.feathers()).c_str(), particles(game.waterParticles()).c_str(), snd.c_str());
        if (rails) {
            // the train of the railroad the hero is on (its row by jsRound, as RailRoadRow::trainShouldCheckCollision)
            const RowRef *row = game.map().getRow(jsRound(p.z));
            if (row && row->type == RowType::RailRoad)
                std::fprintf(f, " train=%.3f,%.3f", rd(row->railRoad->train.mesh->position.x),
                             rd(row->railRoad->train.collisionBox));
            else
                std::fprintf(f, " train=-");
        }
        if (water) {
            // O12: the hero's row (by jsRound, as the water checks do) and whether a log or a lily pad is under it -
            // a hop chain crossing a river shows water=1,0 on every step without ever drowning
            const RowRef *row = game.map().getRow(jsRound(p.z));
            if (row && row->type == RowType::Water)
                std::fprintf(f, " water=1,%d", row->water->getRidableForPosition(game.hero().position()) ? 1 : 0);
            else
                std::fprintf(f, " water=0,0");
            // O16: the log the hero rides - its centre, its rounded width and how far along it the hero sits. The
            // SF2000 has no depth buffer and sorts whole objects by their centre, so a hero far enough towards +x of
            // a long log's centre sorts behind the log and is drawn under it (build/log_ride_check.sh).
            if (const RowEntity *e = game.hero().ridingOn)
                std::fprintf(f, " ride=%.3f,%d,%.3f", rd(e->mesh->position.x), e->width,
                             rd(game.hero().position().x - e->mesh->position.x));
            else
                std::fprintf(f, " ride=-");
        }
        std::fprintf(f, "\n");
        if (t >= debugFrom && t <= debugTo) {
            std::string path = out + ".gsap.txt";
            FILE *g = std::fopen(path.c_str(), t == debugFrom ? "w" : "a");
            if (g) {
                std::fprintf(g, "---- after tick %d\n%s", t, game.gsapEngine().describe().c_str());
                std::fclose(g);
            }
        }
        game.endFrame();
    }
    std::fclose(f);
    logf("trace %s: %d ticks", out.c_str(), steps);
    return 0;
}
