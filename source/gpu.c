#include <nds.h>
#include <stdlib.h>
#include <string.h>
#include "psx.h"

#define VW 1024
#define VH 512

#define F_GOUR 1
#define F_TEX  2
#define F_RAW  4
#define F_SEMI 8

enum { M_CMD, M_POLY, M_WRITE, M_READ };

static uint16_t *vram;   /* 1024x512, BGR555 (bit15 = mascara) */
static uint16_t *fb;     /* tela de cima (256x256, 16 bits) */

typedef struct { int x, y, r, g, b, u, v; } Vtx;

static struct {
    /* texpage / janela de textura / area de desenho */
    uint32_t texpage;
    int tpx, tpy, depth, semi, dither, draw_display, tex_disable;
    int twm_x, twm_y, two_x, two_y;
    int da_x1, da_y1, da_x2, da_y2, ox, oy;
    int set_mask, check_mask;
    /* video */
    int disp_x, disp_y, dmode, disp_off, dir, irq1;
    int field;
    unsigned frame;
    /* parser GP0 */
    int mode;
    uint32_t buf[16];
    int have, need;
    /* transferencia de VRAM */
    int tx, ty, tw, th, ix, iy;
    int remaining;
    uint32_t read_latch;
    /* polyline */
    int pl_gour, pl_semi, pl_have;
    int pl_x, pl_y, pl_r, pl_g, pl_b;
    int pl_fr, pl_fg, pl_fb;
    uint32_t pl_c;
} st;

static int clut_x, clut_y;
int gpu_frameskip = 1;
static bool skip_draw;   /* quadro atual descartado pelo frameskip */
static unsigned flips;   /* trocas de buffer (GP1(05) com endereco novo) */
static int vb_since_flip;

/* Frameskip: a decisao e tomada quando o jogo troca o buffer exibido, que e
   quando um quadro novo comeca a ser desenhado. Jogos que desenham direto no
   buffer visivel (ex.: menu da BIOS em 640x480) nunca trocam, entao nunca sao
   pulados - pular ali deixaria a imagem pela metade. */
static void frameskip_on_flip(void)
{
    flips++;
    vb_since_flip = 0;
    skip_draw = gpu_frameskip > 0 && (flips % (unsigned)(gpu_frameskip + 1)) != 0;
}
static uint32_t n_gp0, n_gp1;

void gpu_stats(uint32_t *gp0n, uint32_t *gp1n, int *disp_off)
{
    *gp0n = n_gp0; *gp1n = n_gp1; *disp_off = st.disp_off;
}

/* ------------------------------------------------------------------ */
/* Utilitarios de pixel                                                */
/* ------------------------------------------------------------------ */

static inline int sext11(uint32_t v) { return (int)((v & 0x7FF) << 21) >> 21; }
static inline int clamp255(int v) { return v < 0 ? 0 : (v > 255 ? 255 : v); }

static inline uint16_t blend(uint16_t b, uint16_t f, int mode)
{
    int br = b & 31, bg = (b >> 5) & 31, bb = (b >> 10) & 31;
    int fr = f & 31, fg = (f >> 5) & 31, fb2 = (f >> 10) & 31;
    int r, g, bl;
    switch (mode) {
    case 0:  r = (br + fr) >> 1; g = (bg + fg) >> 1; bl = (bb + fb2) >> 1; break;
    case 1:  r = br + fr; g = bg + fg; bl = bb + fb2; break;
    case 2:  r = br - fr; g = bg - fg; bl = bb - fb2; break;
    default: r = br + (fr >> 2); g = bg + (fg >> 2); bl = bb + (fb2 >> 2); break;
    }
    if (r < 0) r = 0;
    if (r > 31) r = 31;
    if (g < 0) g = 0;
    if (g > 31) g = 31;
    if (bl < 0) bl = 0;
    if (bl > 31) bl = 31;
    return (uint16_t)(r | (g << 5) | (bl << 10));
}

/* c: cor 15 bits; bit15 = bit "stp" do texel (so em texturas) */
static inline void put_pixel(int x, int y, uint16_t c, bool semi)
{
    if (x < st.da_x1 || x > st.da_x2 || y < st.da_y1 || y > st.da_y2) return;
    uint16_t *p = &vram[(y & (VH - 1)) * VW + (x & (VW - 1))];
    if (st.check_mask && (*p & 0x8000)) return;
    uint16_t out = c & 0x7FFF;
    if (semi) out = blend(*p, out, st.semi);
    *p = out | (c & 0x8000) | (st.set_mask ? 0x8000 : 0);
}

static inline uint16_t modulate(uint16_t t, int cr, int cg, int cb)
{
    int r = ((t & 31) * cr) >> 4;
    int g = (((t >> 5) & 31) * cg) >> 4;
    int b = (((t >> 10) & 31) * cb) >> 4;
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (uint16_t)((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10) | (t & 0x8000));
}

