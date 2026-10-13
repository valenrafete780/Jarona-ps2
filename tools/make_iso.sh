#!/bin/sh
# Uso: sh tools/make_iso.sh JARONA.ELF JARONA.ISO [NTSC|PAL]
# Crea una imagen ISO9660 arrancable en PS2: SYSTEM.CNF + ejecutable + datos (nombres 8.3 en mayusculas).
set -e
ELF="${1:-JARONA.ELF}"; ISO="${2:-JARONA.ISO}"; REGION="${3:-NTSC}"
[ -f "$ELF" ] || { echo "No existe $ELF (compila primero con make)"; exit 1; }
for f in TEX.DAT JARONA.PCM ULTIMO.PCM PODER.PCM OMEGA.PCM SECRETO.PCM MUSICA.PCM; do
  [ -f "data/$f" ] || { echo "Falta data/$f: ejecuta antes  python3 tools/convert_assets.py ../jarona"; exit 1; }
done
if [ "$REGION" = "PAL" ]; then BOOT=SLES_000.01; else BOOT=SLUS_000.01; REGION=NTSC; fi

rm -rf iso_root && mkdir iso_root
cp "$ELF" "iso_root/$BOOT"
cp data/TEX.DAT data/*.PCM iso_root/
printf 'BOOT2 = cdrom0:\\%s;1\r\nVER = 1.00\r\nVMODE = %s\r\n' "$BOOT" "$REGION" > iso_root/SYSTEM.CNF

if   command -v mkisofs    >/dev/null 2>&1; then MK="mkisofs"
elif command -v genisoimage >/dev/null 2>&1; then MK="genisoimage"
elif command -v xorriso    >/dev/null 2>&1; then MK="xorriso -as mkisofs"
else echo "Instala mkisofs, genisoimage o xorriso"; exit 1; fi

$MK -quiet -iso-level 1 -sysid PLAYSTATION -V JARONA -o "$ISO" iso_root
echo "Creada $ISO ($(du -h "$ISO" | cut -f1)) - arranca $BOOT en modo $REGION"
