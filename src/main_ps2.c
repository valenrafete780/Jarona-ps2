/*
 * JARONA! para PS2 - capa de plataforma (PS2SDK + gsKit + audsrv + libpad).
 *
 *  - Graficos : gsKit, texturas RGBA32 con alfa, 640x448 (NTSC) o 640x512 (PAL)
 *  - Audio    : un hilo mezcla musica + efectos por software (mixer.c) y los manda a audsrv a 44.1 kHz estereo
 *  - Mando    : X = tocar a Flowery, Triangulo/Circulo = "Cambiar a ...", Start/X = empezar
 *  - Ficheros : cdrom0:\NOMBRE;1 (ISO), o la carpeta del .elf (mass:, host:, ...)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libpad.h>
#include <audsrv.h>

#include "platform.h"
#include "game.h"
#include "mixer.h"

/* modulos IOP incrustados en el ELF (los genera el Makefile con bin2c) */
extern unsigned char sio2man_irx[]; extern unsigned int size_sio2man_irx;
extern unsigned char padman_irx[];  extern unsigned int size_padman_irx;
extern unsigned char libsd_irx[];   extern unsigned int size_libsd_irx;
extern unsigned char audsrv_irx[];  extern unsigned int size_audsrv_irx;

extern void *_gp;

/* =============================== ficheros =============================== */
static char g_dir[128];   /* carpeta desde la que se lanzo el .elf, p.ej. "mass:/JARONA/" o "cdrom0:\" */

static void set_base_dir(const char *argv0)
{
    g_dir[0] = 0;
    if (!argv0) return;
    int last = -1;
    for (int i = 0; argv0[i]; i++) if (argv0[i] == '/' || argv0[i] == '\\' || argv0[i] == ':') last = i;
    if (last >= 0 && last < (int)sizeof g_dir - 1) { memcpy(g_dir, argv0, last + 1); g_dir[last + 1] = 0; }
}

static FILE *open_data(const char *name)
{
    char p[256];
    FILE *f;
    snprintf(p, sizeof p, "cdrom0:\\%s;1", name);        if ((f = fopen(p, "rb"))) return f;   /* ISO / disco */
    snprintf(p, sizeof p, "%s%s", g_dir, name);          if ((f = fopen(p, "rb"))) return f;
    snprintf(p, sizeof p, "%sDATA/%s", g_dir, name);     if ((f = fopen(p, "rb"))) return f;
    snprintf(p, sizeof p, "%sdata/%s", g_dir, name);     if ((f = fopen(p, "rb"))) return f;
    snprintf(p, sizeof p, "host:data/%s", name);         if ((f = fopen(p, "rb"))) return f;
    snprintf(p, sizeof p, "data/%s", name);              if ((f = fopen(p, "rb"))) return f;
    return NULL;
}