static inline uint16_t tex_fetch(int u, int v)
{
    u = (u & ~(st.twm_x * 8)) | ((st.two_x & st.twm_x) * 8);
    v = (v & ~(st.twm_y * 8)) | ((st.two_y & st.twm_y) * 8);
    u &= 0xFF; v &= 0xFF;
    int row = ((st.tpy + v) & (VH - 1)) * VW;
    switch (st.depth) {
    case 0: {
        uint16_t w = vram[row + ((st.tpx + (u >> 2)) & (VW - 1))];
        int idx = (w >> ((u & 3) * 4)) & 0xF;
        return vram[(clut_y & (VH - 1)) * VW + ((clut_x + idx) & (VW - 1))];
    }
    case 1: {
        uint16_t w = vram[row + ((st.tpx + (u >> 1)) & (VW - 1))];
        int idx = (w >> ((u & 1) * 8)) & 0xFF;
        return vram[(clut_y & (VH - 1)) * VW + ((clut_x + idx) & (VW - 1))];
    }
    default:
        return vram[row + ((st.tpx + u) & (VW - 1))];
    }
}

static void set_texpage(uint32_t v)
{
    st.texpage = v;
    st.tpx = (int)(v & 0xF) * 64;
    st.tpy = (int)((v >> 4) & 1) * 256;
    st.semi = (v >> 5) & 3;
    st.depth = (v >> 7) & 3; if (st.depth == 3) st.depth = 2;
    st.dither = (v >> 9) & 1;
    st.draw_display = (v >> 10) & 1;
    st.tex_disable = (v >> 11) & 1;
}

/* ------------------------------------------------------------------ */
/* Triangulos                                                          */
/* ------------------------------------------------------------------ */

static inline void grad(int v0, int v1, int v2, const Vtx *a, const Vtx *b, const Vtx *c,
                        int area, int32_t *gx, int32_t *gy)
{
    int64_t nx = (int64_t)(v1 - v0) * (c->y - a->y) - (int64_t)(v2 - v0) * (b->y - a->y);
    int64_t ny = (int64_t)(v2 - v0) * (b->x - a->x) - (int64_t)(v1 - v0) * (c->x - a->x);
    *gx = (int32_t)((nx * 65536) / area);
    *gy = (int32_t)((ny * 65536) / area);
}

/* Um trecho horizontal [x0, x1] de um triangulo. "f" e sempre constante na
   chamada, entao o compilador gera uma versao enxuta para cada combinacao. */
static inline __attribute__((always_inline))
void tri_span(uint16_t *row, int x0, int x1,
              uint32_t cr, uint32_t cg, uint32_t cb, uint32_t cu, uint32_t cv,
              int32_t gr, int32_t gg, int32_t gb, int32_t gu, int32_t gv, const int f)
{
    const uint16_t smask = st.set_mask ? 0x8000 : 0;
    const int chk = st.check_mask;

    if (!(f & (F_TEX | F_GOUR))) {                         /* cor solida */
        int R = clamp255((int32_t)cr >> 16), G = clamp255((int32_t)cg >> 16), B = clamp255((int32_t)cb >> 16);
        uint16_t c = (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10));
        if (!(f & F_SEMI) && !chk) {
            uint16_t o = c | smask;
            for (int x = x0; x <= x1; x++) row[x] = o;
            return;
        }
        for (int x = x0; x <= x1; x++) {
            uint16_t *p = &row[x];
            if (chk && (*p & 0x8000)) continue;
            uint16_t o = c;
            if (f & F_SEMI) o = blend(*p, o, st.semi);
            *p = o | smask;
        }
        return;
    }

    for (int x = x0; x <= x1; x++) {
        uint16_t *p = &row[x];
        int R = 0, G = 0, B = 0;
        if (f & F_GOUR) {
            R = clamp255((int32_t)cr >> 16); G = clamp255((int32_t)cg >> 16); B = clamp255((int32_t)cb >> 16);
            cr += (uint32_t)gr; cg += (uint32_t)gg; cb += (uint32_t)gb;
        } else {
            R = clamp255((int32_t)cr >> 16); G = clamp255((int32_t)cg >> 16); B = clamp255((int32_t)cb >> 16);
        }
        if (f & F_TEX) {
            uint16_t t = tex_fetch((int)(cu >> 16), (int)(cv >> 16));
            cu += (uint32_t)gu; cv += (uint32_t)gv;
            if (t == 0) continue;
            if (chk && (*p & 0x8000)) continue;
            uint16_t o = (f & F_RAW) ? t : modulate(t, R, G, B);
            uint16_t keep = o & 0x8000;
            o &= 0x7FFF;
            if ((f & F_SEMI) && keep) o = blend(*p, o, st.semi);
            *p = o | keep | smask;
        } else {
            if (chk && (*p & 0x8000)) continue;
            uint16_t o = (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10));
            if (f & F_SEMI) o = blend(*p, o, st.semi);
            *p = o | smask;
        }
    }
}

