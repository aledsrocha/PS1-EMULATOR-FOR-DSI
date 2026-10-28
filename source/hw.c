#include <stdio.h>
#include <string.h>
#include "psx.h"

/* ------------------------------------------------------------------ */
/* Estado geral                                                        */
/* ------------------------------------------------------------------ */

static uint8_t  shadow[0x2000];   /* registradores sem logica propria */
static uint32_t istat, imask;
uint32_t st_vblank, st_istat_wr;

static void irq_update(void) { cpu_set_irq((istat & imask) != 0); }

void irq_raise(int n)
{
    istat |= 1u << n;
    irq_update();
}

uint32_t hw_istat(void) { return istat; }
uint32_t hw_imask(void) { return imask; }

static inline uint32_t szmask(int size)
{
    return size == 4 ? 0xFFFFFFFFu : (size == 2 ? 0xFFFFu : 0xFFu);
}

/* ------------------------------------------------------------------ */
/* DMA                                                                 */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t madr, bcr, chcr; } Dma;
static Dma      dma[7];
static uint32_t dpcr, dicr;

static inline uint32_t ram_r(uint32_t a) { return *(uint32_t *)(bus.ram + (a & 0x1FFFFC)); }
static inline void ram_w(uint32_t a, uint32_t v) { *(uint32_t *)(bus.ram + (a & 0x1FFFFC)) = v; }

static void dicr_update(void)
{
    bool old = (dicr >> 31) & 1;
    bool m = (dicr & (1u << 15)) ||
             ((dicr & (1u << 23)) && ((dicr & 0x7F0000u) & ((dicr >> 8) & 0x7F0000u)));
    dicr = (dicr & 0x7FFFFFFFu) | ((uint32_t)m << 31);
    if (m && !old) irq_raise(3);
}

static void dma_run(int ch)
{
    Dma *d = &dma[ch];
    uint32_t chcr = d->chcr;
    int mode = (chcr >> 9) & 3;
    bool from_ram = chcr & 1;
    int32_t step = (chcr & 2) ? -4 : 4;
    uint32_t addr = d->madr & 0x1FFFFC;
    uint32_t words;

    if (mode == 1)      words = (d->bcr & 0xFFFF) * (d->bcr >> 16);
    else                words = d->bcr & 0xFFFF;
    if (mode != 2 && words == 0) words = 0x10000;

    switch (ch) {
    case 6: {   /* OTC: monta a ordering table de tras pra frente */
        for (uint32_t n = words; n > 0; n--) {
            uint32_t next = (n == 1) ? 0xFFFFFF : ((addr - 4) & 0xFFFFFF);
            ram_w(addr, next);
            addr = (addr - 4) & 0x1FFFFC;
        }
        break; }
    case 2:     /* GPU */
        if (mode == 2) {
            for (int guard = 0; guard < 0x100000; guard++) {
                uint32_t hdr = ram_r(addr);
                uint32_t cnt = hdr >> 24;
                for (uint32_t i = 1; i <= cnt; i++) gpu_gp0(ram_r(addr + 4 * i));
                if (hdr & 0x800000) break;
                addr = hdr & 0x1FFFFC;
            }
        } else if (from_ram) {
            for (uint32_t i = 0; i < words; i++) { gpu_gp0(ram_r(addr)); addr = (addr + step) & 0x1FFFFC; }
        } else {
            for (uint32_t i = 0; i < words; i++) { ram_w(addr, gpu_read()); addr = (addr + step) & 0x1FFFFC; }
        }
        break;
    case 3:     /* CD-ROM -> RAM */
        if (!from_ram) {
            /* tamanho 0 = um setor inteiro (Ape Escape), nao 64K palavras */
            if ((d->bcr & 0xFFFF) == 0) words = cd_sector_words();
            /* como no pcsx4all: copia so o que o FIFO tem; nao escreve zeros
               por cima da RAM se o jogo pedir mais que o setor */
            uint32_t avail = (cd_data_left() + 3) / 4;
            if (words > avail) words = avail;
            for (uint32_t i = 0; i < words; i++) { ram_w(addr, cd_dma_read32()); addr = (addr + step) & 0x1FFFFC; }
        }
        break;
    default:    /* MDEC, SPU, PIO: conclui sem fazer nada por enquanto */
        {
            static int warned[7];
            if (warned[ch] < 3) {
                warned[ch]++;
                dbg_log("DMA%d (%s) usado: madr=%08lX bcr=%08lX chcr=%08lX - ainda nao emulado\n", ch,
                        ch == 0 ? "MDEC entrada" : ch == 1 ? "MDEC saida" : ch == 4 ? "SPU" : "outro",
                        (unsigned long)d->madr, (unsigned long)d->bcr, (unsigned long)chcr);
            }
        }
        break;
    }

    d->chcr &= ~0x11000000u;
    if (dicr & (1u << (16 + ch))) dicr |= 1u << (24 + ch);
    dicr_update();
}

