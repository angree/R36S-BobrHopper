// SF2000 side of Audio (audio.h): there is no audio thread - the core calls mix() itself from retro_run and hands the
// samples to audio_batch_cb, so the device hooks have nothing to open and nothing to lock.
#include "engine/audio.h"

namespace cr {

bool Audio::init(bool) { return true; }

void Audio::shutdown() {}

void Audio::lockDevice() {}

void Audio::unlockDevice() {}

void Audio::callback(void *user, uint8_t *stream, int len)
{
    static_cast<Audio *>(user)->mix(reinterpret_cast<int16_t *>(stream), len / 2);
}

} // namespace cr