/* limites [lo, hi] (relativos a minx) onde a aresta c + A*k >= 0 */
static inline bool edge_range(int c, int A, int *lo, int *hi)
{
    if (A > 0) {
        if (c < 0) { int k = (-c + A - 1) / A; if (k > *lo) *lo = k; }
    } else if (A < 0) {
        if (c < 0) return false;
        int k = c / (-A);
        if (k < *hi) *hi = k;
    } else if (c < 0) return false;
    return true;
}

static void draw_tri(const Vtx *p0, const Vtx *p1, const Vtx *p2, int f)
{
    Vtx a = *p0, b = *p1, c = *p2;

    int area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
    if (area == 0) return;
    if (area < 0) { Vtx t = b; b = c; c = t; area = -area; }

    /* limites do hardware */
    int mnx = a.x < b.x ? a.x : b.x; if (c.x < mnx) mnx = c.x;
    int mxx = a.x > b.x ? a.x : b.x; if (c.x > mxx) mxx = c.x;
    int mny = a.y < b.y ? a.y : b.y; if (c.y < mny) mny = c.y;
    int mxy = a.y > b.y ? a.y : b.y; if (c.y > mxy) mxy = c.y;
    if (mxx - mnx > 1023 || mxy - mny > 511) return;

    int minx = mnx < st.da_x1 ? st.da_x1 : mnx;
    int maxx = mxx > st.da_x2 ? st.da_x2 : mxx;
    int miny = mny < st.da_y1 ? st.da_y1 : mny;
    int maxy = mxy > st.da_y2 ? st.da_y2 : mxy;
    if (minx > maxx || miny > maxy) return;

    /* arestas: E(p) = B*(py - ay) + A*(px - ax) */
    int A0 = -(b.y - a.y), B0 = b.x - a.x;
    int A1 = -(c.y - b.y), B1 = c.x - b.x;
    int A2 = -(a.y - c.y), B2 = a.x - c.x;
    int bias0 = ((b.y - a.y) < 0 || ((b.y - a.y) == 0 && (b.x - a.x) > 0)) ? 0 : -1;
    int bias1 = ((c.y - b.y) < 0 || ((c.y - b.y) == 0 && (c.x - b.x) > 0)) ? 0 : -1;
    int bias2 = ((a.y - c.y) < 0 || ((a.y - c.y) == 0 && (a.x - c.x) > 0)) ? 0 : -1;

    int rw0 = B0 * (miny - a.y) + A0 * (minx - a.x) + bias0;
    int rw1 = B1 * (miny - b.y) + A1 * (minx - b.x) + bias1;
    int rw2 = B2 * (miny - c.y) + A2 * (minx - c.x) + bias2;

    int32_t gr = 0, gg = 0, gb = 0, gu = 0, gv = 0;
    int32_t hr = 0, hg = 0, hb = 0, hu = 0, hv = 0;   /* gradiente em y */
    if (f & F_GOUR) {
        grad(a.r, b.r, c.r, &a, &b, &c, area, &gr, &hr);
        grad(a.g, b.g, c.g, &a, &b, &c, area, &gg, &hg);
        grad(a.b, b.b, c.b, &a, &b, &c, area, &gb, &hb);
    }
    if (f & F_TEX) {
        grad(a.u, b.u, c.u, &a, &b, &c, area, &gu, &hu);
        grad(a.v, b.v, c.v, &a, &b, &c, area, &gv, &hv);
    }

    int dx0 = minx - a.x, dy0 = miny - a.y;
    uint32_t rr = (uint32_t)(((int64_t)a.r << 16) + (int64_t)gr * dx0 + (int64_t)hr * dy0);
    uint32_t rg = (uint32_t)(((int64_t)a.g << 16) + (int64_t)gg * dx0 + (int64_t)hg * dy0);
    uint32_t rb = (uint32_t)(((int64_t)a.b << 16) + (int64_t)gb * dx0 + (int64_t)hb * dy0);
    uint32_t ru = (uint32_t)(((int64_t)a.u << 16) + (int64_t)gu * dx0 + (int64_t)hu * dy0);
    uint32_t rv = (uint32_t)(((int64_t)a.v << 16) + (int64_t)gv * dx0 + (int64_t)hv * dy0);

    const int width = maxx - minx;
    for (int y = miny; y <= maxy; y++) {
        int lo = 0, hi = width;
        if (edge_range(rw0, A0, &lo, &hi) && edge_range(rw1, A1, &lo, &hi) &&
            edge_range(rw2, A2, &lo, &hi) && lo <= hi) {
            uint16_t *row = vram + (y & (VH - 1)) * VW;
            uint32_t k = (uint32_t)lo;
            uint32_t cr = rr + (uint32_t)gr * k, cg = rg + (uint32_t)gg * k, cb = rb + (uint32_t)gb * k;
            uint32_t cu = ru + (uint32_t)gu * k, cv = rv + (uint32_t)gv * k;
            int x0 = minx + lo, x1 = minx + hi;
            switch (f) {
            case 0:                          tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, 0); break;
            case F_SEMI:                     tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_SEMI); break;
            case F_GOUR:                     tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_GOUR); break;
            case F_GOUR | F_SEMI:            tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_GOUR | F_SEMI); break;
            case F_TEX:                      tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX); break;
            case F_TEX | F_SEMI:             tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_SEMI); break;
            case F_TEX | F_RAW:              tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_RAW); break;
            case F_TEX | F_RAW | F_SEMI:     tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_RAW | F_SEMI); break;
            case F_TEX | F_GOUR:             tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_GOUR); break;
            case F_TEX | F_GOUR | F_SEMI:    tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_GOUR | F_SEMI); break;
            case F_TEX | F_RAW | F_GOUR:     tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_RAW); break;
            default:                         tri_span(row, x0, x1, cr, cg, cb, cu, cv, gr, gg, gb, gu, gv, F_TEX | F_RAW | F_SEMI); break;
            }
        }
        rw0 += B0; rw1 += B1; rw2 += B2;
        rr += (uint32_t)hr; rg += (uint32_t)hg; rb += (uint32_t)hb;
        ru += (uint32_t)hu; rv += (uint32_t)hv;
    }
}

