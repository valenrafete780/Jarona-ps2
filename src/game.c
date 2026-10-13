/*
 * JARONA! - port a PS2 de la pagina web (index.html).
 *
 * Este fichero es un port fiel del <script> principal: las mismas fases (tap1 -> busy -> omega <-> normal),
 * los mismos tiempos y la misma escena secreta. Todo el dibujo pasa por platform.h y todo el sonido por mixer.h,
 * asi que la misma logica corre en la PS2 y en PC (para pruebas).
 *
 * Diferencia importante con la pagina: aqui el audio SI se mezcla de verdad. La musica es una voz que nunca
 * se pausa y los efectos suenan encima (en la pagina para PS4 la musica callaba durante cada efecto porque
 * el navegador solo deja sonar un <audio> a la vez).
 */
#include "game.h"
#include "platform.h"
#include "mixer.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- ajustes de la secuencia (copiados de la pagina) ---------------- */
#define TAPS_TO_TRANSFORM 30
#define FRAME_MS          100
#define FREEZE_MS         3500
#define FADE_MS           2400
#define IMPLODE_MS        800
#define IMPLODE_GAP_MS    150
#define CHAR_FIRST_MS     600
#define CHAR_GAP_MS       750
#define POP_MS            350
#define HOLD_MS           900
#define ORBIT_SECONDS     5.5f
#define SWAPS_TO_TRIGGER  8
#define SWAP_WINDOW_MS    5000
#define POSE_MS           450
#define OM_COUNT          45
#define OM_MS             65
#define N_CHARS           6
#define N_POWER           5
#define MAX_TREE          32
#define RX                21.0f   /* radios de la orbita, en "pixeles de sprite" */
#define RY                26.0f
#define PI_F              3.14159265f

/* tamano en pantalla de cada personaje que orbita (data-w / data-h de la pagina) */
static const float CHAR_W[N_CHARS] = { 16.45f, 8.71f, 17.0f, 11.24f, 10.85f, 13.77f };
static const float CHAR_H[N_CHARS] = { 17.0f, 17.0f, 11.33f, 17.0f, 17.0f, 17.0f };

/* ---------------- estado ---------------- */
typedef enum { G_LOADING, G_START, G_PLAY, G_ERROR } GState;
typedef enum { P_TAP1, P_BUSY, P_OMEGA, P_NORMAL } Phase;

typedef struct { int id, w, h; } Tex;

static GState gstate = G_LOADING;
static Phase  phase = P_TAP1;
static int    taps;
static int    powering;
static int    secret_on;
static int32_t g_now;                 /* ms del fotograma actual */
static int32_t g_prev;
static float  U = 3.0f;               /* cada pixel de sprite = U pixeles de pantalla */
static int    SW, SH;                 /* tamano de pantalla */

static Tex t_flowery, t_pose, t_power, t_char[N_CHARS], t_omega, t_tree, t_floor;
static const uint8_t *pw_frames[N_POWER], *om_frames[OM_COUNT];   /* fotogramas en RAM; en VRAM solo el visible */
static int pw_cur = -1, om_cur = -1;
static Tex t_title, t_loading, t_hint, t_start, t_start_p, t_btnA, t_btnA_p, t_btnB, t_btnB_p;
/* escena secreta (TREE.DAT): fotogramas de 8 bits + paleta; se expande a RGBA solo el fotograma visible */
static const uint8_t *tree_idx[MAX_TREE];
static uint8_t tree_pal[256 * 4];
static uint8_t *tree_rgba;
static int tree_count, tree_cur = -1, tree_w, tree_h;
static int32_t tree_ms[MAX_TREE];

/* visibilidad de capas (equivale a los atributos hidden de la pagina) */
static int flowery_hidden, pose_hidden = 1, slide_hidden = 1, orbit_hidden = 1, floor_hover;
static int power_idx = -1;            /* fotograma visible de la transformacion, -1 = ninguno */
static int32_t pose_due = -1;
static int push_down;
static float down_amt;
static int32_t hop_t0 = -1;
static float flash_op; static int flash_on;
static float slide_x;
static int omega_frame; static int32_t omega_next;
static int32_t omega_t0, hover_t0;
static int swap_locked = 1, swap_held; static int32_t swap_pop_t0 = -1; static int swap_to_omega_label;
static int32_t swap_times[16]; static int swap_n;
static int32_t orbit_start, imp_start = -1;
static int32_t secret_t0;
static float load_frac;

