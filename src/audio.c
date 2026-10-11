/* Tiny software synthesizer for MSKman's sound effects and jingle. */
#include "audio.h"

#include <SDL.h>
#include <math.h>
#include <string.h>

#define RATE 44100
#define MAX_VOICES 8
#define TWO_PI 6.28318530718

typedef struct {
    int active;
    int snd;
    Uint32 t;      /* samples since start */
    double ph[2];  /* oscillator phases (0..1) */
} Voice;

static SDL_AudioDeviceID dev;
static Voice voices[MAX_VOICES];
static int muted;
static float siren_speed = 1.0f;

static float sq(double p) { return p - floor(p) < 0.5 ? 1.0f : -1.0f; }
static float tri(double p)
{
    double f = p - floor(p);
    return (float)(f < 0.5 ? 4.0 * f - 1.0 : 3.0 - 4.0 * f);
}

static double midi_hz(int n) { return 440.0 * pow(2.0, (n - 69) / 12.0); }

/* Original start jingle: lead melody in eighth notes plus a bass line. */
static const int JINGLE_LEAD[] = {72, 76, 79, 84, 83, 79, 76, 79, 77, 81, 84, 81,
                                  79, 76, 72, 76, 74, 77, 81, 79, 76, 74, 72, 72};
static const int JINGLE_BASS[] = {48, 48, 52, 52, 53, 53, 48, 48, 50, 50, 43, 48};
#define JINGLE_NOTE 0.17
#define JINGLE_LEN (24 * JINGLE_NOTE)

/* Advance one oscillator by frequency f and return its new phase. */
static double osc(Voice *v, int i, double f)
{
    v->ph[i] += f / RATE;
    if (v->ph[i] > 1e6)
        v->ph[i] -= floor(v->ph[i]);
    return v->ph[i];
}

/* Produce one sample for a voice; clears v->active when a one-shot ends. */
static float synth(Voice *v)
{
    double s = (double)v->t / RATE;
    float out = 0.0f;
    v->t++;

    switch (v->snd) {
    case SND_START: {
        if (s >= JINGLE_LEN) {
            v->active = 0;
            break;
        }
        int n = (int)(s / JINGLE_NOTE);
        double in_note = s - n * JINGLE_NOTE;
        double env = in_note < 0.13 ? 1.0 - in_note * 3.0 : 0.0;
        if (n == 23)
            env = 1.0 - (s - 23 * JINGLE_NOTE) / JINGLE_NOTE;
        out += 0.13f * (float)env * sq(osc(v, 0, midi_hz(JINGLE_LEAD[n])));
        int b = n / 2;
        double in_bass = s - b * 2 * JINGLE_NOTE;
        double benv = in_bass < 0.3 ? 1.0 : 0.4;
        out += 0.22f * (float)benv * tri(osc(v, 1, midi_hz(JINGLE_BASS[b])));
        break;
    }
    case SND_CHOMP_A:
    case SND_CHOMP_B: {
        const double len = 0.11;
        if (s >= len) {
            v->active = 0;
            break;
        }
        double k = s / len;
        if (v->snd == SND_CHOMP_B)
            k = 1.0 - k;
        out = 0.22f * tri(osc(v, 0, 240.0 + 520.0 * k));
        break;
    }
    case SND_SIREN: {
        double period = 0.42 / siren_speed;
        double k = tri(s / period) * 0.5 + 0.5;
        out = 0.07f * (float)sin(TWO_PI * osc(v, 0, 360.0 + 260.0 * k + 60.0 * (siren_speed - 1.0)));
        break;
    }
    case SND_POWER: {
        double k = s * 7.5 - floor(s * 7.5);
        out = 0.06f * sq(osc(v, 0, 170.0 + 420.0 * k));
        break;
    }
    case SND_EAT_GHOST: {
        const double len = 0.38;
        if (s >= len) {
            v->active = 0;
            break;
        }
        double k = s / len;
        out = 0.14f * sq(osc(v, 0, 140.0 + 1500.0 * k * k));
        break;
    }
    case SND_FRUIT: {
        static const int notes[] = {72, 76, 79, 84, 79, 84};
        const double step = 0.05;
        if (s >= 6 * step) {
            v->active = 0;
            break;
        }
        out = 0.12f * sq(osc(v, 0, midi_hz(notes[(int)(s / step)])));
        break;
    }
    case SND_EXTRA: {
        if (s >= 1.0) {
            v->active = 0;
            break;
        }
        double k = s * 8.0 - floor(s * 8.0);
        out = k < 0.5 ? 0.12f * sq(osc(v, 0, 1046.5)) : 0.0f;
        break;
    }
    case SND_DEATH: {
        if (s >= 1.65) {
            v->active = 0;
            break;
        }
        if (s < 1.2) {
            double f = 820.0 * (1.0 - s / 1.25) + 70.0 * sin(TWO_PI * 11.0 * s) + 90.0;
            out = 0.16f * tri(osc(v, 0, f)) * (float)(1.0 - s / 2.0);
        } else {
            double p = s < 1.42 ? s - 1.3 : s - 1.5;
            if (p >= 0.0 && p < 0.07)
                out = 0.18f * sq(osc(v, 0, 120.0 + 5000.0 * p));
        }
        break;
    }
    default:
        v->active = 0;
    }
    return out;
}

