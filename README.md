# JARONA! para PS2

Port de `jarona/index.html` a PlayStation 2 (C + PS2SDK + gsKit + audsrv).

## Estado — leer primero

| Parte | Estado |
|---|---|
| Lógica del juego (fases, 30 toques, órbita, fundido, Omega, escena secreta) | Portada línea a línea del `<script>` y **probada en PC** con los assets reales (`tests/`) |
| Mezclador de audio (música + efectos a la vez) | **Probado en PC**: la mezcla es exactamente música + efectos, la música nunca se pausa |
| Conversión de assets (`tools/convert_assets.py`) | Ejecutada, genera `data/` |
| Capa PS2 (`src/main_ps2.c`: gsKit, audsrv, libpad) y `Makefile` | **Sin compilar ni probar en consola/emulador**: donde se preparó no había toolchain de PS2 ni red. Está escrita con las APIs estándar de PS2SDK; si el compilador protesta por algún nombre o firma, será ahí |
| `JARONA.ELF` / `JARONA.ISO` | **No generados aún** (necesitan ps2dev). Los pasos de abajo los producen |

## Controles
- **X**: tocar a Flowery (30 toques → transformación). Mantener X mantiene la pose.
- **Triángulo / Círculo**: botón "Cambiar a …" (aparece cuando Omega termina de entrar).
- **Start / X**: ¡Empezar! (también arranca la música).
- Escena secreta: 8 cambios en menos de 5 s. Sin salida: reinicia la consola.

## Compilar
1. Datos (necesita Python 3 + Pillow + ffmpeg; ya vienen generados en `data/`):
   `python3 tools/convert_assets.py ../jarona`
2. ELF con ps2dev (instalado, o con Docker):
   `docker run --rm -v "$PWD":/src -w /src ps2dev/ps2dev:latest make`
   → `JARONA.ELF`
3. ISO (necesita `genisoimage`, `mkisofs` o `xorriso` en tu equipo):
   `sh tools/make_iso.sh JARONA.ELF JARONA.ISO NTSC`   (o `PAL`)

## Ejecutar
- **PCSX2**: *Sistema → Arrancar ISO* con `JARONA.ISO`.
- **Consola real**: ISO con OPL (USB/SMB/HDD) o grabada con FreeMcBoot/modchip.
- **Solo el .elf** (uLaunchELF/PCSX2 "Run ELF"): copia `data/` junto al `.elf`; busca los datos en `cdrom0:`, junto al ELF (`DATA/`), `host:data/` y `data/`.

## Audio
En la página para PS4 el motor de respaldo usaba UN `<audio>` con `mix` y la música callaba durante cada efecto.
Aquí **no se usa `mix`**: cada pista va por separado (`MUSICA.PCM`, `JARONA.PCM`, `ULTIMO.PCM`, `PODER.PCM`, `OMEGA.PCM`, `SECRETO.PCM`, mono 22 kHz) y `src/mixer.c` las suma por software:
- la música es una voz que avanza siempre, en bucle y a ganancia 0.35 (fundido de entrada de 1.5 s, como la página);
- los efectos usan hasta 8 voces y suenan encima, incluso el mismo efecto solapado consigo mismo;
- al entrar en la escena secreta, efectos y música se apagan (0.3 s / 0.5 s) y suena `secreto` en bucle.
Un hilo de EE entrega bloques de 23 ms a `audsrv` (44.1 kHz estéreo).
`tests/test_mixer.c` comprueba que `música+efectos == música sola + efectos solos`.

## Diferencias con la web
- Fuente: Pixelify Sans no se puede usar; los textos se pre-renderizan con DejaVu Sans Bold sin suavizado (`--font` para usar otra TTF).
- Mando en lugar de ratón/táctil; el tiempo avanza por vsync (59.94 / 50 Hz).
- Música a 22 kHz mono (8.5 MB en RAM) para que cargue entera y el bucle sea perfecto.
- Escala fija de sprites (U = 3) en lugar de calcularla según la ventana.

## Estructura
```
src/game.c       lógica + dibujo (portable)         src/mixer.c      mezclador (portable)
src/main_ps2.c   gsKit / audsrv / libpad / ficheros  src/platform.h   interfaz con la plataforma
tools/           convert_assets.py, make_iso.sh      tests/           pruebas en PC (run_tests.sh)
data/            TEX.DAT + *.PCM (generados)
```