/* ---------------- temporizadores (setTimeout) ---------------- */
enum { EV_POWERUP, EV_POWER_STEP, EV_TRANSFORM, EV_IMPLODE };
typedef struct { int active; int32_t due; int id; } Ev;
static Ev evs[16];

static void after(int32_t ms, int id)
{
    for (int i = 0; i < 16; i++) if (!evs[i].active) { evs[i].active = 1; evs[i].due = g_now + ms; evs[i].id = id; return; }
}

/* ---------------- tweens (animaciones por fotogramas) ---------------- */
enum { TW_FLASH_IN, TW_FLASH_OUT, TW_SLIDE, TW_COUNT };
typedef struct { int active; int32_t t0, ms; int ease; } Tween;
static Tween tw[TW_COUNT];

/* curvas cubic-bezier como en CSS */
typedef struct { float x1, y1, x2, y2; } Bez;
static const Bez BEZ[] = {
    {0.42f, 0, 1, 1},          /* 0 ease-in    */
    {0, 0, 0.58f, 1},          /* 1 ease-out   */
    {0.2f, 0.7f, 0.3f, 1},     /* 2 slide      */
    {0.42f, 0, 0.58f, 1},      /* 3 ease-in-out*/
    {0.2f, 1.4f, 0.4f, 1},     /* 4 pop        */
    {0.25f, 0.1f, 0.25f, 1},   /* 5 ease (CSS) */
};
enum { E_IN, E_OUT, E_SLIDE, E_INOUT, E_POP, E_EASE };

static float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }

static float ease(int kind, float x)
{
    const Bez *b = &BEZ[kind];
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    float lo = 0, hi = 1, t = x;
    for (int i = 0; i < 24; i++) {
        float m = 1 - t;
        float v = 3 * m * m * t * b->x1 + 3 * m * t * t * b->x2 + t * t * t;
        if (fabsf(v - x) < 0.00001f) break;
        if (v < x) lo = t; else hi = t;
        t = (lo + hi) / 2;
    }
    float m = 1 - t;
    return 3 * m * m * t * b->y1 + 3 * m * t * t * b->y2 + t * t * t;
}

static float easeOutBack(float p) { float c1 = 1.70158f, c3 = c1 + 1; float q = p - 1; return 1 + c3 * q * q * q + c1 * q * q; }

static void tween_start(int k, int32_t ms, int e) { tw[k].active = 1; tw[k].t0 = g_now; tw[k].ms = ms; tw[k].ease = e; }

/* ---------------- audio ---------------- */
static void snd_play(int id) { mixer_play_sfx(id); }
static int32_t snd_dur(int id, int32_t fb) { uint32_t d = mixer_sfx_ms(id); return d ? (int32_t)d : fb; }

/* ---------------- carga ---------------- */
static const uint8_t *texdat; static uint32_t texdat_n;

typedef struct { char name[16]; uint32_t w, h, off, size; } Entry;

static const Entry *find_entry(const char *name)
{
    if (!texdat) return NULL;
    uint32_t n; memcpy(&n, texdat + 4, 4);
    const Entry *e = (const Entry *)(texdat + 8);
    for (uint32_t i = 0; i < n; i++) if (strncmp(e[i].name, name, 16) == 0) return &e[i];
    return NULL;
}

static int mk(const char *name, Tex *t, int smooth)
{
    const Entry *e = find_entry(name);
    if (!e) { fprintf(stderr, "falta la textura %s\n", name); return -1; }
    t->w = (int)e->w; t->h = (int)e->h;
    t->id = gfx_tex_create(t->w, t->h, texdat + e->off, smooth);
    return t->id < 0 ? -1 : 0;
}

static void draw_bar_screen(int show_title, float frac);

/* ---- carga en segundo plano (hilo aparte): musica (4 MB), pista secreta (1.3 MB) y arbol secreto (0.7 MB) ---- */
static const char *const BG_FILES[3] = { "MUSICA.ULW", "SECRETO.ULW", "TREE.DAT" };
static int bg_done[3];
static int music_wanted, music_loaded, tree_ready;

static void tree_init(const uint8_t *d, uint32_t size);

static void bg_poll(void)
{
    for (int i = 0; i < 3; i++) {
        if (bg_done[i]) continue;
        void *data = NULL; uint32_t size = 0;
        int r = plat_async_poll(i, &data, &size);
        if (r == 0) break;                                   /* se cargan en orden: si esta no esta, las siguientes tampoco */
        bg_done[i] = 1;
        if (r < 0 || !data) { plat_log("fallo cargando un fichero"); continue; }
        if (i == 0) { plat_log("musica lista"); mixer_set_music((const uint8_t *)data, size); music_loaded = 1; if (music_wanted) mixer_music_start(); }
        else if (i == 1) { mixer_set_secret((const uint8_t *)data, size); plat_log("pista secreta lista"); }
        else tree_init((const uint8_t *)data, size);
    }
}