void *plat_load(const char *name, uint32_t *size, void (*progress)(uint32_t, uint32_t))
{
    FILE *f = open_data(name);
    if (!f) { printf("JARONA: no encuentro %s\n", name); return NULL; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *buf = (uint8_t *)memalign(64, (size_t)n + 64);
    if (!buf) { printf("JARONA: sin memoria para %s (%ld bytes)\n", name, n); fclose(f); return NULL; }
    long done = 0;
    while (done < n) {
        long k = n - done > 131072 ? 131072 : n - done;
        long r = (long)fread(buf + done, 1, (size_t)k, f);
        if (r <= 0) break;
        done += r;
        if (progress) progress((uint32_t)done, (uint32_t)n);
    }
    fclose(f);
    if (done != n) { free(buf); return NULL; }
    *size = (uint32_t)n;
    return buf;
}

/* =============================== graficos =============================== */
#define MAX_TEX 160

typedef struct { GSTEXTURE t; int w, h; u32 *mem; } PTex;
static volatile int pad_ready, audio_ready, audio_failed;   /* los ponen los hilos de inicio */
static volatile int audio_stage;              /* 0 hilo sin arrancar, 1 esperando a audsrv_init, 2 init hecho */
static int mod_state[10];                     /* modulos IOP: 0 sin intentar, 1 cargado, -1 fallo */
static GSGLOBAL *gs;
static PTex ptex[MAX_TEX];
static int  nptex;
static u32  vram_used;
#ifndef GSKIT_ALLOC_ERROR
#define GSKIT_ALLOC_ERROR 0xFFFFFFFFu
#endif

static int p2(int v) { int p = 8; while (p < v) p <<= 1; return p; }

static void gfx_init(void)
{
    gs = gsKit_init_global();
    gs->PSM = GS_PSM_CT32;
    gs->PSMZ = GS_PSMZ_16S;
    gs->ZBuffering = GS_SETTING_OFF;
    gs->DoubleBuffering = GS_SETTING_ON;
    gs->PrimAlphaEnable = GS_SETTING_ON;

    dmaKit_init(D_CTRL_RELE_OFF, D_CTRL_MFD_OFF, D_CTRL_STS_UNSPEC, D_CTRL_STD_OFF, D_CTRL_RCYC_8, 1 << DMA_CHANNEL_GIF);
    dmaKit_chan_init(DMA_CHANNEL_GIF);

    gsKit_init_screen(gs);
    gsKit_mode_switch(gs, GS_ONESHOT);
    gsKit_set_test(gs, GS_ZTEST_OFF);
    gsKit_set_primalpha(gs, GS_SETREG_ALPHA(0, 1, 0, 1, 0), 0);    /* mezcla normal: Cs*As + Cd*(1-As) */
}

int gfx_width(void)  { return gs->Width; }
int gfx_height(void) { return gs->Height; }

static void tex_fill(PTex *p, const uint8_t *rgba)
{
    int pw = p->t.Width;
    memset(p->mem, 0, (size_t)pw * p->t.Height * 4);
    for (int y = 0; y < p->h; y++) {
        const uint8_t *s = rgba + (size_t)y * p->w * 4;
        u32 *d = p->mem + (size_t)y * pw;
        for (int x = 0; x < p->w; x++, s += 4)
            d[x] = (u32)s[0] | ((u32)s[1] << 8) | ((u32)s[2] << 16) | ((u32)(s[3] >> 1) << 24);   /* alfa de la GS: 0..0x80 */
    }
}

int gfx_tex_create(int w, int h, const uint8_t *rgba, int smooth)
{
    if (nptex >= MAX_TEX) return -1;
    PTex *p = &ptex[nptex];
    memset(p, 0, sizeof *p);
    p->w = w; p->h = h;
    p->t.Width = p2(w); p->t.Height = p2(h);                         /* la GS pide potencias de 2: se rellena con transparente */
    p->t.PSM = GS_PSM_CT32;
    p->t.Filter = smooth ? GS_FILTER_LINEAR : GS_FILTER_NEAREST;
    p->mem = (u32 *)memalign(128, (size_t)p->t.Width * p->t.Height * 4);
    if (!p->mem) return -1;
    p->t.Mem = p->mem;
    tex_fill(p, rgba);
    u32 sz = gsKit_texture_size(p->t.Width, p->t.Height, p->t.PSM);
    p->t.Vram = gsKit_vram_alloc(gs, sz, GSKIT_ALLOC_USERBUFFER);
    vram_used += sz;
    if (p->t.Vram == GSKIT_ALLOC_ERROR) {
        printf("JARONA: SIN VRAM en la textura %d (%dx%d), usado hasta ahora %u bytes\n", nptex, p->w, p->h, (unsigned)vram_used);
        return -1;                                                    /* game_load lo detecta: pantalla roja, sin colgar la GS */
    }
    gsKit_texture_upload(gs, &p->t);
    return nptex++;
}

void gfx_tex_update(int id, const uint8_t *rgba)
{
    PTex *p = &ptex[id];
    tex_fill(p, rgba);
    gsKit_texture_upload(gs, &p->t);
}

void gfx_begin(uint8_t r, uint8_t g, uint8_t b)
{
    /* OJO: alfa 0x80 (opaco). gsKit_clear dibuja un rectangulo y, con la mezcla alfa activada, alfa 0 no pintaba nada. */
    gsKit_clear(gs, GS_SETREG_RGBAQ(r, g, b, 0x80, 0x00));
}

static u64 col(uint8_t r, uint8_t g, uint8_t b, float a)
{
    int al = (int)(a * 128.0f + 0.5f);
    if (al < 0) al = 0;
    if (al > 128) al = 128;
    return GS_SETREG_RGBAQ(r, g, b, al, 0x00);
}

void gfx_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, float alpha)
{
    if (alpha <= 0.0f) return;
    gsKit_prim_sprite(gs, x, y, x + w, y + h, 1, col(r, g, b, alpha));
}

