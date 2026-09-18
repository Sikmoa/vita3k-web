// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Browser null audio sink: Limbo opens audio ports during boot but the web
// port has no audio device (no SDL/cubeb in the HLE runtime). This adapter
// accepts ports and discards output non-blockingly so the game's audio
// thread paces on buffer completion instead of hanging on a host device.
// Production SceAudio bodies (validation, port registry, volumes) run
// unchanged; only the device layer is a sink.
#include <audio/state.h>

#include <chrono>
#include <cstdint>
#include <memory>

struct NullAudioAdapter : AudioAdapter {
    explicit NullAudioAdapter(AudioState &audio_state)
        : AudioAdapter(audio_state) {}

    bool init() override { return true; }

    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override {
        auto port = std::make_shared<AudioOutPort>();
        port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
        port->len_microseconds = (static_cast<uint64_t>(nb_sample) * 1000000) / static_cast<uint64_t>(freq);
        return port;
    }

    void audio_output(AudioOutPort & /*out_port*/, const void * /*buffer*/) override {
        // Sink: consume immediately, never block the guest audio thread.
    }

    int get_rest_sample(AudioOutPort & /*out_port*/) override {
        // Nothing buffered: the sink drains instantly.
        return 0;
    }
};

void vita3k_web_install_null_audio(AudioState &audio) {
    audio.adapter = std::make_unique<NullAudioAdapter>(audio);
    audio.audio_backend = "Null";
}

// AudioState device layer (subset of vita3k/audio/src/audio.cpp): the full
// TU requires the SDL/cubeb adapters, so the web runtime provides only the
// four methods the HLE audio path calls, all delegating to the adapter
// (always the null sink above in web builds).
AudioOutPortPtr AudioState::open_port(int nb_channels, int freq, int nb_sample) {
    AudioOutPortPtr port = adapter->open_port(nb_channels, freq, nb_sample);
    set_volume(*port, port->volume);
    return port;
}

void AudioState::audio_output(AudioOutPort &out_port, const void *buffer) {
    if (out_port.stopping)
        return;
    // No pacing sleep: upstream waits ~50% of the buffer duration to match
    // the host device clock, but a host sleep would block the browser's only
    // thread. The null sink drains instantly; last_output still advances so
    // any guest-side pacing math sees monotonic progress.
    adapter->audio_output(out_port, buffer);
    out_port.last_output = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch())
        .count());
}

void AudioState::set_volume(AudioOutPort &out_port, float volume) {
    out_port.volume = volume;
    adapter->set_volume(out_port, volume * global_volume);
}

int AudioState::get_rest_sample(AudioOutPort &out_port) {
    return adapter->get_rest_sample(out_port);
}
