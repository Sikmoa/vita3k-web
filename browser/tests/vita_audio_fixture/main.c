// Web Audio audibility probe: genuine VitaSDK homebrew that outputs a 440 Hz
// square wave (stereo int16, 48 kHz, 40 x 1024-frame buffers) through the
// production sceAudioOut HLE path, then exits with code 7. The wave is
// full-scale by construction: anything downstream that zeroes, drops, mutes,
// or misroutes PCM turns this run silent, and the runner asserts peak.
//
// Built with the web runtime (target vita3k_vita_audio_fixture). No libm:
// square wave, integer phase.
#include <psp2/audioout.h>
#include <stdint.h>

#define FRAMES 1024
#define CHUNKS 40
#define PERIOD (48000u / 440u) // ~440.4 Hz at 48 kHz

static int16_t pcm[FRAMES * 2];

int main(void) {
    int port = sceAudioOutOpenPort(SCE_AUDIO_OUT_PORT_TYPE_BGM, FRAMES, 48000, SCE_AUDIO_OUT_MODE_STEREO);
    if (port < 0)
        return 1;
    uint32_t phase = 0;
    for (int c = 0; c < CHUNKS; c++) {
        for (int i = 0; i < FRAMES; i++) {
            const int16_t s = (phase < PERIOD / 2u) ? 16000 : -16000;
            pcm[i * 2] = s;
            pcm[i * 2 + 1] = s;
            if (++phase >= PERIOD)
                phase = 0;
        }
        if (sceAudioOutOutput(port, pcm) < 0) {
            sceAudioOutReleasePort(port);
            return 2;
        }
    }
    sceAudioOutReleasePort(port);
    return 7;
}
