#ifndef MSKMAN_AUDIO_H
#define MSKMAN_AUDIO_H

/* All sounds are synthesized at runtime; no sample files are shipped. */
enum {
    SND_START,
    SND_CHOMP_A,
    SND_CHOMP_B,
    SND_SIREN,   /* looping */
    SND_POWER,   /* looping */
    SND_EAT_GHOST,
    SND_FRUIT,
    SND_EXTRA,
    SND_DEATH,
    SND_COUNT
};

int audio_init(int enabled);
void audio_quit(void);
void audio_play(int snd);
void audio_loop(int snd, int on);
void audio_stop_all(void);
void audio_set_siren_speed(float speed);
int audio_toggle_mute(void);

#endif
