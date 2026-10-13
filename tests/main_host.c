/* Plataforma de PRUEBA para PC: renderizador por software + entrada simulada.
 * Ejecuta game.c con los datos reales y guarda capturas (PPM) de momentos clave. */
#include "../src/platform.h"
#include "../src/game.h"
#include "../src/mixer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define W 640
#define H 448
static uint8_t fb[W * H * 3];
typedef struct { int w, h, smooth; uint8_t *px; } HT;
static HT tex[256]; static int ntex;
static int frame_no; static const char *dump_name;

int gfx_width(void) { return W; }
int gfx_height(void) { return H; }
int gfx_tex_create(int w, int h, const uint8_t *rgba, int smooth)
{
    HT *t = &tex[ntex]; t->w = w; t->h = h; t->smooth = smooth; t->px = malloc((size_t)w * h * 4); memcpy(t->px, rgba, (size_t)w * h * 4);
    return ntex++;
}
void gfx_tex_update(int id, const uint8_t *rgba) { memcpy(tex[id].px, rgba, (size_t)tex[id].w * tex[id].h * 4); }
void gfx_begin(uint8_t r, uint8_t g, uint8_t b) { for (int i = 0; i < W * H; i++) { fb[i * 3] = r; fb[i * 3 + 1] = g; fb[i * 3 + 2] = b; } }
static void blend(int x, int y, float r, float g, float b, float a)
{
    if (x < 0 || y < 0 || x >= W || y >= H || a <= 0) return;
    uint8_t *p = &fb[(y * W + x) * 3];
    p[0] = (uint8_t)(p[0] + (r - p[0]) * a); p[1] = (uint8_t)(p[1] + (g - p[1]) * a); p[2] = (uint8_t)(p[2] + (b - p[2]) * a);
}
void gfx_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, float a)
{
    for (int j = (int)floorf(y + .5f); j < (int)floorf(y + h + .5f); j++)
        for (int i = (int)floorf(x + .5f); i < (int)floorf(x + w + .5f); i++) blend(i, j, r, g, b, a);
}
static void tri(HT *t, const float *v, const float *uv, float alpha)
{
    float minx = fminf(v[0], fminf(v[2], v[4])), maxx = fmaxf(v[0], fmaxf(v[2], v[4]));
    float miny = fminf(v[1], fminf(v[3], v[5])), maxy = fmaxf(v[1], fmaxf(v[3], v[5]));
    float d = (v[3] - v[5]) * (v[0] - v[4]) + (v[4] - v[2]) * (v[1] - v[5]);
    if (fabsf(d) < 1e-6f) return;
    for (int y = (int)floorf(miny); y <= (int)ceilf(maxy); y++)
        for (int x = (int)floorf(minx); x <= (int)ceilf(maxx); x++) {
            float px = x + .5f, py = y + .5f;
            float a = ((v[3] - v[5]) * (px - v[4]) + (v[4] - v[2]) * (py - v[5])) / d;
            float b = ((v[5] - v[1]) * (px - v[4]) + (v[0] - v[4]) * (py - v[5])) / d;
            float c = 1 - a - b;
            if (a < 0 || b < 0 || c < 0) continue;
            float u = a * uv[0] + b * uv[2] + c * uv[4], w = a * uv[1] + b * uv[3] + c * uv[5];
            int ix = (int)floorf(u), iy = (int)floorf(w);
            if (ix < 0) ix = 0; if (iy < 0) iy = 0; if (ix >= t->w) ix = t->w - 1; if (iy >= t->h) iy = t->h - 1;
            uint8_t *s = &t->px[(iy * t->w + ix) * 4];
            blend(x, y, s[0], s[1], s[2], s[3] / 255.0f * alpha);
        }
}
void gfx_quad(int id, const float c[8], float alpha)
{
    HT *t = &tex[id]; float W_ = (float)t->w, H_ = (float)t->h;
    float t1[6] = { c[0], c[1], c[2], c[3], c[4], c[5] }, u1[6] = { 0, 0, W_, 0, W_, H_ };
    float t2[6] = { c[0], c[1], c[4], c[5], c[6], c[7] }, u2[6] = { 0, 0, W_, H_, 0, H_ };
    tri(t, t1, u1, alpha); tri(t, t2, u2, alpha);
}
void gfx_end(void)
{
    frame_no++;
    if (dump_name) {
        FILE *f = fopen(dump_name, "wb"); fprintf(f, "P6\n%d %d\n255\n", W, H); fwrite(fb, 1, sizeof fb, f); fclose(f); dump_name = NULL;
    }
}
void *plat_load(const char *name, uint32_t *size, void (*progress)(uint32_t, uint32_t))
{
    char p[256]; snprintf(p, sizeof p, "../data/%s", name);
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint8_t *b = malloc(n); long done = 0;
    while (done < n) { long k = n - done > 262144 ? 262144 : n - done; if (fread(b + done, 1, k, f) != (size_t)k) break; done += k; if (progress) progress(done, n); }
    fclose(f); *size = n; return b;
}

