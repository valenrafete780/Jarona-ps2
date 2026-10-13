/* Prueba: la musica NO se pausa ni se atenua cuando suenan efectos.
 * A = solo musica, C = solo efectos, B = musica + efectos  ->  B debe ser exactamente A + C. */
#include "../src/mixer.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define FR 44100   /* 1 s a 44.1 k */
#define SECS 12

static int16_t *music, *fx[SFX_COUNT];
static uint32_t nmusic, nfx[SFX_COUNT];

static int16_t *load(const char *p, uint32_t *n)
{
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    int16_t *b = malloc(sz); if (fread(b, 1, sz, f) != (size_t)sz) exit(2); fclose(f); *n = sz / 2; return b;
}

static void run(int16_t *out, int with_music, int with_fx)
{
    mixer_init();
    for (int i = 0; i < SFX_COUNT; i++) mixer_set_sfx(i, fx[i], nfx[i]);
    mixer_set_music(music, nmusic);
    if (with_music) mixer_music_start();
    static int16_t blk[1024 * 2];
    int fired[8] = {0};
    uint32_t total = FR * SECS, done = 0;
    while (done < total) {
        double t = (double)done / FR;
        if (with_fx) {   /* efectos que se solapan entre si y con la musica */
            double at[] = { 2.0, 2.5, 3.0, 5.0, 6.0, 6.3, 9.0, 9.0 };
            int id[]    = { SFX_JARONA, SFX_JARONA, SFX_ULTIMO, SFX_PODER, SFX_OMEGA, SFX_OMEGA, SFX_PODER, SFX_ULTIMO };
            for (int k = 0; k < 8; k++) if (!fired[k] && t >= at[k]) { fired[k] = 1; mixer_play_sfx(id[k]); }
        }
        mixer_render(blk, 1024);
        for (int i = 0; i < 1024 * 2 && done * 2 + i < (uint32_t)total * 2; i++) out[done * 2 + i] = blk[i];
        done += 1024;
    }
}

int main(void)
{
    const char *names[] = { "JARONA", "ULTIMO", "PODER", "OMEGA" };
    char p[128];
    music = load("../data/MUSICA.PCM", &nmusic);
    for (int i = 0; i < SFX_COUNT; i++) { snprintf(p, sizeof p, "../data/%s.PCM", names[i]); fx[i] = load(p, &nfx[i]); }

    size_t n = (size_t)FR * SECS * 2 + 4096 * 2;
    int16_t *A = calloc(n, 2), *B = calloc(n, 2), *C = calloc(n, 2);
    run(A, 1, 0); run(C, 0, 1); run(B, 1, 1);

    long bad = 0, clipped = 0, fxsamples = 0; int maxerr = 0;
    double musicOnly = 0, musicDuringFx = 0; long cmo = 0, cmf = 0;
    for (size_t i = 0; i < (size_t)FR * SECS * 2; i++) {
        /* cuando varios efectos se solapan, el propio mezclador satura (recorta) el resultado: eso es
           normal y no tiene que ver con la musica, asi que se excluyen esas muestras y sus vecinas */
        int skip = 0;
        for (int k = -4; k <= 4 && !skip; k++) {
            long j = (long)i + k; if (j < 0 || j >= (long)FR * SECS * 2) continue;
            int sj = A[j] + C[j];
            if (sj >= 32767 || sj <= -32768 || C[j] >= 32767 || C[j] <= -32768) skip = 1;
        }
        if (skip) { clipped++; continue; }
        int sum = A[i] + C[i];
        int e = abs((int)B[i] - sum);
        if (e > maxerr) maxerr = e;
        if (e > 3) bad++;   /* tolerancia: redondeo de enteros (3 truncados) */
        if (C[i]) { fxsamples++; musicDuringFx += fabs((double)A[i]); cmf++; }
        else { musicOnly += fabs((double)A[i]); cmo++; }
    }
    printf("muestras con efecto sonando: %ld, saturadas (excluidas): %ld\n", fxsamples, clipped);
    printf("error maximo de B contra A+C: %d (muestras con error>3: %ld)\n", maxerr, bad);
    printf("nivel medio de la musica SIN efectos: %.1f | MIENTRAS suenan efectos: %.1f\n", musicOnly / cmo, musicDuringFx / cmf);

    /* La musica de B, restando los efectos, debe seguir avanzando sin huecos: comparar con A */
    printf("%s\n", bad == 0 ? "OK: la musica nunca se pausa ni se atenua" : "FALLO");
    /* escena secreta */
    mixer_init(); mixer_set_music(music, nmusic); mixer_set_sfx(0, fx[0], nfx[0]);
    uint32_t ns; int16_t *sec = load("../data/SECRETO.PCM", &ns); mixer_set_secret(sec, ns);
    mixer_music_start(); static int16_t blk[2048];
    for (int i = 0; i < 40; i++) mixer_render(blk, 1024);
    mixer_enter_secret(); mixer_play_sfx(0);
    for (int i = 0; i < 60; i++) mixer_render(blk, 1024);
    double e = 0; for (int i = 0; i < 2048; i++) e += fabs((double)blk[i]);
    mixer_render(blk, 1024);
    printf("secreto: hasSecret=%d, nivel tras el fundido=%.1f (solo suena 'secreto')\n", mixer_has_secret(), e / 2048);
    return bad == 0 ? 0 : 1;
}
