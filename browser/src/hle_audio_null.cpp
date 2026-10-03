// Vita3K emulator project
// SPDX-License-Identifier: GPL-2.0-or-later
// Browser null audio sink: Limbo opens audio ports during boot but the web
// port has no audio device (no SDL/cubeb in the HLE runtime). This adapter
// accepts ports and discards output non-blockingly so the game's audio
// thread paces on buffer completion instead of hanging on a host device.
// Production SceAudio bodies (validation, port registry, volumes) run
// unchanged; only the device layer is a sink.
#include <audio/state.h>
#include "thread_bridge.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <climits>
#include <cstdlib>
#include <new>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#ifdef __EMSCRIPTEN__
#include <emscripten/emscripten.h>
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
#include <emscripten/threading.h>
#endif

// Web Audio handoff (contract in browser/src/vita_runtime.h): forward one
// int16-interleaved PCM buffer per Output call. The scratch copy is ordinary
// Wasm heap, so JS can read it under both memory models; the guest buffer is
// never passed to JS directly. The page must copy the view synchronously.
// `port` tells concurrently open ports apart: each is its own stream, which
// the page mixes (a movie's port plays alongside the game's mixer port).
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
[[maybe_unused]] static void vita3k_web_post_audio_hook(int freq, int channels, int frames, const uint8_t *ptr, int bytes, int port) {
    browser::coordinator_call("audio", {uint64_t(freq), uint64_t(channels), uint64_t(frames), reinterpret_cast<uintptr_t>(ptr), uint64_t(bytes), uint64_t(port)});
}
#else
EM_JS(void, vita3k_web_post_audio_hook, (int freq, int channels, int frames, const uint8_t *ptr, int bytes, int port), {
    if (typeof vita3kWebOnAudio === 'function')
        vita3kWebOnAudio(freq, channels, frames, Module['vita3kHostBytes'](ptr, bytes), port);
});
#endif
#else
static void vita3k_web_post_audio_hook(int, int, int, const uint8_t *, int, int) {}
#endif

#ifdef __EMSCRIPTEN_SHARED_MEMORY__
// Threaded build (THREADS.md, Audio): every port owns a PCM ring in shared
// memory that browser/web/audio_ring_worklet.js reads on the audio thread.
// Positions are monotonic frame counts; the capacity is a power of two, so
// `position % capacity` stays correct when they wrap.
struct AudioRing {
    std::atomic<uint32_t> write; // frames this port produced
    std::atomic<uint32_t> read; // frames the worklet consumed (futex word)
    uint32_t capacity, channels, freq;
    std::atomic<uint32_t> generation; // bumped on reuse: an old reader stops
    std::atomic<uint32_t> closed;
    uint32_t reserved;
    int16_t *samples() { return reinterpret_cast<int16_t *>(this + 1); }
};
static_assert(sizeof(AudioRing) == 32, "audio_ring_worklet.js reads this layout");

// Rings are recycled, never freed: a page reader may still hold one.
struct AudioRingPool {
    std::mutex mutex;
    std::vector<AudioRing *> idle;
    AudioRing *take(uint32_t capacity, uint32_t channels, uint32_t freq) {
        AudioRing *ring = nullptr;
        {
            const std::lock_guard<std::mutex> guard(mutex);
            for (auto it = idle.begin(); it != idle.end(); ++it)
                if ((*it)->capacity == capacity && (*it)->channels == channels) {
                    ring = *it;
                    idle.erase(it);
                    break;
                }
        }
        if (!ring) {
            void *memory = std::aligned_alloc(16, sizeof(AudioRing) + size_t(capacity) * channels * sizeof(int16_t));
            ring = new (memory) AudioRing{};
            ring->capacity = capacity;
            ring->channels = channels;
        }
        ring->freq = freq;
        ring->write = 0;
        ring->read = 0;
        ring->closed = 0;
        ring->generation.fetch_add(1);
        return ring;
    }
    void give(AudioRing *ring) {
        ring->closed = 1;
        const std::lock_guard<std::mutex> guard(mutex);
        idle.push_back(ring);
    }
};
static AudioRingPool &audio_rings() {
    static AudioRingPool pool;
    return pool;
}
#endif

