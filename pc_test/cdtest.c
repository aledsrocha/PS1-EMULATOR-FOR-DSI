#include <stdio.h>
#include <string.h>
#include "psx.h"
uint16_t pc_fb[256*256];
void tty_putc(char c){(void)c;} void dbg_log(const char*f,...){(void)f;}
void irq_raise(int n){(void)n;}
static void cmd(uint8_t c, int np, uint8_t a, uint8_t b, uint8_t d){
  cd_write(0,0); if(np>0)cd_write(2,a); if(np>1)cd_write(2,b); if(np>2)cd_write(2,d); cd_write(1,c);
  for(int i=0;i<200 && 1;i++){ cd_tick(1000); if(cd_read(3)&7){ cd_write(0,1); cd_write(3,0x1F); cd_write(0,0);} }
}
int main(int argc,char**argv){ if(argc<3){puts("uso: cdtest jogo.cue jogo.bin (setor 22)");return 1;}
  cd_init(); if(!cd_open(argv[1])) return 1;
  FILE*f=fopen(argv[2],"rb"); static uint8_t raw[2352]; fseek(f,22L*2352,SEEK_SET); fread(raw,1,2352,f); fclose(f);
  cmd(0x0E,1,0xA0,0,0);              /* Setmode A0: setor 2340 */
  cmd(0x02,3,0x00,0x02,0x22);        /* Setloc 00:02:22 = LBA 22 */
  cd_write(0,0); cd_write(1,0x06);   /* ReadN */
  int got=0; for(int i=0;i<2000 && !got;i++){ cd_tick(1000); uint8_t fl; cd_write(0,1); fl=cd_read(3)&7; cd_write(0,0);
     if(fl==1) got=1; else if(fl){ cd_write(0,1); cd_write(3,0x1F); cd_write(0,0);} }
  if(!got){ puts("sem INT1"); return 1; }
  cd_write(0,0); cd_write(3,0x00); cd_write(3,0x80);     /* padrao do libcd */
  uint32_t hdr[3], data[512];
  for(int i=0;i<3;i++) hdr[i]=cd_dma_read32();
  cd_write(3,0x80);                                     /* pede de novo no meio */
  for(int i=0;i<512;i++) data[i]=cd_dma_read32();
  int okh = memcmp(hdr, raw+12, 12)==0, okd = memcmp(data, raw+24, 2048)==0;
  printf("cabecalho %s, dados %s (primeiros bytes: %.8s)\n", okh?"OK":"ERRADO", okd?"OK":"DESLOCADOS", (char*)data);
  return !(okh&&okd);
}
