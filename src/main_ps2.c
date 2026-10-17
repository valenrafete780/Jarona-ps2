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
#include <fcntl.h>
#include <unistd.h>
#include <stdarg.h>
#include <tamtypes.h>
#include <kernel.h>
#include <sifrpc.h>
#include <loadfile.h>
#include <gsKit.h>
#include <dmaKit.h>
#include <libpad.h>
#include <libcdvd.h>
#include <sbv_patches.h>
#include <audsrv.h>

#include "platform.h"
#include "game.h"
#include "mixer.h"
#include "dbgfont.h"

/* modulos IOP incrustados en el ELF (los genera el Makefile con bin2c) */
extern unsigned char libsd_irx[];   extern unsigned int size_libsd_irx;
#ifdef HAVE_FREESD
extern unsigned char freesd_irx[];  extern unsigned int size_freesd_irx;   /* si la imagen de ps2dev trae freesd, es el que usan OPL y compania */
#endif
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

/* IMPORTANTE: se usa open()/read() con bloques grandes. fread() en la PS2 lee de ~1 KB en ~1 KB aunque se le pida 1 MB,
 * y cada trozo es una llamada lenta al IOP: leer 672 KB tardaba mas de 30 s en el emulador. */
static int open_data_fd(const char *name)
{
    char p[256];
    int fd;
    snprintf(p, sizeof p, "cdrom0:\\%s;1", name);        if ((fd = open(p, O_RDONLY)) >= 0) return fd;   /* ISO / disco */
    snprintf(p, sizeof p, "%s%s", g_dir, name);          if ((fd = open(p, O_RDONLY)) >= 0) return fd;
    snprintf(p, sizeof p, "%sDATA/%s", g_dir, name);     if ((fd = open(p, O_RDONLY)) >= 0) return fd;
    snprintf(p, sizeof p, "%sdata/%s", g_dir, name);     if ((fd = open(p, O_RDONLY)) >= 0) return fd;
    snprintf(p, sizeof p, "host:data/%s", name);         if ((fd = open(p, O_RDONLY)) >= 0) return fd;
    snprintf(p, sizeof p, "data/%s", name);              if ((fd = open(p, O_RDONLY)) >= 0) return fd;
    return -1;
}

static long read_all(int fd, uint8_t *buf, long n, void (*progress)(uint32_t, uint32_t), long chunk)
{
    long done = 0;
    while (done < n) {
        long k = n - done > chunk ? chunk : n - done;
        long r = (long)read(fd, buf + done, (size_t)k);
        if (r <= 0) break;
        done += r;
        if (progress) progress((uint32_t)done, (uint32_t)n);
    }
    return done;
}

void *plat_load(const char *name, uint32_t *size, void (*progress)(uint32_t, uint32_t))
{
    int fd = open_data_fd(name);
    if (fd < 0) return NULL;
    long n = (long)lseek(fd, 0, SEEK_END);
    lseek(fd, 0, SEEK_SET);
    uint8_t *buf = (n > 0) ? (uint8_t *)memalign(64, (size_t)n + 64) : NULL;
    if (!buf) { close(fd); return NULL; }
    long done = read_all(fd, buf, n, progress, 1048576);
    close(fd);
    if (done != n) { free(buf); return NULL; }
    *size = (uint32_t)n;
    return buf;
}

/* =============================== registro en pantalla =============================== */
/* Muchos emuladores no dejan leer la consola: el juego escribe aqui lo que pasa (abajo a la izquierda). */
#define LOG_COLS  32
#define LOG_LINES 14
static char logl[LOG_LINES][LOG_COLS + 1];
static volatile int log_dirty;

/* SELECT muestra/oculta los indicadores y el registro de diagnostico. */
static volatile int dbg_show = 0;           /* oculto por defecto: SELECT lo muestra (indicadores y registro de diagnostico) */
static volatile int g_frames;                       /* fotogramas desde que arranco el bucle principal (para marcar el tiempo) */

static void dbg_log(const char *fmt, ...)
{
    char tmp[96], line[112];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    snprintf(line, sizeof line, "%d %s", g_frames / 50, tmp);   /* segundos aprox. delante de cada linea */
    int en = DIntr();                                /* varios hilos escriben: se desactivan las interrupciones un instante */
    for (int i = 0; i < LOG_LINES - 1; i++) memcpy(logl[i], logl[i + 1], LOG_COLS + 1);
    memset(logl[LOG_LINES - 1], 0, LOG_COLS + 1);
    strncpy(logl[LOG_LINES - 1], line, LOG_COLS);
    log_dirty = 1;
    if (en) EIntr();
}

