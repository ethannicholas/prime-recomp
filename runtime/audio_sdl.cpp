// SDL device output for the portable mix in audio.cpp. Desktop only; Android uses
// audio_aaudio.cpp, which is the same handful of lines against a different API.
#include "runtime.h"
#include <SDL.h>

void audio_render(int16_t* out, int frames);
void audio_open_wav();

static constexpr int OUT_RATE = 48000;
static SDL_AudioDeviceID g_dev;

static void audio_callback(void*, uint8_t* stream, int len) {
    audio_render((int16_t*)stream, len / 4);
}

bool audio_open() {
    audio_open_wav();
    SDL_AudioSpec want{}, have{};
    want.freq = OUT_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    want.callback = audio_callback;
    g_dev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (!g_dev) {
        LOG(LOG_AI, "audio: cannot open device: %s", SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(g_dev, 0);
    return true;
}