void gfx_quad(int id, const float c[8], float alpha)
{
    if (alpha <= 0.0f || id < 0) return;
    PTex *p = &ptex[id];
    float w = (float)p->w, h = (float)p->h;
    /* gsKit dibuja el cuadrilatero como tira de triangulos: arriba-izq, abajo-izq, arriba-der, abajo-der */
    gsKit_prim_quad_texture(gs, &p->t,
                            c[0], c[1], 0.0f, 0.0f,      /* TL */
                            c[6], c[7], 0.0f, h,         /* BL */
                            c[2], c[3], w,    0.0f,      /* TR */
                            c[4], c[5], w,    h,         /* BR */
                            2, col(0x80, 0x80, 0x80, alpha));
}

static u64 dot(int s)      /* 1 verde, 0 amarillo (esperando), -1 rojo, 9 gris (sin intentar) */
{
    if (s == 1) return GS_SETREG_RGBAQ(0, 200, 0, 0x80, 0);
    if (s < 0) return GS_SETREG_RGBAQ(255, 0, 0, 0x80, 0);
    if (s == 9) return GS_SETREG_RGBAQ(90, 90, 90, 0x80, 0);
    return GS_SETREG_RGBAQ(230, 200, 0, 0x80, 0);
}

void gfx_end(void)
{
#ifndef NO_DEBUG_DOTS
    /* Fila 1: mando | audio | etapa de audsrv_init (gris = hilo sin arrancar, amarillo = esperando, verde = hecho).
     * Fila 2: modulos IOP: SIO2MAN y PADMAN (BIOS), libsd, audsrv, libsd (BIOS, 2o intento), audsrv (2o), sio2man y padman
     *         incrustados, libsd y audsrv (3er intento). Gris = sin intentar, verde = cargado, rojo = fallo. */
    gsKit_prim_sprite(gs, 4, 4, 14, 14, 3, dot(pad_ready ? 1 : 0));
    gsKit_prim_sprite(gs, 18, 4, 28, 14, 3, dot(audio_ready ? 1 : (audio_failed ? -1 : 0)));
    gsKit_prim_sprite(gs, 32, 4, 42, 14, 3, dot(audio_stage == 0 ? 9 : (audio_stage == 1 ? 0 : 1)));
    for (int i = 0; i < 10; i++) gsKit_prim_sprite(gs, 4.0f + i * 10, 18, 11.0f + i * 10, 25, 3, dot(mod_state[i] ? mod_state[i] : 9));
#endif
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

/* =============================== hilos auxiliares =============================== */
/* audsrv_init() y padInit() esperan SIN limite a que su modulo del IOP responda. Si no responde, quien los llame se
 * queda colgado para siempre. Por eso van en hilos de BAJA prioridad: el juego nunca depende de ellos para arrancar. */
static void start_thread(void (*fn)(void *), void *stack, int size, int prio)
{
    ee_thread_t th;
    memset(&th, 0, sizeof th);
    th.func = (void *)fn;
    th.stack = stack;
    th.stack_size = size;
    th.gp_reg = &_gp;
    th.initial_priority = prio;
    int id = CreateThread(&th);
    if (id < 0) { printf("JARONA: no pude crear un hilo\n"); return; }
    StartThread(id, NULL);
}

/* =============================== carga en segundo plano =============================== */
typedef struct { void *data; uint32_t size; volatile int state; } ARes;      /* state: 0 pendiente, 1 listo, -1 fallo */
static ARes ares[8];
static const char *const *a_names;
static int a_n;
static u8 loader_stack[16 * 1024] __attribute__((aligned(16)));

static void loader_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < a_n; i++) {
        FILE *f = open_data(a_names[i]);
        if (!f) { printf("JARONA: no encuentro %s\n", a_names[i]); ares[i].state = -1; continue; }
        fseek(f, 0, SEEK_END);
        long n = ftell(f);
        fseek(f, 0, SEEK_SET);
        uint8_t *buf = (uint8_t *)malloc((size_t)n + 16);
        long done = 0;
        if (buf) {
            while (done < n) {                                        /* este hilo se duerme esperando al disco; el juego no */
                long k = n - done > 65536 ? 65536 : n - done;
                long r = (long)fread(buf + done, 1, (size_t)k, f);
                if (r <= 0) break;
                done += r;
            }
        }
        fclose(f);
        if (!buf || done != n) { free(buf); printf("JARONA: lectura incompleta de %s\n", a_names[i]); ares[i].state = -1; continue; }
        ares[i].data = buf; ares[i].size = (uint32_t)n;
        __sync_synchronize();
        ares[i].state = 1;
        printf("JARONA: %s cargado en segundo plano (%ld bytes)\n", a_names[i], n);
    }
    ExitDeleteThread();
}

