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

// how many columns a reach mask holds
static int columnCount(int mask)
{
    int n = 0;
    for (int b = 0; b < 9; b++)
        if (mask & (1 << b)) n++;
    return n;
}

// `need` columns per row: 1 for one player, 2 for two (O23 - the user asked for two ways through in that mode)
static int pathCheck(Game &game, uint32_t seed, int rows, int &checked, int &printed, int need = 1)
{
    GameMap &map = game.map();
    // the hero starts on column 0 of the starting row; two players start either side of it
    int blocked = 0, reach = need > 1 ? ((1 << 3) | (1 << 5)) : (1 << 4);
    for (int z = settings::startingRow; z < settings::startingRow + rows; z++) {
        while (map.rowCount <= z) map.newRow(game.context());
        const RowRef *row = map.getRow(real(z));
        const Node *object = !row ? nullptr : row->grass ? row->grass->object : row->water ? row->water->object
                                            : row->road ? row->road->object : row->railRoad->object;
        if (!object || object->position.z != real(z)) break; // its pool row was reused already (init only)
        int next = reachOfRow(map, z, reach);
        checked++;
        if (columnCount(next) < need) {
            blocked++;
            if (printed++ < 1000) {
                const RowRef *prev = map.getRow(real(z - 1));
                std::printf("blocked seed=%u z=%d %s after %s (%d of %d columns)\n", seed, z, rowTypeName(row->type),
                            prev ? rowTypeName(prev->type) : "-", columnCount(next), need);
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

// ---------------------------------------------------------------- --two-player-check (O23)
//
// The two-player rules have no original to compare against - the upstream game's multiplayer button is wired to an
// empty function - so they are checked as INVARIANTS over many seeds instead of against a recorded trace:
//   1. two live players never stand on the same tile unless one is on the other's head;
//   2. a carried player sits exactly on its carrier's column and row;
//   3. the gap between two live players never passes kMaxGap (the duel kills, the co-op pulls back);
//   4. the game is over only when no player is left alive;
//   5. Classic never revives a dead player - it is about records, so dead is dead;
//   6. Progression WITHOUT infinite respawn takes everybody down together, because a level has to be crossed by
//      both of them; WITH it nobody stays dead, and the level never ends by itself.
// Three scripted phases make the interesting things happen often enough to mean something: one drives a player onto
// the other's head, one holds a player still until the gap rule fires, one just plays both forward for a while.
struct TwoPlayerStats {
    int seeds = 0, skipped = 0;
    int carries = 0, escapes = 0, gapKills = 0, pullBacks = 0, revives = 0, deaths = 0, wipes = 0, cancelled = 0;
    // phase 3 exists to make these two happen: a death on a water row, and both players dying in the SAME step.
    // If they stay at zero the scenario proved nothing, whatever the violation count says.
    int drownings = 0, bothInOneStep = 0;
    int violations = 0;
};

static void tpFail(TwoPlayerStats &st, const char *what, uint32_t seed, int level, int phase, int t, const Game &game)
{
    st.violations++;
    if (st.violations > 8) return;
    const Player &a = game.hero(0), &b = game.hero(1);
    std::printf("  VIOLATION %s seed=%lu level=%d phase=%d t=%d  p1=(%.2f,%.2f,%.2f alive=%d carried=%d) "
                "p2=(%.2f,%.2f,%.2f alive=%d carried=%d) retry=%d level_done=%d state=%s\n",
                what, (unsigned long)seed, level, phase, t, rd(a.position().x), rd(a.position().y), rd(a.position().z),
                a.isAlive ? 1 : 0, a.carriedBy ? 1 : 0, rd(b.position().x), rd(b.position().y), rd(b.position().z),
                b.isAlive ? 1 : 0, b.carriedBy ? 1 : 0, game.levelRetrySteps(), game.levelDone() ? 1 : 0,
                stateName(game.state()));
}

// phase 0: player 0 hops sideways onto player 1, then player 1 walks out from under it
// phase 1: player 1 walks forward while player 0 stands still, until the gap rule fires
// phase 2: both walk forward on different cadences for as long as they live
// phase 3: both walk forward IN LOCKSTEP, so they meet every water row on the same row in the same step and drown
//          together - the user's second report, "obaj wpadli do wody i sie nie zresetowalo ani game over ani nic".
//          Two players dying in the same step is the case none of the other phases makes happen often.
static void twoPlayerScript(Game &game, int phase, int t)
{
    auto hop = [&game](int player, Swipe dir) {
        game.beginMoveWithDirection(player);
        game.moveWithDirection(dir, player);
    };
    // phase 4: the same lockstep, but hopping FASTER than a hop lasts, so every hop interrupts the one before it.
    // That is the only way into O12's drowning (Game::moveWithDirection, `interrupted`), which is where the second
    // player's death was being handed to the first one.
    if (phase == 4) {
        if (t >= 20 && t % 5 == 0) {
            static const Swipe kDirs[4] = {Swipe::Up, Swipe::Up, Swipe::Up, Swipe::Left};
            hop(0, kDirs[(t / 5) % 4]);
            hop(1, kDirs[(t / 5 + 1) % 4]);
        }
        return;
    }
    if (phase == 3) {
        // In LOCKSTEP, so both stand on the same row and meet the same water at the same moment, but not in a
        // straight line: the sideways hops put them on logs, on the SAME log, and on one tile over water, which is
        // where the interesting combinations are. Up four times out of six, so they keep going forward.
        if (t >= 20 && t % 14 == 0) {
            static const Swipe kDirs[6] = {Swipe::Up, Swipe::Up, Swipe::Left, Swipe::Up, Swipe::Right, Swipe::Up};
            const int k = (t / 14) % 6;
            hop(0, kDirs[k]);
            hop(1, kDirs[(k + 2) % 6]);
        }
        return;
    }
    if (phase == 0) {
        // Swipe::Left moves towards +x, so player 0 at x = -1 reaches player 1 at x = +1 in two hops
        if (t == 20 || t == 40) hop(0, Swipe::Left);
        if (t == 90) hop(1, Swipe::Up);
    } else if (phase == 1) {
        if (t >= 20 && t % 16 == 0) hop(1, Swipe::Up);
    } else {
        if (t >= 20 && t % 13 == 0) hop(0, Swipe::Up);
        if (t >= 20 && t % 17 == 0) hop(1, Swipe::Up);
    }
}

static void twoPlayerCheck(const ModelLibrary &models, uint32_t seed, int seeds, int steps, TwoPlayerStats &st)
{
    for (int s = 0; s < seeds; s++) {
        const uint32_t sd = seed + uint32_t(s);
        for (int phase = 0; phase < 5; phase++) {
            // 0 = Classic, 1 = Progression, 2 = Progression with infinite respawn on,
            // 3 = ONE player with infinite respawn (its own code path: there is no partner to come back onto),
            // 4 = infinite respawn TURNED OFF part way through, which is what a player does in the pause menu and
            //     is the likeliest way into "both died and nothing happened" (user report)
            for (int mode = 0; mode <= 4; mode++) {
                const int level = mode == 0 ? 0 : 1;
                const int players = mode == 3 ? 1 : 2;
                const bool respawn = mode >= 2; // modes 2, 3 and 4 start with the option on
                Game game(models, sd);
                game.context().originalBehaviour = false;
                game.setPlayerCount(players);
                game.setInfiniteRespawn(respawn);
                game.setLevel(level);
                game.setupGame("beaver");
                game.init();
                // phase 0 needs both starting tiles free, or player 1 begins inside a tree
                if (phase == 0 && players == 1) continue; // nobody to land on
                if (phase == 0 && (game.map().treeCollision(Vec3{real(1), 0, real(settings::startingRow)}) ||
                                   game.map().treeCollision(Vec3{real(-1), 0, real(settings::startingRow)}))) {
                    st.skipped++;
                    continue;
                }
                st.seeds++;
                game.startPlaying();
                game.endFrame();
                bool wasCarried = false;
                int stuckSteps = 0; // steps with nobody alive and nobody on the way back
                int deadSince[2] = {-1, -1};
                int movingSteps[2] = {0, 0}; // steps a live hero has spent mid-hop without moving, for invariant 7
                Vec3 wasAt[2] = {game.hero(0).position(), game.hero(1).position()};
                bool wasAlive[2] = {true, true};
                for (int t = 1; t <= steps; t++) {
                    // mode 4: the player changes their mind in the pause menu at the WORST possible moment -
                    // while somebody is waiting to come back. That waiting player is now waiting for nothing, and
                    // if the other one dies too the game has every hero dead and nobody on the way back. Switching
                    // at a fixed step instead almost never lands on that instant, and the first version of this
                    // test passed against code that had the bug.
                    if (mode == 4 && game.infiniteRespawn() && game.levelRetrySteps() > 0) {
                        game.setInfiniteRespawn(false);
                        st.cancelled++;
                    }
                    twoPlayerScript(game, phase, t);
                    game.step();
                    game.endFrame();
                    const Player &a = game.hero(0), &b = game.hero(1);
                    const int alive = players > 1 ? (a.isAlive ? 1 : 0) + (b.isAlive ? 1 : 0) : (a.isAlive ? 1 : 0);

                    // 5 and 6: who may come back, and who must take the others down with them
                    int diedThisStep = 0;
                    for (int i = 0; i < players; i++) {
                        const Player &h = game.hero(i);
                        const bool partnerAlive = players > 1 && game.hero(i == 0 ? 1 : 0).isAlive;
                        if (wasAlive[i] && !h.isAlive) {
                            st.deaths++;
                            diedThisStep++;
                            const RowRef *row = game.map().getRow(rfloor(h.position().z + real(0.5)));
                            if (row && row->type == RowType::Water) st.drownings++;
                            deadSince[i] = t;
                            if (h.warnSteps > 0 || (mode == 0 && partnerAlive)) st.gapKills++;
                            // 6. PROGRESSION TAKES EVERYBODY DOWN, whether endless retries is on or off - the
                            // option decides whether the level starts again by itself, never who dies. Checked in
                            // every Progression mode now, not just the one without retries.
                            if (level > 0 && players > 1) {
                                if (partnerAlive)
                                    tpFail(st, "coop-left-a-player-alive-after-a-death", sd, level, phase, t, game);
                                else st.wipes++;
                            }
                        }
                        if (!wasAlive[i] && h.isAlive) {
                            st.revives++;
                            if (!respawn || !game.infiniteRespawn())
                                tpFail(st, "revived-where-nothing-may-revive", sd, level, phase, t, game);
                            // 8. AND THEY COME BACK AT THE START OF THE LEVEL. The author asked for this three
                            // times - "obaj sie resetuja na starcie - musza obaj przejsc" - and got a player put
                            // back on its partner's head in the middle of the level instead, which is my own
                            // invention and not the rule. Everybody is alive, on the starting row, score zero.
                            if (rabs(h.position().z - real(settings::startingRow)) > real(0.01) || game.score(i) != 0)
                                tpFail(st, "came-back-somewhere-other-than-the-start", sd, level, phase, t, game);
                            for (int k = 0; k < players; k++)
                                if (!game.hero(k).isAlive)
                                    tpFail(st, "level-started-again-with-somebody-still-dead", sd, level, phase, t, game);
                            deadSince[i] = -1;
                        }
                        if (respawn && game.infiniteRespawn() && !h.isAlive && deadSince[i] >= 0 &&
                            t - deadSince[i] > settings::respawnSteps + 5)
                            tpFail(st, "endless-retries-never-started-the-level-again", sd, level, phase, t, game);
                        if (respawn && h.carriedBy && !wasAlive[i] && h.isAlive) st.pullBacks++;
                        wasAlive[i] = h.isAlive;
                    }
                    if (diedThisStep > 1) st.bothInOneStep++;

                    // 7. NOBODY IS STUCK MID-HOP. A hop clears `moving` when its animation ends, so a live hero left
                    // with the flag set is a hero the water will never drown, the rows will never test and the player
                    // can no longer move - which is what "obaj wpadli do wody i nic sie nie dzialo" looked like from
                    // the sofa. It has to be STUCK, not merely moving: a chain of fast hops keeps the flag up across
                    // hop after hop quite legitimately, which is what the first version of this check failed on. So
                    // the test is the one a player would make - still hopping, and not actually going anywhere.
                    for (int i = 0; i < players; i++) {
                        const Player &h = game.hero(i);
                        const bool still = rabs(h.position().x - wasAt[i].x) < real(0.001) &&
                                           rabs(h.position().y - wasAt[i].y) < real(0.001) &&
                                           rabs(h.position().z - wasAt[i].z) < real(0.001);
                        wasAt[i] = h.position();
                        if (h.isAlive && h.moving && still) {
                            if (++movingSteps[i] > 90)
                                tpFail(st, "a-hero-is-stuck-mid-hop", sd, level, phase, t, game);
                        } else {
                            movingSteps[i] = 0;
                        }
                    }

                    // 4. the game ends only when nobody is left - and with infinite respawn it does not end at all,
                    // short of finishing the level
                    if (game.state() == GameState::GameOver && alive > 0 && !game.levelDone())
                        tpFail(st, "game-over-with-a-live-player", sd, level, phase, t, game);
                    // ...and the other way round, which is what a player actually notices: with nobody alive and
                    // nobody coming back, the game MUST end. The user found this by playing - "obaj zgineli i nic
                    // sie nie dzialo" - and the old check could not see it, because it only ever looked at runs
                    // that were still being played.
                    //
                    // Judged the way a PLAYER judges it: every hero dead and the game still going, for longer than
                    // a revival could possibly take. Deliberately NOT asking the game whether anybody is "waiting
                    // to come back" - the first version of this check did, and it was blind to the very bug it was
                    // written for, where a waiting counter was frozen and nothing was ever going to act on it.
                    if (alive == 0 && game.state() == GameState::Playing) {
                        if (++stuckSteps > settings::respawnSteps + 10)
                            tpFail(st, "everybody-dead-and-the-game-goes-on", sd, level, phase, t, game);
                    } else {
                        stuckSteps = 0;
                    }
                    if (respawn && game.infiniteRespawn() && game.state() == GameState::GameOver && !game.levelDone())
                        tpFail(st, "infinite-respawn-still-ended-the-level", sd, level, phase, t, game);
                    if (game.state() != GameState::Playing) break;

                    // 2. a carried player rides the carrier's tile - once it has LANDED there. O24 gives the two
                    // rule-driven moves onto a head (the co-op pull-back, two on one tile) the same little arc a
                    // hop has, because appearing up there out of nowhere was what the author reported; while that
                    // arc is in the air the player is deliberately between the two tiles.
                    for (int i = 0; i < players; i++) {
                        const Player &h = game.hero(i);
                        if (!h.carriedBy || h.carryHopSteps > 0) continue;
                        const Player &u = *h.carriedBy;
                        if (rabs(h.position().x - u.position().x) > real(0.01) ||
                            rabs(h.position().z - u.position().z) > real(0.01) ||
                            h.position().y <= u.position().y)
                            tpFail(st, "carried-off-the-head", sd, level, phase, t, game);
                    }
                    if ((a.carriedBy || b.carriedBy) && !wasCarried) st.carries++;
                    if (wasCarried && !a.carriedBy && !b.carriedBy) st.escapes++;
                    wasCarried = a.carriedBy != nullptr || b.carriedBy != nullptr;

                    // 1. no two live players on one tile unless one is carried
                    if (players > 1 && a.isAlive && b.isAlive && !a.moving && !b.moving && !a.carriedBy && !b.carriedBy &&
                        jsRound(a.position().z) == jsRound(b.position().z) &&
                        rabs(a.position().x - b.position().x) < real(0.5))
                        tpFail(st, "two-players-on-one-tile", sd, level, phase, t, game);

                    // 3. the gap never passes the limit (the rule fires in the same step it is reached, so one row
                    // of slack covers a hop that is still in the air)
                    if (players > 1 && a.isAlive && b.isAlive && !a.carriedBy && !b.carriedBy) {
                        const real gap = rabs(a.position().z - b.position().z);
                        if (gap > real(Game::kMaxGap) + real(1.05))
                            tpFail(st, "gap-past-the-limit", sd, level, phase, t, game);
                    }

                }
            }
        }
    }
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
    int twoPlayerSeeds = 0; // --two-player-check N: the O23 two-player rules as invariants over N seeds
    bool twoPaths = false;  // --two-paths: with --path-check, demand TWO ways through every row (O23, two players)
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
        else if (a == "--two-player-check") twoPlayerSeeds = std::atoi(next().c_str());
        else if (a == "--two-paths") twoPaths = true;
    }
    Manifest manifest;
    if (!loadManifest(dataDir() + "manifest.txt", manifest)) return 2;
    ModelLibrary models;
    if (!models.load(manifest, dataDir())) return 3;

    if (twoPlayerSeeds > 0) {
        // --two-player-check N [--steps ticks]: the O23 rules as invariants over N seeds
        TwoPlayerStats st;
        twoPlayerCheck(models, seed, twoPlayerSeeds, steps, st);
        std::printf("two_player_check seeds=%d runs=%d skipped=%d carries=%d escapes=%d deaths=%d gap_deaths=%d "
                    "revives=%d coop_wipes=%d retries_cancelled=%d drownings=%d both_died_in_one_step=%d "
                    "violations=%d\n",
                    twoPlayerSeeds, st.seeds, st.skipped, st.carries, st.escapes, st.deaths, st.gapKills, st.revives,
                    st.wipes, st.cancelled, st.drownings, st.bothInOneStep, st.violations);
        return st.violations == 0 ? 0 : 1;
    }

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
            if (twoPaths) game.setPlayerCount(2);
            game.setupGame("chicken");
            game.init();
            int b = pathCheck(game, seed + uint32_t(i), steps, checked, printed, twoPaths ? 2 : 1);
            blocked += b;
            if (b) seedsBlocked++;
        }
        std::printf("path_check mode=%s players=%d seeds=%d rows=%d blocked=%d seeds_blocked=%d\n",
                    gameSounds ? "game" : "original", twoPaths ? 2 : 1, pathSeeds, checked, blocked, seedsBlocked);
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
