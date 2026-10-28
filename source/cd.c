#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "psx.h"

/* Para o emulador lento nao levar uma eternidade lendo o disco, os tempos de
   leitura/seek sao divididos por este fator. 1 = tempo real do PS1. */
#define SPEEDUP     4
#define CYC_SECTOR  451584      /* 33868800 / 75 (velocidade 1x) */
#define T_ACK       20000

typedef struct { int delay; uint8_t type; uint8_t data[8]; int len; bool first; } Ev;

static struct {
    FILE *f;
    bool disc;
    int  ssize;                 /* 2352 (raw) ou 2048 (iso) */
    long total;                 /* setores no arquivo */
    int  ntracks;
    long tstart[100];           /* inicio de cada trilha, em setores do arquivo */

    uint8_t index, ie, flags;
    uint8_t params[16]; int nparams;
    uint8_t resp[16];   int rlen, rpos;
    Ev q[8]; int qn;

    uint8_t mode;
    uint8_t ffile, fchan;
    long loc, pos;
    bool loc_set;
    bool reading;
    int  rtimer;

    uint8_t sec[2352];
    uint8_t hdr[8];
    int  dptr, dend;
    bool drq;
    char region;                /* 'A', 'E' ou 'I' */
    unsigned long xa_skipped;   /* setores de audio XA descartados */
} cd;

long cd_position(void) { return cd.pos; }
unsigned long cd_xa_skipped(void) { return cd.xa_skipped; }

static inline uint8_t bin2bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }
static inline int bcd2bin(uint8_t v) { return (v >> 4) * 10 + (v & 15); }

static void msf(long lba, uint8_t *m, uint8_t *s, uint8_t *f)   /* lba do arquivo -> MSF absoluto BCD */
{
    long a = lba + 150;
    *m = bin2bcd((int)(a / 75 / 60));
    *s = bin2bcd((int)((a / 75) % 60));
    *f = bin2bcd((int)(a % 75));
}

static uint8_t cd_stat(void)
{
    if (!cd.disc) return 0x10;                    /* tampa aberta / sem disco */
    return (uint8_t)(0x02 | (cd.reading ? 0x20 : 0));
}

static void post(int delay, uint8_t type, const uint8_t *d, int len, bool first)
{
    if (cd.qn >= 8) return;
    Ev *e = &cd.q[cd.qn++];
    e->delay = delay; e->type = type; e->len = len; e->first = first;
    memcpy(e->data, d, (size_t)len);
}

static void post1(int delay, uint8_t type, uint8_t b, bool first)
{
    post(delay, type, &b, 1, first);
}

/* ------------------------------------------------------------------ */
/* Imagem de disco                                                     */
/* ------------------------------------------------------------------ */

static bool read_sector(long lba)
{
    if (!cd.f || lba < 0 || lba >= cd.total) { memset(cd.sec, 0, sizeof cd.sec); return false; }
    if (fseek(cd.f, lba * cd.ssize, SEEK_SET) != 0) return false;
    if (cd.ssize == 2352) {
        if (fread(cd.sec, 1, 2352, cd.f) != 2352) return false;
    } else {
        memset(cd.sec, 0, 24);
        if (fread(cd.sec + 24, 1, 2048, cd.f) != 2048) return false;
        msf(lba, &cd.sec[12], &cd.sec[13], &cd.sec[14]);
        cd.sec[15] = 2;                              /* modo 2 */
        cd.sec[18] = cd.sec[22] = 0x08;              /* submode: dados */
    }
    memcpy(cd.hdr, cd.sec + 12, 8);
    return true;
}

static bool contains(const uint8_t *b, int n, const char *s)
{
    int l = (int)strlen(s);
    for (int i = 0; i + l <= n; i++)
        if (memcmp(b + i, s, (size_t)l) == 0) return true;
    return false;
}

static char cd_err[96];
const char *cd_error(void) { return cd_err; }

static void set_err(const char *msg, const char *name)
{
    snprintf(cd_err, sizeof cd_err, "%s%s%.40s", msg, name ? ": " : "", name ? name : "");
}

