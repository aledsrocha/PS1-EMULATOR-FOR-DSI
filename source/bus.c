#include <stdlib.h>
#include <string.h>
#include "psx.h"

Bus bus;

static const uint32_t region_mask[8] = {
    0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF, 0xFFFFFFFF,
    0x7FFFFFFF,
    0x1FFFFFFF,
    0xFFFFFFFF, 0xFFFFFFFF
};

/* Reserva a RAM do PS1 (2 MB). Se embedded_bios != NULL, a BIOS fica nessa
   area (memoria de video do DS) em vez de reservar mais 512 KB de RAM. */
bool bus_init(uint8_t *embedded_bios)
{
    memset(&bus, 0, sizeof bus);
    bus.ram = (uint8_t *)malloc(RAM_SIZE);
    if (!bus.ram) return false;
    memset(bus.ram, 0, RAM_SIZE);
    if (embedded_bios) {
        bus.bios = embedded_bios;
        return true;
    }
    bus.bios = (uint8_t *)malloc(BIOS_SIZE);
    if (!bus.bios) return false;
    memset(bus.bios, 0xFF, BIOS_SIZE);
    return true;
}

#define IS_IO(p) (((p) - 0x1F801000u) < 0x2000u)

static inline uint8_t *map(uint32_t p, bool wr)
{
    if (p < 0x00800000)
        return bus.ram + (p & (RAM_SIZE - 1));
    if (p >= 0x1F800000 && p < 0x1F800400)
        return bus.scratch + (p - 0x1F800000);
    if (!wr && p >= 0x1FC00000 && p < 0x1FC80000)
        return bus.bios + (p - 0x1FC00000);
    return NULL;
}

uint32_t bus_read8(uint32_t a)
{
    uint32_t p = a & region_mask[a >> 29];
    if (IS_IO(p)) return hw_read(p - 0x1F801000u, 1);
    uint8_t *m = map(p, false);
    return m ? *m : 0xFF;
}

uint32_t bus_read16(uint32_t a)
{
    uint32_t p = a & region_mask[a >> 29];
    if (IS_IO(p)) return hw_read(p - 0x1F801000u, 2);
    uint8_t *m = map(p, false);
    return m ? *(uint16_t *)m : 0xFFFF;
}

uint32_t bus_read32(uint32_t a)
{
    uint32_t p = a & region_mask[a >> 29];
    if (IS_IO(p)) return hw_read(p - 0x1F801000u, 4);
    uint8_t *m = map(p, false);
    if (m) return *(uint32_t *)m;
    if (a == 0xFFFE0130) return bus.cache_ctrl;
    return 0xFFFFFFFF;
}

void bus_write8(uint32_t a, uint32_t v)
{
    uint32_t p = a & region_mask[a >> 29];
    if (IS_IO(p)) { hw_write(p - 0x1F801000u, v, 1); return; }
    uint8_t *m = map(p, true);
    if (m) *m = (uint8_t)v;
}

void bus_write16(uint32_t a, uint32_t v)
{
    uint32_t p = a & region_mask[a >> 29];
    if (IS_IO(p)) { hw_write(p - 0x1F801000u, v, 2); return; }
    uint8_t *m = map(p, true);
    if (m) *(uint16_t *)m = (uint16_t)v;
}

void bus_write32(uint32_t a, uint32_t v)
{
    uint32_t p = a & region_mask[a >> 29];
    if (IS_IO(p)) { hw_write(p - 0x1F801000u, v, 4); return; }
    uint8_t *m = map(p, true);
    if (m) { *(uint32_t *)m = v; return; }
    if (a == 0xFFFE0130) bus.cache_ctrl = v;
}