/* ------------------------------------------------------------------ */
/* Linhas e retangulos                                                 */
/* ------------------------------------------------------------------ */

static void draw_line(int x0, int y0, int r0, int g0, int b0,
                      int x1, int y1, int r1, int g1, int b1, bool semi)
{
    int dx = x1 - x0, dy = y1 - y0;
    if (abs(dx) > 1023 || abs(dy) > 511) return;
    int n = abs(dx) > abs(dy) ? abs(dx) : abs(dy);

    if (n == 0) {
        put_pixel(x0, y0, (uint16_t)((r0 >> 3) | ((g0 >> 3) << 5) | ((b0 >> 3) << 10)), semi);
        return;
    }
    int32_t sx = (dx * 65536) / n, sy = (dy * 65536) / n;
    int32_t sr = ((r1 - r0) * 65536) / n, sg = ((g1 - g0) * 65536) / n, sb = ((b1 - b0) * 65536) / n;
    int32_t x = x0 * 65536 + 0x8000, y = y0 * 65536 + 0x8000;
    int32_t r = r0 * 65536, g = g0 * 65536, b = b0 * 65536;

    for (int i = 0; i <= n; i++) {
        int R = clamp255(r >> 16), G = clamp255(g >> 16), B = clamp255(b >> 16);
        put_pixel(x >> 16, y >> 16, (uint16_t)((R >> 3) | ((G >> 3) << 5) | ((B >> 3) << 10)), semi);
        x += sx; y += sy; r += sr; g += sg; b += sb;
    }
}