typedef struct { const char *name; float weight; } Item;
static const Item ITEMS[] = {
    { "TEX.DAT", 3.7f }, { "JARONA.PCM", 0.06f }, { "ULTIMO.PCM", 0.15f }, { "PODER.PCM", 0.2f }, { "OMEGA.PCM", 0.2f },
};
#define N_ITEMS ((int)(sizeof ITEMS / sizeof ITEMS[0]))
static float item_base, item_weight, total_weight;
static int   cur_show_title;

static void on_progress(uint32_t done, uint32_t total)
{
    float f = total ? (float)done / (float)total : 1.0f;
    load_frac = (item_base + item_weight * f) / total_weight;
    draw_bar_screen(cur_show_title, load_frac);
}

static void *load_item(int i, uint32_t *size)
{
    item_weight = ITEMS[i].weight;
    return plat_load(ITEMS[i].name, size, on_progress);
}

int game_load(void)
{
    SW = gfx_width(); SH = gfx_height();
    total_weight = 0; for (int i = 0; i < N_ITEMS; i++) total_weight += ITEMS[i].weight;

    /* 1) texturas */
    item_base = 0; cur_show_title = 0;
    draw_bar_screen(0, 0);
    uint32_t sz;
    texdat = load_item(0, &sz); texdat_n = sz;
    if (!texdat || sz < 8 || memcmp(texdat, "JTEX", 4) != 0) { gstate = G_ERROR; return -1; }
    item_base += ITEMS[0].weight;

    int bad = 0;
    bad |= mk("title", &t_title, 0); bad |= mk("loading", &t_loading, 0); bad |= mk("hint", &t_hint, 0);
    bad |= mk("start", &t_start, 0); bad |= mk("start_p", &t_start_p, 0);
    bad |= mk("btnA", &t_btnA, 0); bad |= mk("btnA_p", &t_btnA_p, 0);
    bad |= mk("btnB", &t_btnB, 0); bad |= mk("btnB_p", &t_btnB_p, 0);
    bad |= mk("floor", &t_floor, 1);
    bad |= mk("flowery", &t_flowery, 0); bad |= mk("pose", &t_pose, 0);
    char nm[16];
    for (int i = 0; i < N_POWER; i++) {
        snprintf(nm, sizeof nm, "power%d", i);
        const Entry *e = find_entry(nm);
        if (!e) { bad = -1; break; }
        pw_frames[i] = texdat + e->off;
    }
    for (int i = 0; i < OM_COUNT; i++) {
        snprintf(nm, sizeof nm, "om%02d", i);
        const Entry *e = find_entry(nm);
        if (!e) { bad = -1; break; }
        om_frames[i] = texdat + e->off;
    }
    bad |= mk("power0", &t_power, 0); pw_cur = 0;
    bad |= mk("om00", &t_omega, 0);   om_cur = 0;
    for (int i = 0; i < N_CHARS; i++) { snprintf(nm, sizeof nm, "char%d", i); bad |= mk(nm, &t_char[i], i == 5); }
    if (bad) { gstate = G_ERROR; return -1; }

    /* 2) audio: efectos y musica por separado, listos para mezclarse */
    cur_show_title = 1;
    static const struct { const char *f; int id; } sfxs[] = {
        { "JARONA.PCM", SFX_JARONA }, { "ULTIMO.PCM", SFX_ULTIMO }, { "PODER.PCM", SFX_PODER }, { "OMEGA.PCM", SFX_OMEGA } };
    mixer_ensure_init();
    for (int i = 1; i < N_ITEMS; i++) {
        uint32_t n = 0;
        int16_t *pcm = (int16_t *)load_item(i, &n);
        if (!pcm) { fprintf(stderr, "falta %s\n", ITEMS[i].name); gstate = G_ERROR; return -1; }
        for (unsigned k = 0; k < sizeof sfxs / sizeof sfxs[0]; k++)
            if (strcmp(ITEMS[i].name, sfxs[k].f) == 0) mixer_set_sfx(sfxs[k].id, pcm, n / 2);
        item_base += ITEMS[i].weight;
        draw_bar_screen(1, item_base / total_weight);
    }
    draw_bar_screen(1, 1.0f);
    plat_async_begin(BG_FILES, 3);                           /* la musica y el arbol llegan mientras ya se ve el juego */
    gstate = G_START;
    return 0;
}