bool cd_open(const char *path)
{
    char bin[256];
    char dir[256];
    char stem[256];
    int ssize = 2352;
    int ntracks = 1;
    long tstart[100];
    memset(tstart, 0, sizeof tstart);
    cd_err[0] = 0;

    strncpy(bin, path, sizeof bin - 1); bin[sizeof bin - 1] = 0;
    strncpy(dir, path, sizeof dir - 1); dir[sizeof dir - 1] = 0;
    char *sl = strrchr(dir, '/');
    if (sl) sl[1] = 0; else dir[0] = 0;

    strncpy(stem, path, sizeof stem - 1); stem[sizeof stem - 1] = 0;
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    const char *ext = strrchr(path, '.');
    bool is_cue = ext && !strcasecmp(ext, ".cue");
    char cuefile[128] = {0};

    if (is_cue) {
        FILE *c = fopen(path, "r");
        if (!c) { set_err("cue nao abriu", path); return false; }
        char line[300];
        int track = 0, nfile = 0;
        bin[0] = 0;
        while (fgets(line, sizeof line, c)) {
            char *p = line;
            if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;  /* BOM */
            while (*p == ' ' || *p == '\t') p++;
            if (!strncasecmp(p, "FILE", 4)) {
                nfile++;
                if (nfile == 1) {
                    char *q = p + 4;
                    while (*q == ' ' || *q == '\t') q++;
                    char name[200]; int n = 0;
                    if (*q == '"') {
                        q++;
                        while (*q && *q != '"' && n < 199) name[n++] = *q++;
                    } else {
                        while (*q && *q != ' ' && *q != '\t' && *q != '\r' && *q != '\n' && n < 199) name[n++] = *q++;
                    }
                    name[n] = 0;
                    for (char *z = name; *z; z++) if (*z == '\\') *z = '/';
                    char *base = strrchr(name, '/');
                    base = base ? base + 1 : name;
                    strncpy(cuefile, base, sizeof cuefile - 1);
                    snprintf(bin, sizeof bin, "%s%s", dir, base);
                }
            } else if (!strncasecmp(p, "TRACK", 5)) {
                int n; char mode[16] = {0};
                if (sscanf(p + 5, " %d %15s", &n, mode) >= 1 && n >= 1 && n <= 99) {
                    track = n;
                    if (n > ntracks) ntracks = n;
                    if (n == 1 && strstr(mode, "2048")) ssize = 2048;
                }
            } else if (!strncasecmp(p, "INDEX", 5)) {
                int idx, m, s, f;
                if (sscanf(p + 5, " %d %d:%d:%d", &idx, &m, &s, &f) == 4 && idx == 1 && track >= 1 && track <= 99)
                    tstart[track - 1] = (nfile > 1) ? -1 : (long)(m * 60 + s) * 75 + f;
            }
        }
        fclose(c);
        if (!bin[0]) { set_err("cue sem linha FILE", NULL); return false; }
    }

    FILE *f = fopen(bin, "rb");
    if (!f && is_cue) {                       /* plano B: mesmo nome do .cue, com .bin */
        char alt[260];
        snprintf(alt, sizeof alt, "%s.bin", stem);
        f = fopen(alt, "rb");
        if (!f) { snprintf(alt, sizeof alt, "%s.BIN", stem); f = fopen(alt, "rb"); }
    }
    if (!f) { set_err("bin nao abriu", is_cue ? cuefile : bin); return false; }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) { fclose(f); set_err("arquivo vazio/ilegivel", NULL); return false; }

    if (!is_cue) {
        if (ext && !strcasecmp(ext, ".iso")) ssize = 2048;
        else ssize = (size % 2352 == 0) ? 2352 : 2048;
    } else if (size % ssize != 0 && size % 2352 == 0) {
        ssize = 2352;
    }

    if (cd.f) fclose(cd.f);
    cd.f = f;
    cd.ssize = ssize;
    cd.total = size / ssize;
    cd.ntracks = ntracks;
    for (int i = 0; i < 100; i++) cd.tstart[i] = (tstart[i] < 0) ? cd.total : tstart[i];
    cd.disc = true;

    /* regiao: procura a licenca no setor 4 */
    cd.region = 'A';
    if (read_sector(4)) {
        const uint8_t *d = cd.sec + 24;
        if (contains(d, 2048, "Euro")) cd.region = 'E';
        else if (contains(d, 2048, "Amer")) cd.region = 'A';
        else if (contains(d, 2048, "Inc")) cd.region = 'I';
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Comandos                                                            */
/* ------------------------------------------------------------------ */

static void cmd_nodisc(uint8_t cmd, uint8_t p0)
{
    uint8_t st = cd_stat();
    switch (cmd) {
    case 0x19:
        if (p0 == 0x20) { static const uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; post(T_ACK, 3, v, 4, true); }
        else post1(T_ACK, 3, st, true);
        break;
    case 0x1A: { static const uint8_t id[8] = {0x08, 0x40, 0, 0, 0, 0, 0, 0};
                 post1(T_ACK, 3, st, true); post(20000, 5, id, 8, false); break; }
    case 0x0A: case 0x08: case 0x09:
        post1(T_ACK, 3, st, true); post1(20000, 2, st, false); break;
    case 0x03: case 0x06: case 0x15: case 0x16: case 0x1B: case 0x1E: {
        uint8_t e[2] = { (uint8_t)(st | 1), 0x80 };
        post(T_ACK, 5, e, 2, true);
        break; }
    default:
        post1(T_ACK, 3, st, true);
        break;
    }
}

static void cd_command(uint8_t cmd)
{
    uint8_t p[16];
    int np = cd.nparams;
    memcpy(p, cd.params, (size_t)np);
    dbg_log("CD cmd %02X (%d par: %02X %02X %02X) pos=%ld\n", cmd, np,
            np > 0 ? cd.params[0] : 0, np > 1 ? cd.params[1] : 0, np > 2 ? cd.params[2] : 0, cd.pos);
    cd.nparams = 0;

    if (!cd.disc) { cmd_nodisc(cmd, np ? p[0] : 0); return; }

    uint8_t st = cd_stat();
    uint8_t r[8];

    switch (cmd) {
    case 0x01: post1(T_ACK, 3, st, true); break;                         /* GetStat */

    case 0x02:                                                           /* Setloc */
        if (np >= 3) {
            long m = bcd2bin(p[0]), s = bcd2bin(p[1]), f = bcd2bin(p[2]);
            cd.loc = (m * 60 + s) * 75 + f - 150;
            if (cd.loc < 0) cd.loc = 0;
            cd.loc_set = true;
        }
        post1(T_ACK, 3, st, true);
        break;

    case 0x03: case 0x04: case 0x05:                                     /* Play/Fwd/Rew: sem audio CD */
        post1(T_ACK, 3, st, true);
        break;

    case 0x06: case 0x1B:                                                /* ReadN / ReadS */
        if (cd.loc_set) { cd.pos = cd.loc; cd.loc_set = false; }
        cd.reading = true;
        cd.rtimer = 150000 / SPEEDUP;
        post1(T_ACK, 3, st, true);
        break;

    case 0x08:                                                           /* Stop */
        cd.reading = false;
        post1(T_ACK, 3, st, true);
        post1(60000, 2, 0x00, false);
        break;

    case 0x09:                                                           /* Pause */
        cd.reading = false;
        post1(T_ACK, 3, st, true);
        post1(70000 / SPEEDUP + 20000, 2, 0x02, false);
        break;

    case 0x0A:                                                           /* Init */
        cd.mode = 0; cd.reading = false; cd.drq = false;
        post1(T_ACK, 3, 0x02, true);
        post1(40000, 2, 0x02, false);
        break;

    case 0x0B: case 0x0C: post1(T_ACK, 3, st, true); break;              /* Mute / Demute */

    case 0x0D:                                                           /* Setfilter */
        if (np >= 2) { cd.ffile = p[0]; cd.fchan = p[1]; }
        post1(T_ACK, 3, st, true);
        break;

    case 0x0E:                                                           /* Setmode */
        if (np >= 1) cd.mode = p[0];
        post1(T_ACK, 3, st, true);
        break;

    case 0x0F:                                                           /* Getparam */
        r[0] = st; r[1] = cd.mode; r[2] = 0; r[3] = cd.ffile; r[4] = cd.fchan;
        post(T_ACK, 3, r, 5, true);
        break;

    case 0x10:                                                           /* GetlocL */
        memcpy(r, cd.hdr, 8);
        post(T_ACK, 3, r, 8, true);
        break;

    case 0x11: {                                                         /* GetlocP */
        uint8_t m, s, f, am, as, af;
        long rel = cd.pos;
        m = bin2bcd((int)(rel / 75 / 60)); s = bin2bcd((int)((rel / 75) % 60)); f = bin2bcd((int)(rel % 75));
        msf(cd.pos, &am, &as, &af);
        r[0] = 1; r[1] = 1; r[2] = m; r[3] = s; r[4] = f; r[5] = am; r[6] = as; r[7] = af;
        post(T_ACK, 3, r, 8, true);
        break; }

    case 0x13:                                                           /* GetTN */
        r[0] = st; r[1] = 0x01; r[2] = bin2bcd(cd.ntracks);
        post(T_ACK, 3, r, 3, true);
        break;

    case 0x14: {                                                         /* GetTD */
        int t = np ? bcd2bin(p[0]) : 0;
        long lba = (t == 0 || t > cd.ntracks) ? cd.total : cd.tstart[t - 1];
        uint8_t m, s, f;
        msf(lba, &m, &s, &f);
        r[0] = st; r[1] = m; r[2] = s;
        post(T_ACK, 3, r, 3, true);
        break; }

    case 0x15: case 0x16:                                                /* SeekL / SeekP */
        if (cd.loc_set) { cd.pos = cd.loc; cd.loc_set = false; }
        cd.reading = false;
        post1(T_ACK, 3, st, true);
        post1(100000 / SPEEDUP + 20000, 2, 0x02, false);
        break;

    case 0x19:                                                           /* Test */
        if (np && p[0] == 0x20) { static const uint8_t v[4] = {0x94, 0x09, 0x19, 0xC0}; post(T_ACK, 3, v, 4, true); }
        else post1(T_ACK, 3, st, true);
        break;

    case 0x1A:                                                           /* GetID */
        r[0] = 0x02; r[1] = 0x00; r[2] = 0x20; r[3] = 0x00;
        r[4] = 'S'; r[5] = 'C'; r[6] = 'E'; r[7] = (uint8_t)cd.region;
        post1(T_ACK, 3, st, true);
        post(33000, 2, r, 8, false);
        break;

    case 0x1E:                                                           /* ReadTOC */
        post1(T_ACK, 3, st, true);
        post1(200000 / SPEEDUP, 2, 0x02, false);
        break;

    default: {                                                           /* comando desconhecido */
        uint8_t e[2] = { (uint8_t)(st | 1), 0x40 };
        post(T_ACK, 5, e, 2, true);
        break; }
    }
}

/* ------------------------------------------------------------------ */
/* Registradores                                                       */
/* ------------------------------------------------------------------ */

void cd_init(void)
{
    FILE *f = cd.f;
    bool disc = cd.disc;
    int ssize = cd.ssize, ntracks = cd.ntracks;
    long total = cd.total;
    long tstart[100];
    char region = cd.region;
    memcpy(tstart, cd.tstart, sizeof tstart);

    memset(&cd, 0, sizeof cd);

    cd.f = f; cd.disc = disc; cd.ssize = ssize; cd.total = total;
    cd.ntracks = ntracks; cd.region = region;
    memcpy(cd.tstart, tstart, sizeof tstart);
}

void cd_write(int reg, uint8_t v)
{
    if (reg == 0) { cd.index = v & 3; return; }

    switch (cd.index * 4 + reg) {
    case 0 * 4 + 1: cd_command(v); break;                                   /* comando */
    case 0 * 4 + 2: if (cd.nparams < 16) cd.params[cd.nparams++] = v; break; /* parametro */
    case 0 * 4 + 3:                                                          /* pedido de dados */
        /* Como no pcsx4all: o setor so e carregado no FIFO na PRIMEIRA vez que
           o jogo pede (bit 7). Pedir de novo no meio da leitura nao volta ao
           inicio - antes voltava, e um jogo que le o cabecalho (12 bytes),
           pede de novo e le os dados recebia tudo deslocado 12 bytes
           (Marvel vs. Capcom saltava para o texto " EXE"). Escrever 0 nao
           apaga nada; o FIFO so e descartado quando chega o proximo setor. */
        if ((v & 0x80) && !cd.drq) {
            if (cd.mode & 0x20) { cd.dptr = 12; cd.dend = 12 + 2340; }
            else                { cd.dptr = 24; cd.dend = 24 + 2048; }
            cd.drq = true;
        }
        break;
    case 1 * 4 + 2:                                                          /* mascara de IRQ */
        cd.ie = v & 0x1F;
        if (cd.flags & cd.ie) irq_raise(2);
        break;
    case 1 * 4 + 3:                                                          /* ack de flags */
        cd.flags &= (uint8_t)~(v & 0x1F);
        if (v & 0x40) cd.nparams = 0;
        break;
    default: break;
    }
}

uint8_t cd_read(int reg)
{
    switch (reg) {
    case 0: {
        uint8_t s = (uint8_t)(cd.index | 0x10);
        if (!cd.nparams) s |= 0x08;
        if (cd.rpos < cd.rlen) s |= 0x20;
        if (cd.drq && cd.dptr < cd.dend) s |= 0x40;
        if (cd.qn && cd.q[0].first) s |= 0x80;
        return s; }
    case 1:
        return (cd.rpos < cd.rlen) ? cd.resp[cd.rpos++] : 0;
    case 2:
        if (cd.drq && cd.dptr < cd.dend) return cd.sec[cd.dptr++];
        return 0;
    default:
        return (cd.index & 1) ? (uint8_t)(cd.flags | 0xE0) : (uint8_t)(cd.ie | 0xE0);
    }
}

/* tamanho do setor em palavras no modo atual (DMA com tamanho 0) */
uint32_t cd_sector_words(void)
{
    return (cd.mode & 0x20) ? 2340 / 4 : 2048 / 4;
}

/* bytes ainda disponiveis no FIFO de dados */
uint32_t cd_data_left(void)
{
    return (cd.drq && cd.dptr < cd.dend) ? (uint32_t)(cd.dend - cd.dptr) : 0;
}

uint32_t cd_dma_read32(void)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        uint8_t b = (cd.drq && cd.dptr < cd.dend) ? cd.sec[cd.dptr++] : 0;
        v |= (uint32_t)b << (8 * i);
    }
    return v;
}