struct NullAudioPort : AudioOutPort {
    std::mutex mutex;
    std::vector<uint8_t> pcm;
    std::chrono::steady_clock::time_point next_output{};
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
    AudioRing *ring = nullptr;
    ~NullAudioPort() override {
        if (ring)
            audio_rings().give(ring);
    }
#endif
};

struct NullAudioAdapter : AudioAdapter {
    explicit NullAudioAdapter(AudioState &audio_state)
        : AudioAdapter(audio_state) {}

    bool init() override { return true; }

    AudioOutPortPtr open_port(int nb_channels, int freq, int nb_sample) override {
        auto port = std::make_shared<NullAudioPort>();
        port->len_bytes = nb_sample * nb_channels * static_cast<int>(sizeof(int16_t));
        port->len_microseconds = (static_cast<uint64_t>(nb_sample) * 1000000) / static_cast<uint64_t>(freq);
        return port;
    }

    void audio_output(AudioOutPort &out_port, const void *buffer) override {
        auto &port = static_cast<NullAudioPort &>(out_port);
        const std::lock_guard<std::mutex> guard(port.mutex);
#ifdef __EMSCRIPTEN_SHARED_MEMORY__
        ring_output(port, buffer);
        return;
#endif
        auto &pcm = port.pcm;
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
        vita3k_web_post_audio_hook(out_port.freq, channels, static_cast<int>(frames), pcm.data(), static_cast<int>(bytes),
            static_cast<int>(reinterpret_cast<std::uintptr_t>(&out_port) & 0x7fffffff));
    }

#ifdef __EMSCRIPTEN_SHARED_MEMORY__
    // The ring paces this thread from the page's audio clock: it waits for
    // room, which the worklet frees as it plays. Until audio runs (no user
    // gesture yet), or if it stalls, the wall clock paces instead and the
    // chunk is dropped, so the guest never waits on a silent page.
    static void ring_output(NullAudioPort &port, const void *buffer) {
        if (!buffer || port.len_bytes <= 0 || port.freq <= 0)
            return;
        const uint32_t channels = port.mode == 1 ? 2 : 1; // SceAudioOutMode STEREO
        const uint32_t frames = uint32_t(port.len_bytes) / (channels * sizeof(int16_t));
        if (frames == 0)
            return;
        if (!port.ring || port.ring->channels != channels || port.ring->freq != uint32_t(port.freq)
            || port.ring->capacity < 4 * frames) {
            if (port.ring)
                audio_rings().give(port.ring);
            const uint32_t capacity = std::bit_ceil(std::max<uint32_t>(4 * frames, 2048));
            port.ring = audio_rings().take(capacity, channels, uint32_t(port.freq));
            browser::coordinator_call("audio-ring", {reinterpret_cast<uintptr_t>(port.ring), capacity, channels,
                uint64_t(port.freq), port.ring->generation.load(), reinterpret_cast<uintptr_t>(&port) & 0x7fffffff});
        }
        AudioRing &ring = *port.ring;
        const auto duration = std::chrono::microseconds(port.len_microseconds);
        auto now = std::chrono::steady_clock::now();
        if (port.next_output.time_since_epoch().count() == 0 || now > port.next_output + 4 * duration)
            port.next_output = now;
        port.next_output += duration;
        // One buffer past the wall-clock schedule means nobody is playing.
        const auto deadline = port.next_output + duration;
        const uint32_t write = ring.write.load(std::memory_order_relaxed);
        while (true) {
            const uint32_t read = ring.read.load(std::memory_order_acquire);
            if (ring.capacity - (write - read) >= frames)
                break;
            now = std::chrono::steady_clock::now();
            if (now >= deadline || port.stopping)
                return;
            const double wait_ms = std::chrono::duration<double, std::milli>(deadline - now).count();
            emscripten_futex_wait(reinterpret_cast<volatile void *>(&ring.read), read, wait_ms);
        }
        const auto *input = static_cast<const int16_t *>(buffer);
        const uint32_t mask = ring.capacity - 1;
        const uint32_t first = std::min(frames, ring.capacity - (write & mask));
        std::memcpy(ring.samples() + size_t(write & mask) * channels, input, size_t(first) * channels * sizeof(int16_t));
        std::memcpy(ring.samples(), input + size_t(first) * channels, size_t(frames - first) * channels * sizeof(int16_t));
        ring.write.store(write + frames, std::memory_order_release);
    }
#endif

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