void plat_log(const char *msg) { dbg_log("%s", msg); }

#define LOGW (LOG_COLS * 6)
#define LOGH (LOG_LINES * 8)
static uint8_t log_px[LOGW * LOGH * 4];

static void log_render(void)
{
    for (int i = 0; i < LOGW * LOGH; i++) { log_px[i * 4] = 0; log_px[i * 4 + 1] = 0; log_px[i * 4 + 2] = 0; log_px[i * 4 + 3] = 170; }
    for (int l = 0; l < LOG_LINES; l++)
        for (int c = 0; c < LOG_COLS && logl[l][c]; c++) {
            int ch = (unsigned char)logl[l][c];
            if (ch >= 'a' && ch <= 'z') ch -= 32;
            if (ch < 32 || ch > 95) ch = '?';
            const unsigned char *g = dbg_font[ch - 32];
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 6; x++)
                    if (g[y] & (0x80 >> x)) {
                        uint8_t *p = &log_px[(((l * 8) + y) * LOGW + c * 6 + x) * 4];
                        p[0] = p[1] = p[2] = 255; p[3] = 255;
                    }
        }
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

static int log_tex = -1;

void gfx_end(void)
{
    if (dbg_show) {
        if (log_tex < 0 && nptex < MAX_TEX) { log_render(); log_tex = gfx_tex_create(LOGW, LOGH, log_px, 0); }
        if (log_tex >= 0) {
            if (log_dirty) { log_dirty = 0; log_render(); gfx_tex_update(log_tex, log_px); }
            float y = (float)gs->Height - LOGH - 4, c[8] = { 4, y, 4 + LOGW, y, 4 + LOGW, y + LOGH, 4, y + LOGH };
            gfx_quad(log_tex, c, 1.0f);
        }
        /* Fila 1: mando | audio | etapa de audsrv_init (gris = hilo sin arrancar, amarillo = esperando, verde = hecho).
         * Fila 2: modulos IOP: SIO2MAN y PADMAN (BIOS), freesd, audsrv. Gris = sin intentar, verde = cargado, rojo = fallo. */
        gsKit_prim_sprite(gs, 4, 4, 14, 14, 3, dot(pad_ready ? 1 : 0));
        gsKit_prim_sprite(gs, 18, 4, 28, 14, 3, dot(audio_ready ? 1 : (audio_failed ? -1 : 0)));
        gsKit_prim_sprite(gs, 32, 4, 42, 14, 3, dot(audio_stage == 0 ? 9 : (audio_stage == 1 ? 0 : 1)));
        for (int i = 0; i < 10; i++) gsKit_prim_sprite(gs, 4.0f + i * 10, 18, 11.0f + i * 10, 25, 3, dot(mod_state[i] ? mod_state[i] : 9));
    }
    gsKit_queue_exec(gs);
    gsKit_sync_flip(gs);
}

/* =============================== hilos auxiliares =============================== */
/* audsrv_init() y padInit() esperan SIN limite a que su modulo del IOP responda. Si no responde, quien los llame se
 * queda colgado para siempre. Por eso van en hilos de BAJA prioridad: el juego nunca depende de ellos para arrancar. */
static int start_thread(void (*fn)(void *), void *stack, int size, int prio)
{
    ee_thread_t th;
    memset(&th, 0, sizeof th);
    th.func = (void *)fn;
    th.stack = stack;
    th.stack_size = size;
    th.gp_reg = &_gp;
    th.initial_priority = prio;
    int id = CreateThread(&th);
    if (id < 0) { dbg_log("ERROR: CreateThread=%d (prio %02X)", id, prio); return id; }
    int r = StartThread(id, NULL);
    if (r < 0) dbg_log("ERROR: StartThread=%d", r);
    return id;
}

/* =============================== carga en segundo plano =============================== */
typedef struct { void *data; uint32_t size; volatile int state; volatile uint32_t done, total; } ARes;      /* state: 0 pendiente, 1 listo, -1 fallo */
static ARes ares[8];
static const char *const *a_names;
static int a_n;
static u8 loader_stack[16 * 1024] __attribute__((aligned(16)));

static volatile int cur_req;
static void loader_progress(uint32_t d, uint32_t t) { ares[cur_req].done = d; ares[cur_req].total = t; }

