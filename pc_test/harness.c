/* Harness de PC: roda o nucleo do emulador (cpu/bus/hw/cd/gpu) sem o DS.
   Uso: ./psx_pc <milhoes_de_instrucoes> [disco.cue|.bin|.iso] [fast]
   Saidas: tela.png (o que o DS mostraria, 256x192) e vram.png (VRAM inteira).
   O texto da BIOS (TTY) sai no terminal. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"
#include "../source/gpu.c"
uint16_t pc_fb[256*256];
#include <stdarg.h>
void tty_putc(char c){ fputc(c, stderr); }
void dbg_log(const char *fmt, ...){ va_list ap; va_start(ap, fmt); vfprintf(stderr, fmt, ap); va_end(ap); }
static void skip_shell(void){ cpu.pc = cpu.r[31]; cpu.next_pc = cpu.pc + 4; cpu.in_delay = false; }
static void save_ppm(const char *fn, const uint16_t *src, int stride, int w, int h){
  FILE *f=fopen(fn,"wb"); fprintf(f,"P6 %d %d 255\n",w,h);
  for(int y=0;y<h;y++)for(int x=0;x<w;x++){uint16_t p=src[y*stride+x];
    unsigned char c[3]={(p&31)<<3,((p>>5)&31)<<3,((p>>10)&31)<<3}; fwrite(c,1,3,f);}
  fclose(f);
}
int main(int argc,char**argv){
  long n = argc>1 ? atol(argv[1]) : 300;
  bus_init(NULL);
  FILE *b=fopen("../scph1001.BIN","rb"); if(!b){puts("coloque scph1001.BIN na pasta do projeto");return 1;}
  fread(bus.bios,1,BIOS_SIZE,b); fclose(b);
  gpu_init(); hw_init();
  if(argc>2 && !cd_open(argv[2])){ printf("disco: %s\n",cd_error()); return 1; }
  cpu_reset();
  if(argc>3) cpu_boot_hook = skip_shell;
  for(long i=0;i<n;i++){ cpu_run(1000000);
    if(i%50==49) fprintf(stderr,"[%ldM pc=%08X vb=%u irq=%u]\n",i+1,cpu.pc,st_vblank,st_irq_taken); }
  save_ppm("tela.ppm",pc_fb,256,256,192);
  save_ppm("vram.ppm",vram,1024,1024,512);
  printf("pc=%08X vblanks=%u\n",cpu.pc,st_vblank);
  return 0;
}
