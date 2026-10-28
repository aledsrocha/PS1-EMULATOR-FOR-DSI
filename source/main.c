#include <nds.h>
#include <fat.h>
#include <dirent.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <stdarg.h>
#include <unistd.h>
#include "psx.h"

/* A BIOS NAO e mais embutida no .nds: ela vem do cartao (/psx/scph1001.bin ou
   /scph1001.bin). Embutida, ela ocupava 512 KB da RAM principal, e no modo DS
   (4 MB no total) nao sobrava espaco para a RAM (2 MB) e a VRAM (1 MB) do PS1.
   No modo DS a BIOS vai para a memoria de video livre do DS (bancos B..H,
   exatamente 512 KB contiguos em 0x06820000). No modo DSi (16 MB) vai para a
   RAM principal, que e mais rapida. */
#define DS_VRAM_BIOS ((uint8_t *)0x06820000)

static const char *bios_paths[] = {
    "/psx/scph1001.bin",
    "/scph1001.bin",
    "fat:/psx/scph1001.bin",
    "fat:/scph1001.bin",
    "sd:/psx/scph1001.bin",
    "sd:/scph1001.bin",
    NULL
};

static const char *exe_paths[] = { "/psx/test.exe", "/test.exe", NULL };

static uint8_t *exe_data = NULL;
static size_t   exe_size = 0;

/* Log no cartao: tudo que a BIOS/jogo escreve no TTY + eventos do CD.
   Fica em /psx_log.txt (raiz do SD). Limitado para nao encher o cartao. */
static FILE *logf = NULL;
static int   log_lines = 0;
#define LOG_MAX_LINES 20000

void dbg_log(const char *fmt, ...)
{
    if (!logf || log_lines >= LOG_MAX_LINES) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    log_lines++;
    if (log_lines == LOG_MAX_LINES) fputs("\n[log cheio]\n", logf);
    fflush(logf);
    if (log_lines < 300) fsync(fileno(logf));   /* inicio do boot: grava ja */
}

/* Grava de verdade no cartao. Sem o fsync a libfat so atualiza o tamanho do
   arquivo no fclose, e como o emulador nunca fecha o log, ele ficava vazio. */
static void log_sync(void)
{
    if (!logf) return;
    fflush(logf);
    fsync(fileno(logf));
}

/* ultimas linhas do console do PS1, mostradas na tela de status */
#define TTY_LINES 12
#define TTY_COLS  31   /* 32 colunas na tela: 31 evita quebra dupla */
static char tty_ring[TTY_LINES][TTY_COLS + 1];
static int  tty_cur = 0, tty_col = 0;

void tty_putc(char c)
{
    putchar(c);
    if (c == '\n') {
        tty_cur = (tty_cur + 1) % TTY_LINES;
        tty_ring[tty_cur][0] = 0;
        tty_col = 0;
    } else if (c != '\r' && tty_col < TTY_COLS) {
        tty_ring[tty_cur][tty_col++] = (c == '\t') ? ' ' : c;
        tty_ring[tty_cur][tty_col] = 0;
    }
    if (logf && log_lines < LOG_MAX_LINES) {
        fputc(c, logf);
        if (c == '\n') { log_lines++; fflush(logf); }
    }
}

/* ------------------------------------------------------------------ */
/* Arquivos                                                            */
/* ------------------------------------------------------------------ */

static uint8_t *load_file(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz <= 0) { fclose(f); return NULL; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return NULL; }
    fclose(f);
    *size = (size_t)sz;
    return buf;
}

/* retorna 1 = ok, 0 = nao achou, -1 = tamanho errado */
static int load_bios(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz != BIOS_SIZE) { fclose(f); return -1; }
    /* le em pedacos e copia com escritas de 32 bits: a memoria de video do DS
       ignora escritas de 8 bits, entao o fread nao pode escrever direto nela */
    static uint32_t chunk[1024];
    size_t total = 0;
    while (total < BIOS_SIZE) {
        size_t n = fread(chunk, 1, sizeof chunk, f);
        if (n != sizeof chunk) break;
        uint32_t *dst = (uint32_t *)(bus.bios + total);
        for (size_t i = 0; i < n / 4; i++) dst[i] = chunk[i];
        total += n;
    }
    fclose(f);
    return total == BIOS_SIZE ? 1 : -1;
}

