#ifndef GAME_H
#define GAME_H
#include <stdint.h>

typedef struct {
    int tap_down, tap_up;      /* boton X: tocar a Flowery            */
    int swap_down, swap_up;    /* boton Triangulo: "Cambiar a ..."    */
    int start_down;            /* Start / X en la pantalla de inicio  */
} GameInput;

int  game_load(void);                                   /* 0 = bien. Dibuja la barra de carga */
void game_frame(uint32_t now_ms, const GameInput *in);  /* actualiza y dibuja UN fotograma */

/* para pruebas */
const char *game_debug_state(void);
#endif