void plat_async_begin(const char *const *names, int n)
{
    a_names = names;
    a_n = n < 8 ? n : 8;
    start_thread(loader_thread, loader_stack, sizeof loader_stack, 0x58);   /* por debajo del juego, por encima del audio en espera */
}

int plat_async_poll(int i, void **data, uint32_t *size)
{
    if (i < 0 || i >= a_n) return -1;
    int st = ares[i].state;
    if (st == 1) { *data = ares[i].data; *size = ares[i].size; }
    return st;
}

/* =============================== audio =============================== */
#define AUDIO_FRAMES 1024                      /* 1024 tramas a 44.1 kHz = 23 ms por bloque */
static u8 audio_stack[32 * 1024] __attribute__((aligned(16)));
static int16_t audio_buf[AUDIO_FRAMES * 2] __attribute__((aligned(64)));

static void audio_thread(void *arg)
{
    (void)arg;
    audio_stage = 1;
    if (audsrv_init() != 0) {
        printf("JARONA: audsrv_init fallo: %s\n", audsrv_get_error_string());
        audio_failed = 1;
        ExitDeleteThread();
    }
    audio_stage = 2;
    audsrv_fmt_t fmt;
    fmt.bits = 16; fmt.freq = 44100; fmt.channels = 2;
    audsrv_set_format(&fmt);
    audsrv_set_volume(MAX_VOLUME);
    audio_ready = 1;
    ChangeThreadPriority(GetThreadId(), 0x20);                       /* ya iniciado: pasa a mas prioritario que el principal */
    for (;;) {
        mixer_render(audio_buf, AUDIO_FRAMES);                       /* musica + efectos, todo a la vez */
        audsrv_wait_audio(AUDIO_FRAMES * 4);
        audsrv_play_audio((char *)audio_buf, AUDIO_FRAMES * 4);
    }
}

static void audio_start(void)
{
    start_thread(audio_thread, audio_stack, sizeof audio_stack, 0x60);
}

/* =============================== mando =============================== */
static char padbuf[256] __attribute__((aligned(64)));
static u8 pad_stack[16 * 1024] __attribute__((aligned(16)));

#ifndef PAD_CROSS
#define PAD_CROSS    0x4000
#define PAD_CIRCLE   0x2000
#define PAD_TRIANGLE 0x1000
#define PAD_START    0x0008
#endif

static void pad_thread(void *arg)
{
    (void)arg;
    padInit(0);                                                      /* se queda aqui si padman no responde */
    padPortOpen(0, 0, padbuf);
    pad_ready = 1;
    ExitDeleteThread();
}

static void pad_start(void)
{
    start_thread(pad_thread, pad_stack, sizeof pad_stack, 0x60);
}

static u32 pad_read(void)
{
    struct padButtonStatus b;
    if (!pad_ready) return 0;
    int st = padGetState(0, 0);
    if (st != PAD_STATE_STABLE && st != PAD_STATE_FINDCTP1) return 0;
    if (!padRead(0, 0, &b)) return 0;
    return 0xffff ^ b.btns;                                          /* los botones llegan invertidos */
}

/* =============================== diagnostico de arranque =============================== */
/* Cuadros verdes = etapas completadas. Si la pantalla se queda en N cuadros, el fallo esta justo despues. */
static void stage_wait(int n)                                         /* un fotograma: n cuadros verdes + uno amarillo parpadeando */
{
    gfx_begin(0, 0, 48);
    for (int i = 0; i < n; i++) gfx_rect(24.0f + i * 40, 24, 32, 32, 0, 255, 0, 1);
    gfx_rect(24.0f + n * 40, 24, 32, 32, 255, 220, 0, 1);
    gfx_end();
}

static void stage(int n, const char *msg)
{
    printf("JARONA: etapa %d - %s\n", n, msg);
    for (int k = 0; k < 2; k++) {                                     /* 2 fotogramas: se ve en los dos buffers */
        gfx_begin(0, 0, 48);
        for (int i = 0; i < n; i++) gfx_rect(24.0f + i * 40, 24, 32, 32, 0, 255, 0, 1);
        gfx_end();
    }
}