static uint32_t rd32(const uint8_t *p)
{
    return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ------------------------------------------------------------------ */
/* Seletor de disco (navega pelas pastas do SD)                        */
/* ------------------------------------------------------------------ */

#define MAXF 200
#define NAMELEN 128
typedef struct { char name[NAMELEN]; bool dir; } Entry;
static Entry flist[MAXF];
static bool  opt_fastboot = true;      /* pula a animacao da BIOS quando ha disco */

static bool has_ext(const char *n, const char *e)
{
    size_t a = strlen(n), b = strlen(e);
    return a > b && strcasecmp(n + a - b, e) == 0;
}

static bool is_disc_name(const char *nm)
{
    if (strncasecmp(nm, "scph", 4) == 0) return false;               /* BIOS nao e jogo */
    if (!(has_ext(nm, ".cue") || has_ext(nm, ".iso") || has_ext(nm, ".img") || has_ext(nm, ".bin")))
        return false;
    /* esconde trilhas extras "(Track N).bin" */
    if (has_ext(nm, ".bin") && strstr(nm, "(Track ") &&
        !strstr(nm, "(Track 1)") && !strstr(nm, "(Track 01)")) return false;
    return true;
}

static void join_path(char *out, size_t n, const char *dir, const char *name)
{
    size_t l = strlen(dir);
    snprintf(out, n, "%s%s%s", dir, (l && dir[l - 1] == '/') ? "" : "/", name);
}

static int cmp_entry(const void *a, const void *b)
{
    const Entry *x = (const Entry *)a, *y = (const Entry *)b;
    if (x->dir != y->dir) return x->dir ? -1 : 1;                    /* pastas primeiro */
    return strcasecmp(x->name, y->name);
}

/* retorna o numero de itens, ou -1 se a pasta nao abriu */
static int scan_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    char full[300];
    while ((e = readdir(d)) && n < MAXF) {
        const char *nm = e->d_name;
        if (nm[0] == '.' || strlen(nm) >= NAMELEN) continue;
        join_path(full, sizeof full, dir, nm);
        struct stat stt;
        bool isdir = (stat(full, &stt) == 0) && S_ISDIR(stt.st_mode);
        if (!isdir && !is_disc_name(nm)) continue;
        strcpy(flist[n].name, nm);
        flist[n].dir = isdir;
        n++;
    }
    closedir(d);

    /* esconde .bin que tem .cue com o mesmo nome */
    for (int i = 0; i < n; i++) {
        if (flist[i].dir || !has_ext(flist[i].name, ".bin")) continue;
        size_t li = strlen(flist[i].name) - 4;
        for (int j = 0; j < n; j++) {
            if (flist[j].dir || !has_ext(flist[j].name, ".cue")) continue;
            if (strlen(flist[j].name) - 4 == li && strncasecmp(flist[i].name, flist[j].name, li) == 0) {
                flist[i].name[0] = 0;
                break;
            }
        }
    }
    int m = 0;
    for (int i = 0; i < n; i++)
        if (flist[i].name[0]) { if (m != i) flist[m] = flist[i]; m++; }
    qsort(flist, (size_t)m, sizeof(Entry), cmp_entry);
    return m;
}

/* Retorna true se o usuario escolheu um disco (caminho em "out").
   Item 0 = iniciar so a BIOS. A = abrir/escolher, B = pasta de cima,
   SELECT = liga/desliga pular intro, L/R = frameskip. */
