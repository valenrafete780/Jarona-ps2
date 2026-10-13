#!/bin/sh
# Pruebas en PC (no necesitan PS2SDK): mezclador de audio + secuencia completa del juego con renderizado por software.
set -e
cd "$(dirname "$0")"
gcc -O2 -Wall -Wextra -o test_mixer test_mixer.c ../src/mixer.c -lm && ./test_mixer
gcc -O2 -Wall -Wextra -Wno-misleading-indentation -o main_host main_host.c ../src/game.c ../src/mixer.c -lm && ./main_host | tail -3
echo "Capturas en /tmp/snap_*.ppm"
