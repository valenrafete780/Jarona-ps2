/*
 * Mezclador de audio por software (portable, sin dependencias de la PS2).
 *
 * La musica es UNA voz que nunca se detiene ni se pausa: los efectos se suman encima
 * (hasta MIX_VOICES a la vez, incluso el mismo efecto solapado consigo mismo).
 * Todo se mezcla a 22050 Hz mono y se entrega a 44100 Hz estereo.
 */
#ifndef MIXER_H
#define MIXER_H
#include <stdint.h>

#define MIX_RATE   22050
#define MIX_VOICES 8

enum { SFX_JARONA, SFX_ULTIMO, SFX_PODER, SFX_OMEGA, SFX_COUNT };

void     mixer_init(void);              /* reinicia todo */
void     mixer_ensure_init(void);       /* solo si aun no se inicio (lo llaman main_ps2 y game_load) */
void     mixer_set_sfx(int id, const int16_t *pcm, uint32_t samples);
/* Musica y pista secreta vienen en mu-law de 8 bits (la mitad de datos que cargar): 'samples' = numero de bytes */
void     mixer_set_music(const uint8_t *ulaw, uint32_t samples);
void     mixer_set_secret(const uint8_t *ulaw, uint32_t samples);

void     mixer_music_start(void);       /* empieza la musica en bucle (fundido de entrada 1.5 s) */
void     mixer_play_sfx(int id);        /* suena encima de la musica, sin tocarla */
void     mixer_enter_secret(void);      /* escena secreta: calla musica y efectos, suena "secreto" en bucle */

uint32_t mixer_sfx_ms(int id);          /* duracion de un efecto */
int      mixer_has_secret(void);

/* Rellena 'frames' tramas estereo a 44100 Hz (frames debe ser par). Se llama desde el hilo de audio. */
void     mixer_render(int16_t *out, uint32_t frames);

/* Para pruebas: tamano total de muestras mezcladas hasta ahora */
uint64_t mixer_samples_rendered(void);
#endif