static void tree_expand(int f)
{
    const uint8_t *s = tree_idx[f];
    int n = tree_w * tree_h;
    for (int i = 0; i < n; i++) memcpy(tree_rgba + (size_t)i * 4, tree_pal + (size_t)s[i] * 4, 4);
    gfx_tex_update(t_tree.id, tree_rgba);
    tree_cur = f;
}

static void tree_init(const uint8_t *d, uint32_t size)
{
    uint32_t hdr[4];
    if (size < 20 || memcmp(d, "JTRE", 4) != 0) { plat_log("arbol: cabecera mala"); return; }
    memcpy(hdr, d + 4, 16);
    uint32_t count = hdr[0], w = hdr[1], h = hdr[2], ncol = hdr[3];
    if (count == 0 || count > MAX_TREE || ncol > 256 || w == 0 || h == 0) { plat_log("arbol: datos raros"); return; }
    uint32_t off = 20;
    if (size < off + count * 4 + ncol * 4 + count * w * h) { plat_log("arbol: fichero corto"); return; }
    for (uint32_t i = 0; i < count; i++) { uint32_t ms; memcpy(&ms, d + off + i * 4, 4); tree_ms[i] = (int32_t)ms; }
    off += count * 4;
    memset(tree_pal, 0, sizeof tree_pal);
    memcpy(tree_pal, d + off, ncol * 4);
    off += ncol * 4;
    for (uint32_t i = 0; i < count; i++) tree_idx[i] = d + off + (size_t)i * w * h;
    tree_w = (int)w; tree_h = (int)h; tree_count = (int)count;
    tree_rgba = (uint8_t *)malloc((size_t)w * h * 4);
    if (!tree_rgba) return;
    /* textura con el primer fotograma; luego se re-sube el que toque */
    for (uint32_t i = 0; i < w * h; i++) memcpy(tree_rgba + (size_t)i * 4, tree_pal + (size_t)tree_idx[0][i] * 4, 4);
    t_tree.w = tree_w; t_tree.h = tree_h;
    t_tree.id = gfx_tex_create(tree_w, tree_h, tree_rgba, 0);
    if (t_tree.id < 0) { plat_log("arbol: SIN VRAM"); return; }
    tree_cur = 0;
    tree_ready = 1;
    plat_log("arbol listo");
}

/* ---------------- dibujo: utilidades ---------------- */
#define BG_R 255
#define BG_G 216
#define BG_B 61

static void draw_tex_xywh(const Tex *t, float x, float y, float w, float h, float a)
{
    float c[8] = { x, y, x + w, y, x + w, y + h, x, y + h };
    gfx_quad(t->id, c, a);
}

static void draw_tex_scaled(const Tex *t, float cx, float cy, float s, float a)
{
    float w = t->w * s, h = t->h * s;
    draw_tex_xywh(t, cx - w / 2, cy - h / 2, w, h, a);
}

/* Pantalla de carga / inicio (igual que #loading de la pagina) */
static void draw_bar_screen(int show_title, float frac)
{
    if (SW == 0) { SW = gfx_width(); SH = gfx_height(); }
    gfx_begin(BG_R, BG_G, BG_B);
    float bw = SW * 0.7f; if (bw > 520) bw = 520;
    float bx = (SW - bw) / 2, by = SH * 0.38f;
    if (show_title) draw_tex_xywh(&t_title, (SW - t_title.w) / 2.0f, by - t_title.h - 28, (float)t_title.w, (float)t_title.h, 1);
    gfx_rect(bx, by, bw, 28, 59, 31, 0, 1);
    gfx_rect(bx + 4, by + 4, bw - 8, 20, 255, 243, 166, 1);
    gfx_rect(bx + 4, by + 4, (bw - 8) * clamp01(frac), 20, 59, 31, 0, 1);
    if (show_title) {
        if (frac >= 1.0f && gstate == G_START) { /* el boton lo dibuja game_frame */ }
        else draw_tex_xywh(&t_loading, (SW - t_loading.w) / 2.0f, by + 28 + 16, (float)t_loading.w, (float)t_loading.h, 1);
    }
    gfx_end();
}

/* ---------------- transformacion del grupo ".push" (salto / pulsado) ---------------- */
typedef struct { float ty, rot, sx, sy; } Xf;
static float push_x, push_y, push_w, push_h;       /* caja de .push en pantalla */
static Xf gxf;

