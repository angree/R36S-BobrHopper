// BobrHopper for PC and R36S.
//
// Play:        bobrhopper [--seed N] [--view-scale 2]
// Automation:  bobrhopper --hidden --seed 1 --auto "w60 s w30 u w20 u w20 l w40 shot:start" --shot-dir out/shots
//              --frames N stops after N logic steps; --fast runs steps without real-time pacing
//              script tokens: wN = wait N steps, s = start (like releasing Up on the home screen),
//              u/d/l/r = hop (key down this step, key up next), a = A button, shot:name = screenshot
#include <SDL.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/config.h"
#include "engine/input.h"
#include "engine/log.h"
#include "engine/platform.h"
#include "engine/png_write.h"
#include "engine/renderer.h"
#include "engine/text.h"
#include "game/game.h"
#include "game/scene_render.h"
#include "game/settings.h"
#include "game/smoke_bot.h"
#include "game/sound_volume.h"
#include "ui/debug_overlay.h"
#include "ui/hud.h"
#include "ui/lang.h"
#include "ui/controls.h"
#include "ui/screens.h"

using namespace cr;

namespace {

struct Options {
    uint32_t seed = 0;
    bool hidden = false, headless = false, fast = false, fullscreen = false;
    bool noIdle = false;  // stop the idle squash before shots (frozen-frame comparisons)
    ShadowMode shadows = ShadowMode::Full;
    bool shadowsCli = false, viewCli = false; // given on the command line: settings do not override them
    int width = 640, height = 480;
    float viewScale = settings::defaultViewScale;
    float viewShift = settings::defaultViewShift;
    long frames = 0;
    std::string autoScript, shotDir = ".";
    std::string confPath; // --conf; automated runs (hidden/headless) only save when it is given
    long smoke = 0;              // --smoke N: a bot plays N steps; pool/animation counts per game, leak check
    std::string record, replay;  // --record / --replay: Input bitmask per logic step
    bool framingStats = false;   // --framing-stats: where the hero sits on screen over the whole run (K.1)
    bool hud = true;             // --no-hud: 3D scene only (comparisons with the original's canvas)
    bool overlay = false;        // --overlay (or conf fps_counter=1, Select+L): frame timing and counters
    std::string scenario;        // --scenario NAME: seed, steps and script from build/trace_scenarios.txt
    std::vector<long> shotSteps; // --shots 100,200: screenshot after these steps as <prefix>_t<step>.png
    std::string shotPrefix = "shot";
    int renderStats = 0;         // --render-stats N: hidden runs render every Nth step of live play for counters (6.5)
    bool batch = true;           // --no-batch: draw grass row obstacles one by one (A/B check of 6.5b)
    bool realtime = false;       // --realtime: a hidden run keeps real-time pacing and renders every frame (fps checks)
    std::string character;       // --character ID: play as this character (chicken, bacon, brent, ...)
    int level = 0;               // --level N: start in Progression level N (0 = Classic), for tests and screenshots
    // --depth-bits N: force the depth buffer the context asks for. The R36S gets 16 bits and its shadows flickered
    // there; a PC hands out 24 and hides the problem, so this reproduces the device's condition (O19).
    int depthBits = 0;
};

struct ScriptOp {
    enum Kind { Wait, Start, Hop, A, Shot, Button } kind;
    int value = 0;
    Swipe dir = Swipe::Up;
    int player = 0; // O23: "2u" hops the SECOND player; plain "u" is the first, as it always was
    std::string name;
};

std::vector<ScriptOp> parseScript(const std::string &text)
{
    std::vector<ScriptOp> ops;
    std::istringstream in(text);
    std::string tok;
    while (in >> tok) {
        ScriptOp op{ScriptOp::Wait};
        if (tok[0] == 'w') {
            op.kind = ScriptOp::Wait;
            op.value = std::atoi(tok.c_str() + 1);
        } else if (tok == "s") {
            op.kind = ScriptOp::Start;
        } else if (tok == "a") {
            op.kind = ScriptOp::A;
        } else if (tok == "u" || tok == "d" || tok == "l" || tok == "r") {
            op.kind = ScriptOp::Hop;
            op.dir = tok == "u" ? Swipe::Up : tok == "d" ? Swipe::Down : tok == "l" ? Swipe::Left : Swipe::Right;
        } else if (tok.size() == 2 && (tok[0] == '1' || tok[0] == '2') &&
                   (tok[1] == 'u' || tok[1] == 'd' || tok[1] == 'l' || tok[1] == 'r')) {
            // O23: "1u" / "2l" - a hop by a named player, for two-player screenshots
            op.kind = ScriptOp::Hop;
            op.player = tok[0] - '1';
            op.dir = tok[1] == 'u' ? Swipe::Up : tok[1] == 'd' ? Swipe::Down : tok[1] == 'l' ? Swipe::Left
                                                                                            : Swipe::Right;
        } else if (tok.compare(0, 4, "btn:") == 0) {
            // a pad button through the real input path: held for one step (the next one), then released
            static const struct { const char *name; Action act; } buttons[] = {
                {"up", ActUp},   {"down", ActDown}, {"left", ActLeft},   {"right", ActRight}, {"a", ActA},
                {"b", ActB},     {"start", ActStart}, {"select", ActSelect}, {"l", ActL},       {"r", ActR}};
            op.kind = ScriptOp::Button;
            for (const auto &b : buttons)
                if (tok.substr(4) == b.name) op.value = b.act;
            if (!op.value) {
                logf("auto: unknown button %s", tok.c_str());
                continue;
            }
        } else if (tok.compare(0, 5, "shot:") == 0) {
            op.kind = ScriptOp::Shot;
            op.name = tok.substr(5);
        } else {
            logf("auto: unknown token %s", tok.c_str());
            continue;
        }
        ops.push_back(op);
    }
    return ops;
}

bool saveShot(Renderer &renderer, int w, int h, const std::string &path)
{
    std::vector<uint8_t> rgba;
    renderer.readPixels(w, h, rgba);
    bool ok = png::writeRGBA(path, w, h, rgba.data());
    logf("shot %s %s", path.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--seed") opt.seed = uint32_t(std::strtoul(next().c_str(), nullptr, 10));
        else if (a == "--hidden") opt.hidden = true;
        else if (a == "--headless") opt.headless = true;
        else if (a == "--fast") opt.fast = true;
        else if (a == "--fullscreen") opt.fullscreen = true;
        else if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--auto") opt.autoScript = next();
        else if (a == "--shot-dir") opt.shotDir = next();
        else if (a == "--view-scale") opt.viewScale = float(std::atof(next().c_str())), opt.viewCli = true;
        else if (a == "--view-shift") opt.viewShift = float(std::atof(next().c_str())), opt.viewCli = true;
        else if (a == "--size") std::sscanf(next().c_str(), "%dx%d", &opt.width, &opt.height);
        else if (a == "--no-idle") opt.noIdle = true;
        else if (a == "--no-shadows") opt.shadows = ShadowMode::Off, opt.shadowsCli = true;
        else if (a == "--shadows") {
            std::string m = next();
            opt.shadows = m == "off" ? ShadowMode::Off : m == "simple" ? ShadowMode::Simple : ShadowMode::Full;
            opt.shadowsCli = true;
        }
        else if (a == "--conf") opt.confPath = next();
        else if (a == "--smoke") opt.smoke = std::atol(next().c_str());
        else if (a == "--level") opt.level = std::atoi(next().c_str());
        else if (a == "--record") opt.record = next();
        else if (a == "--replay") opt.replay = next();
        else if (a == "--shot-prefix") opt.shotPrefix = next();
        else if (a == "--framing-stats") opt.framingStats = true;
        else if (a == "--no-hud") opt.hud = false;
        else if (a == "--overlay") opt.overlay = true;
        else if (a == "--scenario") opt.scenario = next();
        else if (a == "--render-stats") opt.renderStats = std::atoi(next().c_str());
        else if (a == "--no-batch") opt.batch = false;
        else if (a == "--depth-bits") opt.depthBits = std::atoi(next().c_str());
        else if (a == "--realtime") opt.realtime = true;
        else if (a == "--character") opt.character = next();
        else if (a == "--shots") {
            std::istringstream list(next());
            std::string item;
            while (std::getline(list, item, ','))
                if (!item.empty()) opt.shotSteps.push_back(std::atol(item.c_str()));
        }
    }
#if defined(__linux__) && defined(__aarch64__)
    if (!opt.hidden && !opt.headless) opt.fullscreen = true;
#endif
    if ((opt.hidden && !opt.realtime) || opt.headless) opt.fast = true;
    if (!opt.scenario.empty()) {
        // build/trace_scenarios.txt: name|seed|steps|script (repo checkout, or next to out/pc/)
        const std::string candidates[] = {"build/trace_scenarios.txt", baseDir() + "../../build/trace_scenarios.txt"};
        bool found = false;
        for (const std::string &path : candidates) {
            FILE *f = std::fopen(path.c_str(), "rb");
            if (!f) continue;
            char buf[4096];
            while (!found && std::fgets(buf, sizeof buf, f)) {
                std::string line = buf;
                while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
                if (line.empty() || line[0] == '#') continue;
                size_t a = line.find('|');
                size_t b = a == std::string::npos ? a : line.find('|', a + 1);
                size_t c = b == std::string::npos ? b : line.find('|', b + 1);
                if (c == std::string::npos || line.substr(0, a) != opt.scenario) continue;
                opt.seed = uint32_t(std::strtoul(line.substr(a + 1, b - a - 1).c_str(), nullptr, 10));
                if (!opt.frames) opt.frames = std::atol(line.substr(b + 1, c - b - 1).c_str());
                opt.autoScript = line.substr(c + 1);
                found = true;
            }
            std::fclose(f);
            if (found) break;
        }
        if (!found) {
            std::fprintf(stderr, "unknown scenario %s (build/trace_scenarios.txt)\n", opt.scenario.c_str());
            return 9;
        }
    }
    if (opt.smoke) {
        opt.fast = true;
        opt.frames = opt.smoke;
    }

    logOpen(baseDir() + "bobrhopper.log");
    logf("BobrHopper start: data=%s", dataDir().c_str());

    Manifest manifest;
    if (!loadManifest(dataDir() + "manifest.txt", manifest)) {
        logf("FATAL no data (manifest.txt) in %s", dataDir().c_str());
        return 2;
    }
    ModelLibrary models;
    if (!models.load(manifest, dataDir())) return 3;

    PlatformConfig pc;
    pc.width = opt.width;
    pc.height = opt.height;
    pc.hidden = opt.hidden;
    pc.headless = opt.headless;
    pc.fullscreen = opt.fullscreen;
    pc.depthBits = opt.depthBits;
    Platform platform;
    if (!platform.init(pc)) return 4;

    Renderer renderer;
    SceneRenderer sceneRenderer;
    TextRenderer text;
    Screens screens;
    RenderTarget target;
    int viewW = platform.width(), viewH = platform.height();
    if (!opt.headless) {
        renderer.hasStencil = platform.stencilBits() >= 8; // O19: without one the shadow pass must not mask itself
        if (!renderer.init() || !sceneRenderer.init(renderer, models, manifest, dataDir())) return 5;
        if (!text.load(renderer, dataDir())) return 5;
        if (!screens.load(renderer, dataDir())) return 5;
        if (opt.hidden && !renderer.createTarget(target, viewW, viewH)) return 6;
    }

    Audio audio;
    audio.init(!opt.hidden && !opt.headless);
    audio.loadBank(manifest, dataDir());
    screens.playSound = [&audio](const std::string &s) { audio.play(s); };

    Input input;
    input.init();
    if (!opt.record.empty()) input.startRecording(opt.record);
    if (!opt.replay.empty() && !input.loadReplay(opt.replay)) {
        logf("FATAL cannot read replay %s", opt.replay.c_str());
        return 7;
    }

    if (opt.seed == 0) opt.seed = uint32_t(SDL_GetPerformanceCounter() & 0x7fffffff) | 1u;
    logf("seed %u", opt.seed);
    // GameProvider: highscore rehydrated from storage, cached again whenever it changes
    bool saveConf = !opt.confPath.empty() || (!opt.hidden && !opt.headless);
    std::string confPath = opt.confPath.empty() ? baseDir() + "conf/crossy.cfg" : opt.confPath;
    Config conf;
    conf.load(confPath);

    // settings screen values: conf/crossy.cfg, applied live, command-line --shadows / --view-* win
    UserSettings userSettings;
    userSettings.volume = std::max(0, std::min(10, conf.getInt("volume", 10)));
    userSettings.shadows = std::max(0, std::min(2, conf.getInt("shadows", 0)));
    userSettings.fpsCounter = conf.getInt("fps_counter", 0) != 0;
    userSettings.framing = std::max(0, std::min(1, conf.getInt("framing", 0)));
    userSettings.language = std::max(0, std::min(1, conf.getInt("language", 0)));
    userSettings.music = std::max(0, std::min(100, conf.getInt("music_volume", 22)));
    // O23: how many play, and the device each of them uses (the names are kControlNames below)
    userSettings.players = std::max(1, std::min(2, conf.getInt("players", 1)));
    userSettings.control[0] = std::max(0, std::min(3, conf.getInt("control_p1", 0)));
    userSettings.control[1] = std::max(0, std::min(3, conf.getInt("control_p2", 1)));
    {
        const std::string id = opt.character.empty() ? conf.get("character", "beaver") : opt.character;
        for (int i = 0; i < kCharacterCount; i++)
            if (id == kCharacters[i].id) userSettings.character = i;
    }
    const bool overlayCli = opt.overlay;
    auto applySettings = [&]() {
        lang::set(userSettings.language); // O11.5: every screen reads the language from here
        audio.setMasterVolume(float(userSettings.volume) / 10.0f);
        audio.setMusicVolume(float(userSettings.music) / 100.0f);
        if (!opt.shadowsCli)
            opt.shadows = userSettings.shadows == 1 ? ShadowMode::Simple
                          : userSettings.shadows == 2 ? ShadowMode::Off : ShadowMode::Full;
        opt.overlay = overlayCli || userSettings.fpsCounter;
        if (!opt.viewCli) {
            // K.1: normal = the chosen framing, wide = the alternative (whole lane visible more often)
            // O23: two players need the wider view to both stay in frame, so it is forced there
            const bool wide = userSettings.framing != 0 || userSettings.players > 1;
            opt.viewScale = wide ? 3.5f : float(settings::defaultViewScale);
            opt.viewShift = wide ? 0.0f : float(settings::defaultViewShift);
        }
    };
    auto saveSettings = [&]() {
        conf.setInt("volume", userSettings.volume);
        conf.setInt("shadows", userSettings.shadows);
        conf.setInt("fps_counter", userSettings.fpsCounter ? 1 : 0);
        conf.setInt("framing", userSettings.framing);
        conf.setInt("language", userSettings.language);
        conf.setInt("music_volume", userSettings.music);
        conf.set("character", kCharacters[userSettings.character].id);
        conf.setInt("players", userSettings.players);
        conf.setInt("control_p1", userSettings.control[0]);
        conf.setInt("control_p2", userSettings.control[1]);
        if (saveConf && !conf.save(confPath)) logf("cannot save %s", confPath.c_str());
    };
    applySettings();
    screens.settings = &userSettings;
    // O23: the devices this build offers, in the order the settings screen steps through them. They are Input's
    // devices 1..3 (playerDevice adds the one), and a single player still reads all of them at once.
    static const char *const kControlNames[] = {"ARROWS", "WSAD", "PAD 1", "PAD 2"};
    screens.controlNames = kControlNames;
    screens.controlCount = 4;
    // Defaults that suit the machine the game is actually on. A console with two pads should hand one to each
    // player without anybody visiting this screen first; a PC with no pad splits the keyboard instead.
    if (conf.getInt("control_p1", -1) < 0) { // nothing saved yet: pick from the machine
        const int pads = input.padCount();
        userSettings.control[0] = pads >= 1 ? 2 : 0;          // PAD 1, else the arrows
        userSettings.control[1] = pads >= 2 ? 3 : (pads >= 1 ? 0 : 1); // PAD 2, else the arrows, else WSAD
        logf("input: %d pad(s) - player one on %s, player two on %s", pads, kControlNames[userSettings.control[0]],
             kControlNames[userSettings.control[1]]);
    }
    // O11.4: the Progression level the career screen offers to continue with
    int careerLevel = std::max(1, conf.getInt("career_level", 1));
    bool careerDirty = false;
    int pendingLevel = 0; // O11.9: the level to start once the restart fade has built its new scene
    screens.careerLevel = careerLevel;


    Game game(models, opt.seed);
    game.setHighscore(conf.getInt("highscore", 0));
    // O23: before the first scene, so --auto scripts and --level runs get the two-player map straight away
    game.setPlayerCount(userSettings.players);
    game.setupGame(kCharacters[userSettings.character].id);
    game.init();
    // O11.3: --level N plays a Progression level straight away (the menu does the same with setLevel)
    if (opt.level > 0) game.setLevel(opt.level);
    // GameEngine.unpause() renders -- and so ticks the engine -- once when the GL context appears, before the
    // animation-frame loop (tools/webref/trace.mjs reports it as pre_ticks 1)
    game.tickEngineOnly();
    game.takeSounds();

    std::vector<ScriptOp> script = parseScript(opt.autoScript);
    size_t scriptPos = 0;
    int waitLeft = 0;
    bool pendingRelease = false;
    int pendingPlayer = 0; // O23
    Swipe pendingDir = Swipe::Up;
    uint16_t scriptMask = 0; // btn: tokens
    int scriptMaskSteps = 0;
    bool selectCombo = false; // Select held together with Start/L: its release is not a tap
    bool settingsDirty = false; // changed in the settings menu, written when it closes
    std::vector<std::string> pendingShots;

    // determinism digest over every step (FNV-1a) and per-game resource counts for the leak check
    SmokeBot bot(opt.seed);
    uint64_t digest = 1469598103934665603ull;
    auto mix = [&](uint64_t v) {
        for (int i = 0; i < 8; i++) {
            digest ^= (v >> (i * 8)) & 0xff;
            digest *= 1099511628211ull;
        }
    };
    auto mixd = [&](double d) {
        uint64_t v;
        std::memcpy(&v, &d, sizeof v);
        mix(v);
    };
    int games = 0, bestScore = 0;
    std::vector<size_t> poolAtStart;
    std::vector<int> animsAtStart;
    GameState prevState = game.state();

    // --framing-stats: per step of live play, measured from the camera math alone (no drawing)
    std::vector<double> heroYs;
    std::vector<int> aheads, behinds;
    long laneHeroSteps = 0;

    // --render-stats: scene counters of sampled frames of live play
    std::vector<int> statDraws, statTris, statCasters;
    double statKinds[SceneRenderer::KindCount] = {};

    // music: the title song on the home and game over screens, the next game track for every game (only with a
    // real audio device, so hidden/headless test runs do not decode anything)
    int gameTrack = -1;
    int musicState = -1;
    auto updateMusic = [&]() {
        if (!audio.deviceOpen() || manifest.music.empty() || int(game.state()) == musicState) return;
        musicState = int(game.state());
        std::string name = manifest.music[0];
        if (game.state() == GameState::Playing && manifest.music.size() > 1) {
            gameTrack = (gameTrack + 1) % int(manifest.music.size() - 1);
            name = manifest.music[size_t(gameTrack + 1)];
        }
        audio.playMusic(dataDir() + "music/" + name + ".ogg");
    };

    FrameTimer frameTimer;
    bool paused = false, running = true;
    double acc = 0, last = platform.now();
    long stepsDone = 0;

    auto doStep = [&]() {
        uint16_t syntheticNow = 0;
        if (opt.smoke && opt.replay.empty()) {
            syntheticNow = bot.next(game.state());
        } else {
            syntheticNow = scriptMaskSteps > 0 ? scriptMask : 0;
            if (scriptMaskSteps > 0) scriptMaskSteps--;
        }
        input.setSynthetic(syntheticNow);
        // O23: in an automated run the bot or the script IS the input, so it also has to reach the devices the
        // players read - a device is otherwise only itself, and with two players nothing would move. Real keys
        // fill those devices from SDL, and this only runs when there are no real keys to speak of.
        if (opt.smoke || !script.empty())
            for (int p = 0; p < 2; p++) input.setDevice(playerDevice(userSettings, p), syntheticNow);
        input.step();
        // scripted input
        if (pendingRelease) {
            game.moveWithDirection(pendingDir, pendingPlayer);
            pendingRelease = false;
        } else if (scriptPos < script.size()) {
            if (waitLeft > 0) {
                waitLeft--;
            } else {
                const ScriptOp &op = script[scriptPos++];
                switch (op.kind) {
                case ScriptOp::Wait: waitLeft = op.value - 1; break;
                case ScriptOp::Start: game.startPlaying(); break;
                case ScriptOp::Hop:
                    game.beginMoveWithDirection(op.player);
                    pendingRelease = true;
                    pendingDir = op.dir;
                    pendingPlayer = op.player;
                    break;
                case ScriptOp::A:
                    if (game.state() == GameState::GameOver) game.restart();
                    break;
                case ScriptOp::Shot: pendingShots.push_back(op.name); break;
                case ScriptOp::Button:
                    scriptMask = uint16_t(op.value);
                    scriptMaskSteps = 1;
                    break;
                }
            }
        }

        if (input.down(ActSelect) && input.pressed(ActStart)) running = false;
        if (input.down(ActSelect) && input.pressed(ActL)) opt.overlay = !opt.overlay;
        if (input.down(ActSelect) && (input.pressed(ActStart) || input.pressed(ActL))) selectCombo = true;
        const bool selectTap = input.released(ActSelect) && !selectCombo;
        if (input.released(ActSelect)) selectCombo = false;

        MenuResult menu;
        const bool menuInput = screens.handleInput(input, userSettings, menu);
        // O4.1 (as the SF2000 core): applied at once, the hero rebuilt only when the character changed, and the file
        // written once when the settings screen closes (and on exit), not on every press
        if (menu.settingsChanged) {
            applySettings();
            settingsDirty = true;
            if (game.character() != kCharacters[userSettings.character].id)
                game.setCharacter(kCharacters[userSettings.character].id);
        }
        if (settingsDirty && screens.menu() != Menu::Settings) {
            saveSettings();
            settingsDirty = false;
        }
        if (menu.quitToHome) game.quitToHome();
        if (menu.exitGame) running = false;
        // O11.2/O11.4: the home screen picked a game (0 = Classic, k = Progression level k); the restart fade must
        // finish first, it has a new scene of its own coming
        if (menu.startLevel >= 0 && !game.restarting()) {
            if (menu.resetCareer) {
                careerLevel = 1;
                screens.careerLevel = careerLevel;
                conf.setInt("career_level", careerLevel);
                if (saveConf && !conf.save(confPath)) logf("cannot save %s", confPath.c_str());
            }
            // O23: one player or two is decided before the scene is built (the starting columns and the rows differ)
            game.setPlayerCount(userSettings.players);
            game.setLevel(menu.startLevel);
            game.startPlaying();
        }
        if (!menuInput) {
            switch (game.state()) {
            case GameState::None:
                if (selectTap) screens.openSettings(false);
                break;
            case GameState::Playing:
                if (input.pressed(ActStart) && !input.down(ActSelect)) {
                    screens.openPause();
                } else {
                    applyPlayerInput(input, userSettings, game); // O23: src/ui/controls.cpp, one or two players
                }
                break;
            case GameState::GameOver:
                // O11.9: in Progression A carries on (the next level when this one was finished, the same one after a
                // death) and B goes back to the menu; in Classic A goes home as before
                if (input.released(ActA)) {
                    if (game.level() > 0) pendingLevel = game.levelDone() ? game.level() + 1 : game.level();
                    game.restart();
                } else if (input.released(ActB) && game.level() > 0) {
                    game.restart();
                } else if (selectTap) {
                    screens.openSettings(false);
                }
                break;
            default:
                break;
            }
        }
        paused = screens.pausesGame();

        if (!paused) {
            game.step();
            game.endFrame();
        }
        screens.update(game);
        updateMusic();
        // O11.4: a finished level unlocks the next one, written once the game over screen is left (the SF2000 core
        // writes its files the same way: not at the moment the game ends)
        if (game.levelDone() && game.level() >= careerLevel) {
            careerLevel = game.level() + 1;
            screens.careerLevel = careerLevel;
            careerDirty = true;
        }
        // O11.9: carry on with the career as soon as the restart fade's new scene is there
        if (pendingLevel > 0 && !game.restarting() && game.state() == GameState::None) {
            game.setPlayerCount(userSettings.players);
            game.setLevel(pendingLevel);
            game.startPlaying();
            pendingLevel = 0;
        }
        if (careerDirty && game.state() != GameState::GameOver) {
            conf.setInt("career_level", careerLevel);
            if (saveConf && !conf.save(confPath)) logf("cannot save %s", confPath.c_str());
            careerDirty = false;
        }
        if (game.highscore() != conf.getInt("highscore", 0)) {
            conf.setInt("highscore", game.highscore());
            if (saveConf && !conf.save(confPath)) logf("cannot save %s", confPath.c_str());
            else if (saveConf) logf("highscore %d saved", game.highscore());
        }
        for (const std::string &s : game.takeSounds()) audio.play(s, soundVolume(s));

        mixd(game.hero().position().x);
        mixd(game.hero().position().y);
        mixd(game.hero().position().z);
        mix(uint64_t(game.score()));
        mix(uint64_t(game.state()));
        mix(uint64_t(game.map().rowCount));
        if (game.state() == GameState::Playing && prevState != GameState::Playing) {
            games++;
            size_t pool = game.context().pool->live();
            int anims = game.gsapEngine().liveAnimations();
            if (opt.smoke) logf("smoke: game %d at step %ld pool=%zu anims=%d", games, stepsDone, pool, anims);
            // games 1-2 warm up (spare row pools fill); after that the counts only vary with the map (foam loops
            // per water row), so a leak shows as a trend: see the halves comparison at the end
            if (games >= 3) {
                poolAtStart.push_back(pool);
                animsAtStart.push_back(anims);
            }
        }
        bestScore = std::max(bestScore, game.score());
        if (game.state() != prevState && !opt.smoke)
            logf("state %d -> %d at step %ld", int(prevState), int(game.state()), stepsDone + 1);
        prevState = game.state();

        if (opt.framingStats && game.state() == GameState::Playing && game.hero().isAlive) {
            updateWorld(game.sceneRoot());
            sceneRenderer.viewShift = opt.viewShift;
            sceneRenderer.setupCamera(game, viewW, viewH, opt.viewScale);
            sceneRenderer.measureFraming(game, viewH);
            const FramingInfo &fr = sceneRenderer.framing;
            heroYs.push_back(fr.heroScreenY);
            aheads.push_back(fr.rowsAhead);
            behinds.push_back(fr.rowsBehind);
            laneHeroSteps += fr.laneAtHero ? 1 : 0;
        }

        stepsDone++;
        for (long s : opt.shotSteps)
            if (s == stepsDone) pendingShots.push_back(opt.shotPrefix + "_t" + std::to_string(s));
        if (opt.frames && stepsDone >= opt.frames) running = false;
        if (input.replayFinished()) running = false;
    };

    while (running) {
        if (!platform.pump()) break;
        input.handleEvents(platform.events());

        if (opt.fast) {
            doStep();
        } else {
            double now = platform.now();
            acc += now - last;
            last = now;
            if (acc > 0.25) acc = 0.25;
            while (acc >= Game::kDt) {
                doStep();
                acc -= Game::kDt;
            }
        }

        if (opt.headless) continue;
        bool wantShot = !pendingShots.empty() || !running;
        const bool statsFrame = opt.renderStats > 0 && stepsDone % opt.renderStats == 0 &&
                                game.state() == GameState::Playing && game.hero().isAlive;
        if (opt.hidden && !opt.realtime && !wantShot && !statsFrame) continue;
        renderer.bindTarget(opt.hidden ? &target : nullptr);
        renderer.viewport(0, 0, viewW, viewH);
        renderer.clear(0x87 / 255.0f, 0xC6 / 255.0f, 0xFF / 255.0f);
        renderer.resetStats();
        if (opt.noIdle && game.state() == GameState::None) game.hero().stopIdle(game.context());
        sceneRenderer.shadowMode = opt.shadows;
        sceneRenderer.viewShift = opt.viewShift;
        sceneRenderer.batchStatic = opt.batch;
        sceneRenderer.render(renderer, game, viewW, viewH, opt.viewScale);
        const RenderStats sceneStats = renderer.stats; // the 3D scene alone, before HUD and overlay
        if (statsFrame) {
            statDraws.push_back(sceneStats.drawCalls);
            statTris.push_back(sceneStats.triangles);
            statCasters.push_back(sceneRenderer.shadowCasters);
            for (int k = 0; k < SceneRenderer::KindCount; k++) statKinds[k] += sceneRenderer.drawsByKind[k];
        }
        if (opt.hud) {
            screens.drawSceneFade(renderer, viewW, viewH);
            drawHud(renderer, text, game, viewW, viewH);
            screens.draw(renderer, text, game, viewW, viewH);
        }
        if (opt.overlay) {
            OverlayCounters oc;
            oc.drawCalls = sceneStats.drawCalls;
            oc.triangles = sceneStats.triangles;
            oc.casters = sceneRenderer.shadowCasters;
            drawDebugOverlay(renderer, text, frameTimer, oc, viewW, viewH);
        }

        for (const std::string &name : pendingShots) {
            saveShot(renderer, viewW, viewH, opt.shotDir + "/" + name + ".png");
            const FramingInfo &fr = sceneRenderer.framing;
            logf("framing %s scale=%.2f shift=%.2f heroY=%.2f heroPx=%d ahead=%d behind=%d laneHero=%d laneTop=%d",
                 name.c_str(), opt.viewScale, opt.viewShift, fr.heroScreenY, fr.heroHeightPx, fr.rowsAhead,
                 fr.rowsBehind, fr.laneAtHero ? 1 : 0, fr.laneAtTop ? 1 : 0);
        }
        pendingShots.clear();
        if (!running && opt.hidden) saveShot(renderer, viewW, viewH, opt.shotDir + "/final.png");
        if (!opt.hidden) platform.swap();
        frameTimer.frame(platform.now());
    }

    logf("end: steps=%ld state=%d score=%d draws=%d tris=%d culled=%d casters=%d nodes=%zu frames=%ld fps=%.1f",
         stepsDone, int(game.state()), game.score(), renderer.stats.drawCalls, renderer.stats.triangles,
         sceneRenderer.culled, sceneRenderer.shadowCasters, game.context().pool->live(), frameTimer.frames(),
         frameTimer.overallFps());
    int rc = 0;
    if (opt.smoke || !opt.replay.empty()) {
        // leak = the later half of the games needs clearly more nodes/animations than the earlier half ever did
        size_t half = poolAtStart.size() / 2, poolA = 0, poolB = 0;
        int animsA = 0, animsB = 0;
        for (size_t i = 0; i < poolAtStart.size(); i++) {
            size_t &p = i < half ? poolA : poolB;
            int &a = i < half ? animsA : animsB;
            p = std::max(p, poolAtStart[i]);
            a = std::max(a, animsAtStart[i]);
        }
        bool leak = half >= 4 && (poolB > poolA + 64 || animsB > animsA + 64);
        logf("smoke: steps=%ld games=%d best=%d pool max %zu -> %zu, anims max %d -> %d (earlier -> later half) %s",
             stepsDone, games, bestScore, poolA, poolB, animsA, animsB, leak ? "LEAK" : half >= 4 ? "ok" : "too few games");
        if (leak) rc = 8;
    }
    if (opt.framingStats && !heroYs.empty()) {
        auto pct = [](std::vector<double> v, double q) {
            std::sort(v.begin(), v.end());
            return v[size_t(q * double(v.size() - 1))];
        };
        auto pcti = [](std::vector<int> v, double q) {
            std::sort(v.begin(), v.end());
            return v[size_t(q * double(v.size() - 1))];
        };
        long off = 0, edge = 0; // feet below the bottom edge / within 5% of it
        for (double y : heroYs) {
            off += y > 1.0 ? 1 : 0;
            edge += y > 0.95 ? 1 : 0;
        }
        const double n = double(heroYs.size());
        logf("framing-stats scale=%.2f shift=%.2f steps=%zu heroY p5=%.2f p50=%.2f p95=%.2f max=%.2f off=%.1f%% "
             "nearEdge=%.1f%% ahead p5=%d p50=%d behind p5=%d p50=%d laneHero=%.0f%%",
             opt.viewScale, opt.viewShift, heroYs.size(), pct(heroYs, 0.05), pct(heroYs, 0.5), pct(heroYs, 0.95),
             pct(heroYs, 1.0), 100.0 * off / n, 100.0 * edge / n, pcti(aheads, 0.05), pcti(aheads, 0.5),
             pcti(behinds, 0.05), pcti(behinds, 0.5), 100.0 * laneHeroSteps / n);
    }
    if (opt.renderStats > 0 && !statDraws.empty()) {
        auto p = [](std::vector<int> v, double q) {
            std::sort(v.begin(), v.end());
            return v[size_t(q * double(v.size() - 1))];
        };
        static const char *const modeNames[] = {"full", "simple", "off"};
        logf("render-stats shadows=%s scale=%.2f shift=%.2f frames=%zu draws p50=%d p95=%d max=%d "
             "tris p50=%d p95=%d max=%d casters p50=%d p95=%d max=%d",
             modeNames[int(opt.shadows)], opt.viewScale, opt.viewShift, statDraws.size(), p(statDraws, 0.5),
             p(statDraws, 0.95), p(statDraws, 1.0), p(statTris, 0.5), p(statTris, 0.95), p(statTris, 1.0),
             p(statCasters, 0.5), p(statCasters, 0.95), p(statCasters, 1.0));
        const double n = double(statDraws.size());
        logf("render-kinds shadows=%s scale=%.2f mean draws: floor=%.1f obstacle=%.1f entity=%.1f hero=%.1f shape=%.1f "
             "shadow=%.1f", modeNames[int(opt.shadows)], opt.viewScale, statKinds[0] / n, statKinds[1] / n,
             statKinds[2] / n, statKinds[3] / n, statKinds[4] / n, statKinds[5] / n);
    }
    logf("digest=%016llx", (unsigned long long)digest);
    if (settingsDirty) saveSettings(); // the settings screen was still open when the game ended
    input.shutdown();
    audio.shutdown();
    platform.shutdown();
    return rc;
}