/* =============================== modulos IOP =============================== */
static int load_irx(void *buf, unsigned size, const char *what)
{
    int ret = 0;
    int id = SifExecModuleBuffer(buf, size, 0, NULL, &ret);
    if (id < 0 || ret == 1) { printf("JARONA: no pude cargar %s (id=%d ret=%d)\n", what, id, ret); return -1; }
    printf("JARONA: %s cargado (id=%d)\n", what, id);
    return 1;
}

static int load_rom(const char *path)
{
    int id = SifLoadModule(path, 0, NULL);
    printf("JARONA: %s -> id=%d\n", path, id);
    return id >= 0 ? 1 : -1;
}

int main(int argc, char **argv)
{
    set_base_dir(argc > 0 ? argv[0] : NULL);
    SifInitRpc(0);
    ChangeThreadPriority(GetThreadId(), 0x40);

    printf("JARONA: build 5 (carga en hilo aparte, musica mu-law)\n");
    mixer_ensure_init();                                              /* antes de que exista ningun hilo de audio */
    gfx_init();
    stage(1, "graficos iniciados");

    /* Mando: en tu emulador hacen falta los modulos de la BIOS Y los incrustados. Se cargan seguidos y NO se espera:
     * el hilo del mando se despierta solo cuando padman responde, mientras el juego carga. */
    mod_state[0] = load_rom("rom0:SIO2MAN");
    mod_state[1] = load_rom("rom0:PADMAN");
    mod_state[6] = load_irx(sio2man_irx, size_sio2man_irx, "sio2man");
    mod_state[7] = load_irx(padman_irx, size_padman_irx, "padman");
    pad_start();
    stage(2, "mando (arranca en segundo plano)");

    /* Audio: tambien en segundo plano; asi puede arrancar mientras se leen los datos del disco */
    mod_state[2] = load_irx(libsd_irx, size_libsd_irx, "libsd");
    mod_state[3] = load_irx(audsrv_irx, size_audsrv_irx, "audsrv");
    audio_start();
    stage(3, "audio (arranca en segundo plano)");

    int load_err = game_load();                                      /* solo texturas + 4 efectos (~1.5 MB); lo demas llega despues */
    if (load_err) printf("JARONA: game_load fallo (%d)\n", load_err);

    /* El tiempo del juego avanza por fotogramas (vsync): 59.94 Hz en NTSC, 50 Hz en PAL */
    const float frame_ms = (gs->Mode == GS_MODE_PAL) ? 20.0f : 16.6833f;
    double clock_ms = 0;
    u32 prev = 0;
    int frames = 0, audio_retry = 0;
    for (;;) {
        u32 cur = pad_read();
        u32 pressed = cur & ~prev, released = prev & ~cur;
        GameInput in;
        memset(&in, 0, sizeof in);
        in.tap_down   = (pressed  & PAD_CROSS) != 0;
        in.tap_up     = (released & PAD_CROSS) != 0;
        in.swap_down  = (pressed  & (PAD_TRIANGLE | PAD_CIRCLE)) != 0;
        in.swap_up    = (released & (PAD_TRIANGLE | PAD_CIRCLE)) != 0;
        in.start_down = (pressed  & PAD_START) != 0;
        prev = cur;

        /* Si el audio sigue sin responder, reintentos con otras combinaciones de modulos (se ven en los puntos) */
        frames++;
        if (!audio_ready && audio_retry == 0 && frames == 150) {
            audio_retry = 1;
            printf("JARONA: el audio no responde: 2o intento con rom0:LIBSD\n");
            mod_state[4] = load_rom("rom0:LIBSD");
            mod_state[5] = load_irx(audsrv_irx, size_audsrv_irx, "audsrv (2o intento)");
            if (audio_failed) { audio_failed = 0; audio_start(); }
        } else if (!audio_ready && audio_retry == 1 && frames == 400) {
            audio_retry = 2;
            printf("JARONA: el audio sigue sin responder: 3er intento recargando libsd y audsrv incrustados\n");
            mod_state[8] = load_irx(libsd_irx, size_libsd_irx, "libsd (3er intento)");
            mod_state[9] = load_irx(audsrv_irx, size_audsrv_irx, "audsrv (3er intento)");
            if (audio_failed) { audio_failed = 0; audio_start(); }
        }
        game_frame((uint32_t)clock_ms, &in);                         /* dibuja y espera al vsync (gfx_end) */
        clock_ms += frame_ms;
    }
    return 0;
}