static void dma_chcr_write(int ch, uint32_t v)
{
    dma[ch].chcr = v;
    bool start = (v & 0x01000000u) != 0;
    bool trig  = ((v >> 9) & 3) != 0 || (v & 0x10000000u) != 0;   /* modo 0 precisa do trigger */
    if (start && trig) dma_run(ch);
}

/* ------------------------------------------------------------------ */
/* Timers                                                              */
/* ------------------------------------------------------------------ */

typedef struct { uint32_t count, mode, target, frac; bool done, sync_free; } Timer;
static Timer    tmr[3];
static uint32_t line_cycles;
static int      line;

#define LINE_CYCLES 2146   /* ~33.8688MHz / (60 * 263) */

static void timer_tick(int i, uint32_t c, uint32_t lines)
{
    Timer *t = &tmr[i];
    uint32_t src = (t->mode >> 8) & 3, inc, div = 1;

    if (i == 1 && (src & 1)) {
        inc = lines;                              /* fonte = hblank */
    } else {
        if (i == 0 && (src & 1)) div = 6;         /* dotclock aproximado */
        if (i == 2 && (src & 2)) div = 8;         /* sysclk/8 */
        t->frac += c;
        inc = t->frac / div;
        t->frac %= div;
    }
    if (i == 2 && (t->mode & 1)) {                /* sync: modos 0 e 3 param o timer 2 */
        uint32_t sm = (t->mode >> 1) & 3;
        if (sm == 0 || sm == 3) inc = 0;
    }
    if (i == 1 && (t->mode & 1)) {                /* timer 1: sincroniza com o VBlank */
        uint32_t sm = (t->mode >> 1) & 3;
        bool vb = (line >= 240);
        if (sm == 0 && vb) inc = 0;               /* pausa durante o VBlank */
        if (sm == 2 && !vb) inc = 0;              /* so conta durante o VBlank */
        if (sm == 3 && !t->sync_free) inc = 0;    /* pausado ate o 1o VBlank */
    }
    if (!inc) return;

    uint32_t cnt = t->count + inc, tgt = t->target;
    bool hit_t = false, hit_f = false;

    if (t->count < tgt && cnt >= tgt) hit_t = true;
    if (hit_t && (t->mode & 8)) {
        cnt = cnt - tgt;
        if (cnt > 0xFFFF) cnt &= 0xFFFF;
    } else if (cnt > 0xFFFF) {
        hit_f = true;
        cnt &= 0xFFFF;
    }
    t->count = cnt;

    if (hit_t) t->mode |= 0x800;
    if (hit_f) t->mode |= 0x1000;

    if ((hit_t && (t->mode & 0x10)) || (hit_f && (t->mode & 0x20))) {
        if ((t->mode & 0x40) || !t->done) {
            irq_raise(4 + i);
            t->done = true;
        }
    }
}

/* chamado no inicio do VBlank */
static void timers_vblank_event(void)
{
    Timer *t = &tmr[1];
    if (t->mode & 1) {
        uint32_t sm = (t->mode >> 1) & 3;
        if (sm == 1 || sm == 2) t->count = 0;     /* zera o contador no VBlank */
        if (sm == 3) t->sync_free = true;
    }
}


/* ------------------------------------------------------------------ */
/* SIO0: controle (digital pad) na porta 1                             */
/* ------------------------------------------------------------------ */

static struct {
    uint16_t mode, ctrl, baud;
    uint8_t  rx;
    bool     rx_ready, dsr, irqf;
    int      idx;
    int      ack_delay;
} sio;

static uint16_t pad_state = 0xFFFF;

void pad_set(uint16_t m) { pad_state = m; }

static void sio_tx(uint8_t b)
{
    uint8_t rx = 0xFF;
    bool ack = false;

    sio.dsr = false;
    if (!(sio.ctrl & 0x2000)) {                   /* porta 1 selecionada */
        switch (sio.idx) {
        case 0:
            if (b == 0x01) { ack = true; sio.idx = 1; }            /* acesso a controle */
            break;                                                  /* 0x81 = memory card: ausente */
        case 1:
            if (b == 0x42) { rx = 0x41; ack = true; sio.idx = 2; } /* ID: digital pad */
            else sio.idx = 0;
            break;
        case 2: rx = 0x5A; ack = true; sio.idx = 3; break;
        case 3: rx = pad_state & 0xFF; ack = true; sio.idx = 4; break;
        case 4: rx = pad_state >> 8; sio.idx = 0; break;           /* ultimo byte: sem ACK */
        }
    }
    sio.rx = rx;
    sio.rx_ready = true;
    if (ack) sio.ack_delay = 400;
}

