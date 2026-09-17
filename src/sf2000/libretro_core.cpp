// SF2000 libretro core. v015 "game": the game itself - 16.16 logic at 60 Hz steps, the software renderer and the shared
// UI - with the diagnostic modes of v002 kept. The ROM file name picks the mode:
//  - start:   the game, reported to the firmware as 30 fps,
//  - start60: the game, reported as 60 fps,
//  - screen:  only a diagnostic screen - the retro_run rate the console gives a trivial core,
//  - bench:   the logic at full speed (up to 200 steps or 20 ms per call) to 20000 steps with the smoke bot; the
//             determinism digest must equal the PC host's (build/host_smoke.sh).
// Start-up stages go to the screen (when something fails), xlog and stage.txt next to the ROM; the game writes
// game_<mode>.txt (frames, logic steps, time per stage) every ~10 s, the bench bench_<mode>.txt.
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "boot_font.h"
#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/config.h"
#include "engine/input.h"
#include "engine/log.h"
#include "engine/renderer.h"
#include "engine/text.h"
#include "game/game.h"
#include "game/models.h"
#include "game/scene.h"
#include "game/scene_render.h"
#include "game/smoke_bot.h"
#include "game/sound_volume.h"
#include "libretro.h"
#include "sf2000/render_bench.h"
#include "sf2000/sf2000_fw.h"
#include "ui/hud.h"
#include "ui/lang.h"
#include "ui/screens.h"

namespace {

const char *const kCoreVersion = "v028";
const int kWidth = 320;
const int kHeight = 240;
const int kSampleRate = 22050;

// Framing (C2.5): the orthographic camera shows zoom / (2 * scale) pixels per unit whatever the width, so view scale 6
// on 320 px shows exactly the R36S picture at 640 px with scale 3 (K.1: the lane and 8 rows ahead) - the same NDC
// framing, only smaller. VIEW NORMAL: 6 / -0.15, WIDE: 7 / 0 (R36S 3.5 / 0).
const cr::mreal kViewScaleNormal = 6.0, kViewShiftNormal = -0.15;
const cr::mreal kViewScaleWide = 7.0, kViewShiftWide = 0.0;
// the screens and the HUD keep their 640x480 layouts in logical pixels; the renderer maps them onto 320x240 and the
// text comes from half-size fonts (TextRenderer::glyphScale)
const int kUiWidth = 640, kUiHeight = 480;
// logic steps one retro_run may catch up; a slower frame than that makes the game slow down instead of stutter on
const int kMaxCatchUp = 6; // logic steps one call may catch up: frames up to 100 ms keep the game at full speed (O3.6)

// the bot session of build/smoke_test.sh (seed 9, 20000 steps); the reference digest is the PC host's run of this
// very core (16.16 on the SF2000; the double build of bobrhopper --smoke gives e5287627110f838e)
const uint32_t kBenchSeed = 9;
const long kBenchSteps = 20000;
#ifdef CR_FIXED
const uint64_t kExpectedDigest = 0x8eefa860b4ab642dull;
#else
const uint64_t kExpectedDigest = 0xe5287627110f838eull;
#endif
const int kMaxStepsPerRun = 200;
const uint32_t kStepBudgetMs = 20;

enum class Mode { Screen, Game30, Game60, Bench, RenderBench };
Mode g_mode = Mode::Game30;
int g_fps = 30;

const char *modeName(Mode m)
{
    switch (m) {
    case Mode::Screen: return "screen";
    case Mode::Game30: return "game30";
    case Mode::Game60: return "game60";
    case Mode::Bench: return "bench";
    case Mode::RenderBench: return "renderbench";
    }
    return "?";
}

bool isGame() { return g_mode == Mode::Game30 || g_mode == Mode::Game60; }

retro_environment_t environ_cb;
retro_video_refresh_t video_cb;
retro_audio_sample_batch_t audio_batch_cb;
retro_input_poll_t input_poll_cb;
retro_input_state_t input_state_cb;

uint16_t framebuffer[kWidth * kHeight];
int16_t silence[2 * (kSampleRate / 30)];
// The game sends as many samples as real time has passed, but never more than 2048 in one audio_batch_cb: v004 allowed
// 5512 (250 ms), and after its first frame (448 ms in video_cb) it sent that many and the console froze on the frame
// (docs/device/sf2000_2026-09-15_1044). 2048 is the largest batch the user's Santa game hands this firmware
// (MAX_AUDIO_BUFFER); longer gaps (loading, the first frame) lose the rest of their sound.
const int kMaxAudioFrames = 2048;
int16_t audioMono[kMaxAudioFrames + 1];
int16_t audioStereo[2 * (kMaxAudioFrames + 1)];

// a global with a constructor: 12345 on screen means __libc_init_array ran
struct CtorProbe {
    int value;
    CtorProbe() : value(12345) {}
};
CtorProbe g_ctorProbe;

struct Bench {
    std::unique_ptr<cr::ModelLibrary> models;
    std::unique_ptr<cr::Game> game;
    std::unique_ptr<cr::Input> input;
    std::unique_ptr<cr::SmokeBot> bot;
    uint64_t digest = 1469598103934665603ull;
    long steps = 0;
    uint32_t logicMs = 0;
    long worldCalls = 0;
    uint32_t worldMs = 0;
    int games = 0, best = 0;
    cr::GameState prevState = cr::GameState::None;
    bool done = false, reported = false;
    uint32_t lastReport = 0;

    void mix(uint64_t v)
    {
        for (int i = 0; i < 8; i++) {
            digest ^= (v >> (i * 8)) & 0xff;
            digest *= 1099511628211ull;
        }
    }
    // bobrhopper.cpp mixes the 8 bytes of each double; the 16.16 build mixes the raw value (its own digest)
    void mixd(cr::real d)
    {
#ifdef CR_FIXED
        mix(uint64_t(uint32_t(d.v)));
#else
        uint64_t v;
        std::memcpy(&v, &d, sizeof v);
        mix(v);
#endif
    }

