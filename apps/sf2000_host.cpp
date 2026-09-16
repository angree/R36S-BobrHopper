// A tiny libretro frontend for the SF2000 core, so the core runs on the PC (and under qemu as mipsel soft-float)
// exactly as the multicore firmware calls it: set callbacks, retro_init, retro_load_game(path), retro_run per frame.
// It records frames as PNG, feeds joypad buttons from a script and hashes every frame (FNV-1a) for determinism checks.
//
//   sf2000_host --frames 120 --rom out/sf2000/hostcard/ROMS/bobrhopper/start --shot-dir out/check/host \
//               --script "w30 btn:a w10 hold:right:20 shot:after" --shots 1,60
//
// Script tokens (one step = one retro_run): wN wait N frames, btn:NAME press for 2 frames, hold:NAME:N hold N frames,
// shot:NAME save the next frame. Buttons: up down left right a b x y l r start select.
// --record FILE stores the joypad bitmask of every retro_run (u16 little endian); --replay FILE plays one back instead
// of the script (buttons as the core read them, so a replay draws the same frames).
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>
#include <string>
#include <vector>

#include "engine/png_write.h"
#include "libretro.h"

extern uint32_t g_hostTickMs;
extern int g_hostSyncCount;
extern bool g_hostQuietLog;

namespace {

struct Options {
    long frames = 120;
    std::string rom = "out/sf2000/hostcard/ROMS/bobrhopper/start";
    std::string shotDir = "out/check/host";
    std::string shotPrefix = "host";
    std::vector<long> shots;
    std::string script;
    int tickMs = 33; // simulated firmware clock per retro_run (30 fps)
    bool realtime = false;
    bool quiet = false;
    std::string record, replay;
};

struct Event {
    long frame;
    unsigned id;
    long length;
};

int g_width = 0, g_height = 0;
std::vector<uint16_t> g_frame;
bool g_rgb565 = false;
uint64_t g_hash = 1469598103934665603ULL;
long g_videoFrames = 0;
long g_audioFrames = 0;
uint16_t g_buttons = 0;

void hashBytes(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        g_hash ^= p[i];
        g_hash *= 1099511628211ULL;
    }
}

bool environment(unsigned cmd, void *data)
{
    switch (cmd) {
    case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
        g_rgb565 = *static_cast<enum retro_pixel_format *>(data) == RETRO_PIXEL_FORMAT_RGB565;
        return g_rgb565; // the SF2000 path: RGB565 only
    default:
        return false;
    }
}

void videoRefresh(const void *data, unsigned width, unsigned height, size_t pitch)
{
    if (!data) return; // a duplicated frame
    g_width = int(width);
    g_height = int(height);
    g_frame.resize(width * height);
    const size_t stride = pitch / 2;
    const uint16_t *src = static_cast<const uint16_t *>(data);
    for (unsigned y = 0; y < height; y++) std::memcpy(&g_frame[y * width], src + y * stride, width * 2);
    hashBytes(reinterpret_cast<const uint8_t *>(g_frame.data()), g_frame.size() * 2);
    g_videoFrames++;
}

// the audio samples get their own FNV-1a (printed as audio_fnv, apart from the frame hash)
uint64_t g_audioHash = 1469598103934665603ULL;
long g_audioNonZero = 0;

size_t audioBatch(const int16_t *data, size_t frames)
{
    g_audioFrames += long(frames);
    for (size_t i = 0; i < frames * 2; i++) {
        const uint16_t s = uint16_t(data[i]);
        g_audioHash = (g_audioHash ^ (s & 0xff)) * 1099511628211ULL;
        g_audioHash = (g_audioHash ^ (s >> 8)) * 1099511628211ULL;
        g_audioNonZero += data[i] != 0;
    }
    return frames;
}

void audioSample(int16_t, int16_t) { g_audioFrames++; }

void inputPoll() {}

int16_t inputState(unsigned port, unsigned device, unsigned, unsigned id)
{
    if (port != 0 || device != RETRO_DEVICE_JOYPAD || id >= 16) return 0;
    return (g_buttons >> id) & 1;
}