static void sio_ctrl_write(uint16_t v)
{
    if (v & 0x40) {                               /* reset */
        sio.mode = 0; sio.ctrl = 0; sio.idx = 0;
        sio.rx_ready = false; sio.irqf = false; sio.dsr = false; sio.ack_delay = 0;
        return;
    }
    if (v & 0x10) { sio.irqf = false; }           /* ack do IRQ */
    if (!(v & 0x02)) sio.idx = 0;                 /* DTR baixo: reinicia protocolo */
    sio.ctrl = v & ~0x50u;
}

static void sio_tick(uint32_t c)
{
    if (sio.ack_delay > 0) {
        sio.ack_delay -= (int)c;
        if (sio.ack_delay <= 0) {
            sio.ack_delay = 0;
            sio.dsr = true;
            sio.irqf = true;
            if (sio.ctrl & 0x1000) irq_raise(7);
        }
    }
}

/* ------------------------------------------------------------------ */
/* Leitura / escrita de registradores                                  */
/* ------------------------------------------------------------------ */

void hw_init(void)
{
    memset(shadow, 0, sizeof shadow);
    memset(dma, 0, sizeof dma);
    memset(tmr, 0, sizeof tmr);
    memset(&sio, 0, sizeof sio);
    cd_init();
    istat = imask = 0;
    dpcr = 0x07654321;
    dicr = 0;
    line = 0; line_cycles = 0;
}

static uint32_t reg32_read(uint32_t base)
{
    if (base >= 0x080 && base < 0x0F0) {
        Dma *d = &dma[(base - 0x080) >> 4];
        switch (base & 0xF) {
        case 0: return d->madr;
        case 4: return d->bcr;
        case 8: return d->chcr;
        default: return 0;
        }
    }
    if (base >= 0x100 && base < 0x130) {
        Timer *t = &tmr[(base - 0x100) >> 4];
        switch (base & 0xF) {
        case 0: return t->count;
        case 4: { uint32_t m = t->mode; t->mode &= ~0x1800u; return m; }
        case 8: return t->target;
        default: return 0;
        }
    }
    switch (base) {
    case 0x040: { uint32_t v = sio.rx; sio.rx_ready = false; return v; }
    case 0x044: return 0x5u | (sio.rx_ready ? 2u : 0) | (sio.dsr ? 0x80u : 0) | (sio.irqf ? 0x200u : 0);
    case 0x048: return (uint32_t)sio.mode | ((uint32_t)sio.ctrl << 16);
    case 0x04C: return (uint32_t)sio.baud << 16;
    case 0x070: return istat;
    case 0x074: return imask;
    case 0x0F0: return dpcr;
    case 0x0F4: return dicr;
    case 0x810: return gpu_read();
    case 0x814: return gpu_stat();
    case 0x824: return 0x80040000u;   /* MDEC status: FIFO vazio */
    default:
        return shadow[base] | (shadow[base + 1] << 8) | (shadow[base + 2] << 16) | ((uint32_t)shadow[base + 3] << 24);
    }
}

static uint32_t hw_read_impl(uint32_t off, int size)
{
    if (off >= 0x800 && off < 0x804) return cd_read((int)(off & 3));

    if (off >= 0xC00 && off < 0x1000) {                    /* SPU */
        if (off == 0xDAE) return (shadow[0xDAA] | (shadow[0xDAB] << 8)) & 0x3F;   /* SPUSTAT = SPUCNT[5:0] */
        uint32_t v = 0;
        for (int i = 0; i < size; i++) v |= (uint32_t)shadow[off + i] << (8 * i);
        return v;
    }
    uint32_t sh = (off & 3) * 8;
    return (reg32_read(off & ~3u) >> sh) & szmask(size);
}