/* ------------------------------------------------------------------ */
/* Passagem do tempo                                                   */
/* ------------------------------------------------------------------ */

static void deliver(const uint8_t *data, int len, uint8_t type)
{
    cd.rlen = len;
    cd.rpos = 0;
    memcpy(cd.resp, data, (size_t)len);
    cd.flags = type;
    if (cd.flags & cd.ie) irq_raise(2);
}

void cd_tick(uint32_t c)
{
    if (cd.qn && cd.flags == 0) {
        cd.q[0].delay -= (int)c;
        if (cd.q[0].delay <= 0) {
            Ev e = cd.q[0];
            for (int i = 1; i < cd.qn; i++) cd.q[i - 1] = cd.q[i];
            cd.qn--;
            deliver(e.data, e.len, e.type);
        }
        return;
    }

    if (cd.reading && cd.qn == 0) {
        if (cd.rtimer > 0) cd.rtimer -= (int)c;
        if (cd.rtimer <= 0 && cd.flags == 0) {
            if (cd.pos >= cd.total || !read_sector(cd.pos)) {
                dbg_log("CD: leitura falhou no setor %ld (total %ld)\n", cd.pos, cd.total);
                cd.reading = false;
                uint8_t s = cd_stat();
                deliver(&s, 1, 4);                       /* INT4: fim dos dados */
                return;
            }
            cd.pos++;
            int base = (cd.mode & 0x80) ? CYC_SECTOR / 2 : CYC_SECTOR;
            cd.rtimer = base / SPEEDUP;
            /* Modo XA-ADPCM (bit 6): setores de AUDIO (submode bit 2) vao para o
               chip de som e NAO chegam ao jogo como dados (como no pcsx4all).
               Sem SPU ainda, eles sao so descartados. Videos STR e musicas
               intercalam audio e dados no mesmo trecho do disco. */
            if ((cd.mode & 0x40) && cd.ssize == 2352 && (cd.sec[18] & 0x04)) {
                cd.xa_skipped++;
                return;
            }
            cd.drq = false;
            uint8_t s = cd_stat();
            deliver(&s, 1, 1);                           /* INT1: setor pronto */
        }
    }
}