static Xf xf_lerp(Xf a, Xf b, float t)
{
    Xf r = { a.ty + (b.ty - a.ty) * t, a.rot + (b.rot - a.rot) * t, a.sx + (b.sx - a.sx) * t, a.sy + (b.sy - a.sy) * t };
    return r;
}

static Xf compute_xf(void)
{
    float k = U / 6.4f;                              /* la pagina usa px de CSS con U ~ 6.4 */
    Xf id = { 0, 0, 1, 1 };
    Xf down = { 8 * k, 0, 1.04f, 0.94f };
    Xf peak = { -14 * k, -3.0f * PI_F / 180.0f, 0.98f, 1.03f };
    if (hop_t0 >= 0) {
        float ms = (float)(g_now - hop_t0);
        if (ms < 380) {
            if (ms < 171) return xf_lerp(down, peak, ease(E_OUT, ms / 171));
            return xf_lerp(peak, id, ease(E_OUT, (ms - 171) / (380 - 171)));
        }
        hop_t0 = -1;
    }
    return xf_lerp(id, down, down_amt);
}

/* Dibuja un sprite en coordenadas locales de .push aplicando el movimiento del grupo */
static void spr(const Tex *t, float lx, float ly, float lw, float lh, float alpha)
{
    float ox = push_w / 2, oy = push_h;              /* transform-origin: 50% 100% */
    float px[4] = { lx, lx + lw, lx + lw, lx }, py[4] = { ly, ly, ly + lh, ly + lh };
    float c[8], cs = cosf(gxf.rot), sn = sinf(gxf.rot);
    for (int i = 0; i < 4; i++) {
        float dx = (px[i] - ox) * gxf.sx, dy = (py[i] - oy) * gxf.sy;
        c[i * 2]     = push_x + ox + dx * cs - dy * sn;
        c[i * 2 + 1] = push_y + oy + dx * sn + dy * cs + gxf.ty;
    }
    gfx_quad(t->id, c, alpha);
}

/* ---------------- logica (port de las funciones del script) ---------------- */
static void show_stand(void)
{
    pose_due = -1;
    pose_hidden = 1;
    if (!powering && phase != P_OMEGA) flowery_hidden = 0;
}

static void show_pose(int hold)
{
    pose_due = -1;
    flowery_hidden = 1;
    pose_hidden = 0;
    if (!hold) pose_due = g_now + POSE_MS;
}

static void start_orbit(void)
{
    orbit_hidden = 0;
    imp_start = -1;
    orbit_start = g_now;
}

static void start_power_up(void)
{
    powering = 1;
    pose_due = -1;
    pose_hidden = 1;
    flowery_hidden = 1;
    power_idx = 0;                                   /* primer fotograma, congelado */
    start_orbit();                                   /* los personajes aparecen uno por uno */
    int32_t all_in = CHAR_FIRST_MS + (N_CHARS - 1) * CHAR_GAP_MS + POP_MS + HOLD_MS;
    after(all_in + FRAME_MS, EV_POWER_STEP);
}

static void unlock_swap(void)
{
    swap_locked = 0;
    swap_pop_t0 = g_now;
}

static void transform(void)
{
    snd_play(SFX_PODER);
    flash_on = 1;
    tween_start(TW_FLASH_IN, FADE_MS, E_IN);
    after(FADE_MS - IMPLODE_MS - IMPLODE_GAP_MS > 0 ? FADE_MS - IMPLODE_MS - IMPLODE_GAP_MS : 0, EV_IMPLODE);
}

/* con la pantalla en blanco se cambia el personaje */
static void flash_in_done(void)
{
    power_idx = -1;
    orbit_hidden = 1;
    slide_x = (float)SW;
    slide_hidden = 0;
    floor_hover = 1; hover_t0 = g_now;
    omega_t0 = g_now;
    flash_op = 1;
    tween_start(TW_FLASH_OUT, 900, E_OUT);
    int32_t T = snd_dur(SFX_PODER, 4500);
    int32_t dur = T - FADE_MS; if (dur < 1500) dur = 1500;
    tween_start(TW_SLIDE, dur, E_SLIDE);
}

static void slide_done(void)
{
    slide_x = 0;
    powering = 0;
    phase = P_OMEGA;
    snd_play(SFX_OMEGA);
    unlock_swap();
}