static void loader_thread(void *arg)
{
    (void)arg;
    for (int i = 0; i < a_n; i++) {
        int fd = open_data_fd(a_names[i]);
        if (fd < 0) { dbg_log("%s: NO ENCONTRADO", a_names[i]); ares[i].state = -1; continue; }
        long n = (long)lseek(fd, 0, SEEK_END);
        lseek(fd, 0, SEEK_SET);
        dbg_log("leyendo %s %ldK", a_names[i], n / 1024);
        uint8_t *buf = (n > 0) ? (uint8_t *)memalign(64, (size_t)n + 64) : NULL;
        cur_req = i; ares[i].total = (uint32_t)n; ares[i].done = 0;
        long done = buf ? read_all(fd, buf, n, loader_progress, 524288) : 0;      /* bloques de 512 KB: la barra avanza con suavidad */
        close(fd);
        if (!buf || done != n) { free(buf); dbg_log("%s: lectura incompleta", a_names[i]); ares[i].state = -1; continue; }
        ares[i].data = buf; ares[i].size = (uint32_t)n;
        __sync_synchronize();
        ares[i].state = 1;
        dbg_log("%s cargado", a_names[i]);
    }
    ExitDeleteThread();
}

void plat_async_begin(const char *const *names, int n)
{
    a_names = names;
    a_n = n < 8 ? n : 8;
    dbg_log("carga en 2o plano: %d ficheros", a_n);
    start_thread(loader_thread, loader_stack, sizeof loader_stack, 0x58);   /* por debajo del juego, por encima del audio en espera */
}

static int async_all_done(void)
{
    if (a_n == 0) return 0;
    for (int i = 0; i < a_n; i++) if (ares[i].state == 0) return 0;
    return 1;
}

int plat_async_progress(int i, uint32_t *done, uint32_t *total)
{
    if (i < 0 || i >= a_n) return 0;
    *done = ares[i].done; *total = ares[i].total;
    return 1;
}

int plat_audio_ready(void) { return audio_ready; }

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
    /* audsrv 0.93 inicializa el reproductor de CD (cdda) y habla con cdvdman: se inicializa el CDVD antes, como hacen los
     * juegos. Va DENTRO de este hilo: si algo se cuelga, solo se cuelga el audio. */
    dbg_log("sceCdInit...");
    sceCdInit(0);
    dbg_log("sceCdInit hecho");
    dbg_log("audsrv_init: llamando...");
    int r = audsrv_init();
    if (r != 0) {
        dbg_log("audsrv_init ERR %d", r);
        dbg_log("%s", audsrv_get_error_string());
        audio_failed = 1;
        ExitDeleteThread();
    }
    audio_stage = 2;
    dbg_log("audsrv_init OK");
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

static volatile int audio_thread_started;

/* 1 = el servidor de audsrv existe en el IOP, 0 = no, -1 = error al preguntar. Una sola consulta, no se queda esperando. */
static int audsrv_bound(void)
{
    SifRpcClientData_t cd;
    memset(&cd, 0, sizeof cd);
    if (SifBindRpc(&cd, 0x870884, 0) < 0) return -1;     /* 0x870884 = AUDSRV_IRX */
    return cd.server != 0;
}

static void audio_start(void)
{
    audio_thread_started = 1;
    int id = start_thread(audio_thread, audio_stack, sizeof audio_stack, 0x60);   /* 0x60: la prioridad con la que el hilo SI llego a ejecutarse */
    dbg_log("hilo de audio id=%d", id);
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

#ifndef PAD_SELECT
#define PAD_SELECT   0x0001
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
    if (!dbg_show) { gfx_begin(255, 216, 61); gfx_end(); return; }
    gfx_begin(0, 0, 48);
    for (int i = 0; i < n; i++) gfx_rect(24.0f + i * 40, 24, 32, 32, 0, 255, 0, 1);
    gfx_rect(24.0f + n * 40, 24, 32, 32, 255, 220, 0, 1);
    gfx_end();
}

static void stage(int n, const char *msg)
{
    printf("JARONA: etapa %d - %s\n", n, msg);
    for (int k = 0; k < 2; k++) {                                     /* 2 fotogramas: se ve en los dos buffers */
        if (!dbg_show) { gfx_begin(255, 216, 61); gfx_end(); continue; }
        gfx_begin(0, 0, 48);
        for (int i = 0; i < n; i++) gfx_rect(24.0f + i * 40, 24, 32, 32, 0, 255, 0, 1);
        gfx_end();
    }
}

