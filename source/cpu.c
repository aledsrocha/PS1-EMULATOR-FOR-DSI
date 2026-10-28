#include <string.h>
#include "psx.h"

/* Ciclos por instrucao. O R3000A do PS1 leva ~2 ciclos por instrucao na media
   (acessos a RAM custam mais que 1). Com 1, a BIOS e os jogos achavam que a CPU
   era 2x mais rapida: o VSync() estourava o tempo ("VSync: timeout") e cada
   quadro exigia o dobro de instrucoes. 2 e o mesmo valor que o PCSX usa. */
#ifndef CPI
#define CPI 2
#endif

Cpu cpu DTCM_BSS;
void (*cpu_boot_hook)(void) = NULL;
uint32_t st_irq_taken;

static uint8_t *g_ram  DTCM_BSS;
static uint8_t *g_bios DTCM_BSS;
static uint32_t rmask[8] DTCM_DATA = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0x7FFFFFFF, 0x1FFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF
};

/* Acesso rapido: RAM e BIOS direto; o resto (scratchpad, I/O) vai pelo bus.c */
static inline uint32_t mrd32(uint32_t a)
{
    uint32_t p = a & rmask[a >> 29];
    if (p < RAM_SIZE) return *(uint32_t *)(g_ram + p);
    if ((p >> 19) == 0x3F8) return *(uint32_t *)(g_bios + (p & 0x7FFFC));
    return bus_read32(a);
}
static inline uint32_t mrd16(uint32_t a)
{
    uint32_t p = a & rmask[a >> 29];
    if (p < RAM_SIZE) return *(uint16_t *)(g_ram + p);
    if ((p >> 19) == 0x3F8) return *(uint16_t *)(g_bios + (p & 0x7FFFE));
    return bus_read16(a);
}
static inline uint32_t mrd8(uint32_t a)
{
    uint32_t p = a & rmask[a >> 29];
    if (p < RAM_SIZE) return g_ram[p];
    if ((p >> 19) == 0x3F8) return g_bios[p & 0x7FFFF];
    return bus_read8(a);
}
static inline void mwr32(uint32_t a, uint32_t v)
{
    uint32_t p = a & rmask[a >> 29];
    if (p < RAM_SIZE) { *(uint32_t *)(g_ram + p) = v; return; }
    bus_write32(a, v);
}
static inline void mwr16(uint32_t a, uint32_t v)
{
    uint32_t p = a & rmask[a >> 29];
    if (p < RAM_SIZE) { *(uint16_t *)(g_ram + p) = (uint16_t)v; return; }
    bus_write16(a, v);
}
static inline void mwr8(uint32_t a, uint32_t v)
{
    uint32_t p = a & rmask[a >> 29];
    if (p < RAM_SIZE) { g_ram[p] = (uint8_t)v; return; }
    bus_write8(a, v);
}

#define SR    (cpu.cop0[12])
#define CAUSE (cpu.cop0[13])

enum { EXC_INT = 0, EXC_ADEL = 4, EXC_ADES = 5, EXC_SYS = 8,
       EXC_BP = 9, EXC_RI = 10, EXC_CPU = 11, EXC_OV = 12 };

void cpu_reset(void)
{
    memset(&cpu, 0, sizeof cpu);
    g_ram = bus.ram;
    g_bios = bus.bios;
    cpu.pc = 0xBFC00000;
    cpu.next_pc = cpu.pc + 4;
    SR = 0x10900000;   /* BEV=1 */
    cpu.cop0[15] = 2;  /* PRID */
    gte_reset();
}

void cpu_set_irq(bool active)
{
    if (active) CAUSE |= 0x400; else CAUSE &= ~0x400u;
}

static int n_warn_exc;

