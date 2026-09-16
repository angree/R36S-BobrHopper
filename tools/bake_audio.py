#!/usr/bin/env python3
"""wav/mp3 -> .snd baker via ffmpeg: mono, signed 16-bit, 22050 Hz, original loudness kept.

.snd (little endian):
  char[4] "CRS1"
  u32 sample_rate, u32 frame_count
  frame_count x s16 sample

Usage: python tools/bake_audio.py --all <upstream_assets_dir> <data_dir>
"""
import array
import os
import struct
import subprocess
import sys

RATE = 22050


PEAK = 0.97  # how close to full scale a normalised sound is allowed to get


def normalise(pcm):
    """O15: bring every sound up to the same peak. The generated takes come at wildly different levels - the
    beaver was barely audible next to the rest - so each one is scaled until its loudest sample nearly fills
    16 bits. Silence is left alone."""
    samples = array.array("h")
    samples.frombytes(pcm[:len(pcm) // 2 * 2])
    if sys.byteorder == "big":
        samples.byteswap()
    peak = max((abs(s) for s in samples), default=0)
    if peak == 0:
        return pcm, 1.0
    gain = PEAK * 32767.0 / peak
    if abs(gain - 1.0) < 0.01:
        return pcm, 1.0
    for i, s in enumerate(samples):
        v = int(s * gain)
        samples[i] = -32768 if v < -32768 else 32767 if v > 32767 else v
    if sys.byteorder == "big":
        samples.byteswap()
    return samples.tobytes(), gain


def bake(src, dst):
    pcm = subprocess.run(
        ["ffmpeg", "-v", "error", "-i", src, "-ac", "1", "-ar", str(RATE), "-f", "s16le", "-acodec", "pcm_s16le", "-"],
        check=True, capture_output=True).stdout
    pcm, gain = normalise(pcm)
    frames = len(pcm) // 2
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    with open(dst, "wb") as f:
        f.write(b"CRS1")
        f.write(struct.pack("<II", RATE, frames))
        f.write(pcm[:frames * 2])
    return frames, gain


def main(argv):
    if len(argv) == 3 and argv[0] == "--all":
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        from assets_manifest import SOUNDS, SOUNDS_EXTRA
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        for name, rel in list(SOUNDS.items()) + list(SOUNDS_EXTRA.items()):
            src = os.path.join(root, rel) if name in SOUNDS_EXTRA else os.path.join(argv[1], rel)
            # O15: a sound chosen in tools/sound_studio.py replaces whatever the manifest points at - the picks land
            # in assets_extra/sounds/<name>.mp3 and every one of them wins over the upstream file of the same name.
            # Without this the game kept playing the original recordings even after they had been replaced.
            for ext in (".mp3", ".wav"):
                own = os.path.join(root, "assets_extra", "sounds", name + ext)
                if os.path.exists(own):
                    src = own
                    break
            # a port sound that has not been chosen yet (tools/sound_studio.py) simply is not baked
            if name in SOUNDS_EXTRA and not os.path.exists(src):
                print(f"snd {name:20s} -- brak pliku, pomijam ({rel})")
                continue
            frames, gain = bake(src, os.path.join(argv[2], "sounds", name + ".snd"))
            print(f"snd {name:20s} {frames / RATE:6.3f}s  glosniej x{gain:.1f}")
        return 0
    print(__doc__)
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