static void hw_write_impl(uint32_t off, uint32_t v, int size)
{
    if (off >= 0x800 && off < 0x804) { cd_write((int)(off & 3), (uint8_t)v); return; }

    if (off >= 0xC00 && off < 0x1000) {                    /* SPU: so guarda os valores */
        for (int i = 0; i < size; i++) shadow[off + i] = (uint8_t)(v >> (8 * i));
        return;
    }

    uint32_t sh = (off & 3) * 8;
    uint32_t mask = szmask(size) << sh;
    uint32_t val = (v << sh) & mask;
    uint32_t base = off & ~3u;
#define MERGE(cur) ((((cur)) & ~mask) | val)

    if (base >= 0x080 && base < 0x0F0) {
        int ch = (int)((base - 0x080) >> 4);
        switch (base & 0xF) {
        case 0: dma[ch].madr = MERGE(dma[ch].madr) & 0xFFFFFF; break;
        case 4: dma[ch].bcr  = MERGE(dma[ch].bcr); break;
        case 8: dma_chcr_write(ch, MERGE(dma[ch].chcr)); break;
        }
        return;
    }
    if (base >= 0x100 && base < 0x130) {
        Timer *t = &tmr[(base - 0x100) >> 4];
        switch (base & 0xF) {
        case 0: t->count = MERGE(t->count) & 0xFFFF; break;
        case 4: t->mode = (MERGE(t->mode) & 0x3FF) | 0x400;
                t->count = 0; t->frac = 0; t->done = false; t->sync_free = false; break;
        case 8: t->target = MERGE(t->target) & 0xFFFF; break;
        }
        return;
    }

    switch (base) {
    case 0x040: sio_tx((uint8_t)v); return;
    case 0x044: return;
    case 0x048:
        sio.mode = (uint16_t)MERGE((uint32_t)sio.mode);
        if (mask & 0xFFFF0000u) sio_ctrl_write((uint16_t)((MERGE((uint32_t)sio.mode | ((uint32_t)sio.ctrl << 16))) >> 16));
        return;
    case 0x04C:
        if (mask & 0xFFFF0000u) sio.baud = (uint16_t)(val >> 16);
        return;
    case 0x070:                                    /* I_STAT: escrever 0 confirma o IRQ */
        istat &= (val | ~mask);
        st_istat_wr++;
        irq_update();
        return;
    case 0x074:
        imask = MERGE(imask) & 0x7FF;
        irq_update();
        return;
    case 0x0F0: dpcr = MERGE(dpcr); return;
    case 0x0F4: {
        uint32_t w = MERGE(dicr);
        uint32_t flags = dicr & 0x7F000000u & ~(val & 0x7F000000u);
        dicr = (w & 0x00FF803Fu) | flags | (dicr & 0x80000000u);
        dicr_update();
        return; }
    case 0x820: case 0x824: {
        static int mdec_warn;
        if (mdec_warn < 5) { mdec_warn++; dbg_log("MDEC: escrita %08lX em 1F80%04lX - ainda nao emulado\n", (unsigned long)v, (unsigned long)(0x1000 + base)); }
        for (int i = 0; i < size; i++) shadow[off + i] = (uint8_t)(v >> (8 * i));
        return; }
    case 0x810: gpu_gp0(v); return;
    case 0x814: gpu_gp1(v); return;
    default:
        for (int i = 0; i < size; i++) shadow[off + i] = (uint8_t)(v >> (8 * i));
        return;
    }
#undef MERGE
}

/* ------------------------------------------------------------------ */
/* Passagem do tempo (chamada pela CPU a cada ~32 instrucoes)          */
/* ------------------------------------------------------------------ */

void hw_tick(uint32_t c)
{
    uint32_t lines = 0;
    line_cycles += c;
    while (line_cycles >= LINE_CYCLES) {
        line_cycles -= LINE_CYCLES;
        lines++;
        if (++line == 240) { irq_raise(0); st_vblank++; gpu_vblank(); timers_vblank_event(); }   /* VBlank */
        if (line >= 263) line = 0;
    }
    for (int i = 0; i < 3; i++) timer_tick(i, c, lines);
    sio_tick(c);
    cd_tick(c);
}

/* ------------------------------------------------------------------ */
/* Trace dos ultimos acessos a I/O (diagnostico)                       */
/* ------------------------------------------------------------------ */

static struct { uint16_t off; uint32_t val; char rw; } tr[8];
static int tr_i;

static void trace(char rw, uint32_t off, uint32_t val)
{
    tr[tr_i & 7].off = (uint16_t)off;
    tr[tr_i & 7].val = val;
    tr[tr_i & 7].rw = rw;
    tr_i++;
}

uint32_t hw_read(uint32_t off, int size)
{
    uint32_t v = hw_read_impl(off, size);
    trace('r', off, v);
    return v;
}

void hw_write(uint32_t off, uint32_t v, int size)
{
    trace('w', off, v);
    hw_write_impl(off, v, size);
}

void hw_trace_dump(char *buf, int n)
{
    int len = 0;
    buf[0] = 0;
    for (int k = 4; k >= 1 && len < n - 1; k--) {
        int i = (tr_i - k) & 7;
        len += snprintf(buf + len, (size_t)(n - len), "%c%04X:%08lX%c",
                        tr[i].rw, 0x1000 + tr[i].off, (unsigned long)tr[i].val,
                        (k == 3 || k == 1) ? '\n' : ' ');
    }
}