    // one logic step of bobrhopper.cpp's doStep for bot input (the bot never uses Start/Select, and no menu is open)
    void step()
    {
        using namespace cr;
        Game &g = *game;
        input->setSynthetic(bot->next(g.state()));
        input->step();
        static const struct {
            Action act;
            Swipe dir;
        } dirs[] = {{ActUp, Swipe::Up}, {ActDown, Swipe::Down}, {ActLeft, Swipe::Left}, {ActRight, Swipe::Right},
                    {ActA, Swipe::Up}};
        switch (g.state()) {
        case GameState::None:
            if (input->released(ActUp) || input->released(ActA)) g.startPlaying();
            break;
        case GameState::Playing:
            for (const auto &d : dirs) {
                if (input->pressed(d.act)) g.beginMoveWithDirection();
                if (input->released(d.act)) g.moveWithDirection(d.dir);
            }
            break;
        case GameState::GameOver:
            if (input->released(ActA)) g.restart();
            break;
        default:
            break;
        }
        g.step();
        g.endFrame();
        g.takeSounds();
        mixd(g.hero().position().x);
        mixd(g.hero().position().y);
        mixd(g.hero().position().z);
        mix(uint64_t(g.score()));
        mix(uint64_t(g.state()));
        mix(uint64_t(g.map().rowCount));
        if (g.state() == GameState::Playing && prevState != GameState::Playing) games++;
        if (g.score() > best) best = g.score();
        prevState = g.state();
        steps++;
    }
};

// Config::syncFile: the firmware keeps written files in a cache until fs_sync
void syncToCard(const char *path) { fs_sync(path); }

int clampInt(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// The game as bobrhopper.cpp runs it.
struct GameApp {
    // settings and the best score: <rom folder>/conf/crossy.cfg, or crossy.cfg next to the ROM when the card has no
    // conf folder the firmware can write into
    cr::Config conf;
    std::string confPath;
    std::unique_ptr<cr::ModelLibrary> models;
    std::unique_ptr<cr::Game> game;
    cr::Renderer renderer;
    cr::SceneRenderer scene;
    cr::TextRenderer text;
    cr::Screens screens;
    cr::Input input;
    cr::UserSettings settings;
    cr::Audio audio;
    std::string dataDir;
    std::vector<std::string> music; // manifest order: the title song, then one track per game
    int gameTrack = -1, musicState = -1;
    uint32_t audioMs = 0;
    cr::mreal viewScale = kViewScaleNormal;
    bool selectCombo = false;
    bool settingsDirty = false; // changed in the settings menu, written when it closes
    bool started = false;
    uint32_t lastTick = 0;
    uint32_t acc = 0; // elapsed milliseconds * 60: 1000 is one logic step
    uint32_t audioAcc = 0; // elapsed milliseconds * kSampleRate: 1000 is one audio frame
    long steps = 0, frames = 0, droppedSteps = 0;
    uint32_t logicMs = 0, renderMs = 0, lastReport = 0;
    // O3.7 profile per stage, summed over every frame: the firmware clock has 1 ms steps, so single readings are
    // mostly 0 or 1, but their mean over thousands of frames is the stage's time
    uint32_t clearMs = 0, sceneMs = 0, uiMs = 0, videoMs = 0;
    int64_t usTransform = 0, usRaster = 0, usShadows = 0, usOverlay = 0;
    int64_t trianglesDrawn = 0, pixels = 0;
    // O5.5: the scene's parts (SceneRenderer::Profile), the firmware's time between two calls and inside audio_batch_cb
    int64_t sceneUpdateWorld = 0, sceneTraverse = 0, sceneSort = 0, sceneFloors = 0, sceneShadows = 0, sceneObjects = 0,
            sceneFraming = 0;
    uint32_t outsideMs = 0, audioCallMs = 0, lastEnd = 0;
    int loggedState = -1;

    static uint64_t tickMicros() { return uint64_t(os_get_tick_count()) * 1000u; }

    bool load(const cr::Manifest &manifest, const std::string &dataDir, uint32_t seed, std::string &error)
    {
        using namespace cr;
        models.reset(new ModelLibrary());
        if (!models->load(manifest, dataDir)) return error = "models", false;
        renderer.init(kWidth, kHeight);
        if (!scene.init(renderer, *models, manifest, dataDir)) return error = "meshes", false;
        text.glyphScale = kUiWidth / kWidth;
        if (!text.load(renderer, dataDir)) return error = "fonts", false;
        if (!screens.load(renderer, dataDir)) return error = "images", false;
        screens.settings = &settings;
        screens.versionLabel = kCoreVersion;
        this->dataDir = dataDir;
        music = manifest.music;
        // every track into memory now (8.8 MB of the 64 MB heap): opening a track on the card when the game started or
        // ended froze the game for a moment (user report after v015)
        for (const std::string &name : music)
            if (!audio.preloadMusic(dataDir + "music/" + name + ".wav")) xlog("bobrhopper: music %s not preloaded\n", name.c_str());
        Config::syncFile = syncToCard;
        confPath = baseDir() + "conf/crossy.cfg";
        if (!conf.load(confPath)) {
            Config beside;
            if (beside.load(baseDir() + "crossy.cfg")) {
                conf = beside;
                confPath = baseDir() + "crossy.cfg";
            }
        }
        settings.volume = clampInt(conf.getInt("volume", 10), 0, 10);
        settings.shadows = clampInt(conf.getInt("shadows", 0), 0, 2);
        settings.fpsCounter = conf.getInt("fps_counter", 0) != 0;
        settings.framing = clampInt(conf.getInt("framing", 0), 0, 1);
        settings.language = clampInt(conf.getInt("language", 0), 0, 1);
        settings.music = clampInt(conf.getInt("music_volume", 22), 0, 100);
        const std::string character = conf.get("character", "beaver");
        for (int i = 0; i < kCharacterCount; i++)
            if (character == kCharacters[i].id) settings.character = i;
        careerLevel = conf.getInt("career_level", 1) < 1 ? 1 : conf.getInt("career_level", 1);
        screens.careerLevel = careerLevel;
        xlog("bobrhopper: settings loaded volume=%d music=%d shadows=%d character=%s best=%d from %s\n", settings.volume,
             settings.music, settings.shadows, kCharacters[settings.character].id, conf.getInt("highscore", 0),
             confPath.c_str());
        audio.init(false);
        if (!audio.loadBank(manifest, dataDir)) xlog("bobrhopper: some sounds are missing (audio.log lines)\n");
        screens.playSound = [this](const std::string &s) { audio.play(s); };
        applySettings();
        game.reset(new Game(*models, seed));
        game->setHighscore(conf.getInt("highscore", 0));
        game->setupGame(kCharacters[settings.character].id);
        game->init();
        // GameEngine.unpause() renders -- and so ticks the engine -- once before the frame loop (bobrhopper.cpp)
        game->tickEngineOnly();
        game->takeSounds();
        return true;
    }

    // O11.4: the Progression level the career screen offers to continue with, saved in the config
    int careerLevel = 1;
    bool careerDirty = false;
    int pendingLevel = 0; // O11.9: the level to start once the restart fade has built its new scene

    void applySettings()
    {
        cr::lang::set(settings.language); // O11.5: every screen reads the language from here
        scene.shadowMode = settings.shadows == 1   ? cr::ShadowMode::Simple
                           : settings.shadows == 2 ? cr::ShadowMode::Off
                                                   : cr::ShadowMode::Full;
        viewScale = settings.framing ? kViewScaleWide : kViewScaleNormal;
        scene.viewShift = settings.framing ? kViewShiftWide : kViewShiftNormal;
        audio.setMasterVolume(cr::mreal(settings.volume) / cr::mreal(10));
        audio.setMusicVolume(cr::mreal(settings.music) / cr::mreal(100));
    }

    void saveConf()
    {
        if (conf.save(confPath)) return;
        const std::string beside = cr::baseDir() + "crossy.cfg";
        if (confPath != beside && conf.save(beside)) {
            xlog("bobrhopper: cannot write %s, settings now in %s\n", confPath.c_str(), beside.c_str());
            confPath = beside;
            return;
        }
        xlog("bobrhopper: cannot save settings to %s\n", confPath.c_str());
    }

    void saveSettings()
    {
        conf.setInt("volume", settings.volume);
        conf.setInt("shadows", settings.shadows);
        conf.setInt("fps_counter", settings.fpsCounter ? 1 : 0);
        conf.setInt("framing", settings.framing);
        conf.setInt("language", settings.language);
        conf.setInt("music_volume", settings.music);
        conf.set("character", cr::kCharacters[settings.character].id);
        if (game) conf.setInt("highscore", std::max(game->highscore(), conf.getInt("highscore", 0)));
        saveConf();
    }

    // bobrhopper.cpp's updateMusic: the title song on the home and game over screens, the next game track for every game
    void updateMusic()
    {
        if (music.empty() || int(game->state()) == musicState) return;
        musicState = int(game->state());
        std::string name = music[0];
        if (game->state() == cr::GameState::Playing && music.size() > 1) {
            gameTrack = (gameTrack + 1) % int(music.size() - 1);
            name = music[size_t(gameTrack + 1)];
        }
        if (!audio.playMusic(dataDir + "music/" + name + ".wav")) xlog("bobrhopper: no music %s\n", name.c_str());
    }

    // bobrhopper.cpp's doStep for pad input
    void step(uint16_t mask)
    {
        using namespace cr;
        Game &g = *game;
        input.setSynthetic(mask);
        input.step();
        static const struct {
            Action act;
            Swipe dir;
        } dirs[] = {{ActUp, Swipe::Up}, {ActDown, Swipe::Down}, {ActLeft, Swipe::Left}, {ActRight, Swipe::Right},
                    {ActA, Swipe::Up}};
        if (input.down(ActSelect) && (input.pressed(ActStart) || input.pressed(ActL))) selectCombo = true;
        const bool selectTap = input.released(ActSelect) && !selectCombo;
        if (input.released(ActSelect)) selectCombo = false;
        if (input.down(ActSelect) && input.pressed(ActL)) settings.fpsCounter = !settings.fpsCounter;

        MenuResult menu;
        const bool menuInput = screens.handleInput(input, settings, menu);
        // v006: every press in the settings menu wrote the settings to the card (two files, fs_sync) and rebuilt the
        // hero - the game froze on every volume step (user report); now applied at once, the character rebuilt only
        // when it changed, and the file written once when the settings screen closes
        if (menu.settingsChanged) {
            applySettings();
            settingsDirty = true;
            if (g.character() != kCharacters[settings.character].id) g.setCharacter(kCharacters[settings.character].id);
        }
        if (settingsDirty && screens.menu() != Menu::Settings) {
            saveSettings();
            settingsDirty = false;
        }
        if (menu.quitToHome) g.quitToHome();
        // O11.2/O11.4: the home screen picked a game (0 = Classic, k = Progression level k); the restart fade must
        // finish first, it has a new scene of its own coming
        if (menu.startLevel >= 0 && !g.restarting()) {
            if (menu.resetCareer) {
                careerLevel = 1;
                screens.careerLevel = careerLevel;
                conf.setInt("career_level", careerLevel);
                saveConf();
            }
            g.setLevel(menu.startLevel);
            g.startPlaying();
        }
        // menu.exitGame: the firmware's own menu leaves the core
        if (!menuInput) {
            switch (g.state()) {
            case GameState::None:
                if (selectTap) screens.openSettings(false);
                break;
            case GameState::Playing:
                if (input.pressed(ActStart) && !input.down(ActSelect)) {
                    screens.openPause();
                } else {
                    for (const auto &d : dirs) {
                        if (input.pressed(d.act)) g.beginMoveWithDirection();
                        if (input.released(d.act)) g.moveWithDirection(d.dir);
                    }
                }
                break;
            case GameState::GameOver:
                // O11.9: in Progression A carries on (the next level when this one was finished, the same one after a
                // death) and B goes back to the menu; in Classic A goes home as before
                if (input.released(ActA)) {
                    if (g.level() > 0) pendingLevel = g.levelDone() ? g.level() + 1 : g.level();
                    g.restart();
                } else if (input.released(ActB) && g.level() > 0) {
                    g.restart();
                } else if (selectTap) {
                    screens.openSettings(false);
                }
                break;
            default:
                break;
            }
        }
        if (!screens.pausesGame()) {
            g.step();
            g.endFrame();
        }
        screens.update(g);
        updateMusic();
        // O11.4: a finished level unlocks the next one; written when the game over screen is left, never at the
        // moment the game ends (writing two files and fs_sync then froze the game)
        if (g.levelDone() && g.level() >= careerLevel) {
            careerLevel = g.level() + 1;
            screens.careerLevel = careerLevel;
            careerDirty = true;
        }
        // O11.9: carry on with the career as soon as the restart fade's new scene is there
        if (pendingLevel > 0 && !g.restarting() && g.state() == GameState::None) {
            g.setLevel(pendingLevel);
            g.startPlaying();
            pendingLevel = 0;
        }
        if (careerDirty && g.state() != GameState::GameOver) {
            conf.setInt("career_level", careerLevel);
            saveConf();
            careerDirty = false;
        }
        // the best score is written once the game-over screen is left (restart, home) and when the core closes
        // (saveSettings): writing two files and fs_sync at the moment of death froze the game (user report after v015)
        if (g.state() != GameState::GameOver && g.highscore() != conf.getInt("highscore", 0)) {
            conf.setInt("highscore", g.highscore());
            saveConf();
            xlog("bobrhopper: best score %d saved\n", g.highscore());
        }
        for (const std::string &s : g.takeSounds()) audio.play(s, cr::soundVolume(s));
        steps++;
        if (int(g.state()) != loggedState) {
            loggedState = int(g.state());
            xlog("bobrhopper: state %d at step %ld score %d\n", loggedState, steps, g.score());
        }
    }

    void render()
    {
        static const cr::mreal skyR = 0x87 / 255.0f, skyG = 0xC6 / 255.0f, skyB = 0xFF / 255.0f;
        renderer.profileClock = tickMicros;
        scene.profileClock = tickMicros;
        game->profileClock = tickMicros;
        const uint32_t t0 = os_get_tick_count();
        renderer.bindTarget(nullptr);
        renderer.clear(skyR, skyG, skyB);
        renderer.resetStats();
        const uint32_t t1 = os_get_tick_count();
        scene.render(renderer, *game, kWidth, kHeight, viewScale);
        const uint32_t t2 = os_get_tick_count();
        screens.drawSceneFade(renderer, kUiWidth, kUiHeight);
        drawHud(renderer, text, *game, kUiWidth, kUiHeight);
        screens.draw(renderer, text, *game, kUiWidth, kUiHeight);
        const uint32_t t3 = os_get_tick_count();
        clearMs += t1 - t0;
        sceneMs += t2 - t1;
        uiMs += t3 - t2;
        usTransform += renderer.stats.usTransform;
        usRaster += renderer.stats.usRaster;
        usShadows += renderer.stats.usShadows;
        usOverlay += renderer.stats.usOverlay;
        trianglesDrawn += renderer.stats.trianglesDrawn;
        pixels += renderer.stats.pixels;
        const cr::SceneRenderer::Profile &p = scene.profile;
        sceneUpdateWorld += p.updateWorld;
        sceneTraverse += p.traverse;
        sceneSort += p.sort;
        sceneFloors += p.floors;
        sceneShadows += p.shadows;
        sceneObjects += p.objects;
        sceneFraming += p.framing;
    }
};

struct Status {
    int stage = 0;
    std::string stageName;
    std::string romPath, dataDir, error;
    bool manifestOk = false, modelsOk = false;
    int models = 0, sounds = 0, music = 0;
    bool heapOk = false;
    int sinMilli = 0;
    uint32_t mulMs = 0, sinMs = 0, loadModelsMs = 0, gameInitMs = 0;
    int avInfoCalls = 0, avInfoCallsBeforeLoad = 0;
    uint32_t frames = 0;
    uint32_t secondStart = 0;
    int callsThisSecond = 0, callsPerSecond = 0;
    uint32_t lastCall = 0, worstGapMs = 0;
    uint16_t buttons = 0;
    bool loaded = false;
};
Status g_status;
Bench g_bench;
std::unique_ptr<GameApp> g_app;

void writeFile(const std::string &name, const std::string &text)
{
    if (g_status.romPath.empty()) return; // the folder is known from retro_load_game on
    const std::string path = cr::baseDir() + name;
    FILE *f = std::fopen(path.c_str(), "wb");
    if (!f) return;
    std::fwrite(text.data(), 1, text.size(), f);
    std::fclose(f);
    fs_sync(path.c_str());
}

void stage(int n, const char *what)
{
    g_status.stage = n;
    g_status.stageName = what;
    xlog("bobrhopper: stage %d %s\n", n, what);
    char buf[160];
    snprintf(buf, sizeof buf, "BobrHopper %s\nmode %s\nstage %d %s\nframe %u\n", kCoreVersion, modeName(g_mode), n,
             what, unsigned(g_status.frames));
    writeFile("stage.txt", buf);
}

void fill(uint16_t color)
{
    for (int i = 0; i < kWidth * kHeight; i++) framebuffer[i] = color;
}

void rect(int x, int y, int w, int h, uint16_t color)
{
    for (int yy = y < 0 ? 0 : y; yy < y + h && yy < kHeight; yy++)
        for (int xx = x < 0 ? 0 : x; xx < x + w && xx < kWidth; xx++) framebuffer[yy * kWidth + xx] = color;
}

// The retro font has letters, digits and space only. Punctuation used in diagnostics gets a plain 5x7 glyph here:
// 7 rows of 5 bits, top row first, bit 4 = leftmost column.
struct PunctGlyph {
    char c;
    uint8_t rows[7];
};
const PunctGlyph kPunct[] = {
    {'/', {0x01, 0x02, 0x02, 0x04, 0x08, 0x08, 0x10}}, {'.', {0, 0, 0, 0, 0, 0x0c, 0x0c}},
    {'_', {0, 0, 0, 0, 0, 0, 0x1f}},                   {':', {0, 0x0c, 0x0c, 0, 0x0c, 0x0c, 0}},
    {'(', {0x02, 0x04, 0x08, 0x08, 0x08, 0x04, 0x02}}, {')', {0x08, 0x04, 0x02, 0x02, 0x02, 0x04, 0x08}},
    {'-', {0, 0, 0, 0x1f, 0, 0, 0}},                   {'%', {0x19, 0x19, 0x02, 0x04, 0x08, 0x13, 0x13}},
    {'=', {0, 0, 0x1f, 0, 0x1f, 0, 0}},                {',', {0, 0, 0, 0, 0x0c, 0x04, 0x08}},
    {'+', {0, 0x04, 0x04, 0x1f, 0x04, 0x04, 0}},       {'?', {0x0e, 0x11, 0x01, 0x06, 0x04, 0, 0x04}},
    {'!', {0x04, 0x04, 0x04, 0x04, 0x04, 0, 0x04}},    {';', {0, 0x0c, 0x0c, 0, 0x0c, 0x04, 0x08}},
    {'[', {0x0e, 0x08, 0x08, 0x08, 0x08, 0x08, 0x0e}}, {']', {0x0e, 0x02, 0x02, 0x02, 0x02, 0x02, 0x0e}},
};

// the boot font into a 320x240 RGB565 buffer (the diagnostic screen, or the game's frame for the fps counter)
void drawText(uint16_t *buffer, int x, int y, const char *s, uint16_t color)
{
    const int top = kBootFontLineHeight - 9; // punctuation sits on the letters' baseline area
    for (; *s; s++) {
        unsigned c = uint8_t(*s);
        if (c >= 128) c = '?';
        if (kBootFontGlyphs[c].advance == 0) {
            for (const PunctGlyph &p : kPunct) {
                if (p.c != char(c)) continue;
                for (int yy = 0; yy < 7; yy++)
                    for (int xx = 0; xx < 5; xx++) {
                        if (!(p.rows[yy] & (0x10 >> xx))) continue;
                        const int px = x + xx, py = y + top + yy;
                        if (px >= 0 && px < kWidth && py >= 0 && py < kHeight) buffer[py * kWidth + px] = color;
                    }
                break;
            }
            x += 6;
            continue;
        }
        const BootGlyph &g = kBootFontGlyphs[c];
        for (int yy = 0; yy < g.h; yy++)
            for (int xx = 0; xx < g.w; xx++) {
                const int bit = (g.y + yy) * kBootFontAtlasW + g.x + xx;
                if (!(kBootFontBits[bit >> 3] & (1 << (bit & 7)))) continue;
                const int px = x + g.xoff + xx, py = y + g.yoff + yy;
                if (px >= 0 && px < kWidth && py >= 0 && py < kHeight) buffer[py * kWidth + px] = color;
            }
        x += g.advance;
    }
}

void line(int row, uint16_t color, const char *fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    drawText(framebuffer, 6, 3 + row * kBootFontLineHeight, buf, color);
}

uint32_t stepMicros() { return g_bench.steps ? uint32_t(uint64_t(g_bench.logicMs) * 1000u / uint64_t(g_bench.steps)) : 0; }
uint32_t worldMicros()
{
    return g_bench.worldCalls ? uint32_t(uint64_t(g_bench.worldMs) * 1000u / uint64_t(g_bench.worldCalls)) : 0;
}

void reportBench()
{
    Status &s = g_status;
    Bench &b = g_bench;
    char buf[900];
    snprintf(buf, sizeof buf,
             "BobrHopper %s\n"
             "mode %s %s\n"
             "digest %016llx expected %016llx %s\n"
             "steps %ld logic_ms %u us_per_step %u\n"
             "update_world calls %ld ms %u us_per_call %u\n"
             "games %d best %d\n"
             "fixed mul_add_200k_ms %u sin_20k_ms %u sin1_milli %d\n"
             "load models_ms %u game_init_ms %u models %d sounds %d music %d\n"
             "fps_reported %d retro_run_per_s %d worst_gap_ms %u av_info_calls %d before_load %d\n"
             "frames %u ticks_ms %u\n",
             kCoreVersion, modeName(g_mode), b.done ? "done" : "running", (unsigned long long)b.digest,
             (unsigned long long)kExpectedDigest,
             !b.done ? "-" : (b.digest == kExpectedDigest ? "ok" : "MISMATCH"), b.steps, unsigned(b.logicMs),
             unsigned(stepMicros()), b.worldCalls, unsigned(b.worldMs), unsigned(worldMicros()), b.games, b.best,
             unsigned(s.mulMs), unsigned(s.sinMs), s.sinMilli, unsigned(s.loadModelsMs), unsigned(s.gameInitMs),
             s.models, s.sounds, s.music, g_fps, s.callsPerSecond, unsigned(s.worstGapMs), s.avInfoCalls,
             s.avInfoCallsBeforeLoad, unsigned(s.frames), unsigned(os_get_tick_count()));
    writeFile(std::string("bench_") + modeName(g_mode) + ".txt", buf);
    xlog("bobrhopper: bench %s", buf);
}

void reportGame()
{
    if (!g_app || !g_app->game) return;
    const Status &s = g_status;
    const GameApp &a = *g_app;
    const long frames = a.frames ? a.frames : 1;
    char buf[1800];
    const uint64_t f = uint64_t(frames);
    auto perFrame = [f](uint32_t ms) { return long(uint64_t(ms) * 1000u / f); };
    auto perFrameUs = [frames](int64_t us) { return long(us / frames); };
    const long steps = a.steps ? a.steps : 1;
    auto perStepUs = [steps](int64_t us) { return long(us / steps); };
    snprintf(buf, sizeof buf,
             "BobrHopper %s\n"
             "mode %s fps_reported %d retro_run_per_s %d worst_gap_ms %u\n"
             "frames %ld steps %ld dropped_steps %ld\n"
             "logic_ms %u per_frame_us %ld\n"
             "render_ms %u per_frame_us %ld\n"
             "audio_ms %u per_frame_us %ld\n"
             "last_frame draws %d tris %d drawn %d spans %d\n"
             "state %d score %d\n"
             "load models_ms %u game_init_ms %u\n"
             "per_frame_us clear %ld scene %ld ui %ld video %ld\n"
             "scene_us transform %ld raster %ld shadows %ld overlay %ld\n"
             "per_frame tris_drawn %ld pixels %ld\n"
             "scene_parts_us update_world %ld traverse %ld sort %ld floors %ld shadows %ld objects %ld framing %ld\n"
             "logic_parts_us_per_step gsap %ld map %ld hero %ld rest_of_step %ld\n"
             "per_frame_us outside_core %ld audio_batch_cb %ld\n",
             kCoreVersion, modeName(g_mode), g_fps, s.callsPerSecond, unsigned(s.worstGapMs), a.frames, a.steps,
             a.droppedSteps, unsigned(a.logicMs), perFrame(a.logicMs), unsigned(a.renderMs), perFrame(a.renderMs),
             unsigned(a.audioMs), perFrame(a.audioMs), a.renderer.stats.drawCalls, a.renderer.stats.triangles,
             a.renderer.stats.trianglesDrawn, a.renderer.stats.spans, int(a.game->state()), a.game->score(),
             unsigned(s.loadModelsMs), unsigned(s.gameInitMs), perFrame(a.clearMs), perFrame(a.sceneMs),
             perFrame(a.uiMs), perFrame(a.videoMs), perFrameUs(a.usTransform), perFrameUs(a.usRaster),
             perFrameUs(a.usShadows), perFrameUs(a.usOverlay), perFrameUs(a.trianglesDrawn), perFrameUs(a.pixels),
             perFrameUs(a.sceneUpdateWorld), perFrameUs(a.sceneTraverse), perFrameUs(a.sceneSort),
             perFrameUs(a.sceneFloors), perFrameUs(a.sceneShadows), perFrameUs(a.sceneObjects),
             perFrameUs(a.sceneFraming), perStepUs(a.game->stepProfile.gsap), perStepUs(a.game->stepProfile.map),
             perStepUs(a.game->stepProfile.hero), perStepUs(a.game->stepProfile.frame), perFrame(a.outsideMs),
             perFrame(a.audioCallMs));
    writeFile(std::string("game_") + modeName(g_mode) + ".txt", buf);
}

uint16_t actionsFromPad(uint16_t buttons)
{
    static const struct {
        unsigned id;
        cr::Action act;
    } map[] = {{RETRO_DEVICE_ID_JOYPAD_UP, cr::ActUp},       {RETRO_DEVICE_ID_JOYPAD_DOWN, cr::ActDown},
               {RETRO_DEVICE_ID_JOYPAD_LEFT, cr::ActLeft},   {RETRO_DEVICE_ID_JOYPAD_RIGHT, cr::ActRight},
               {RETRO_DEVICE_ID_JOYPAD_A, cr::ActA},         {RETRO_DEVICE_ID_JOYPAD_B, cr::ActB},
               {RETRO_DEVICE_ID_JOYPAD_START, cr::ActStart}, {RETRO_DEVICE_ID_JOYPAD_SELECT, cr::ActSelect},
               {RETRO_DEVICE_ID_JOYPAD_L, cr::ActL},         {RETRO_DEVICE_ID_JOYPAD_R, cr::ActR}};
    uint16_t mask = 0;
    for (const auto &m : map)
        if (buttons & (1u << m.id)) mask |= m.act;
    return mask;
}

void runGame(uint16_t buttons)
{
    Status &s = g_status;
    GameApp &a = *g_app;
    const uint32_t now = os_get_tick_count();
    if (a.lastEnd) a.outsideMs += now - a.lastEnd; // the firmware between the previous call's end and this one
    if (!a.started) {
        a.started = true;
        a.lastTick = now;
        a.acc = 1000; // a step on the first call
    }
    uint32_t elapsed = now - a.lastTick;
    a.lastTick = now;
    if (elapsed > 250) elapsed = 250;
    a.acc += elapsed * 60;
    const uint16_t mask = actionsFromPad(buttons);
    const uint32_t t0 = os_get_tick_count();
    for (int n = 0; a.acc >= 1000 && n < kMaxCatchUp; n++) {
        a.step(mask);
        a.acc -= 1000;
    }
    if (a.acc >= 1000) {
        a.droppedSteps += long(a.acc / 1000);
        a.acc %= 1000;
    }
    const uint32_t t1 = os_get_tick_count();
    a.render();
    const uint32_t t2 = os_get_tick_count();
    a.logicMs += t1 - t0;
    a.renderMs += t2 - t1;
    a.frames++;
    if (a.settings.fpsCounter) {
        char buf[48];
        snprintf(buf, sizeof buf, "RUN/S %d %s", s.callsPerSecond, kCoreVersion);
        // over the finished frame, straight into the renderer's buffer
        drawText(const_cast<uint16_t *>(a.renderer.target().color.data()), 4, kHeight - 14, buf, 0xffff);
    }
    // v005: breadcrumbs for the first calls of the game (v004 froze after its first frame): stage.txt on the card shows
    // the last firmware call a frozen console was in
    const bool trace = a.frames <= 5;
    if (trace) stage(11, "video_cb");
    const uint32_t tv = os_get_tick_count();
    if (video_cb) video_cb(a.renderer.target().color.data(), kWidth, kHeight, kWidth * sizeof(uint16_t));
    a.videoMs += os_get_tick_count() - tv;

    // The sound of the time that has passed, not of one nominal frame: the firmware plays 22050 samples a second
    // whatever the call rate, so a fixed 735 per call starved it (and stuttered) whenever a frame took over 33 ms.
    // The mono mix is duplicated into the stereo pairs the frontend expects.
    a.audioAcc += elapsed * uint32_t(kSampleRate);
    int audioFrames = int(a.audioAcc / 1000);
    a.audioAcc %= 1000;
    if (audioFrames > kMaxAudioFrames) audioFrames = kMaxAudioFrames;
    const uint32_t t3 = os_get_tick_count();
    a.audio.mix(audioMono, audioFrames);
    for (int i = 0; i < audioFrames; i++) audioStereo[2 * i] = audioStereo[2 * i + 1] = audioMono[i];
    a.audioMs += os_get_tick_count() - t3;
    if (trace) {
        char what[48];
        snprintf(what, sizeof what, "audio_batch_cb %d frames", audioFrames);
        stage(12, what);
    }
    const uint32_t ta = os_get_tick_count();
    if (audio_batch_cb && audioFrames > 0) audio_batch_cb(audioStereo, size_t(audioFrames));
    a.audioCallMs += os_get_tick_count() - ta;
    if (trace) stage(13, "frame done");
    if (now - a.lastReport >= 10000) {
        a.lastReport = now;
        reportGame();
    }
    a.lastEnd = os_get_tick_count();
}

// ROM "RenderBench": the render benchmark (render_bench.h) on the game's data; results in renderbench.txt, rewritten
// after every variant, so switching the console off loses at most one variant
std::unique_ptr<cr::RenderBench> g_rbench;

void runRenderBench()
{
    GameApp &a = *g_app;
    const cr::RenderBench::Frame f = g_rbench->step(a.renderer, a.scene, kViewScaleNormal, os_get_tick_count);
    if (f.writeResults) writeFile("renderbench.txt", f.results);
    // the progress text goes over the finished (and already timed) frame
    uint16_t *buffer = const_cast<uint16_t *>(a.renderer.target().color.data());
    drawText(buffer, 4, 2, f.line1.c_str(), 0xffff);
    drawText(buffer, 4, 2 + kBootFontLineHeight, f.line2.c_str(), 0xffe0);
    drawText(buffer, 4, 2 + 2 * kBootFontLineHeight, f.line3.c_str(), 0xffff);
    drawText(buffer, 4, kHeight - kBootFontLineHeight - 2, "results: renderbench.txt - run 5 minutes", 0x07e0);
    if (video_cb) video_cb(a.renderer.target().color.data(), kWidth, kHeight, kWidth * sizeof(uint16_t));
}

void runDiagnostics(uint16_t buttons)
{
    Status &s = g_status;
    Bench &b = g_bench;
    (void)buttons;
    const uint32_t now = os_get_tick_count();
    // the bench: the logic at full speed (the host's clock stands still within a call, hence the step cap as well)
    if (b.game && !b.done && g_mode == Mode::Bench) {
        const uint32_t start = os_get_tick_count();
        for (int n = 0; n < kMaxStepsPerRun && b.steps < kBenchSteps; n++) {
            b.step();
            if ((n & 7) == 7 && os_get_tick_count() - start >= kStepBudgetMs) break;
        }
        b.logicMs += os_get_tick_count() - start;
        const uint32_t w0 = os_get_tick_count();
        for (int i = 0; i < 4; i++) cr::updateWorld(b.game->sceneRoot());
        b.worldMs += os_get_tick_count() - w0;
        b.worldCalls += 4;
        if (b.steps >= kBenchSteps) {
            b.done = true;
            stage(10, b.digest == kExpectedDigest ? "bench done digest ok" : "bench done digest MISMATCH");
            xlog("bobrhopper: bench digest %016llx %s\n", (unsigned long long)b.digest,
                 b.digest == kExpectedDigest ? "ok" : "MISMATCH");
        }
    }
    // results: once after the end (with a full second of retro_run rate) and every ~10 s while running
    if (b.done && !b.reported && s.callsPerSecond > 0 && s.frames % uint32_t(g_fps) == 0) {
        b.reported = true;
        reportBench();
    } else if (b.game && !b.done && now - b.lastReport >= 10000) {
        b.lastReport = now;
        reportBench();
    }

    const bool mathOk = g_ctorProbe.value == 12345 && s.sinMilli == 841 && s.heapOk;
    const uint16_t white = 0xffff, green = 0x07e0, red = 0xf800, yellow = 0xffe0;
    fill(0x10a6);
    rect(int(s.frames % 300), 228, 20, 10, yellow); // moves 1 px per call: stutter is visible
    line(0, yellow, "CROSSYROAD %s", kCoreVersion);
    line(1, white, "mode %s frame %u", modeName(g_mode), unsigned(s.frames));
    line(2, s.callsPerSecond >= g_fps - 1 ? green : white, "run/s %d fps %d gap %u", s.callsPerSecond, g_fps,
         unsigned(s.worstGapMs));
    line(3, white, "stage %d buttons %04x", s.stage, unsigned(s.buttons));
    line(4, mathOk ? green : red, "ctor sin heap %s", mathOk ? "ok" : "FAILED");
    line(5, s.modelsOk ? green : red, "data %s models %d", s.modelsOk ? "ok" : "MISSING", s.models);
    if (!s.error.empty()) line(6, red, "error %s", s.error.c_str());
    else line(6, white, "load %u ms init %u ms", unsigned(s.loadModelsMs), unsigned(s.gameInitMs));
    line(7, white, "mul %u ms sin %u ms", unsigned(s.mulMs), unsigned(s.sinMs));
    if (g_mode == Mode::Bench) {
        line(8, white, "steps %ld/%ld", b.steps, kBenchSteps);
        line(9, white, "step %u us world %u us", unsigned(stepMicros()), unsigned(worldMicros()));
        line(10, white, "games %d best %d", b.games, b.best);
        if (b.done)
            line(11, b.digest == kExpectedDigest ? green : red, "%016llx %s", (unsigned long long)b.digest,
                 b.digest == kExpectedDigest ? "ok" : "BAD");
        line(12, b.reported ? green : white, b.reported ? "bench txt written" : (b.done ? "bench done" : "running"));
    }
    if (video_cb) video_cb(framebuffer, kWidth, kHeight, kWidth * sizeof(uint16_t));
}

} // namespace

void retro_init(void)
{
    stage(1, "retro_init");
    std::memset(framebuffer, 0, sizeof framebuffer);
}

void retro_deinit(void)
{
    g_app.reset();
    stage(90, "retro_deinit");
}

unsigned retro_api_version(void) { return RETRO_API_VERSION; }

void retro_set_controller_port_device(unsigned, unsigned) {}

void retro_get_system_info(struct retro_system_info *info)
{
    std::memset(info, 0, sizeof *info);
    info->library_name = "BobrHopper";
    info->library_version = kCoreVersion;
    info->need_fullpath = true;
    info->valid_extensions = "start|gba";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
    g_status.avInfoCalls++;
    if (!g_status.loaded) g_status.avInfoCallsBeforeLoad++;
    std::memset(info, 0, sizeof *info);
    // the libretro fields are double: constants only, no int-to-double conversion at run time
    info->timing.fps = g_fps == 60 ? 60.0 : 30.0;
    info->timing.sample_rate = 22050.0;
    info->geometry.base_width = kWidth;
    info->geometry.base_height = kHeight;
    info->geometry.max_width = kWidth;
    info->geometry.max_height = kHeight;
    info->geometry.aspect_ratio = 4.0f / 3.0f;
}

void retro_set_environment(retro_environment_t cb) { environ_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) {}
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

void retro_reset(void) {}

bool retro_load_game(const struct retro_game_info *info)
{
    Status &s = g_status;
    if (info && info->path) {
        s.romPath = info->path;
        const size_t slash = s.romPath.find_last_of("/\\");
        if (slash != std::string::npos) cr::setBaseDir(s.romPath.substr(0, slash + 1));
        const std::string base = slash == std::string::npos ? s.romPath : s.romPath.substr(slash + 1);
        std::string lower = base;
        for (char &c : lower) c = char(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        if (lower.find("renderbench") != std::string::npos) g_mode = Mode::RenderBench;
        else if (lower.find("bench") != std::string::npos) g_mode = Mode::Bench;
        else if (lower.find("screen") != std::string::npos) g_mode = Mode::Screen;
        else if (lower.find("60") != std::string::npos) g_mode = Mode::Game60;
        else g_mode = Mode::Game30;
        g_fps = g_mode == Mode::Game60 ? 60 : 30;
        // the game's own log (engine logf: missing files, sounds, music) next to the ROM; flushed to the card with
        // fs_sync after loading and when the game is closed
        cr::logOpen(cr::baseDir() + "bobrhopper.log");
        cr::logf("BobrHopper %s rom %s mode %s", kCoreVersion, s.romPath.c_str(), modeName(g_mode));
    }
    stage(2, "retro_load_game");
    enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
    if (!environ_cb || !environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) {
        stage(99, "RGB565 refused");
        return false;
    }
    stage(3, "pixel format ok");

    // the maths the game runs on (16.16 on the SF2000): sin(1) from the LUT, 200k multiply+add, 20k sin
    s.sinMilli = int(cr::rsin(cr::real(1)) * cr::real(1000));
    {
        uint32_t t0 = os_get_tick_count();
        cr::real acc = 1;
        const cr::real k = 1.0001, half = 0.5, step = 0.001;
        for (int i = 0; i < 200000; i++) acc = acc * k + half - half;
        s.mulMs = os_get_tick_count() - t0;
        t0 = os_get_tick_count();
        cr::real angle = 0;
        for (int i = 0; i < 20000; i++) {
            acc = acc + cr::rsin(angle);
            angle += step;
        }
        s.sinMs = os_get_tick_count() - t0;
    }
    stage(4, "fixed maths ok");

    {
        std::vector<uint8_t> probe(4 * 1024 * 1024, uint8_t(0x5a));
        s.heapOk = probe.size() == 4u * 1024 * 1024 && probe[probe.size() - 1] == 0x5a;
    }
    stage(5, "heap ok");

    s.dataDir = cr::dataDir();
    cr::Manifest manifest;
    s.manifestOk = cr::loadManifest(s.dataDir + "manifest.txt", manifest);
    s.models = int(manifest.models.size());
    s.sounds = int(manifest.sounds.size());
    s.music = int(manifest.music.size());
    xlog("bobrhopper: rom=%s data=%s manifest=%d models=%d sounds=%d mode=%s fps=%d\n", s.romPath.c_str(),
         s.dataDir.c_str(), s.manifestOk ? 1 : 0, s.models, s.sounds, modeName(g_mode), g_fps);
    stage(6, "files ok");

    if (s.manifestOk && (isGame() || g_mode == Mode::RenderBench)) {
        uint32_t t0 = os_get_tick_count();
        g_app.reset(new GameApp());
        s.modelsOk = g_app->load(manifest, s.dataDir, os_get_tick_count() | 1u, s.error);
        s.loadModelsMs = os_get_tick_count() - t0;
        if (s.modelsOk) {
            stage(7, "game data ok");
            if (g_mode == Mode::RenderBench) {
                // the benchmark draws the game's default view whatever the saved settings say
                g_app->scene.viewShift = kViewShiftNormal;
                g_rbench.reset(new cr::RenderBench());
                g_rbench->init(*g_app->models, manifest, s.dataDir, kCoreVersion);
                cr::logf("renderbench: %d textured meshes", g_rbench->texMeshes());
            }
            stage(8, "game ok");
        } else {
            xlog("bobrhopper: game data failed: %s\n", s.error.c_str());
            stage(98, ("game data FAILED: " + s.error).c_str());
            g_app.reset();
        }
    } else if (s.manifestOk) {
        uint32_t t0 = os_get_tick_count();
        g_bench.models.reset(new cr::ModelLibrary());
        s.modelsOk = g_bench.models->load(manifest, s.dataDir);
        s.loadModelsMs = os_get_tick_count() - t0;
        stage(s.modelsOk ? 7 : 98, s.modelsOk ? "models ok" : "models FAILED");
        if (s.modelsOk) {
            t0 = os_get_tick_count();
            Bench &b = g_bench;
            b.game.reset(new cr::Game(*b.models, kBenchSeed));
            // the logic benchmark keeps the original's behaviour: its digest is the 16.16 reference, and its timings
            // compare with the earlier console runs
            b.game->context().originalBehaviour = true;
            b.game->setupGame("chicken");
            b.game->init();
            b.game->tickEngineOnly(); // GameEngine.unpause(), as bobrhopper.cpp does
            b.game->takeSounds();
            b.input.reset(new cr::Input());
            b.bot.reset(new cr::SmokeBot(kBenchSeed));
            b.prevState = b.game->state();
            s.gameInitMs = os_get_tick_count() - t0;
            stage(8, "game ok");
        }
    }
    s.loaded = true;
    s.secondStart = os_get_tick_count();
    g_bench.lastReport = s.secondStart;
    cr::logf("load: %s models %d in %u ms%s%s", s.modelsOk ? "ok" : "FAILED", s.models, unsigned(s.loadModelsMs),
             s.error.empty() ? "" : " error ", s.error.c_str());
    if (!s.romPath.empty()) fs_sync((cr::baseDir() + "bobrhopper.log").c_str());
    return true;
}

void retro_unload_game(void)
{
    // settings changed in a settings screen that was still open when the firmware's menu left the game
    if (g_app && g_app->settingsDirty) {
        g_app->saveSettings();
        g_app->settingsDirty = false;
    }
    if (g_app) reportGame();
    else if (g_bench.game) reportBench();
    stage(91, "retro_unload_game");
    cr::logf("unload after %u frames", unsigned(g_status.frames));
    cr::logClose();
    if (!g_status.romPath.empty()) fs_sync((cr::baseDir() + "bobrhopper.log").c_str());
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t) { return false; }
size_t retro_serialize_size(void) { return 0; }
bool retro_serialize(void *, size_t) { return false; }
bool retro_unserialize(const void *, size_t) { return false; }
void *retro_get_memory_data(unsigned) { return nullptr; }
size_t retro_get_memory_size(unsigned) { return 0; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned, bool, const char *) {}

void retro_run(void)
{
    Status &s = g_status;
    if (s.frames == 0) stage(9, "first retro_run");
    const uint32_t now = os_get_tick_count();
    if (s.frames > uint32_t(g_fps) && now - s.lastCall > s.worstGapMs) s.worstGapMs = now - s.lastCall;
    s.lastCall = now;
    s.callsThisSecond++;
    if (now - s.secondStart >= 1000) {
        s.callsPerSecond = s.callsThisSecond;
        s.callsThisSecond = 0;
        s.secondStart = now;
    }

    if (input_poll_cb) input_poll_cb();
    uint16_t buttons = 0;
    if (input_state_cb)
        for (unsigned id = 0; id < 16; id++)
            if (input_state_cb(0, RETRO_DEVICE_JOYPAD, 0, id)) buttons |= uint16_t(1u << id);
    if (buttons != s.buttons) xlog("bobrhopper: buttons %04x at frame %u\n", unsigned(buttons), unsigned(s.frames));
    s.buttons = buttons;

    if (g_mode == Mode::RenderBench && g_app && g_rbench) {
        runRenderBench();
        if (audio_batch_cb) audio_batch_cb(silence, size_t(kSampleRate / 30));
    } else if (isGame() && g_app) {
        runGame(buttons);
    } else {
        runDiagnostics(buttons);
        if (audio_batch_cb) audio_batch_cb(silence, size_t(g_fps == 60 ? kSampleRate / 60 : kSampleRate / 30));
    }
    s.frames++;
}