ITCM_CODE static void exception(uint32_t code, uint32_t cur_pc, bool bd, uint32_t cop)
{
    /* excecoes que nao sejam interrupcao/syscall/break costumam ser o jogo
       quebrando: registra as primeiras no log */
    if (__builtin_expect(code != 0 && code != 8 && code != 9, 0) && n_warn_exc < 20) {
        n_warn_exc++;
        dbg_log("CPU: excecao %lu em %08lX (badvaddr %08lX, ra %08lX)\n", (unsigned long)code,
                (unsigned long)cur_pc, (unsigned long)cpu.cop0[8], (unsigned long)cpu.r[31]);
    }
    cpu.cop0[14] = bd ? cur_pc - 4 : cur_pc;
    CAUSE &= ~0xB000007Cu;
    CAUSE |= (code << 2) | (bd ? 0x80000000u : 0) | (cop << 28);
    SR = (SR & ~0x3Fu) | ((SR << 2) & 0x3Fu);   /* empilha IEc/KUc */
    cpu.pc = (SR & (1u << 22)) ? 0xBFC00180 : 0x80000080;
    cpu.next_pc = cpu.pc + 4;
}

/* escrita em registrador GPR; cancela load pendente no mesmo registrador */
#define WR(reg, val) do { uint32_t _r = (reg); if (_r) { r[_r] = (val); if (_r == lr) lr = 0; } } while (0)
#define LOAD(reg, val) do { nr = (reg); nv = (val); } while (0)
#define EXC(code) do { exception((code), cur_pc, bd, 0); goto exc; } while (0)

/* avisos raros no log (instrucao invalida, GTE ainda nao implementado) */
static int n_warn_ri, n_warn_gte;
static void warn_ri(uint32_t pc, uint32_t ins)
{
    if (n_warn_ri < 20) { n_warn_ri++; dbg_log("CPU: instrucao invalida %08lX em %08lX\n", (unsigned long)ins, (unsigned long)pc); }
}
static void warn_gte(uint32_t pc, uint32_t ins)
{
    if (n_warn_gte < 5) { n_warn_gte++; dbg_log("CPU: comando de GTE desconhecido %08lX em %08lX\n", (unsigned long)ins, (unsigned long)pc); }
}
#define BRANCH(cond) do { branch = true; if (cond) cpu.next_pc = cur_pc + 4 + (simm << 2); } while (0)