static void open_secret(void)
{
    if (secret_on) return;
    if (!mixer_has_secret() || !tree_ready) {
        static int said;
        if (!said) { said = 1; plat_log(mixer_has_secret() ? "secreto: falta el arbol" : "secreto: falta el audio"); }
        return;
    }
    secret_on = 1;                                   /* no hay salida: solo reiniciando la consola */
    mixer_enter_secret();                            /* calla musica y efectos, suena "secreto" en bucle */
    secret_t0 = g_now;
}

static void swapped(void)
{
    int n = 0;
    swap_times[swap_n < 16 ? swap_n : 15] = g_now; if (swap_n < 16) swap_n++;
    for (int i = 0; i < swap_n; i++) if (g_now - swap_times[i] <= SWAP_WINDOW_MS) swap_times[n++] = swap_times[i];
    swap_n = n;
    if (swap_n >= SWAPS_TO_TRIGGER) { swap_n = 0; open_secret(); }
}

static void hop(void) { hop_t0 = g_now; }

static void do_swap(void)
{
    if (phase == P_OMEGA) {
        phase = P_NORMAL;
        slide_hidden = 1;
        flowery_hidden = 0;
        floor_hover = 0;
        swap_to_omega_label = 1;                     /* "Cambiar a Omega Flowery" */
    } else if (phase == P_NORMAL) {
        phase = P_OMEGA;
        pose_due = -1;
        pose_hidden = 1;
        flowery_hidden = 1;
        slide_hidden = 0;
        floor_hover = 1; hover_t0 = g_now;
        swap_to_omega_label = 0;                     /* "Cambiar a Flowery" */
    } else return;
    hop();
    swapped();
}

static int on_press(int from_pointer)
{
    if (phase == P_TAP1 || phase == P_NORMAL) show_pose(from_pointer);
    if (phase == P_TAP1) {
        taps++;
        if (taps >= TAPS_TO_TRANSFORM) {
            phase = P_BUSY;
            snd_play(SFX_JARONA);
            after(1350, EV_POWERUP);                 /* empieza al terminar este clip */
        } else snd_play(SFX_JARONA);
        return 1;
    }
    if (phase == P_OMEGA) { snd_play(SFX_ULTIMO); return 1; }
    if (phase == P_NORMAL) { snd_play(SFX_JARONA); return 1; }
    return 0;
}

static void tap_down(void)
{
    hop_t0 = -1;
    if (on_press(1)) push_down = 1;
}

static void tap_up(void)
{
    if (push_down) {
        push_down = 0;
        down_amt = 1;
        hop();
        if (!pose_hidden) pose_due = g_now + POSE_MS;
    }
}

static void run_event(int id)
{
    switch (id) {
    case EV_POWERUP:   start_power_up(); break;
    case EV_POWER_STEP:
        power_idx++;
        if (power_idx < N_POWER - 1) after(FRAME_MS, EV_POWER_STEP);
        else after(FREEZE_MS, EV_TRANSFORM);
        break;
    case EV_TRANSFORM: transform(); break;
    case EV_IMPLODE:   imp_start = g_now; break;
    }
}

static void update(const GameInput *in)
{
    /* entrada */
    if (!secret_on) {
        if (in->tap_down) tap_down();
        if (in->tap_up) tap_up();
        if (in->swap_down && !swap_locked) { swap_held = 1; do_swap(); }
    }
    if (in->swap_up) swap_held = 0;

    /* temporizadores vencidos, en orden */
    for (int guard = 0; guard < 64; guard++) {
        int best = -1;
        for (int i = 0; i < 16; i++) if (evs[i].active && evs[i].due <= g_now && (best < 0 || evs[i].due < evs[best].due)) best = i;
        if (best < 0) break;
        int id = evs[best].id; evs[best].active = 0;
        run_event(id);
    }
    if (pose_due >= 0 && g_now >= pose_due) show_stand();

    /* tweens */
    for (int k = 0; k < TW_COUNT; k++) {
        if (!tw[k].active) continue;
        float p = clamp01((float)(g_now - tw[k].t0) / (float)tw[k].ms);
        float e = ease(tw[k].ease, p);
        if (k == TW_FLASH_IN) flash_op = e;
        else if (k == TW_FLASH_OUT) flash_op = 1 - e;
        else if (k == TW_SLIDE) slide_x = (float)SW * (1 - e);
        if (p >= 1) {
            tw[k].active = 0;
            if (k == TW_FLASH_IN) flash_in_done();
            else if (k == TW_FLASH_OUT) { flash_op = 0; flash_on = 0; }
            else if (k == TW_SLIDE) slide_done();
        }
    }

    /* pulsado suave (.08 s) */
    float dt = (float)(g_now - g_prev);
    if (push_down) { down_amt += dt / 80.0f; if (down_amt > 1) down_amt = 1; }
    else if (hop_t0 < 0) { down_amt -= dt / 80.0f; if (down_amt < 0) down_amt = 0; }

    /* fotogramas de Omega (setInterval 65 ms, solo si esta visible) */
    while (g_now >= omega_next) {
        if (!slide_hidden) omega_frame = (omega_frame + 1) % OM_COUNT;
        omega_next += OM_MS;
        if (omega_next < g_now - 1000) omega_next = g_now;
    }
}

