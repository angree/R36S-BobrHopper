// Mixer test without an audio device: loads the real sound bank and music and mixes offline.
#include <SDL.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "engine/assets.h"
#include "engine/audio.h"
#include "engine/log.h"

using namespace cr;

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, what)                                                                                       \
    do {                                                                                                        \
        g_checks++;                                                                                             \
        if (!(cond)) {                                                                                          \
            std::printf("FAIL %s\n", what);                                                                     \
            g_fail++;                                                                                           \
        }                                                                                                       \
    } while (0)

static bool anyNonZero(const std::vector<int16_t> &v, size_t from, size_t to)
{
    for (size_t i = from; i < to && i < v.size(); i++)
        if (v[i] != 0) return true;
    return false;
}

// fills `sec` with one second of output, mixed in 1024-frame buffers like the SDL callback does
static void mixSecond(Audio &a, std::vector<int16_t> &sec)
{
    for (size_t done = 0; done < sec.size();) {
        const int n = int(std::min<size_t>(1024, sec.size() - done));
        a.mix(sec.data() + done, n);
        done += size_t(n);
    }
}

int main(int, char **)
{
    Manifest m;
    CHECK(loadManifest(dataDir() + "manifest.txt", m), "manifest loads");
    Audio a;
    a.init(false);
    CHECK(a.loadBank(m, dataDir()), "all 26 sounds load");
    CHECK(m.sounds.size() == 26, "manifest lists 26 sounds");
    CHECK(m.music.size() == 8 && m.music[0] == "title", "manifest lists 8 music tracks, title first");

    SoundData buck;
    CHECK(loadSound(dataDir() + "sounds/chicken_move_0.snd", buck), "chicken_move_0 loads");

    // a 22050 Hz voice on the 44100 Hz mixer: even outputs are the samples, odd ones halfway to the next
    a.play("chicken_move_0");
    const size_t n = buck.samples.size();
    std::vector<int16_t> out(2 * n + 100);
    a.mix(out.data(), int(out.size()));
    bool exact = true;
    for (size_t i = 0; i < n; i++) {
        const int32_t s0 = buck.samples[i], s1 = i + 1 < n ? buck.samples[i + 1] : s0;
        exact = exact && out[2 * i] == s0 && out[2 * i + 1] == int16_t(s0 + int32_t((int64_t(s1 - s0) * 32768) >> 16));
    }
    CHECK(exact, "single voice resampled 2x with exact interpolation");
    CHECK(out[2 * n + 50] == 0, "silence after the sound");
    CHECK(a.playingCount() == 0, "voice freed at the end");

    // more sounds than voices: the oldest is replaced, never more than 8 voices
    for (int i = 0; i < 12; i++) a.play("car_passive_0");
    CHECK(a.playingCount() == Audio::kVoices, "voice limit");

    // master volume 0 mutes the effects, and mixing clamps instead of wrapping
    a.setMasterVolume(0);
    a.mix(out.data(), 256);
    bool silent = true;
    for (int i = 0; i < 256; i++) silent = silent && out[size_t(i)] == 0;
    CHECK(silent, "master volume 0 is silent");
    a.stopAll();
    a.setMasterVolume(1);
    for (int i = 0; i < 8; i++) a.play("car_die_1");
    a.mix(out.data(), int(out.size()));
    CHECK(a.playingCount() <= Audio::kVoices, "mix with 8 loud voices survives");
    a.stopAll();

    // music: plays after the fade-in, respects its own volume, loops, switches tracks
    const std::string title = dataDir() + "music/title.ogg", track = dataDir() + "music/track_1.ogg";
    CHECK(a.playMusic(title), "title.ogg decodes");
    a.setMusicVolume(1);
    std::vector<int16_t> sec(Audio::kRate);
    mixSecond(a, sec);
    CHECK(anyNonZero(sec, Audio::kRate / 2, sec.size()), "title plays after the fade-in");
    a.setMusicVolume(0);
    mixSecond(a, sec);
    CHECK(!anyNonZero(sec, 0, sec.size()), "music volume 0 is silent");
    {
        // volume 0 pauses: a track paused for 2 s resumes exactly where one paused for a single frame does
        Audio x, y;
        x.init(false), y.init(false);
        x.playMusic(title), y.playMusic(title);
        x.setMusicVolume(1), y.setMusicVolume(1);
        std::vector<int16_t> bx(Audio::kRate), by(Audio::kRate);
        mixSecond(x, bx), mixSecond(y, by);
        x.setMusicVolume(0), y.setMusicVolume(0);
        mixSecond(x, bx), mixSecond(x, bx);
        y.mix(by.data(), 1);
        x.setMusicVolume(1), y.setMusicVolume(1);
        mixSecond(x, bx), mixSecond(y, by);
        CHECK(anyNonZero(bx, 0, bx.size()) && bx == by, "music volume 0 pauses the track, raising it resumes");
    }
    a.setMusicVolume(0.22f);
    for (int s = 0; s < 72; s++) mixSecond(a, sec); // the title is 70 s long
    CHECK(anyNonZero(sec, 0, sec.size()), "music loops past the end of the track");
    CHECK(a.playMusic(track), "track_1.ogg decodes");
    for (int s = 0; s < 2; s++) mixSecond(a, sec);
    CHECK(anyNonZero(sec, 0, sec.size()) && a.musicPath() == track, "switched to the next track");
    a.stopMusic();
    for (int s = 0; s < 2; s++) mixSecond(a, sec);
    CHECK(!anyNonZero(sec, 0, sec.size()), "stopMusic fades to silence");

    std::printf("test_audio: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
