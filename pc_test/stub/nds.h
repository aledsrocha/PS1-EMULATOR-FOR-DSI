#pragma once
#include <stdint.h>
#define MODE_5_2D 0
#define VRAM_A_MAIN_BG 0
#define BgType_Bmp16 0
#define BgSize_B16_256x256 0
static inline void videoSetMode(int m){(void)m;}
static inline void vramSetBankA(int m){(void)m;}
extern uint16_t pc_fb[256*256];
static inline int bgInit(int a,int b,int c,int d,int e){(void)a;(void)b;(void)c;(void)d;(void)e;return 3;}
static inline void *bgGetGfxPtr(int bg){(void)bg;return pc_fb;}