static void draw_rect(int x, int y, int w, int h, int cr, int cg, int cb,
                      int f, int u0, int v0)
{
    uint16_t flat = (uint16_t)((cr >> 3) | ((cg >> 3) << 5) | ((cb >> 3) << 10));
    for (int j = 0; j < h; j++) {
        int yy = y + j;
        if (yy < st.da_y1 || yy > st.da_y2) continue;
        for (int i = 0; i < w; i++) {
            int xx = x + i;
            if (xx < st.da_x1 || xx > st.da_x2) continue;
            if (f & F_TEX) {
                uint16_t t = tex_fetch(u0 + i, v0 + j);
                if (t == 0) continue;
                uint16_t px = (f & F_RAW) ? t : modulate(t, cr, cg, cb);
                put_pixel(xx, yy, px, (f & F_SEMI) && (t & 0x8000));
            } else {
                put_pixel(xx, yy, flat, f & F_SEMI);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* Transferencias de VRAM                                              */
/* ------------------------------------------------------------------ */

static void vram_begin(uint32_t xy, uint32_t wh)
{
    st.tx = xy & 0x3FF;
    st.ty = (xy >> 16) & 0x1FF;
    st.tw = wh & 0x3FF;        if (st.tw == 0) st.tw = 1024;
    st.th = (wh >> 16) & 0x1FF; if (st.th == 0) st.th = 512;
    st.ix = st.iy = 0;
    st.remaining = st.tw * st.th;
}

static inline uint16_t *vram_cursor(void)
{
    return &vram[((st.ty + st.iy) & (VH - 1)) * VW + ((st.tx + st.ix) & (VW - 1))];
}

static inline void vram_advance(void)
{
    if (++st.ix >= st.tw) { st.ix = 0; st.iy++; }
    st.remaining--;
}

static void vram_write_word(uint32_t w)
{
    for (int k = 0; k < 2 && st.remaining > 0; k++) {
        uint16_t px = (uint16_t)(w >> (16 * k));
        uint16_t *p = vram_cursor();
        if (!(st.check_mask && (*p & 0x8000))) *p = px | (st.set_mask ? 0x8000 : 0);
        vram_advance();
    }
    if (st.remaining <= 0) st.mode = M_CMD;
}

static uint32_t vram_read_word(void)
{
    uint32_t v = 0;
    for (int k = 0; k < 2 && st.remaining > 0; k++) {
        v |= (uint32_t)(*vram_cursor()) << (16 * k);
        vram_advance();
    }
    if (st.remaining <= 0) st.mode = M_CMD;
    return v;
}

/* ------------------------------------------------------------------ */
/* Interpretador de comandos GP0                                       */
/* ------------------------------------------------------------------ */

static int cmd_len(uint32_t cmd)
{
    if (cmd >= 0x20 && cmd <= 0x3F) {
        int nv = (cmd & 8) ? 4 : 3;
        return 1 + nv + ((cmd & 4) ? nv : 0) + ((cmd & 0x10) ? nv - 1 : 0);
    }
    if (cmd >= 0x40 && cmd <= 0x5F) return 3 + ((cmd & 0x10) ? 1 : 0);
    if (cmd >= 0x60 && cmd <= 0x7F) return 2 + ((cmd & 4) ? 1 : 0) + ((((cmd >> 3) & 3) == 0) ? 1 : 0);
    if (cmd >= 0x80 && cmd <= 0x9F) return 4;
    if (cmd >= 0xA0 && cmd <= 0xDF) return 3;
    if (cmd == 0x02) return 3;
    return 1;
}

static void exec_polygon(void)
{
    uint32_t cmd = st.buf[0] >> 24;
    bool quad = cmd & 8, tex = cmd & 4, gour = cmd & 0x10;
    int nv = quad ? 4 : 3, p = 1;
    Vtx v[4];
    uint32_t c0 = st.buf[0] & 0xFFFFFF, c = c0;

    for (int i = 0; i < nv; i++) {
        if (i > 0 && gour) c = st.buf[p++] & 0xFFFFFF;
        uint32_t xy = st.buf[p++];
        v[i].x = sext11(xy) + st.ox;
        v[i].y = sext11(xy >> 16) + st.oy;
        v[i].r = c & 0xFF; v[i].g = (c >> 8) & 0xFF; v[i].b = (c >> 16) & 0xFF;
        v[i].u = v[i].v = 0;
        if (tex) {
            uint32_t t = st.buf[p++];
            v[i].u = t & 0xFF;
            v[i].v = (t >> 8) & 0xFF;
            if (i == 0) { clut_x = ((t >> 16) & 0x3F) * 16; clut_y = (t >> 22) & 0x1FF; }
            if (i == 1) set_texpage((st.texpage & ~0x1FFu) | ((t >> 16) & 0x1FF));
        }
    }
    if (skip_draw) return;   /* texpage/CLUT ja foram atualizados acima */
    int f = (gour ? F_GOUR : 0) | (tex ? F_TEX : 0) | ((tex && (cmd & 1)) ? F_RAW : 0) | ((cmd & 2) ? F_SEMI : 0);
    draw_tri(&v[0], &v[1], &v[2], f);
    if (quad) draw_tri(&v[1], &v[2], &v[3], f);
}

static void exec_line(void)
{
    uint32_t cmd = st.buf[0] >> 24;
    bool gour = cmd & 0x10, poly = cmd & 8, semi = cmd & 2;
    uint32_t c0 = st.buf[0] & 0xFFFFFF, c1 = c0;
    uint32_t xy0 = st.buf[1], xy1;
    int p = 2;
    if (gour) c1 = st.buf[p++] & 0xFFFFFF;
    xy1 = st.buf[p];

    int x0 = sext11(xy0) + st.ox, y0 = sext11(xy0 >> 16) + st.oy;
    int x1 = sext11(xy1) + st.ox, y1 = sext11(xy1 >> 16) + st.oy;
    if (!skip_draw)
    draw_line(x0, y0, c0 & 0xFF, (c0 >> 8) & 0xFF, (c0 >> 16) & 0xFF,
              x1, y1, c1 & 0xFF, (c1 >> 8) & 0xFF, (c1 >> 16) & 0xFF, semi);

    if (poly) {
        st.mode = M_POLY;
        st.pl_gour = gour; st.pl_semi = semi; st.pl_have = 0;
        st.pl_x = x1; st.pl_y = y1;
        st.pl_r = c1 & 0xFF; st.pl_g = (c1 >> 8) & 0xFF; st.pl_b = (c1 >> 16) & 0xFF;
        st.pl_fr = c0 & 0xFF; st.pl_fg = (c0 >> 8) & 0xFF; st.pl_fb = (c0 >> 16) & 0xFF;
    }
}

static void polyline_word(uint32_t w)
{
    if ((w & 0xF000F000u) == 0x50005000u) { st.mode = M_CMD; return; }
    int r = st.pl_fr, g = st.pl_fg, b = st.pl_fb;
    if (st.pl_gour) {
        if (st.pl_have == 0) { st.pl_c = w & 0xFFFFFF; st.pl_have = 1; return; }
        st.pl_have = 0;
        r = st.pl_c & 0xFF; g = (st.pl_c >> 8) & 0xFF; b = (st.pl_c >> 16) & 0xFF;
    }
    int x = sext11(w) + st.ox, y = sext11(w >> 16) + st.oy;
    if (!skip_draw)
        draw_line(st.pl_x, st.pl_y, st.pl_r, st.pl_g, st.pl_b, x, y, r, g, b, st.pl_semi);
    st.pl_x = x; st.pl_y = y; st.pl_r = r; st.pl_g = g; st.pl_b = b;
}

static void exec_rect(void)
{
    uint32_t cmd = st.buf[0] >> 24;
    bool tex = cmd & 4;
    int sz = (cmd >> 3) & 3, p = 1;
    uint32_t c = st.buf[0] & 0xFFFFFF;
    uint32_t xy = st.buf[p++];
    int u0 = 0, v0 = 0, w, h;

    if (tex) {
        uint32_t t = st.buf[p++];
        u0 = t & 0xFF; v0 = (t >> 8) & 0xFF;
        clut_x = ((t >> 16) & 0x3F) * 16; clut_y = (t >> 22) & 0x1FF;
    }
    switch (sz) {
    case 0: { uint32_t s = st.buf[p++]; w = s & 0x3FF; h = (s >> 16) & 0x1FF; break; }
    case 1: w = h = 1; break;
    case 2: w = h = 8; break;
    default: w = h = 16; break;
    }
    if (skip_draw) return;
    int f = (tex ? F_TEX : 0) | ((tex && (cmd & 1)) ? F_RAW : 0) | ((cmd & 2) ? F_SEMI : 0);
    draw_rect(sext11(xy) + st.ox, sext11(xy >> 16) + st.oy, w, h,
              c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, f, u0, v0);
}

static void exec_cmd(void)
{
    uint32_t w0 = st.buf[0];
    uint32_t cmd = w0 >> 24;

    if (cmd >= 0x20 && cmd <= 0x3F) { exec_polygon(); return; }
    if (cmd >= 0x40 && cmd <= 0x5F) { exec_line();    return; }
    if (cmd >= 0x60 && cmd <= 0x7F) { exec_rect();    return; }

    switch (cmd) {
    case 0x02: {   /* preenche retangulo (ignora mascara e area de desenho) */
        uint32_t c = w0 & 0xFFFFFF;
        uint16_t px = (uint16_t)(((c & 0xFF) >> 3) | ((((c >> 8) & 0xFF) >> 3) << 5) | ((((c >> 16) & 0xFF) >> 3) << 10));
        int x = st.buf[1] & 0x3F0, y = (st.buf[1] >> 16) & 0x1FF;
        int w = (int)(((st.buf[2] & 0x3FF) + 0xF) & ~0xFu), h = (st.buf[2] >> 16) & 0x1FF;
        /* no quadro pulado, a limpeza da tela que esta sendo desenhada tambem e
           pulada (senao o buffer apareceria so com o fundo); preenchimentos
           fora da area de desenho (texturas) continuam */
        if (skip_draw && x <= st.da_x2 && x + w - 1 >= st.da_x1 && y <= st.da_y2 && y + h - 1 >= st.da_y1)
            break;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++)
                vram[((y + j) & (VH - 1)) * VW + ((x + i) & (VW - 1))] = px;
        break; }
    case 0x1F:
        st.irq1 = 1; irq_raise(1);
        break;
    case 0x80: case 0x81: case 0x82: case 0x83: case 0x84: case 0x85: case 0x86: case 0x87:
    case 0x88: case 0x89: case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
    case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95: case 0x96: case 0x97:
    case 0x98: case 0x99: case 0x9A: case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F: {  /* VRAM -> VRAM */
        int sx = st.buf[1] & 0x3FF, sy = (st.buf[1] >> 16) & 0x1FF;
        int dx = st.buf[2] & 0x3FF, dy = (st.buf[2] >> 16) & 0x1FF;
        int w = st.buf[3] & 0x3FF, h = (st.buf[3] >> 16) & 0x1FF;
        if (w == 0) w = 1024;
        if (h == 0) h = 512;
        for (int j = 0; j < h; j++)
            for (int i = 0; i < w; i++) {
                uint16_t px = vram[((sy + j) & (VH - 1)) * VW + ((sx + i) & (VW - 1))];
                uint16_t *d = &vram[((dy + j) & (VH - 1)) * VW + ((dx + i) & (VW - 1))];
                if (!(st.check_mask && (*d & 0x8000))) *d = px | (st.set_mask ? 0x8000 : 0);
            }
        break; }
    case 0xA0: case 0xA1: case 0xA2: case 0xA3: case 0xA4: case 0xA5: case 0xA6: case 0xA7:
    case 0xA8: case 0xA9: case 0xAA: case 0xAB: case 0xAC: case 0xAD: case 0xAE: case 0xAF:
    case 0xB0: case 0xB1: case 0xB2: case 0xB3: case 0xB4: case 0xB5: case 0xB6: case 0xB7:
    case 0xB8: case 0xB9: case 0xBA: case 0xBB: case 0xBC: case 0xBD: case 0xBE: case 0xBF:   /* CPU -> VRAM */
        vram_begin(st.buf[1], st.buf[2]);
        st.mode = M_WRITE;
        break;
    case 0xC0: case 0xC1: case 0xC2: case 0xC3: case 0xC4: case 0xC5: case 0xC6: case 0xC7:
    case 0xC8: case 0xC9: case 0xCA: case 0xCB: case 0xCC: case 0xCD: case 0xCE: case 0xCF:
    case 0xD0: case 0xD1: case 0xD2: case 0xD3: case 0xD4: case 0xD5: case 0xD6: case 0xD7:
    case 0xD8: case 0xD9: case 0xDA: case 0xDB: case 0xDC: case 0xDD: case 0xDE: case 0xDF:   /* VRAM -> CPU */
        vram_begin(st.buf[1], st.buf[2]);
        st.mode = M_READ;
        break;
    case 0xE1: set_texpage(w0 & 0xFFFFFF); break;
    case 0xE2:
        st.twm_x = w0 & 0x1F;         st.twm_y = (w0 >> 5) & 0x1F;
        st.two_x = (w0 >> 10) & 0x1F; st.two_y = (w0 >> 15) & 0x1F;
        break;
    case 0xE3: st.da_x1 = w0 & 0x3FF; st.da_y1 = (w0 >> 10) & 0x3FF; break;
    case 0xE4: st.da_x2 = w0 & 0x3FF; st.da_y2 = (w0 >> 10) & 0x3FF; break;
    case 0xE5: st.ox = sext11(w0); st.oy = sext11(w0 >> 11); break;
    case 0xE6: st.set_mask = w0 & 1; st.check_mask = (w0 >> 1) & 1; break;
    default: break;   /* NOP, limpar cache, etc. */
    }
}

void gpu_gp0(uint32_t w)
{
    n_gp0++;
    if (st.mode == M_WRITE) { vram_write_word(w); return; }
    if (st.mode == M_POLY)  { polyline_word(w);   return; }

    if (st.have == 0) st.need = cmd_len(w >> 24);
    st.buf[st.have++] = w;
    if (st.have >= st.need) {
        st.have = 0;
        exec_cmd();
    }
}

/* ------------------------------------------------------------------ */
/* GP1, status e leitura                                               */
/* ------------------------------------------------------------------ */

static void gpu_reset_state(void)
{
    set_texpage(0);
    st.twm_x = st.twm_y = st.two_x = st.two_y = 0;
    st.da_x1 = st.da_y1 = 0; st.da_x2 = 1023; st.da_y2 = 511;
    st.ox = st.oy = 0;
    st.set_mask = st.check_mask = 0;
    st.disp_x = st.disp_y = 0;
    st.dmode = 0; st.disp_off = 1; st.dir = 0; st.irq1 = 0;
    st.mode = M_CMD; st.have = 0;
}

void gpu_gp1(uint32_t w)
{
    n_gp1++;
    uint32_t cmd = (w >> 24) & 0x3F;
    switch (cmd) {
    case 0x00: gpu_reset_state(); break;
    case 0x01: st.mode = M_CMD; st.have = 0; break;
    case 0x02: st.irq1 = 0; break;
    case 0x03: st.disp_off = w & 1; break;
    case 0x04: st.dir = w & 3; break;
    case 0x05: {
        int nx = w & 0x3FF, ny = (w >> 10) & 0x1FF;
        if (nx != st.disp_x || ny != st.disp_y) frameskip_on_flip();
        st.disp_x = nx; st.disp_y = ny;
        break; }
    case 0x08: st.dmode = w & 0xFF; break;
    case 0x06: case 0x07: case 0x09: break;   /* faixas de video: ignoradas por enquanto */
    default:
        if (cmd >= 0x10 && cmd <= 0x1F) {
            switch (w & 0xF) {
            case 2: st.read_latch = (uint32_t)(st.twm_x | (st.twm_y << 5) | (st.two_x << 10) | (st.two_y << 15)); break;
            case 3: st.read_latch = (uint32_t)(st.da_x1 | (st.da_y1 << 10)); break;
            case 4: st.read_latch = (uint32_t)(st.da_x2 | (st.da_y2 << 10)); break;
            case 5: st.read_latch = (uint32_t)((st.ox & 0x7FF) | ((st.oy & 0x7FF) << 11)); break;
            case 7: st.read_latch = 2; break;   /* tipo de GPU */
            default: st.read_latch = 0; break;
            }
        }
        break;
    }
}

uint32_t gpu_read(void)
{
    if (st.mode == M_READ) return vram_read_word();
    return st.read_latch;
}

uint32_t gpu_stat(void)
{
    uint32_t s = 0;
    s |= (uint32_t)((st.tpx / 64) & 0xF);
    s |= (uint32_t)(st.tpy / 256) << 4;
    s |= (uint32_t)st.semi << 5;
    s |= (uint32_t)st.depth << 7;
    s |= (uint32_t)st.dither << 9;
    s |= (uint32_t)st.draw_display << 10;
    s |= (uint32_t)st.set_mask << 11;
    s |= (uint32_t)st.check_mask << 12;
    s |= 1u << 13;
    s |= (uint32_t)st.tex_disable << 15;
    s |= (uint32_t)((st.dmode >> 6) & 1) << 16;
    s |= (uint32_t)(st.dmode & 3) << 17;
    s |= (uint32_t)((st.dmode >> 2) & 1) << 19;
    s |= (uint32_t)((st.dmode >> 3) & 1) << 20;
    s |= (uint32_t)((st.dmode >> 4) & 1) << 21;
    s |= (uint32_t)((st.dmode >> 5) & 1) << 22;
    s |= (uint32_t)st.disp_off << 23;
    s |= (uint32_t)st.irq1 << 24;

    bool rdy_cmd  = (st.mode == M_CMD);
    bool rdy_read = (st.mode == M_READ);
    bool rdy_dma  = (st.mode != M_READ);
    s |= (uint32_t)rdy_cmd << 26;
    s |= (uint32_t)rdy_read << 27;
    s |= (uint32_t)rdy_dma << 28;
    s |= (uint32_t)st.dir << 29;
    uint32_t req = 0;
    switch (st.dir) {
    case 1: req = 1; break;
    case 2: req = rdy_dma; break;
    case 3: req = rdy_read; break;
    default: req = 0; break;
    }
    s |= req << 25;
    s |= (uint32_t)st.field << 31;
    return s;
}

/* ------------------------------------------------------------------ */
/* Saida de video: VRAM -> tela de cima (256x192, escala vizinho)      */
/* ------------------------------------------------------------------ */

static void gpu_present(void)
{
    if (!fb) return;
    if (st.disp_off) {
        for (int i = 0; i < 256 * 192; i++) fb[i] = 0x8000;
        return;
    }
    static const int wtab[4] = { 256, 320, 512, 640 };
    int w = (st.dmode & 0x40) ? 368 : wtab[st.dmode & 3];
    int h = (st.dmode & 4) ? 480 : 240;

    int xmap[256];
    for (int x = 0; x < 256; x++) xmap[x] = (x * w) >> 8;

    for (int y = 0; y < 192; y++) {
        int sy = (st.disp_y + (y * h) / 192) & (VH - 1);
        const uint16_t *src = vram + sy * VW;
        uint16_t *dst = fb + y * 256;
        for (int x = 0; x < 256; x++)
            dst[x] = src[(st.disp_x + xmap[x]) & (VW - 1)] | 0x8000;
    }
}

void gpu_vblank(void)
{
    st.field ^= 1;
    st.frame++;
    /* Nos quadros pulados, poligonos, linhas e sprites nao sao desenhados
       (transferencias e copias de VRAM continuam, senao texturas se perderiam).
       Se o jogo parar de trocar de buffer, volta a desenhar tudo. */
    if (++vb_since_flip > 3) skip_draw = false;
    gpu_present();
}

bool gpu_init(void)
{
    vram = (uint16_t *)malloc(VW * VH * 2);
    if (!vram) return false;
    memset(vram, 0, VW * VH * 2);

    videoSetMode(MODE_5_2D);
    vramSetBankA(VRAM_A_MAIN_BG);
    int bg = bgInit(3, BgType_Bmp16, BgSize_B16_256x256, 0, 0);
    fb = (uint16_t *)bgGetGfxPtr(bg);
    for (int i = 0; i < 256 * 256; i++) fb[i] = 0x8000;

    memset(&st, 0, sizeof st);
    gpu_reset_state();
    return true;
}