static bool pick_disc(char *out, size_t outn)
{
    static const char *start[] = { "/games", "/psx/games", "/psx", "/", NULL };
    char dir[256] = "/";
    int n = -1;
    for (int k = 0; start[k]; k++) {
        n = scan_dir(start[k]);
        if (n > 0) { strcpy(dir, start[k]); break; }
    }
    if (n < 0) { strcpy(dir, "/"); n = scan_dir(dir); }

    int cur = 0;
    for (;;) {
        bool root = strcmp(dir, "/") == 0;
        int total = 1 + (n > 0 ? n : 0);
        if (cur >= total) cur = total - 1;

        consoleClear();
        printf("Escolha o disco\n%.31s\n", dir);
        printf("A:abrir B:voltar SEL:intro L/R:fs\n");
        int first = cur - 7;
        if (first < 0) first = 0;
        for (int i = first; i < total && i < first + 14; i++) {
            if (i == 0) { printf("%c [sem disco: so BIOS]\n", i == cur ? '>' : ' '); continue; }
            const Entry *e = &flist[i - 1];
            printf("%c %s%.27s\n", i == cur ? '>' : ' ', e->dir ? "/" : "", e->name);
        }
        if (n < 0)       printf("\n(pasta nao abriu)\n");
        else if (n == 0) printf("\n(nenhum .cue/.bin/.iso aqui)\n");
        printf("\nIntro da BIOS: %s\n", opt_fastboot ? "pular" : "mostrar");
        printf("Frameskip: %d\n", gpu_frameskip);

        for (;;) {
            if (!pmMainLoop()) return false;
            swiWaitForVBlank();
            scanKeys();
            uint32_t k = keysDown();
            if (!(k & (KEY_UP | KEY_DOWN | KEY_LEFT | KEY_RIGHT | KEY_A | KEY_B | KEY_START |
                       KEY_SELECT | KEY_L | KEY_R))) continue;

            if (k & KEY_UP)    cur = (cur + total - 1) % total;
            if (k & KEY_DOWN)  cur = (cur + 1) % total;
            if (k & KEY_LEFT)  { cur -= 10; if (cur < 0) cur = 0; }
            if (k & KEY_RIGHT) { cur += 10; if (cur >= total) cur = total - 1; }
            if (k & KEY_SELECT) opt_fastboot = !opt_fastboot;
            if ((k & KEY_L) && gpu_frameskip > 0) gpu_frameskip--;
            if ((k & KEY_R) && gpu_frameskip < 5) gpu_frameskip++;

            if ((k & KEY_B) && !root) {                       /* sobe uma pasta */
                char *sl = strrchr(dir, '/');
                if (sl == dir) dir[1] = 0; else if (sl) *sl = 0;
                n = scan_dir(dir); cur = 0;
            } else if (k & (KEY_A | KEY_START)) {
                if (cur == 0) { consoleClear(); return false; }
                const Entry *e = &flist[cur - 1];
                if (e->dir) {
                    char nd[256];
                    join_path(nd, sizeof nd, dir, e->name);
                    if (strlen(nd) < sizeof dir) { strcpy(dir, nd); n = scan_dir(dir); cur = 0; }
                } else {
                    join_path(out, outn, dir, e->name);
                    consoleClear();
                    return true;
                }
            }
            break;
        }
    }
}

/* ------------------------------------------------------------------ */
/* Side-load de PSX-EXE e controles                                    */
/* ------------------------------------------------------------------ */

static bool have_disc = false;

/* Chamado quando a BIOS vai entrar no shell (0x80030000). */
static void shell_hook(void)
{
    printf(">> shell (cyc=%lu)\n", (unsigned long)cpu.cycles);
    dbg_log(">> shell em %lu ciclos\n", (unsigned long)cpu.cycles);

    if (!exe_data) {
        if (have_disc && opt_fastboot) {
            /* Fast boot: o shell e chamado como funcao; voltando direto (jr ra)
               a BIOS segue para ler o SYSTEM.CNF e carregar o jogo. Pula a
               animacao da Sony/PlayStation (~28x menos instrucoes ate o jogo). */
            cpu.pc = cpu.r[31];
            cpu.next_pc = cpu.pc + 4;
            cpu.in_delay = false;
            printf(">> intro da BIOS pulada\n");
            dbg_log(">> intro da BIOS pulada\n");
        }
        return;
    }
    if (exe_size < 0x800 || memcmp(exe_data, "PS-X EXE", 8) != 0) {
        printf(">> PSX-EXE invalido\n");
        return;
    }
    uint32_t pc0   = rd32(exe_data + 0x10);
    uint32_t gp0   = rd32(exe_data + 0x14);
    uint32_t taddr = rd32(exe_data + 0x18) & 0x1FFFFF;
    uint32_t tsize = rd32(exe_data + 0x1C);
    uint32_t sp    = rd32(exe_data + 0x30);
    uint32_t spoff = rd32(exe_data + 0x34);

    if (tsize > exe_size - 0x800) tsize = (uint32_t)(exe_size - 0x800);
    if (taddr + tsize > RAM_SIZE)  tsize = RAM_SIZE - taddr;
    memcpy(bus.ram + taddr, exe_data + 0x800, tsize);

    cpu.r[28] = gp0;
    if (sp) { cpu.r[29] = sp + spoff; cpu.r[30] = cpu.r[29]; }
    cpu.pc = pc0;
    cpu.next_pc = pc0 + 4;
    cpu.in_delay = false;
    printf(">> EXE: pc=%08lX %lu bytes\n", (unsigned long)pc0, (unsigned long)tsize);
}