static void callback(void *userdata, Uint8 *stream, int len)
{
    (void)userdata;
    Sint16 *buf = (Sint16 *)stream;
    int n = len / (int)sizeof(Sint16);
    for (int i = 0; i < n; i++) {
        float mix = 0.0f;
        for (int k = 0; k < MAX_VOICES; k++)
            if (voices[k].active)
                mix += synth(&voices[k]);
        if (muted)
            mix = 0.0f;
        if (mix > 1.0f)
            mix = 1.0f;
        if (mix < -1.0f)
            mix = -1.0f;
        buf[i] = (Sint16)(mix * 30000.0f);
    }
}

int audio_init(int enabled)
{
    muted = !enabled;
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0)
        return 0;
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 1;
    want.samples = 1024;
    want.callback = callback;
    dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (!dev)
        return 0;
    SDL_PauseAudioDevice(dev, 0);
    return 1;
}

void audio_quit(void)
{
    if (dev)
        SDL_CloseAudioDevice(dev);
    dev = 0;
}

static void start_voice(int snd)
{
    Voice *slot = NULL;
    for (int k = 0; k < MAX_VOICES; k++)
        if (voices[k].active && voices[k].snd == snd)
            slot = &voices[k];
    for (int k = 0; !slot && k < MAX_VOICES; k++)
        if (!voices[k].active)
            slot = &voices[k];
    if (!slot)
        return;
    memset(slot, 0, sizeof *slot);
    slot->snd = snd;
    slot->active = 1;
}

void audio_play(int snd)
{
    if (!dev)
        return;
    SDL_LockAudioDevice(dev);
    /* Chomps alternate; never let both overlap. */
    if (snd == SND_CHOMP_A || snd == SND_CHOMP_B)
        for (int k = 0; k < MAX_VOICES; k++)
            if (voices[k].snd == SND_CHOMP_A || voices[k].snd == SND_CHOMP_B)
                voices[k].active = 0;
    start_voice(snd);
    SDL_UnlockAudioDevice(dev);
}

void audio_loop(int snd, int on)
{
    if (!dev)
        return;
    SDL_LockAudioDevice(dev);
    int playing = 0;
    for (int k = 0; k < MAX_VOICES; k++) {
        if (voices[k].active && voices[k].snd == snd) {
            playing = 1;
            if (!on)
                voices[k].active = 0;
        }
    }
    if (on && !playing)
        start_voice(snd);
    SDL_UnlockAudioDevice(dev);
}

void audio_stop_all(void)
{
    if (!dev)
        return;
    SDL_LockAudioDevice(dev);
    for (int k = 0; k < MAX_VOICES; k++)
        voices[k].active = 0;
    SDL_UnlockAudioDevice(dev);
}

void audio_set_siren_speed(float speed) { siren_speed = speed; }

int audio_toggle_mute(void)
{
    muted = !muted;
    return muted;
}