/* ---------------- dibujo de la escena ---------------- */
static float alt_ease(int32_t t0)   /* animacion "alternate infinite" de 2.6 s con ease-in-out */
{
    float t = (float)(g_now - t0) / 2600.0f;
    t = fmodf(t, 2.0f);
    float p = t < 1 ? t : 2 - t;
    return ease(E_INOUT, p);
}

static void draw_orbit(float cx, float cy)
{
    if (orbit_hidden) return;
    float now = (float)g_now;
    float t = now - (float)orbit_start;
    float om = (2 * PI_F) / ORBIT_SECONDS;
    float q = imp_start < 0 ? 0.0f : clamp01((now - (float)imp_start) / IMPLODE_MS);
    for (int i = 0; i < N_CHARS; i++) {
        float p = clamp01((t - (CHAR_FIRST_MS + i * CHAR_GAP_MS)) / POP_MS);
        if (p == 0 || q >= 1) continue;
        float ang = (-90 + i * 60) * PI_F / 180 + om * t / 1000 + q * q * PI_F * 1.5f;
        float r = 1 - q * q * q;
        float x = cosf(ang) * RX * U * r, y = sinf(ang) * RY * U * r;
        float depth = 1 + 0.12f * sinf(ang);
        float s = easeOutBack(p) * depth * (1 - 0.6f * q);
        float op = (p * 2 > 1 ? 1 : p * 2) * (q > 0.85f ? (1 - q) / 0.15f : 1);
        float w = CHAR_W[i] * U * s, h = CHAR_H[i] * U * s;
        draw_tex_xywh(&t_char[i], cx + x - w / 2, cy + y - h / 2, w, h, op);
    }
}

