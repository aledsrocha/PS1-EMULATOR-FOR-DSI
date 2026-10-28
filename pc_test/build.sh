#!/bin/sh
# Compila o harness de PC (Linux/macOS/MSYS2 com gcc). Rode de dentro de pc_test/.
gcc -O2 -w -Istub -I../source -o psx_pc harness.c ../source/cpu.c ../source/bus.c ../source/hw.c ../source/cd.c ../source/gte.c
echo "ok: ./psx_pc 300            (so BIOS, 300M instrucoes)"
echo "    ./psx_pc 60 jogo.cue fast (disco, pulando a intro)"