bool saveShot(const std::string &path)
{
    if (g_frame.empty()) return false;
    std::vector<uint8_t> rgba(size_t(g_width) * size_t(g_height) * 4);
    for (size_t i = 0; i < g_frame.size(); i++) {
        const uint16_t p = g_frame[i];
        const int r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31;
        rgba[i * 4] = uint8_t((r << 3) | (r >> 2));
        rgba[i * 4 + 1] = uint8_t((g << 2) | (g >> 4));
        rgba[i * 4 + 2] = uint8_t((b << 3) | (b >> 2));
        rgba[i * 4 + 3] = 255;
    }
    const bool ok = png::writeRGBA(path, g_width, g_height, rgba.data());
    std::printf("host: shot %s %s\n", path.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

int buttonId(const std::string &name)
{
    static const struct {
        const char *name;
        int id;
    } names[] = {{"up", RETRO_DEVICE_ID_JOYPAD_UP},       {"down", RETRO_DEVICE_ID_JOYPAD_DOWN},
                 {"left", RETRO_DEVICE_ID_JOYPAD_LEFT},   {"right", RETRO_DEVICE_ID_JOYPAD_RIGHT},
                 {"a", RETRO_DEVICE_ID_JOYPAD_A},         {"b", RETRO_DEVICE_ID_JOYPAD_B},
                 {"x", RETRO_DEVICE_ID_JOYPAD_X},         {"y", RETRO_DEVICE_ID_JOYPAD_Y},
                 {"l", RETRO_DEVICE_ID_JOYPAD_L},         {"r", RETRO_DEVICE_ID_JOYPAD_R},
                 {"start", RETRO_DEVICE_ID_JOYPAD_START}, {"select", RETRO_DEVICE_ID_JOYPAD_SELECT}};
    for (const auto &n : names)
        if (name == n.name) return n.id;
    return -1;
}

bool parseScript(const std::string &script, std::vector<Event> &events, std::vector<std::pair<long, std::string>> &shots)
{
    std::istringstream in(script);
    std::string tok;
    long frame = 0;
    while (in >> tok) {
        if (tok[0] == 'w') {
            frame += std::atol(tok.c_str() + 1);
        } else if (tok.compare(0, 4, "btn:") == 0) {
            const int id = buttonId(tok.substr(4));
            if (id < 0) return false;
            events.push_back({frame, unsigned(id), 2});
            frame += 2;
        } else if (tok.compare(0, 5, "hold:") == 0) {
            const size_t colon = tok.find(':', 5);
            if (colon == std::string::npos) return false;
            const int id = buttonId(tok.substr(5, colon - 5));
            const long n = std::atol(tok.c_str() + colon + 1);
            if (id < 0 || n <= 0) return false;
            events.push_back({frame, unsigned(id), n});
            frame += n;
        } else if (tok.compare(0, 5, "shot:") == 0) {
            shots.push_back({frame, tok.substr(5)});
        } else {
            return false;
        }
    }
    return true;
}

} // namespace

int main(int argc, char **argv)
{
    Options opt;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--frames") opt.frames = std::atol(next().c_str());
        else if (a == "--rom") opt.rom = next();
        else if (a == "--shot-dir") opt.shotDir = next();
        else if (a == "--shot-prefix") opt.shotPrefix = next();
        else if (a == "--script") opt.script = next();
        else if (a == "--tick-ms") opt.tickMs = std::atoi(next().c_str());
        else if (a == "--realtime") opt.realtime = true;
        else if (a == "--quiet") opt.quiet = true;
        else if (a == "--record") opt.record = next();
        else if (a == "--replay") opt.replay = next();
        else if (a == "--shots") {
            std::string list = next();
            std::stringstream ss(list);
            std::string item;
            while (std::getline(ss, item, ',')) opt.shots.push_back(std::atol(item.c_str()));
        } else {
            std::fprintf(stderr, "sf2000_host: unknown option %s\n", a.c_str());
            return 2;
        }
    }
    g_hostQuietLog = opt.quiet;

    std::vector<Event> events;
    std::vector<std::pair<long, std::string>> scriptShots;
    if (!parseScript(opt.script, events, scriptShots)) {
        std::fprintf(stderr, "sf2000_host: bad script\n");
        return 2;
    }

    // the call order of the multicore frontend (core_api.c + stock run_emulator)
    retro_set_environment(environment);
    retro_set_video_refresh(videoRefresh);
    retro_set_audio_sample(audioSample);
    retro_set_audio_sample_batch(audioBatch);
    retro_set_input_poll(inputPoll);
    retro_set_input_state(inputState);
    retro_init();

    retro_system_info sys;
    retro_get_system_info(&sys);
    std::printf("host: core %s %s need_fullpath=%d\n", sys.library_name, sys.library_version, sys.need_fullpath ? 1 : 0);

    retro_game_info game;
    std::memset(&game, 0, sizeof game);
    game.path = opt.rom.c_str();
    if (!retro_load_game(&game)) {
        std::printf("host: retro_load_game failed\n");
        return 3;
    }
    retro_system_av_info av;
    retro_get_system_av_info(&av);
    std::printf("host: av %ux%u fps %.1f rate %.0f\n", av.geometry.base_width, av.geometry.base_height, av.timing.fps,
                av.timing.sample_rate);
    retro_set_controller_port_device(0, RETRO_DEVICE_JOYPAD);
    retro_set_controller_port_device(1, RETRO_DEVICE_JOYPAD);

    std::vector<uint16_t> replay, recording;
    if (!opt.replay.empty()) {
        FILE *rf = std::fopen(opt.replay.c_str(), "rb");
        if (!rf) {
            std::fprintf(stderr, "sf2000_host: cannot read %s\n", opt.replay.c_str());
            return 2;
        }
        unsigned char b[2];
        while (std::fread(b, 1, 2, rf) == 2) replay.push_back(uint16_t(b[0] | b[1] << 8));
        std::fclose(rf);
        std::printf("host: replay %s, %zu frames\n", opt.replay.c_str(), replay.size());
    }

    const auto start = std::chrono::steady_clock::now();
    for (long f = 0; f < opt.frames; f++) {
        g_buttons = 0;
        if (!opt.replay.empty()) {
            if (size_t(f) < replay.size()) g_buttons = replay[size_t(f)];
        } else {
            for (const Event &e : events)
                if (f >= e.frame && f < e.frame + e.length) g_buttons |= uint16_t(1u << e.id);
        }
        if (!opt.record.empty()) recording.push_back(g_buttons);
        if (opt.realtime)
            g_hostTickMs = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
        else
            g_hostTickMs = uint32_t(f * opt.tickMs);
        retro_run();
        for (long s : opt.shots)
            if (s == f + 1) saveShot(opt.shotDir + "/" + opt.shotPrefix + "_f" + std::to_string(s) + ".png");
        for (const auto &s : scriptShots)
            if (s.first == f) saveShot(opt.shotDir + "/" + opt.shotPrefix + "_" + s.second + ".png");
    }
    saveShot(opt.shotDir + "/" + opt.shotPrefix + "_final.png");
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();

    if (!opt.record.empty()) {
        FILE *wf = std::fopen(opt.record.c_str(), "wb");
        if (wf) {
            for (uint16_t v : recording) {
                const unsigned char b[2] = {uint8_t(v), uint8_t(v >> 8)};
                std::fwrite(b, 1, 2, wf);
            }
            std::fclose(wf);
        }
        std::printf("host: recorded %zu frames to %s %s\n", recording.size(), opt.record.c_str(), wf ? "ok" : "FAILED");
    }
    retro_unload_game();
    retro_deinit();
    std::printf("host: frames=%ld video=%ld audio_frames=%ld syncs=%d hash=%016llx wall=%.2fs (%.1f frames/s)\n",
                opt.frames, g_videoFrames, g_audioFrames, g_hostSyncCount, (unsigned long long)g_hash, seconds,
                seconds > 0 ? opt.frames / seconds : 0.0);
    std::printf("host: audio_fnv=%016llx audio_nonzero=%ld\n", (unsigned long long)g_audioHash, g_audioNonZero);
    return g_videoFrames == opt.frames ? 0 : 4;
}
