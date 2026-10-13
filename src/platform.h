/* Interfaz entre el juego (game.c, portable) y la plataforma (PS2 real o prueba en PC). */
#ifndef PLATFORM_H
#define PLATFORM_H
#include <stdint.h>
#include <stddef.h>

/* ---- grafico ---- */
int  gfx_width(void);
int  gfx_height(void);
/* rgba: w*h*4 bytes (alfa 0..255). smooth=1 -> filtro lineal, 0 -> pixelado. Devuelve un id de textura */
int  gfx_tex_create(int w, int h, const uint8_t *rgba, int smooth);
void gfx_tex_update(int tex, const uint8_t *rgba);          /* mismo tamano que al crearla */
void gfx_begin(uint8_t r, uint8_t g, uint8_t b);            /* empieza el fotograma y lo limpia */
void gfx_rect(float x, float y, float w, float h, uint8_t r, uint8_t g, uint8_t b, float alpha);
void gfx_quad(int tex, const float c[8], float alpha);      /* esquinas: TL TR BR BL (x,y) */
void gfx_end(void);                                         /* presenta el fotograma (espera al vsync) */

/* ---- ficheros ---- */
/* Lee un fichero entero (malloc). progress(leidos,total) opcional. NULL si no existe. */
void *plat_load(const char *name, uint32_t *size, void (*progress)(uint32_t done, uint32_t total));


/* Mensaje de diagnostico (en la PS2 sale en el registro en pantalla) */
void plat_log(const char *msg);

/* Carga en SEGUNDO PLANO: un hilo aparte lee los ficheros en orden mientras el juego sigue a su ritmo.
 * (En el emulador el disco lee a ~250 KB/s: leer en el hilo principal congelaba la animacion.) */
void plat_async_begin(const char *const *names, int n);                 /* empieza a cargar names[0..n-1] en orden */
int  plat_async_poll(int i, void **data, uint32_t *size);               /* 0 = aun no, 1 = listo (data/size), -1 = fallo */

#endif
