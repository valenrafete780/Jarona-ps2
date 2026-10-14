#include "mixer.h"
#include <string.h>

#define MUSIC_GAIN    0.35f   /* igual que MUSIC_GAIN de la pagina */
#define MUSIC_FADEIN  1.5f    /* segundos */
#define SECRET_FX_OFF 0.3f    /* la pagina calla los efectos en 0.3 s al entrar en la escena secreta */
#define SECRET_MUS_OFF 0.5f   /* ...y la musica en 0.5 s */

typedef struct {
    const int16_t *d;
    uint32_t n;
    volatile uint32_t pos;
    volatile int on;
} Voice;

typedef struct { const int16_t *d; uint32_t n; } Clip;
typedef struct { const uint8_t *d; uint32_t n; } Clip8;     /* mu-law */

static Clip sfx[SFX_COUNT];
static Clip8 music, secret;
static int16_t ulaw_tab[256];
static int ulaw_ready, mixer_started;

static void build_ulaw(void)
{
    for (int i = 0; i < 256; i++) {                          /* G.711 mu-law -> PCM de 16 bits */
        int u = ~i & 0xFF;
        int t = ((u & 0x0F) << 3) + 0x84;
        t <<= (u & 0x70) >> 4;
        ulaw_tab[i] = (int16_t)((u & 0x80) ? (0x84 - t) : (t - 0x84));
    }
    ulaw_ready = 1;
}
static Voice voices[MIX_VOICES];
static uint32_t next_steal;

static volatile int music_on, secret_on, secret_silenced;
static uint32_t music_pos, secret_pos;
static float mgain, fxgain, sgain = 1.0f;      /* ganancias actuales */
static float mtarget, fxtarget = 1.0f;         /* ganancias objetivo */
static float mstep, fxstep;                    /* incremento por muestra */
static int16_t last_s;                          /* para interpolar 22 k -> 44 k */
static uint64_t rendered;

static inline void barrier(void) { __sync_synchronize(); }

void mixer_init(void)
{
    mixer_started = 1;
    if (!ulaw_ready) build_ulaw();
    memset(sfx, 0, sizeof sfx);
    memset(voices, 0, sizeof voices);
    memset(&music, 0, sizeof music);
    memset(&secret, 0, sizeof secret);
    music_on = secret_on = secret_silenced = 0;
    music_pos = secret_pos = 0;
    mgain = 0; mtarget = 0; mstep = 0;
    fxgain = fxtarget = 1.0f; fxstep = 0;
    sgain = 1.0f; last_s = 0; rendered = 0; next_steal = 0;
}

void mixer_ensure_init(void) { if (!mixer_started) mixer_init(); }

void mixer_set_sfx(int id, const int16_t *pcm, uint32_t n) { sfx[id].d = pcm; sfx[id].n = n; }
void mixer_set_music(const uint8_t *u, uint32_t n)         { music.n = 0; music.d = u; music.n = n; }
void mixer_set_secret(const uint8_t *u, uint32_t n)        { secret.n = 0; secret.d = u; secret.n = n; }

static void ramp_music(float to, float secs)
{
    mtarget = to;
    mstep = (to - mgain) / (secs * MIX_RATE);
}

void mixer_music_start(void)
{
    if (music_on || !music.d || !music.n) return;
    music_pos = 0;
    mgain = 0;
    ramp_music(secret_silenced ? 0.0f : MUSIC_GAIN, MUSIC_FADEIN);
    barrier();
    music_on = 1;
}

void mixer_play_sfx(int id)
{
    if (id < 0 || id >= SFX_COUNT || !sfx[id].d || !sfx[id].n) return;
    int slot = -1;
    for (int i = 0; i < MIX_VOICES; i++) if (!voices[i].on) { slot = i; break; }
    if (slot < 0) slot = (int)(next_steal++ % MIX_VOICES);   /* todas ocupadas: se roba la mas antigua */
    Voice *v = &voices[slot];
    v->on = 0;
    barrier();
    v->d = sfx[id].d; v->n = sfx[id].n; v->pos = 0;
    barrier();
    v->on = 1;
}

void mixer_enter_secret(void)
{
    if (secret_silenced) return;
    secret_silenced = 1;
    ramp_music(0.0f, SECRET_MUS_OFF);                /* calla la musica y los efectos aunque no haya pista secreta */
    fxtarget = 0.0f;
    fxstep = (0.0f - fxgain) / (SECRET_FX_OFF * MIX_RATE);
    if (secret.d && secret.n) {                      /* si la pista secreta esta cargada, suena en bucle */
        secret_pos = 0; sgain = 1.0f;
        barrier();
        secret_on = 1;
    }
}

uint32_t mixer_sfx_ms(int id) { return sfx[id].d ? (uint32_t)((uint64_t)sfx[id].n * 1000 / MIX_RATE) : 0; }
int mixer_has_secret(void) { return secret.d != 0 && secret.n != 0; }
uint64_t mixer_samples_rendered(void) { return rendered; }

static inline float approach(float cur, float target, float step)
{
    if (step == 0.0f) return target;
    cur += step;
    if ((step > 0 && cur >= target) || (step < 0 && cur <= target)) return target;
    return cur;
}

/* Una muestra mezclada a 22050 Hz */
static inline int16_t mix_one(void)
{
    float s = 0.0f;

    mgain  = approach(mgain, mtarget, mstep);
    fxgain = approach(fxgain, fxtarget, fxstep);

    if (music_on) {
        s += (float)ulaw_tab[music.d[music_pos]] * mgain;       /* la musica avanza SIEMPRE: nunca se pausa */
        if (++music_pos >= music.n) music_pos = 0;    /* bucle sin cortes */
    }
    for (int i = 0; i < MIX_VOICES; i++) {
        Voice *v = &voices[i];
        if (!v->on) continue;
        uint32_t p = v->pos;
        if (p >= v->n) { v->on = 0; continue; }
        s += (float)v->d[p] * fxgain;
        v->pos = p + 1;
    }
    if (secret_on) {
        s += (float)ulaw_tab[secret.d[secret_pos]] * sgain;
        if (++secret_pos >= secret.n) secret_pos = 0;
    }
    if (s > 32767.0f) s = 32767.0f;
    if (s < -32768.0f) s = -32768.0f;
    return (int16_t)s;
}

void mixer_render(int16_t *out, uint32_t frames)
{
    for (uint32_t i = 0; i < frames; i += 2) {
        int16_t cur = mix_one();
        int16_t mid = (int16_t)(((int)last_s + (int)cur) / 2);   /* interpolacion lineal x2 */
        out[i * 2 + 0] = mid; out[i * 2 + 1] = mid;
        out[i * 2 + 2] = cur; out[i * 2 + 3] = cur;
        last_s = cur;
        rendered++;
    }
}