/* =============================== modulos IOP =============================== */
static int load_irx(void *buf, unsigned size, const char *what, int *out_id)
{
    int ret = 0;
    int id = SifExecModuleBuffer(buf, size, 0, NULL, &ret);
    dbg_log("%s id=%d ret=%d", what, id, ret);
    if (out_id) *out_id = id;
    return (id < 0 || ret == 1) ? -1 : 1;
}

static int last_rom_id;

static int load_rom(const char *path)
{
    int id = SifLoadModule(path, 0, NULL);
    dbg_log("rom0 %s id=%d", path + 5, id);
    last_rom_id = id;
    return id >= 0 ? 1 : -1;
}

int main(int argc, char **argv)
{
    set_base_dir(argc > 0 ? argv[0] : NULL);
    SifInitRpc(0);
    /* IMPRESCINDIBLE: sin este parche la BIOS no soporta SifExecModuleBuffer: devuelve "exito" sin cargar nada
     * (los modulos incrustados salian con ID=319488 = una direccion del IOP, no un identificador de modulo). */
    int lmb = sbv_patch_enable_lmb();
    int pfx = sbv_patch_disable_prefix_check();
    ChangeThreadPriority(GetThreadId(), 0x40);

    mixer_ensure_init();                                              /* antes de que exista ningun hilo de audio */
    gfx_init();
    stage(1, "graficos iniciados");

    /* Mando: modulos de la BIOS (funcionan). El hilo del mando se despierta solo cuando padman responde. */
    mod_state[0] = load_rom("rom0:SIO2MAN");
    mod_state[1] = load_rom("rom0:PADMAN");
    pad_start();
    stage(2, "mando (arranca en segundo plano)");
    dbg_log("build 7 - parche LMB=%d/%d", lmb, pfx);

    /* Audio: tambien en segundo plano; asi puede arrancar mientras se leen los datos del disco */
#ifdef HAVE_FREESD
    mod_state[2] = load_irx(freesd_irx, size_freesd_irx, "freesd", NULL);
#else
    mod_state[2] = load_irx(libsd_irx, size_libsd_irx, "libsd", NULL);
#endif
    mod_state[3] = load_irx(audsrv_irx, size_audsrv_irx, "audsrv", NULL);
    stage(3, "modulos de audio cargados");

    int load_err = game_load();                                      /* solo texturas + 4 efectos (~1.5 MB); lo demas llega despues */
    if (load_err) printf("JARONA: game_load fallo (%d)\n", load_err);

    /* El tiempo del juego avanza por fotogramas (vsync): 59.94 Hz en NTSC, 50 Hz en PAL */
    const float frame_ms = (gs->Mode == GS_MODE_PAL) ? 20.0f : 16.6833f;
    double clock_ms = 0;
    u32 prev = 0;
    int frames = 0, pad_logged = 0, audio_logged = 0, last_bound = -2;
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
        if (pressed & PAD_SELECT) dbg_show = !dbg_show;                /* SELECT: oculta o muestra los indicadores */
        prev = cur;

        /* Si el audio sigue sin responder, reintentos con otras combinaciones de modulos (se ven en los puntos) */
        frames++;
        g_frames = frames;
        if (pad_ready && !pad_logged) { pad_logged = 1; dbg_log("mando OK (cuadro %d)", frames); }
        if (audio_ready && !audio_logged) { audio_logged = 1; dbg_log("AUDIO OK (cuadro %d)", frames); }
        if (!audio_ready && frames % 15 == 0) {
            int b = audsrv_bound();
            if (b != last_bound) { last_bound = b; dbg_log("servidor audsrv: %s", b > 0 ? "EXISTE" : (b == 0 ? "NO existe" : "ERROR")); }
            if (!audio_thread_started) {
                if (b > 0) audio_start();
                /* con TODA la carga terminada el hilo de lectura ya no acapara el procesador: ahora si se prueba audsrv_init */
                else if (async_all_done()) { dbg_log("carga lista: pruebo audsrv_init"); audio_start(); }
            }
        }
        if (audio_failed) { audio_failed = 0; audio_thread_started = 0; }
        game_frame((uint32_t)clock_ms, &in);                         /* dibuja y espera al vsync (gfx_end) */
        clock_ms += frame_ms;
    }
    return 0;
}