ITCM_CODE static void step(void)
{
    uint32_t *r = cpu.r;
    uint32_t cur_pc = cpu.pc;
    bool bd = cpu.in_delay;
    uint32_t lr = cpu.ld_reg, lv = cpu.ld_val, nr = 0, nv = 0;
    bool branch = false;

    /* interrupcao pendente */
    if ((SR & 1) && (CAUSE & SR & 0xFF00)) {
        st_irq_taken++;
        if (lr) r[lr] = lv;
        cpu.ld_reg = 0;
        exception(EXC_INT, cur_pc, bd, 0);
        cpu.in_delay = false;
        return;
    }

    /* side-load de PSX-EXE depois que a BIOS inicializa */
    if (cur_pc == 0x80030000 && cpu_boot_hook) {
        void (*h)(void) = cpu_boot_hook;
        cpu_boot_hook = NULL;
        h();
        cur_pc = cpu.pc;
        bd = false;
    }

    /* hook de TTY: putchar da BIOS (A0:3C e B0:3D) */
    if (__builtin_expect((cur_pc & 0x1FFFFFEFu) == 0xA0, 0)) {
        uint32_t pp = cur_pc & 0x1FFFFFFF;
        if ((pp == 0xA0 && r[9] == 0x3C) || (pp == 0xB0 && r[9] == 0x3D))
            tty_putc((char)r[4]);
    }

    if (cur_pc & 3) {
        cpu.cop0[8] = cur_pc;
        exception(EXC_ADEL, cur_pc, bd, 0);
        cpu.in_delay = false;
        return;
    }

    uint32_t i = mrd32(cur_pc);
    cpu.pc = cpu.next_pc;
    cpu.next_pc += 4;

    uint32_t op   = i >> 26;
    uint32_t rs   = (i >> 21) & 31;
    uint32_t rt   = (i >> 16) & 31;
    uint32_t rd   = (i >> 11) & 31;
    uint32_t sh   = (i >> 6) & 31;
    uint32_t simm = (uint32_t)(int32_t)(int16_t)i;
    uint32_t imm  = i & 0xFFFF;

    switch (op) {
    case 0x00:
        switch (i & 0x3F) {
        case 0x00: WR(rd, r[rt] << sh); break;                              /* SLL */
        case 0x02: WR(rd, r[rt] >> sh); break;                              /* SRL */
        case 0x03: WR(rd, (uint32_t)((int32_t)r[rt] >> sh)); break;         /* SRA */
        case 0x04: WR(rd, r[rt] << (r[rs] & 31)); break;                    /* SLLV */
        case 0x06: WR(rd, r[rt] >> (r[rs] & 31)); break;                    /* SRLV */
        case 0x07: WR(rd, (uint32_t)((int32_t)r[rt] >> (r[rs] & 31))); break; /* SRAV */
        case 0x08: branch = true; cpu.next_pc = r[rs]; break;               /* JR */
        case 0x09: { uint32_t t = r[rs]; branch = true;                     /* JALR */
                     WR(rd, cur_pc + 8); cpu.next_pc = t; break; }
        case 0x0C: EXC(EXC_SYS);                                            /* SYSCALL */
        case 0x0D: EXC(EXC_BP);                                             /* BREAK */
        case 0x10: WR(rd, cpu.hi); break;                                   /* MFHI */
        case 0x11: cpu.hi = r[rs]; break;                                   /* MTHI */
        case 0x12: WR(rd, cpu.lo); break;                                   /* MFLO */
        case 0x13: cpu.lo = r[rs]; break;                                   /* MTLO */
        case 0x18: { int64_t p = (int64_t)(int32_t)r[rs] * (int32_t)r[rt];  /* MULT */
                     cpu.hi = (uint32_t)((uint64_t)p >> 32); cpu.lo = (uint32_t)p; break; }
        case 0x19: { uint64_t p = (uint64_t)r[rs] * r[rt];                  /* MULTU */
                     cpu.hi = (uint32_t)(p >> 32); cpu.lo = (uint32_t)p; break; }
        case 0x1A: { int32_t n = (int32_t)r[rs], d = (int32_t)r[rt];        /* DIV */
                     if (d == 0) { cpu.lo = (n < 0) ? 1 : 0xFFFFFFFF; cpu.hi = (uint32_t)n; }
                     else if ((uint32_t)n == 0x80000000u && d == -1) { cpu.lo = 0x80000000u; cpu.hi = 0; }
                     else { cpu.lo = (uint32_t)(n / d); cpu.hi = (uint32_t)(n % d); }
                     break; }
        case 0x1B: { uint32_t n = r[rs], d = r[rt];                         /* DIVU */
                     if (d == 0) { cpu.lo = 0xFFFFFFFF; cpu.hi = n; }
                     else { cpu.lo = n / d; cpu.hi = n % d; }
                     break; }
        case 0x20: { int32_t s; if (__builtin_add_overflow((int32_t)r[rs], (int32_t)r[rt], &s)) EXC(EXC_OV);
                     WR(rd, (uint32_t)s); break; }                          /* ADD */
        case 0x21: WR(rd, r[rs] + r[rt]); break;                            /* ADDU */
        case 0x22: { int32_t s; if (__builtin_sub_overflow((int32_t)r[rs], (int32_t)r[rt], &s)) EXC(EXC_OV);
                     WR(rd, (uint32_t)s); break; }                          /* SUB */
        case 0x23: WR(rd, r[rs] - r[rt]); break;                            /* SUBU */
        case 0x24: WR(rd, r[rs] & r[rt]); break;
        case 0x25: WR(rd, r[rs] | r[rt]); break;
        case 0x26: WR(rd, r[rs] ^ r[rt]); break;
        case 0x27: WR(rd, ~(r[rs] | r[rt])); break;
        case 0x2A: WR(rd, (int32_t)r[rs] < (int32_t)r[rt]); break;          /* SLT */
        case 0x2B: WR(rd, r[rs] < r[rt]); break;                            /* SLTU */
        default: warn_ri(cur_pc, i); EXC(EXC_RI);
        }
        break;

    case 0x01: {  /* REGIMM: BLTZ BGEZ BLTZAL BGEZAL */
        bool ge  = rt & 1;
        bool lnk = (rt & 0x1E) == 0x10;
        bool take = ge ? ((int32_t)r[rs] >= 0) : ((int32_t)r[rs] < 0);
        if (lnk) WR(31, cur_pc + 8);
        BRANCH(take);
        break; }

    case 0x02: branch = true; cpu.next_pc = ((cur_pc + 4) & 0xF0000000u) | ((i & 0x3FFFFFF) << 2); break;
    case 0x03: branch = true; WR(31, cur_pc + 8);
               cpu.next_pc = ((cur_pc + 4) & 0xF0000000u) | ((i & 0x3FFFFFF) << 2); break;
    case 0x04: BRANCH(r[rs] == r[rt]); break;
    case 0x05: BRANCH(r[rs] != r[rt]); break;
    case 0x06: BRANCH((int32_t)r[rs] <= 0); break;
    case 0x07: BRANCH((int32_t)r[rs] > 0); break;

    case 0x08: { int32_t s; if (__builtin_add_overflow((int32_t)r[rs], (int32_t)simm, &s)) EXC(EXC_OV);
                 WR(rt, (uint32_t)s); break; }                              /* ADDI */
    case 0x09: WR(rt, r[rs] + simm); break;                                 /* ADDIU */
    case 0x0A: WR(rt, (int32_t)r[rs] < (int32_t)simm); break;               /* SLTI */
    case 0x0B: WR(rt, r[rs] < simm); break;                                 /* SLTIU */
    case 0x0C: WR(rt, r[rs] & imm); break;
    case 0x0D: WR(rt, r[rs] | imm); break;
    case 0x0E: WR(rt, r[rs] ^ imm); break;
    case 0x0F: WR(rt, imm << 16); break;                                    /* LUI */

    case 0x10:  /* COP0 */
        if (rs == 0x00) {                                                   /* MFC0 */
            LOAD(rt, cpu.cop0[rd]);
        } else if (rs == 0x04) {                                            /* MTC0 */
            uint32_t v = r[rt];
            switch (rd) {
            case 8: case 15: break;
            case 13: CAUSE = (CAUSE & ~0x300u) | (v & 0x300u); break;
            default: cpu.cop0[rd] = v; break;
            }
        } else if ((rs & 0x10) && (i & 0x3F) == 0x10) {                     /* RFE */
            SR = (SR & ~0xFu) | ((SR >> 2) & 0xFu);
        } else EXC(EXC_RI);
        break;

    case 0x11: case 0x13: exception(EXC_CPU, cur_pc, bd, op & 3); goto exc;  /* COP1/COP3 */
    case 0x12:  /* COP2 (GTE) - gte.c */
        if (!(SR & (1u << 30))) { exception(EXC_CPU, cur_pc, bd, 2); goto exc; }
        if (rs & 0x10) {                                                    /* comando */
            if (!gte_command(i)) warn_gte(cur_pc, i);
        } else switch (rs) {
            case 0x00: LOAD(rt, gte_read_data((int)rd)); break;             /* MFC2 */
            case 0x02: LOAD(rt, gte_read_ctrl((int)rd)); break;             /* CFC2 */
            case 0x04: gte_write_data((int)rd, r[rt]); break;               /* MTC2 */
            case 0x06: gte_write_ctrl((int)rd, r[rt]); break;               /* CTC2 */
            default:   warn_ri(cur_pc, i); EXC(EXC_RI);
        }
        break;

    case 0x20: LOAD(rt, (uint32_t)(int32_t)(int8_t)mrd8(r[rs] + simm)); break;      /* LB */
    case 0x21: { uint32_t a = r[rs] + simm; if (a & 1) { cpu.cop0[8] = a; EXC(EXC_ADEL); }
                 LOAD(rt, (uint32_t)(int32_t)(int16_t)mrd16(a)); break; }           /* LH */
    case 0x22: { uint32_t a = r[rs] + simm, w = mrd32(a & ~3u);                     /* LWL */
                 uint32_t cur = (rt == lr) ? lv : r[rt], v;
                 switch (a & 3) { case 0: v = (cur & 0x00FFFFFF) | (w << 24); break;
                                  case 1: v = (cur & 0x0000FFFF) | (w << 16); break;
                                  case 2: v = (cur & 0x000000FF) | (w << 8);  break;
                                  default: v = w; }
                 LOAD(rt, v); break; }
    case 0x23: { uint32_t a = r[rs] + simm; if (a & 3) { cpu.cop0[8] = a; EXC(EXC_ADEL); }
                 LOAD(rt, mrd32(a)); break; }                                       /* LW */
    case 0x24: LOAD(rt, mrd8(r[rs] + simm)); break;                                 /* LBU */
    case 0x25: { uint32_t a = r[rs] + simm; if (a & 1) { cpu.cop0[8] = a; EXC(EXC_ADEL); }
                 LOAD(rt, mrd16(a)); break; }                                       /* LHU */
    case 0x26: { uint32_t a = r[rs] + simm, w = mrd32(a & ~3u);                     /* LWR */
                 uint32_t cur = (rt == lr) ? lv : r[rt], v;
                 switch (a & 3) { case 0: v = w; break;
                                  case 1: v = (cur & 0xFF000000) | (w >> 8);  break;
                                  case 2: v = (cur & 0xFFFF0000) | (w >> 16); break;
                                  default: v = (cur & 0xFFFFFF00) | (w >> 24); }
                 LOAD(rt, v); break; }

    case 0x28: if (!(SR & 0x10000)) { mwr8(r[rs] + simm, r[rt]); } break;              /* SB */
    case 0x29: { uint32_t a = r[rs] + simm; if (a & 1) { cpu.cop0[8] = a; EXC(EXC_ADES); }
                 if (!(SR & 0x10000)) { mwr16(a, r[rt]); } break; }                    /* SH */
    case 0x2A: { uint32_t a = r[rs] + simm, m = mrd32(a & ~3u), v = r[rt], o;       /* SWL */
                 switch (a & 3) { case 0: o = (m & 0xFFFFFF00) | (v >> 24); break;
                                  case 1: o = (m & 0xFFFF0000) | (v >> 16); break;
                                  case 2: o = (m & 0xFF000000) | (v >> 8);  break;
                                  default: o = v; }
                 if (!(SR & 0x10000)) { mwr32(a & ~3u, o); } break; }
    case 0x2B: { uint32_t a = r[rs] + simm; if (a & 3) { cpu.cop0[8] = a; EXC(EXC_ADES); }
                 if (!(SR & 0x10000)) { mwr32(a, r[rt]); } break; }                    /* SW */
    case 0x2E: { uint32_t a = r[rs] + simm, m = mrd32(a & ~3u), v = r[rt], o;       /* SWR */
                 switch (a & 3) { case 0: o = v; break;
                                  case 1: o = (m & 0x000000FF) | (v << 8);  break;
                                  case 2: o = (m & 0x0000FFFF) | (v << 16); break;
                                  default: o = (m & 0x00FFFFFF) | (v << 24); }
                 if (!(SR & 0x10000)) { mwr32(a & ~3u, o); } break; }

    case 0x32: {                                                                    /* LWC2 */
        if (!(SR & (1u << 30))) { exception(EXC_CPU, cur_pc, bd, 2); goto exc; }
        uint32_t a = r[rs] + simm; if (a & 3) { cpu.cop0[8] = a; EXC(EXC_ADEL); }
        gte_write_data((int)rt, mrd32(a)); break; }
    case 0x3A: {                                                                    /* SWC2 */
        if (!(SR & (1u << 30))) { exception(EXC_CPU, cur_pc, bd, 2); goto exc; }
        uint32_t a = r[rs] + simm; if (a & 3) { cpu.cop0[8] = a; EXC(EXC_ADES); }
        if (!(SR & 0x10000)) mwr32(a, gte_read_data((int)rt));
        break; }

    default: warn_ri(cur_pc, i); EXC(EXC_RI);
    }

    /* fim normal da instrucao */
    if (lr) r[lr] = lv;
    cpu.ld_reg = nr;
    cpu.ld_val = nv;
    r[0] = 0;
    cpu.in_delay = branch;
    return;

exc:
    if (lr) r[lr] = lv;
    cpu.ld_reg = 0;
    r[0] = 0;
    cpu.in_delay = false;
}

ITCM_CODE void cpu_run(int n)
{
    while (n > 0) {
        int c = n > 32 ? 32 : n;
        for (int k = 0; k < c; k++) step();
        cpu.cycles += (uint64_t)c * CPI;
        hw_tick((uint32_t)c * CPI);
        n -= c;
    }
}