static void draw_scene(void)
{
    gfx_begin(BG_R, BG_G, BG_B);

    /* disposicion vertical: titulo, escenario, suelo, boton (centrado en pantalla) */
    float title_h = (float)t_title.h, stage_h = 84 * U;
    float total = title_h + 6 + stage_h + 4 + 18 + 12 + (float)t_btnA.h;
    float top = ((float)SH - total - 22) / 2; if (top < 2) top = 2;
    float cx = SW / 2.0f;
    push_w = 60 * U; push_h = stage_h;
    push_x = cx - push_w / 2; push_y = top + title_h + 6;

    draw_tex_xywh(&t_title, cx - t_title.w / 2.0f, top, (float)t_title.w, (float)t_title.h, 1);

    /* sombra del suelo */
    {
        float fw = 34 * U, fh = 18, fs = 1, fo = 1;
        if (floor_hover) { float e = alt_ease(hover_t0); fs = 1 - 0.22f * e; fo = 1 - 0.35f * e; }
        draw_tex_scaled(&t_floor, cx, push_y + push_h + 4 + fh / 2, fs * fw / (float)t_floor.w, fo);
    }

    /* grupo .push */
    gxf = compute_xf();
    if (!flowery_hidden) spr(&t_flowery, (60 - 22) / 2.0f * U, (84 - 60) * U, 22 * U, 60 * U, 1);
    if (!pose_hidden)    spr(&t_pose, (60 - 41) / 2.0f * U, (84 - 53) * U, 41 * U, 53 * U, 1);
    if (power_idx >= 0) {
        if (power_idx != pw_cur) { gfx_tex_update(t_power.id, pw_frames[power_idx]); pw_cur = power_idx; }
        spr(&t_power, (60 - 33) / 2.0f * U, (84 - 73) * U, 33 * U, 73 * U, 1);
    }
    if (!slide_hidden) {
        float fy = -14 * (U / 6.4f) * alt_ease(omega_t0);          /* .omega { animation: floaty } */
        if (omega_frame != om_cur) { gfx_tex_update(t_omega.id, om_frames[omega_frame]); om_cur = omega_frame; }
        spr(&t_omega, (60 - 40) / 2.0f * U + slide_x, (84 - 3 - 80) * U + fy, 40 * U, 80 * U, 1);
    }

    /* fundido a blanco, y por encima de el los personajes que orbitan */
    if (flash_on && flash_op > 0) gfx_rect(0, 0, (float)SW, (float)SH, 255, 255, 255, flash_op);
    draw_orbit(cx, push_y + 48 * U);

    /* boton "Cambiar a ..." (oculto hasta que Omega termina de entrar) */
    if (!swap_locked) {
        const Tex *b = swap_to_omega_label ? (swap_held ? &t_btnA_p : &t_btnA) : (swap_held ? &t_btnB_p : &t_btnB);
        float s = 1, a = 1;
        if (swap_pop_t0 >= 0) {
            float p = clamp01((float)(g_now - swap_pop_t0) / 450.0f), e = ease(E_POP, p);
            s = 0.5f + 0.5f * e; a = clamp01(e);
            if (p >= 1) swap_pop_t0 = -1;
        }
        draw_tex_scaled(b, cx, push_y + push_h + 4 + 18 + 12 + b->h / 2.0f, s, a);
    }
    draw_tex_xywh(&t_hint, cx - t_hint.w / 2.0f, (float)SH - t_hint.h - 4, (float)t_hint.w, (float)t_hint.h, 1);

    /* escena secreta: arbol sobre fondo negro, aparece en .5 s y no tiene salida */
    if (secret_on) {
        float a = ease(E_EASE, clamp01((float)(g_now - secret_t0) / 500.0f));
        gfx_rect(0, 0, (float)SW, (float)SH, 0, 0, 0, a);
        int32_t total_ms = 0; for (int i = 0; i < tree_count; i++) total_ms += tree_ms[i];
        int32_t m = (g_now - secret_t0) % total_ms, f = 0;
        while (f < tree_count - 1 && m >= tree_ms[f]) { m -= tree_ms[f]; f++; }
        if (f != tree_cur) tree_expand(f);
        float s = 560.0f / t_tree.w; float sh = ((float)SH - 8) / t_tree.h; if (sh < s) s = sh;
        s = floorf(s * 2) / 2;                                      /* escala en pasos de 0.5: pixeles nitidos */
        draw_tex_scaled(&t_tree, SW / 2.0f, SH / 2.0f, s, a);
    }
    gfx_end();
}

static void draw_start_screen(void)
{
    gfx_begin(BG_R, BG_G, BG_B);
    float bw = SW * 0.7f; if (bw > 520) bw = 520;
    float bx = (SW - bw) / 2, by = SH * 0.38f;
    draw_tex_xywh(&t_title, (SW - t_title.w) / 2.0f, by - t_title.h - 28, (float)t_title.w, (float)t_title.h, 1);
    gfx_rect(bx, by, bw, 28, 59, 31, 0, 1);
    gfx_rect(bx + 4, by + 4, bw - 8, 20, 59, 31, 0, 1);
    const Tex *b = (g_now / 400) % 2 ? &t_start_p : &t_start;      /* el boton "respira" para que se vea que espera */
    draw_tex_xywh(b, (SW - b->w) / 2.0f, by + 28 + 22.0f, (float)b->w, (float)b->h, 1);
    gfx_end();
}

void game_frame(uint32_t now_ms, const GameInput *in)
{
    g_prev = g_now; g_now = (int32_t)now_ms;
    if (SW == 0) { SW = gfx_width(); SH = gfx_height(); }
    if (gstate == G_START || gstate == G_PLAY) bg_poll();
    switch (gstate) {
    case G_START:
        draw_start_screen();
        if (in->start_down || in->tap_down) {
            gstate = G_PLAY;
            music_wanted = 1;
            if (music_loaded) mixer_music_start();                 /* la musica empieza aqui y ya no se detiene */
            omega_next = g_now + OM_MS;
            g_prev = g_now;
        }
        break;
    case G_PLAY:
        update(in);
        draw_scene();
        break;
    default:   /* G_ERROR / G_LOADING: pantalla roja */
        gfx_begin(160, 0, 0);
        gfx_end();
        break;
    }
}

const char *game_debug_state(void)
{
    static char b[160];
    static const char *ph[] = { "tap1", "busy", "omega", "normal" };
    snprintf(b, sizeof b, "%s taps=%d power=%d flash=%.2f slide=%.0f secret=%d swaplocked=%d",
             ph[phase], taps, power_idx, flash_op, slide_x, secret_on, swap_locked);
    return b;
}