/* ---- guion de entrada ---- */
static int snap_n;
static void snap(const char *tag) { static char nm[64]; snprintf(nm, sizeof nm, "/tmp/snap_%02d_%s.ppm", snap_n++, tag); dump_name = nm; }

int main(void)
{
    if (game_load() != 0) { fprintf(stderr, "game_load fallo\n"); return 1; }
    const double FRAME = 1001.0 / 60.0;
    uint32_t now = 0; GameInput in; memset(&in, 0, sizeof in);
    static int16_t ablk[2048]; FILE *wav = fopen("/tmp/session.raw", "wb");
    /* eventos: tiempo (ms), tipo */
    int taps_fired = 0, up_pending = 0; uint32_t up_at = 0;
    int swaps = 0; uint32_t next_swap = 0, swap_up_at = 0; int swap_pending = 0;
    int snapped_pose = 0, snapped_orbit = 0, snapped_flash = 0, snapped_om = 0, snapped_btn = 0, snapped_sec = 0, snapped_pow = 0, snapped_slide = 0;
    uint32_t last_log = 0; double audio_acc = 0;
    for (int fr = 0; fr < 60 * 75; fr++) {
        now = (uint32_t)(fr * FRAME);
        memset(&in, 0, sizeof in);
        if (fr == 20) in.start_down = 1;
        const char *st = game_debug_state();
        uint32_t t0 = 1000;                                   /* los toques empiezan a 1 s */
        if (now >= t0 && taps_fired < 30 && now >= t0 + (uint32_t)taps_fired * 150) { in.tap_down = 1; up_at = now + 60; up_pending = 1; taps_fired++; }
        if (up_pending && now >= up_at && !in.tap_down) { in.tap_up = 1; up_pending = 0; }
        if (strstr(st, "swaplocked=0") && !next_swap) next_swap = now + 4500;   /* primero se deja ver Omega y el boton */
        if (next_swap && (strstr(st, "omega") == st || strstr(st, "normal") == st) && strstr(st, "swaplocked=0") && swaps < 8 && now >= next_swap && !swap_pending && !strstr(st, "secret=1")) {
            in.swap_down = 1; swap_pending = 1; swap_up_at = now + 40; swaps++; next_swap = now + 400;
        }
        if (swap_pending && now >= swap_up_at && !in.swap_down) { in.swap_up = 1; swap_pending = 0; }
        if (taps_fired == 3 && !snapped_pose && now > t0 + 300) { snap("pose"); snapped_pose = 1; }
        game_frame(now, &in);
        st = game_debug_state();
        if (now - last_log >= 5000) { last_log = now; printf("t=%5.1fs  %s\n", now / 1000.0, st); }
        if (strstr(st, "busy") && strstr(st, "power=1") && !snapped_pow) { snap("poder_f1_con_orbita"); snapped_pow = 1; }
        if (strstr(st, "busy") && strstr(st, "power=0") && !snapped_orbit && now > 9500) { snap("orbita"); snapped_orbit = 1; }
        if (strstr(st, "flash=0.5") && !snapped_flash) { snap("fundido"); snapped_flash = 1; }
        if (strstr(st, "slide=3") && !snapped_slide) { snap("omega_entrando"); snapped_slide = 1; }
        if (strstr(st, "omega") == st && strstr(st, "swaplocked=0") && !snapped_om) { snap("omega"); snapped_om = 1; }
        if (strstr(st, "omega") == st && strstr(st, "swaplocked=0") && !snapped_btn && now > 0 && snapped_om && swaps == 0 && now > 23000) { snap("omega_boton"); snapped_btn = 1; }
        if (strstr(st, "secret=1") && !snapped_sec) { snap("secreto_ini"); snapped_sec = 1; }
        /* audio simulado: 735 muestras a 44.1 k por fotograma */
        audio_acc += FRAME * 44.1;
        while (audio_acc >= 1024) { mixer_render(ablk, 1024); fwrite(ablk, 2, 2048, wav); audio_acc -= 1024; }
        if (snapped_sec && strstr(st, "secret=1") && now > 0) {
            static uint32_t sec_now; if (!sec_now) sec_now = now;
            if (now - sec_now > 1200 && snap_n < 40) { snap("secreto"); sec_now = now + 100000; }
        }
    }
    fclose(wav);
    printf("snapshots: %d, taps: %d, swaps: %d\n", snap_n, taps_fired, swaps);
    return 0;
}