static uint16_t build_pad(uint32_t k)
{
    uint16_t m = 0xFFFF;
    if (k & KEY_SELECT) m &= ~(1u << 0);
    if (k & KEY_START)  m &= ~(1u << 3);
    if (k & KEY_UP)     m &= ~(1u << 4);
    if (k & KEY_RIGHT)  m &= ~(1u << 5);
    if (k & KEY_DOWN)   m &= ~(1u << 6);
    if (k & KEY_LEFT)   m &= ~(1u << 7);
    if (k & KEY_L)      m &= ~(1u << 10);
    if (k & KEY_R)      m &= ~(1u << 11);
    if (k & KEY_X)      m &= ~(1u << 12);
    if (k & KEY_A)      m &= ~(1u << 13);
    if (k & KEY_B)      m &= ~(1u << 14);
    if (k & KEY_Y)      m &= ~(1u << 15);
    return m;
}

static void fail(const char *msg)
{
    printf("\nERRO: %s\nPressione START\n", msg);
    while (pmMainLoop()) {
        swiWaitForVBlank();
        scanKeys();
        if (keysDown() & KEY_START) break;
    }
}

/* ------------------------------------------------------------------ */

int main(void)
{
    /* Console de texto na tela de baixo usando so o banco I (16 KB), para
       deixar os bancos B..H livres (a BIOS vai para la no modo DS). */
    videoSetModeSub(MODE_0_2D);
    vramSetBankI(VRAM_I_SUB_BG_0x06208000);
    consoleInit(NULL, 0, BgType_Text4bpp, BgSize_T_256x256, 20, 2, false, true);
    printf("PSX-DS  etapa 5b+GTE\n");

    if (isDSiMode()) {
        setCpuClock(true);
        printf("DSi: 133 MHz\n");
    } else {
        printf("Modo DS (67 MHz)\n");
    }

    if (!fatInitDefault()) { fail("FAT/SD nao iniciou"); return 1; }
    logf = fopen("/psx_log.txt", "w");
    dbg_log("PSX-DS etapa 5b+GTE - log (%s)\n", isDSiMode() ? "DSi" : "DS");

    /* Ordem importa: os blocos grandes primeiro (VRAM 1 MB, RAM 2 MB), para a
       memoria nao ficar fragmentada. No modo DS ha so 4 MB no total. */
    if (!gpu_init()) { fail(isDSiMode() ? "sem memoria p/ VRAM" : "sem memoria p/ VRAM\n(modo DS tem so 4 MB;\n use o modo DSi)"); return 1; }

    uint8_t *bios_mem = NULL;
    if (!isDSiMode()) {
        vramSetBankB(VRAM_B_LCD); vramSetBankC(VRAM_C_LCD); vramSetBankD(VRAM_D_LCD);
        vramSetBankE(VRAM_E_LCD); vramSetBankF(VRAM_F_LCD); vramSetBankG(VRAM_G_LCD);
        vramSetBankH(VRAM_H_LCD);
        bios_mem = DS_VRAM_BIOS;
        printf("BIOS na VRAM do DS\n");
    }
    if (!bus_init(bios_mem)) { fail(isDSiMode() ? "sem memoria p/ RAM" : "sem memoria p/ RAM\n(modo DS tem so 4 MB;\n use o modo DSi)"); return 1; }

    int ok = 0;
    for (int k = 0; ok != 1 && bios_paths[k]; k++) {
        ok = load_bios(bios_paths[k]);
        if (ok == 1) { printf("BIOS: %s\n", bios_paths[k]); dbg_log("BIOS: %s\n", bios_paths[k]); }
        else if (ok == -1) { fail("BIOS deve ter 512 KB"); return 1; }
    }
    if (ok != 1) { fail("BIOS nao achada.\nCopie scph1001.bin para\n/psx/ ou para a raiz do SD"); return 1; }

    char disc[200];
    bool want_disc = pick_disc(disc, sizeof disc);

    printf("PSX-DS  etapa 5b+GTE\n");
    printf("intro: %s  frameskip: %d\n", opt_fastboot ? "pular" : "mostrar", gpu_frameskip);
    hw_init();

    if (want_disc) {
        dbg_log("disco: %s\n", disc);
        if (cd_open(disc)) { printf("Disco OK\n"); have_disc = true; dbg_log("disco aberto OK\n"); }
        else { printf("Falha: %s\n%.62s\n", cd_error(), disc); dbg_log("FALHA ao abrir: %s\n", cd_error()); }
    } else {
        for (int k = 0; exe_paths[k] && !exe_data; k++) {
            exe_data = load_file(exe_paths[k], &exe_size);
            if (exe_data) printf("EXE: %s\n", exe_paths[k]);
        }
        if (!exe_data) printf("Sem disco e sem EXE\n");
    }

    cpu_reset();
    cpu_boot_hook = shell_hook;

    printf("Rodando (START+SELECT sai)\n");

    time_t t0 = time(NULL), tlast = t0;
    while (pmMainLoop()) {
        cpu_run(50000);

        scanKeys();
        uint32_t held = keysHeld();
        pad_set(build_pad(held));
        if ((held & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) break;

        time_t now = time(NULL);
        if (now - tlast >= 10) {
            tlast = now;
            long el = (long)(now - t0);
            if (el < 1) el = 1;
            unsigned pct10 = (unsigned)((cpu.cycles * 1000ULL) / (33868800ULL * (unsigned long long)el));
            uint32_t g0, g1; int doff;
            char tb[96];
            gpu_stats(&g0, &g1, &doff);
            hw_trace_dump(tb, sizeof tb);
            dbg_log("[status] pc=%08lX c=%luM %u.%u%% irq=%lu vb=%lu is=%lX im=%lX sr=%08lX cd=%ld xa=%lu gpu=%lu/%lu\n",
                    (unsigned long)cpu.pc, (unsigned long)(cpu.cycles / 1000000), pct10 / 10, pct10 % 10,
                    (unsigned long)st_irq_taken, (unsigned long)st_vblank, (unsigned long)hw_istat(),
                    (unsigned long)hw_imask(), (unsigned long)cpu.cop0[12], cd_position(), cd_xa_skipped(),
                    (unsigned long)g0, (unsigned long)g1);
            log_sync();

            /* tela de status fixa: numeros + ultimas linhas do console do PS1 */
            consoleClear();
            printf("pc=%08lX c=%luM %u.%u%%\n", (unsigned long)cpu.pc,
                   (unsigned long)(cpu.cycles / 1000000), pct10 / 10, pct10 % 10);
            printf("irq=%lu vb=%lu ack=%lu\n", (unsigned long)st_irq_taken,
                   (unsigned long)st_vblank, (unsigned long)st_istat_wr);
            printf("is=%lX im=%lX sr=%08lX g=%lu/%lu\n", (unsigned long)hw_istat(),
                   (unsigned long)hw_imask(), (unsigned long)cpu.cop0[12],
                   (unsigned long)g0, (unsigned long)g1);
            printf("%s", tb);
            printf("--- console do PS1 ---\n");
            for (int k = 1; k <= TTY_LINES; k++) {
                const char *ln = tty_ring[(tty_cur + k) % TTY_LINES];
                if (ln[0]) printf("%s\n", ln);
            }
        }
    }
    return 0;
}
