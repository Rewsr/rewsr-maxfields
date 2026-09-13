/*
 * rewsr_msr.c, portable decoders for well-known MSR values.
 *
 * Pure functions of raw register values; tested with captures in
 * test/test_msr.c. The privileged read itself is in
 * rewsr_msr_linux.c.
 */
#include "rewsr_msr.h"

#include <errno.h>

void rewsr_msr_decode_rapl_units(uint64_t raw,
                                 struct rewsr_rapl_units *out) {
    uint32_t power_exp = (uint32_t)(raw & 0xF);
    uint32_t energy_exp = (uint32_t)((raw >> 8) & 0x1F);
    uint32_t time_exp = (uint32_t)((raw >> 16) & 0xF);
    out->power_unit_div = 1u << power_exp;
    out->energy_unit_div = 1u << energy_exp;
    out->time_unit_div = 1u << time_exp;
}

double rewsr_msr_energy_joules(uint64_t raw_energy_status,
                               const struct rewsr_rapl_units *units) {
    /* Only bits 31:0 are the counter; the top half is reserved and
     * reads back undefined on some parts. */
    uint32_t counter = (uint32_t)(raw_energy_status & 0xFFFFFFFFu);
    return (double)counter / (double)units->energy_unit_div;
}

double rewsr_msr_energy_delta_joules(uint32_t before, uint32_t after,
                                     const struct rewsr_rapl_units *units) {
    /* Unsigned subtraction is already wrap-correct for one wrap:
     * (after - before) mod 2^32. */
    uint32_t ticks = after - before;
    return (double)ticks / (double)units->energy_unit_div;
}

uint64_t rewsr_msr_tsc_delta_ns(uint64_t t0, uint64_t t1, uint64_t tsc_hz) {
    if (tsc_hz == 0) {
        return 0;
    }
    uint64_t ticks = t1 - t0; /* mod 2^64, wrap-correct */
    /* ticks * 1e9 overflows u64 at ~18.4 seconds of 1 GHz ticks;
     * go through 128 bits. */
    unsigned __int128 ns =
        ((unsigned __int128)ticks * 1000000000ull) / tsc_hz;
    if (ns > (unsigned __int128)UINT64_MAX) {
        return UINT64_MAX;
    }
    return (uint64_t)ns;
}

uint32_t rewsr_msr_platform_info_base_mhz(uint64_t raw) {
    uint32_t ratio = (uint32_t)((raw >> 8) & 0xFF);
    return ratio * 100;
}

int rewsr_msr_therm_celsius(uint64_t therm_status, uint64_t temp_target,
                            int *out_celsius) {
    if (((therm_status >> 31) & 1) == 0) {
        /* Reading Valid bit clear: the digital sensor has nothing. */
        return -ENODATA;
    }
    int readout = (int)((therm_status >> 16) & 0x7F);
    int tjmax = (int)((temp_target >> 16) & 0xFF);
    if (tjmax == 0) {
        /* A zero TjMax means TEMPERATURE_TARGET was not readable;
         * refusing beats reporting a temperature 100 degrees off. */
        return -ENODATA;
    }
    *out_celsius = tjmax - readout;
    return 0;
}
