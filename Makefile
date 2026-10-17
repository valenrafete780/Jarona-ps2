# JARONA! para PS2 - Makefile (PS2SDK + gsKit + audsrv)
#
#   python3 tools/convert_assets.py ../jarona    # 1) genera data/TEX.DAT y data/*.PCM (una vez)
#   make                                         # 2) compila JARONA.ELF
#   make iso                                     # 3) crea JARONA.ISO (ELF + datos)
#
# Necesita las variables PS2DEV y PS2SDK (las pone el instalador de ps2dev o su imagen de Docker).

# El objetivo por defecto es SIEMPRE 'all' (antes una regla auxiliar puesta delante hacia que make no compilara nada)
.DEFAULT_GOAL := all

EE_BIN     = JARONA.ELF
EE_BIN_PKD = JARONA-packed.ELF

EE_OBJS = main_ps2.o game.o mixer.o \
          libsd_irx.o audsrv_irx.o

EE_CFLAGS += -O2 -Wall -Wno-unused-function

# Si la imagen de ps2dev trae freesd.irx (el libsd libre que usan OPL y otros), se incrusta y se usa en lugar de libsd.irx
ifneq ($(wildcard $(PS2SDK)/iop/irx/freesd.irx),)
EE_OBJS += freesd_irx.o
EE_CFLAGS += -DHAVE_FREESD
endif
EE_INCS   += -Isrc -I$(PS2DEV)/gsKit/include -I$(PS2SDK)/ports/include
EE_LDFLAGS += -L$(PS2DEV)/gsKit/lib -L$(PS2SDK)/ports/lib
EE_LIBS   += -lgskit -ldmakit -laudsrv -lpad -lpatches -lcdvd -lm

vpath %.c src

# Los modulos del IOP (libsd, audsrv) se incrustan en el ELF; el mando usa los de la BIOS (rom0:)
%_irx.c: $(PS2SDK)/iop/irx/%.irx
	$(PS2SDK)/bin/bin2c $< $@ $*_irx

all: $(EE_BIN)

clean:
	rm -f $(EE_BIN) $(EE_BIN_PKD) *.o *_irx.c
	rm -rf iso_root JARONA.ISO

iso: all
	sh tools/make_iso.sh $(EE_BIN) JARONA.ISO $(REGION)

include $(PS2SDK)/samples/Makefile.pref
include $(PS2SDK)/samples/Makefile.eeglobal
