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
#include <cstring>
#include <memory>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>

// Web Audio handoff (contract in browser/src/vita_runtime.h): forward one
// int16-interleaved PCM buffer per Output call. The scratch copy is ordinary
// Wasm heap, so JS can read it under both memory models; the guest buffer is
// never passed to JS directly. The page must copy the view synchronously.
EM_JS(void, vita3k_web_post_audio_hook, (int freq, int channels, int frames, const uint8_t *ptr, int bytes), {
    if (typeof vita3kWebOnAudio === 'function')
        vita3kWebOnAudio(freq, channels, frames, Module['vita3kHostBytes'](ptr, bytes));
});
#else
static void vita3k_web_post_audio_hook(int, int, int, const uint8_t *, int) {}
#endif

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

    void audio_output(AudioOutPort &out_port, const void *buffer) override {
        // Sink + Web Audio tap: copy the PCM out for the page, never block
        // the guest audio thread. buffer holds out_port.len_bytes of int16
        // interleaved samples (readable host memory, as the SDL backend
        // consumes it); the channel count comes from the port mode the
        // sceAudioOutOpenPort body sets before any Output call.
        if (!buffer || out_port.len_bytes <= 0)
            return;
        const int channels = (out_port.mode == 1) ? 2 : 1; // SceAudioOutMode STEREO
        const size_t bytes = static_cast<size_t>(out_port.len_bytes);
        const size_t frames = bytes / (static_cast<size_t>(channels) * sizeof(int16_t));
        if (frames == 0)
            return;
        if (pcm.size() != bytes)
            pcm.resize(bytes);
        std::memcpy(pcm.data(), buffer, bytes);
        vita3k_web_post_audio_hook(out_port.freq, channels, static_cast<int>(frames), pcm.data(), static_cast<int>(bytes));
    }

    int get_rest_sample(AudioOutPort & /*out_port*/) override {
        // Nothing buffered: the sink drains instantly.
        return 0;
    }

    std::vector<uint8_t> pcm; // PCM scratch for the synchronous JS copy-out
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
