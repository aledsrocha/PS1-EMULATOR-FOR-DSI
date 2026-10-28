#ifndef PSX_H
#define PSX_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ITCM/DTCM: so existem no DS/DSi. No PC (testes) viram nada. */
#ifdef ARM9
#include <nds/ndstypes.h>
#endif
#ifndef ITCM_CODE
#define ITCM_CODE
#endif
#ifndef DTCM_BSS
#define DTCM_BSS
#endif
#ifndef DTCM_DATA
#define DTCM_DATA
#endif

#define RAM_SIZE  (2 * 1024 * 1024)
#define BIOS_SIZE (512 * 1024)

typedef struct {
    uint32_t r[32];
    uint32_t pc, next_pc;
    uint32_t hi, lo;
    uint32_t cop0[16];      /* 8=BadVAddr 12=SR 13=CAUSE 14=EPC 15=PRID */
    uint32_t ld_reg, ld_val;
    bool     in_delay;
    uint64_t cycles;
} Cpu;

typedef struct {
    uint8_t *ram;
    uint8_t *bios;
    uint8_t  scratch[1024];
    uint32_t cache_ctrl;     /* 0xFFFE0130 */
} Bus;

extern Cpu cpu;
extern Bus bus;
extern void (*cpu_boot_hook)(void);
extern uint32_t st_irq_taken, st_vblank, st_istat_wr;   /* contadores de diagnostico */

/* bus.c */
bool     bus_init(uint8_t *bios_area);   /* NULL = reservar 512 KB de RAM p/ a BIOS */
uint32_t bus_read8(uint32_t a);
uint32_t bus_read16(uint32_t a);
uint32_t bus_read32(uint32_t a);
void     bus_write8(uint32_t a, uint32_t v);
void     bus_write16(uint32_t a, uint32_t v);
void     bus_write32(uint32_t a, uint32_t v);

/* cpu.c */
void cpu_reset(void);
void cpu_run(int n);
void cpu_set_irq(bool active);

/* hw.c : IRQ, DMA, timers, SIO0 (controle), CD-ROM (stub), SPU (stub) */
void     hw_init(void);
void     hw_tick(uint32_t cycles);
uint32_t hw_read(uint32_t off, int size);          /* off = addr - 0x1F801000 */
void     hw_write(uint32_t off, uint32_t v, int size);
void     irq_raise(int n);
void     pad_set(uint16_t buttons_active_low);
uint32_t hw_istat(void);
uint32_t hw_imask(void);
void     hw_trace_dump(char *buf, int n);   /* ultimos acessos a I/O */

/* gte.c : coprocessador 2 (GTE), portado do pcsx4all (GPL) */
void     gte_reset(void);
uint32_t gte_read_data(int reg);
void     gte_write_data(int reg, uint32_t v);
uint32_t gte_read_ctrl(int reg);
void     gte_write_ctrl(int reg, uint32_t v);
bool     gte_command(uint32_t instr);

/* cd.c : controlador de CD-ROM + imagem de disco (etapa 5) */
void     cd_init(void);
bool     cd_open(const char *path);          /* .cue, .bin, .img ou .iso */
const char *cd_error(void);                  /* motivo da ultima falha em cd_open */
uint8_t  cd_read(int reg);
void     cd_write(int reg, uint8_t v);
void     cd_tick(uint32_t cycles);
uint32_t cd_dma_read32(void);
uint32_t cd_sector_words(void);              /* 512 ou 585 conforme o modo */
uint32_t cd_data_left(void);                 /* bytes restantes no FIFO */
long     cd_position(void);                  /* proximo setor a ler */
unsigned long cd_xa_skipped(void);           /* setores de audio XA descartados */

/* gpu.c : GPU em software (etapa 3) */
bool     gpu_init(void);
void     gpu_stats(uint32_t *gp0n, uint32_t *gp1n, int *disp_off);
void     gpu_gp0(uint32_t w);
void     gpu_gp1(uint32_t w);
uint32_t gpu_read(void);
uint32_t gpu_stat(void);
void     gpu_vblank(void);
extern int gpu_frameskip;                   /* 0 = desenha todo quadro, 1 = 1 sim/1 nao, ... */

/* main.c */
void tty_putc(char c);
void dbg_log(const char *fmt, ...);          /* grava em /psx_log.txt */

#endif
