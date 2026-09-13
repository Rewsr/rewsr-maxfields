/*
 * rewsr_msr.h, model-specific register access and decoding.
 *
 * The read path uses the msr kernel module's char device,
 * /dev/cpu/N/msr: pread() at file offset M returns the 8 byte value
 * of MSR M as executed on CPU N. Needs CAP_SYS_RAWIO (and the module
 * loaded), which the collector has; everything else here decodes raw
 * values and is portable.
 *
 * MSR numbers are copied from arch/x86/include/asm/msr-index.h, each
 * with its kernel name, so no kernel headers are needed to build.
 */
#ifndef REWSR_MSR_H
#define REWSR_MSR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* msr-index.h: MSR_IA32_TSC */
#define REWSR_MSR_IA32_TSC 0x00000010u
/* msr-index.h: MSR_PLATFORM_INFO (Intel) */
#define REWSR_MSR_PLATFORM_INFO 0x000000CEu
/* msr-index.h: MSR_IA32_THERM_STATUS */
#define REWSR_MSR_IA32_THERM_STATUS 0x0000019Cu
/* msr-index.h: MSR_IA32_TEMPERATURE_TARGET */
#define REWSR_MSR_IA32_TEMPERATURE_TARGET 0x000001A2u
/* msr-index.h: MSR_RAPL_POWER_UNIT (Intel) */
#define REWSR_MSR_RAPL_POWER_UNIT 0x00000606u
/* msr-index.h: MSR_PKG_ENERGY_STATUS (Intel) */
#define REWSR_MSR_PKG_ENERGY_STATUS 0x00000611u
/* msr-index.h: MSR_AMD_RAPL_POWER_UNIT */
#define REWSR_MSR_AMD_RAPL_POWER_UNIT 0xC0010299u
/* msr-index.h: MSR_AMD_PKG_ENERGY_STATUS */
#define REWSR_MSR_AMD_PKG_ENERGY_STATUS 0xC001029Bu

/*
 * Read one MSR on one CPU. Linux only; -ENOTSUP elsewhere. Failure
 * modes worth knowing on Linux:
 *   -ENOENT  msr module not loaded (modprobe msr)
 *   -EACCES  missing CAP_SYS_RAWIO
 *   -EIO     the CPU faulted the RDMSR (register not implemented
 *            on this part), the kernel maps the #GP to EIO
 */
int rewsr_msr_read(int cpu, uint32_t msr, uint64_t *out);

/*
 * RAPL unit register decode (same layout for MSR_RAPL_POWER_UNIT and
 * MSR_AMD_RAPL_POWER_UNIT):
 *   bits  3:0  power unit exponent,  unit = 1 / 2^n W
 *   bits 12:8  energy unit exponent, unit = 1 / 2^n J
 *   bits 19:16 time unit exponent,   unit = 1 / 2^n s
 * The divisors (2^n) are returned instead of floating point units so
 * integer consumers stay exact.
 */
struct rewsr_rapl_units {
    uint32_t power_unit_div;
    uint32_t energy_unit_div;
    uint32_t time_unit_div;
};

void rewsr_msr_decode_rapl_units(uint64_t raw,
                                 struct rewsr_rapl_units *out);

/*
 * Package energy counter (MSR_PKG_ENERGY_STATUS bits 31:0, a
 * free-running accumulator in energy units) to joules.
 */
double rewsr_msr_energy_joules(uint64_t raw_energy_status,
                               const struct rewsr_rapl_units *units);

/*
 * Wrap-correct interval between two energy counter samples, in
 * joules. The hardware counter is 32 bits and wraps in under an hour
 * on a loaded dual-socket box, so callers sampling do need this.
 */
double rewsr_msr_energy_delta_joules(uint32_t before, uint32_t after,
                                     const struct rewsr_rapl_units *units);

/*
 * Nanoseconds between two IA32_TSC samples at a given TSC frequency,
 * correct across one counter wrap (the TSC is 64-bit so wrap takes
 * decades at current clocks, but the math costs nothing). 128-bit
 * intermediate avoids overflow for any realistic delta.
 */
uint64_t rewsr_msr_tsc_delta_ns(uint64_t t0, uint64_t t1, uint64_t tsc_hz);

/*
 * MSR_PLATFORM_INFO bits 15:8 hold the maximum non-turbo ratio; the
 * base frequency is that ratio times the 100 MHz bus clock on every
 * core since Sandy Bridge. Returns MHz, 0 when the field is empty.
 */
uint32_t rewsr_msr_platform_info_base_mhz(uint64_t raw);

/*
 * Package temperature from IA32_THERM_STATUS plus
 * IA32_TEMPERATURE_TARGET:
 *   THERM_STATUS bits 22:16  digital readout, degrees BELOW TjMax
 *   THERM_STATUS bit  31     readout valid
 *   TEMP_TARGET  bits 23:16  TjMax in degrees C
 * Returns 0 and writes degrees C, or -ENODATA when the valid bit is
 * clear.
 */
int rewsr_msr_therm_celsius(uint64_t therm_status, uint64_t temp_target,
                            int *out_celsius);

#ifdef __cplusplus
}
#endif

#endif /* REWSR_MSR_H */
