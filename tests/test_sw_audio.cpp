// The SF2000 audio path (CR_FIXED): MS ADPCM decoding against ffmpeg's decoder of the same file, looping at the end of
// the data, the 22050 Hz mixer (sound effects pass through unresampled) and a music track at full volume after its
// fade-in. Run through build/test_sw_audio.sh, which makes the test files with ffmpeg:
//   test_sw_audio.exe <adpcm.wav> <ffmpeg_decode.raw> <data_sf2000/>
#include <cstdio>
#include <vector>

#include "engine/adpcm.h"
#include "engine/assets.h"
#include "engine/audio.h"

using namespace cr;

static int g_fail = 0, g_checks = 0;
static void check(bool ok, const char *what)
{
    g_checks++;
    if (!ok) {
        g_fail++;
        std::printf("FAIL %s\n", what);
    }
}

int main(int argc, char **argv)
{
    if (argc < 4) {
        std::printf("usage: test_sw_audio <adpcm.wav> <reference.raw> <data_dir/>\n");
        return 2;
    }
    const std::string wavPath = argv[1], dataDir = argv[3];
    std::vector<uint8_t> rawBytes;
    check(readFile(argv[2], rawBytes) && rawBytes.size() > 44100, "reference decode loads");
    std::vector<int16_t> ref(rawBytes.size() / 2);
    for (size_t i = 0; i < ref.size(); i++) ref[i] = int16_t(rawBytes[i * 2] | rawBytes[i * 2 + 1] << 8);

    AdpcmWav wav;
    check(adpcmOpen(wavPath, wav), "the .wav opens as mono MS ADPCM");
    check(wav.sampleRate == 22050 && wav.channels == 1 && wav.blockAlign == 1024, "22050 Hz mono, 1024-byte blocks");
    std::printf("adpcm: %d samples per block, %ld samples in the file, ffmpeg decoded %zu\n", wav.samplesPerBlock,
                wav.totalSamples, ref.size());
    check(wav.totalSamples >= long(ref.size()) && wav.totalSamples - long(ref.size()) < wav.samplesPerBlock,
          "sample count matches ffmpeg's (within the last block)");

    std::vector<int16_t> out(ref.size());
    const int got = adpcmRead(wav, out.data(), int(out.size()));
    long mismatches = 0;
    for (size_t i = 0; i < ref.size(); i++) mismatches += out[i] != ref[i];
    std::printf("adpcm: %d samples read, %ld differ from ffmpeg\n", got, mismatches);
    check(got == int(ref.size()) && mismatches == 0, "decoded samples identical to ffmpeg's decoder");

    // past the end of the data the track starts over
    std::vector<int16_t> tail(size_t(wav.totalSamples - long(ref.size())) + 1000);
    adpcmRead(wav, tail.data(), int(tail.size()));
    bool loops = true;
    for (int i = 0; i < 1000; i++) loops = loops && tail[tail.size() - 1000 + size_t(i)] == ref[size_t(i)];
    check(loops, "reading past the end loops to the first samples");
    adpcmClose(wav);

    // the mixer at 22050 Hz: a sound effect comes out sample for sample
    check(Audio::kRate == 22050, "SF2000 mixer rate 22050 Hz");
    Manifest manifest;
    check(loadManifest(dataDir + "manifest.txt", manifest), "manifest loads");
    Audio audio;
    audio.init(false);
    check(audio.loadBank(manifest, dataDir), "all sounds load");
    SoundData buck;
    check(loadSound(dataDir + "sounds/chicken_move_0.snd", buck), "chicken_move_0 loads");
    audio.play("chicken_move_0");
    std::vector<int16_t> mixed(buck.samples.size() + 100);
    audio.mix(mixed.data(), int(mixed.size()));
    bool exact = true;
    for (size_t i = 0; i < buck.samples.size(); i++) exact = exact && mixed[i] == buck.samples[i];
    check(exact && mixed[mixed.size() - 1] == 0 && audio.playingCount() == 0, "sound effect passes through unchanged");
    audio.setMasterVolume(0);
    audio.play("chicken_move_0");
    audio.mix(mixed.data(), 500);
    bool silent = true;
    for (int i = 0; i < 500; i++) silent = silent && mixed[size_t(i)] == 0;
    check(silent, "master volume 0 is silent");
    audio.stopAll();

    // music: the first buffer swaps the track in, then it fades in over 0.25 s; buffer k (from the second) holds
    // samples k * 735 onwards
    check(audio.playMusic(wavPath), "playMusic opens the .wav");
    audio.setMusicVolume(1);
    std::vector<int16_t> buffer(735);
    audio.mix(buffer.data(), 735);
    for (int k = 0; k < 20; k++) audio.mix(buffer.data(), 735);
    bool full = true;
    for (int i = 0; i < 735; i++) full = full && buffer[size_t(i)] == ref[size_t(19 * 735 + i)];
    check(full, "music at full volume after the fade-in equals the decoded track");
    {
        // volume 0 pauses (no reading from the card): a track paused for 40 buffers resumes exactly where one paused
        // for a single sample does
        Audio x, y;
        x.init(false), y.init(false);
        x.loadBank(manifest, dataDir), y.loadBank(manifest, dataDir);
        x.playMusic(wavPath), y.playMusic(wavPath);
        x.setMusicVolume(1), y.setMusicVolume(1);
        std::vector<int16_t> bx(735), by(735);
        for (int k = 0; k < 10; k++) x.mix(bx.data(), 735), y.mix(by.data(), 735);
        x.setMusicVolume(0), y.setMusicVolume(0);
        for (int k = 0; k < 40; k++) x.mix(bx.data(), 735);
        y.mix(by.data(), 1);
        x.setMusicVolume(1), y.setMusicVolume(1);
        bool same = true, heard = false;
        for (int k = 0; k < 20; k++) {
            x.mix(bx.data(), 735), y.mix(by.data(), 735);
            same = same && bx == by;
            for (int16_t s : bx) heard = heard || s != 0;
        }
        check(same && heard, "music volume 0 pauses the track, raising it resumes");
    }
    audio.stopMusic();
    for (int k = 0; k < 20; k++) audio.mix(buffer.data(), 735);
    bool quiet = true;
    for (int i = 0; i < 735; i++) quiet = quiet && buffer[size_t(i)] == 0;
    check(quiet, "stopMusic fades to silence");

    std::printf("test_sw_audio: %d checks, %d failures\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
