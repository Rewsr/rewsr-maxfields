/*
 * test_dmi.c, exercises the portable SMBIOS parser against a
 * hand-built structure table.
 *
 * The fixture mirrors what a dual-socket EPYC box actually reports:
 * Type 0/1/2/3, two Type 4 sockets (one using the SMBIOS 3.0 word
 * count escape), a spread of Type 17 devices covering every size
 * encoding branch, and the Type 127 terminator.
 */
#include <stdlib.h>
#include <string.h>

#include "../dmi/rewsr_dmi.h"
#include "ctest.h"
#include "tutil.h"

struct blob {
    uint8_t b[16384];
    size_t len;
};

static void blob_append(struct blob *bl, const uint8_t *data, size_t n) {
    if (bl->len + n <= sizeof(bl->b)) {
        memcpy(bl->b + bl->len, data, n);
        bl->len += n;
    } else {
        /* Fixture overflow is a test bug; make it loud. */
        abort();
    }
}

/*
 * Append one structure: formatted area (f[1] is forced to flen so a
 * fixture cannot lie about its own length) followed by the string
 * set. With no strings the set is the bare double NUL.
 */
static void blob_struct(struct blob *bl, const uint8_t *f, size_t flen,
                        const char *const *strings, size_t nstrings) {
    uint8_t hdr[256];
    if (flen > sizeof(hdr)) {
        abort();
    }
    memcpy(hdr, f, flen);
    hdr[1] = (uint8_t)flen;
    blob_append(bl, hdr, flen);
    if (nstrings == 0) {
        blob_append(bl, (const uint8_t *)"\0", 1);
        blob_append(bl, (const uint8_t *)"\0", 1);
        return;
    }
    for (size_t i = 0; i < nstrings; i++) {
        blob_append(bl, (const uint8_t *)strings[i],
                    strlen(strings[i]) + 1);
    }
    blob_append(bl, (const uint8_t *)"\0", 1);
}

/* Type 0: vendor=1, version=2, release date=3. */
static void fixture_bios(struct blob *bl) {
    uint8_t f[0x18] = {0};
    f[0] = REWSR_DMI_TYPE_BIOS;
    f[2] = 0x00; f[3] = 0x00;      /* handle 0x0000 */
    f[0x04] = 1;                   /* vendor */
    f[0x05] = 2;                   /* version */
    f[0x06] = 0x00; f[0x07] = 0xF0; /* starting segment, unused */
    f[0x08] = 3;                   /* release date */
    f[0x09] = 0x7F;                /* ROM size: 64K*(0x7F+1) = 8192KB */
    f[0x14] = 2;                   /* system BIOS major */
    f[0x15] = 5;                   /* minor */
    const char *s[] = { "American Megatrends Inc.", "2.5.6",
                        "07/15/2024" };
    blob_struct(bl, f, sizeof(f), s, 3);
}

/* UUID fixture: text form 03c8e5c4-2a17-4d3b-9a0f-1b2c3d4e5f60 in the
 * SMBIOS >= 2.6 mixed-endian byte order. */
static const uint8_t k_uuid_raw[16] = {
    0xc4, 0xe5, 0xc8, 0x03,   /* time_low, LE */
    0x17, 0x2a,               /* time_mid, LE */
    0x3b, 0x4d,               /* time_hi_and_version, LE */
    0x9a, 0x0f,               /* clock_seq, as written */
    0x1b, 0x2c, 0x3d, 0x4e, 0x5f, 0x60, /* node, as written */
};

static void fixture_system(struct blob *bl) {
    uint8_t f[0x1B] = {0};
    f[0] = REWSR_DMI_TYPE_SYSTEM;
    f[2] = 0x01; f[3] = 0x00;
    f[0x04] = 1; /* manufacturer */
    f[0x05] = 2; /* product */
    f[0x06] = 3; /* version */
    f[0x07] = 4; /* serial */
    memcpy(f + 0x08, k_uuid_raw, 16);
    f[0x18] = 6; /* wake-up type, unused by parser */
    f[0x19] = 5; /* SKU */
    f[0x1A] = 6; /* family */
    /* Trailing spaces on the serial mimic real space-padded firmware
     * and must be trimmed by the parser. */
    const char *s[] = { "Supermicro", "AS -2124BT-HNTR", "1.02",
                        "S424785X1B03274   ", "SKU-2124", "H12" };
    blob_struct(bl, f, sizeof(f), s, 6);
}

static void fixture_baseboard(struct blob *bl) {
    uint8_t f[0x0F] = {0};
    f[0] = REWSR_DMI_TYPE_BASEBOARD;
    f[2] = 0x02; f[3] = 0x00;
    f[0x04] = 1;
    f[0x05] = 2;
    f[0x06] = 3;
    f[0x07] = 4;
    f[0x08] = 0; /* asset tag deliberately absent (index 0) */
    const char *s[] = { "Supermicro", "H12DST-B", "1.01", "WM237S001234" };
    blob_struct(bl, f, sizeof(f), s, 4);
}

static void fixture_chassis(struct blob *bl) {
    uint8_t f[0x16] = {0};
    f[0] = REWSR_DMI_TYPE_CHASSIS;
    f[2] = 0x03; f[3] = 0x00;
    f[0x04] = 1;
    f[0x05] = 0x97; /* bit 7 lock + type 0x17 Rack Mount Chassis */
    f[0x06] = 0;
    f[0x07] = 2;
    f[0x08] = 0;
    const char *s[] = { "Supermicro", "C801XL0034" };
    blob_struct(bl, f, sizeof(f), s, 2);
}

/*
 * Type 4 socket. cores/threads express the SMBIOS 3.0 dance: byte
 * fields saturate at 0xFF and defer to the Count 2 words.
 */
static void fixture_processor(struct blob *bl, uint16_t handle,
                              const char *socket, uint16_t max_mhz,
                              uint8_t cores8, uint16_t cores16,
                              uint8_t threads8, uint16_t threads16,
                              int populated) {
    uint8_t f[0x30] = {0};
    f[0] = REWSR_DMI_TYPE_PROCESSOR;
    f[2] = (uint8_t)(handle & 0xFF);
    f[3] = (uint8_t)(handle >> 8);
    f[0x04] = 1;    /* socket designation */
    f[0x05] = 0x03; /* Central Processor */
    f[0x06] = 0x6B; /* family: Zen */
    f[0x07] = 2;    /* manufacturer */
    /* Processor ID: CPUID leaf 1 EAX in the low dword (EPYC Milan
     * 0x00A00F11), leaf 1 EDX in the high dword. */
    f[0x08] = 0x11; f[0x09] = 0x0F; f[0x0A] = 0xA0; f[0x0B] = 0x00;
    f[0x0C] = 0xFF; f[0x0D] = 0xFB; f[0x0E] = 0x8B; f[0x0F] = 0x17;
    f[0x10] = 3; /* version string */
    f[0x14] = (uint8_t)(max_mhz & 0xFF);
    f[0x15] = (uint8_t)(max_mhz >> 8);
    f[0x16] = (uint8_t)((max_mhz - 450) & 0xFF);
    f[0x17] = (uint8_t)((max_mhz - 450) >> 8);
    f[0x18] = populated ? 0x41 : 0x00; /* status: bit6 populated, CPU enabled */
    f[0x23] = cores8;
    f[0x24] = cores8;
    f[0x25] = threads8;
    f[0x2A] = (uint8_t)(cores16 & 0xFF);
    f[0x2B] = (uint8_t)(cores16 >> 8);
    f[0x2C] = (uint8_t)(cores16 & 0xFF);
    f[0x2D] = (uint8_t)(cores16 >> 8);
    f[0x2E] = (uint8_t)(threads16 & 0xFF);
    f[0x2F] = (uint8_t)(threads16 >> 8);
    const char *s[] = { socket, "Advanced Micro Devices, Inc.",
                        "AMD EPYC 7763 64-Core Processor" };
    blob_struct(bl, f, sizeof(f), s, 3);
}

/* Type 17 DIMM. raw_size/ext_size drive the size decoding branches. */
static void fixture_memdev(struct blob *bl, uint16_t handle,
                           uint16_t raw_size, uint32_t ext_size,
                           uint16_t speed, uint16_t cfg_speed,
                           uint8_t rank, const char *locator,
                           const char *part, int with_strings) {
    uint8_t f[0x28] = {0};
    f[0] = REWSR_DMI_TYPE_MEMDEV;
    f[2] = (uint8_t)(handle & 0xFF);
    f[3] = (uint8_t)(handle >> 8);
    f[0x04] = 0x30; f[0x05] = 0x00; /* physical array handle */
    f[0x06] = 0xFE; f[0x07] = 0xFF; /* error info: not provided */
    f[0x08] = 72; f[0x09] = 0;      /* total width: 64 + 8 ECC */
    f[0x0A] = 64; f[0x0B] = 0;      /* data width */
    f[0x0C] = (uint8_t)(raw_size & 0xFF);
    f[0x0D] = (uint8_t)(raw_size >> 8);
    f[0x0E] = 0x09; /* DIMM */
    f[0x10] = with_strings ? 1 : 0; /* device locator */
    f[0x11] = with_strings ? 2 : 0; /* bank locator */
    f[0x12] = 0x1A; /* DDR4 */
    f[0x15] = (uint8_t)(speed & 0xFF);
    f[0x16] = (uint8_t)(speed >> 8);
    f[0x17] = with_strings ? 3 : 0; /* manufacturer */
    f[0x18] = with_strings ? 4 : 0; /* serial */
    f[0x1A] = with_strings ? 5 : 0; /* part number */
    f[0x1B] = rank & 0x0F;
    f[0x1C] = (uint8_t)(ext_size & 0xFF);
    f[0x1D] = (uint8_t)((ext_size >> 8) & 0xFF);
    f[0x1E] = (uint8_t)((ext_size >> 16) & 0xFF);
    f[0x1F] = (uint8_t)((ext_size >> 24) & 0xFF);
    f[0x20] = (uint8_t)(cfg_speed & 0xFF);
    f[0x21] = (uint8_t)(cfg_speed >> 8);
    if (with_strings) {
        const char *s[5];
        s[0] = locator;
        s[1] = "P0 CHANNEL A";
        s[2] = "Samsung";
        s[3] = "038B5C71";
        s[4] = part;
        blob_struct(bl, f, sizeof(f), s, 5);
    } else {
        blob_struct(bl, f, sizeof(f), NULL, 0);
    }
}

static void fixture_end(struct blob *bl) {
    uint8_t f[4] = { REWSR_DMI_TYPE_END, 0, 0x7F, 0xFF };
    blob_struct(bl, f, sizeof(f), NULL, 0);
}

static void build_fixture(struct blob *bl) {
    bl->len = 0;
    fixture_bios(bl);
    fixture_system(bl);
    fixture_baseboard(bl);
    fixture_chassis(bl);
    /* Socket 0: plain byte counts (64c/128t EPYC 7763). */
    fixture_processor(bl, 0x0010, "CPU1", 3500, 64, 0, 128, 0, 1);
    /* Socket 1: byte counts saturated, word counts carry 256c/512t. */
    fixture_processor(bl, 0x0011, "CPU2", 2100, 0xFF, 256, 0xFF, 512, 1);
    /* 64GB via the 0x7FFF extended-size escape. */
    fixture_memdev(bl, 0x0020, 0x7FFF, 65536, 3200, 2933, 2,
                   "DIMMA0", "M393A8G40AB2-CWE", 1);
    /* 16GB expressed directly in MB. */
    fixture_memdev(bl, 0x0021, 16384, 0, 3200, 3200, 1,
                   "DIMMB0", "HMA82GR7CJR8N-XN", 1);
    /* Empty slot: size 0, no strings at all. */
    fixture_memdev(bl, 0x0022, 0, 0, 0, 0, 0, NULL, NULL, 0);
    /* KB-units size (bit 15): 512KB rounds up to 1MB. */
    fixture_memdev(bl, 0x0023, 0x8000 | 512, 0, 0, 0, 1,
                   "DIMMC0", "LEGACY-512K", 1);
    /* Firmware reports size unknown. */
    fixture_memdev(bl, 0x0024, 0xFFFF, 0, 2666, 0, 0,
                   "DIMMD0", "UNKNOWN-SIZE", 1);
    fixture_end(bl);
}

static void test_walk_counts_structures(void) {
    struct blob bl;
    build_fixture(&bl);

    struct rewsr_dmi_cursor cur;
    struct rewsr_dmi_struct st;
    rewsr_dmi_cursor_init(&cur, bl.b, bl.len);

    int n = 0;
    int rc;
    int saw_end = 0;
    while ((rc = rewsr_dmi_next(&cur, &st)) == 1) {
        n++;
        if (st.type == REWSR_DMI_TYPE_END) {
            saw_end = 1;
        }
    }
    ASSERT_EQ_INT(0, rc);
    /* 4 singletons + 2 processors + 5 memdevs + end */
    ASSERT_EQ_INT(12, n);
    ASSERT_TRUE(saw_end);
}

static void test_string_resolution(void) {
    struct blob bl;
    build_fixture(&bl);

    struct rewsr_dmi_cursor cur;
    struct rewsr_dmi_struct st;
    rewsr_dmi_cursor_init(&cur, bl.b, bl.len);
    ASSERT_EQ_INT(1, rewsr_dmi_next(&cur, &st)); /* Type 0 */
    ASSERT_EQ_INT(REWSR_DMI_TYPE_BIOS, st.type);

    const char *s1 = rewsr_dmi_string(&st, 1);
    const char *s3 = rewsr_dmi_string(&st, 3);
    ASSERT_TRUE(s1 != NULL);
    ASSERT_TRUE(s3 != NULL);
    ASSERT_EQ_STR("American Megatrends Inc.", s1);
    ASSERT_EQ_STR("07/15/2024", s3);
    /* Index 0 is the spec's "no string", 4 dangles past the set. */
    ASSERT_TRUE(rewsr_dmi_string(&st, 0) == NULL);
    ASSERT_TRUE(rewsr_dmi_string(&st, 4) == NULL);
}

static void test_parse_bios_and_system(void) {
    struct blob bl;
    build_fixture(&bl);

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));
    ASSERT_EQ_U64(12, info.structure_count);

    ASSERT_TRUE(info.bios.present);
    ASSERT_EQ_STR("American Megatrends Inc.", info.bios.vendor);
    ASSERT_EQ_STR("2.5.6", info.bios.version);
    ASSERT_EQ_STR("07/15/2024", info.bios.release_date);
    ASSERT_EQ_U64(8192, info.bios.rom_kb);
    ASSERT_EQ_INT(2, info.bios.major_release);
    ASSERT_EQ_INT(5, info.bios.minor_release);

    ASSERT_TRUE(info.system.present);
    ASSERT_EQ_STR("Supermicro", info.system.manufacturer);
    ASSERT_EQ_STR("AS -2124BT-HNTR", info.system.product);
    /* Space padding trimmed. */
    ASSERT_EQ_STR("S424785X1B03274", info.system.serial);
    ASSERT_EQ_STR("SKU-2124", info.system.sku);
    ASSERT_EQ_STR("H12", info.system.family);
    ASSERT_TRUE(info.system.uuid_present);
    ASSERT_EQ_STR("03c8e5c4-2a17-4d3b-9a0f-1b2c3d4e5f60",
                  info.system.uuid);
    ASSERT_MEM_EQ(k_uuid_raw, info.system.uuid_raw, 16);
}

static void test_parse_baseboard_and_chassis(void) {
    struct blob bl;
    build_fixture(&bl);

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));

    ASSERT_TRUE(info.baseboard.present);
    ASSERT_EQ_STR("H12DST-B", info.baseboard.product);
    ASSERT_EQ_STR("WM237S001234", info.baseboard.serial);
    /* Index 0 reference resolves to the empty string. */
    ASSERT_EQ_STR("", info.baseboard.asset_tag);

    ASSERT_TRUE(info.chassis.present);
    ASSERT_EQ_INT(0x17, info.chassis.type);
    ASSERT_TRUE(info.chassis.has_lock);
    ASSERT_EQ_STR("Rack Mount Chassis",
                  rewsr_dmi_chassis_type_name(info.chassis.type));
    ASSERT_EQ_STR("C801XL0034", info.chassis.serial);
}

static void test_parse_processors(void) {
    struct blob bl;
    build_fixture(&bl);

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));

    ASSERT_EQ_U64(2, info.processor_count);
    const struct rewsr_dmi_processor *p0 = &info.processors[0];
    ASSERT_EQ_INT(0x0010, p0->handle);
    ASSERT_EQ_STR("CPU1", p0->socket);
    ASSERT_EQ_STR("Advanced Micro Devices, Inc.", p0->manufacturer);
    ASSERT_EQ_STR("AMD EPYC 7763 64-Core Processor", p0->version);
    /* Low dword of the processor ID carries CPUID leaf 1 EAX. */
    ASSERT_EQ_U64(0x178BFBFF00A00F11ull, p0->id);
    ASSERT_EQ_INT(3500, p0->max_speed_mhz);
    ASSERT_EQ_INT(3050, p0->current_speed_mhz);
    ASSERT_EQ_INT(64, p0->core_count);
    ASSERT_EQ_INT(128, p0->thread_count);
    ASSERT_TRUE(p0->populated);

    /* Second socket exercises the 0xFF byte-count escape into the
     * SMBIOS 3.0 Count 2 words. */
    const struct rewsr_dmi_processor *p1 = &info.processors[1];
    ASSERT_EQ_STR("CPU2", p1->socket);
    ASSERT_EQ_INT(256, p1->core_count);
    ASSERT_EQ_INT(256, p1->cores_enabled);
    ASSERT_EQ_INT(512, p1->thread_count);
}

static void test_parse_memdevs(void) {
    struct blob bl;
    build_fixture(&bl);

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));

    ASSERT_EQ_U64(5, info.memdev_count);

    const struct rewsr_dmi_memdev *m0 = &info.memdevs[0];
    ASSERT_TRUE(m0->installed);
    ASSERT_EQ_U64(65536, m0->size_mb); /* extended-size escape */
    ASSERT_EQ_INT(3200, m0->speed_mts);
    ASSERT_EQ_INT(2933, m0->configured_speed_mts);
    ASSERT_EQ_INT(2, m0->rank);
    ASSERT_EQ_INT(72, m0->total_width_bits);
    ASSERT_EQ_INT(64, m0->data_width_bits);
    ASSERT_EQ_STR("DIMMA0", m0->device_locator);
    ASSERT_EQ_STR("P0 CHANNEL A", m0->bank_locator);
    ASSERT_EQ_STR("Samsung", m0->manufacturer);
    ASSERT_EQ_STR("M393A8G40AB2-CWE", m0->part_number);
    ASSERT_EQ_STR("DDR4", rewsr_dmi_memory_type_name(m0->memory_type));
    ASSERT_EQ_STR("DIMM", rewsr_dmi_form_factor_name(m0->form_factor));

    ASSERT_EQ_U64(16384, info.memdevs[1].size_mb); /* direct MB */

    ASSERT_TRUE(!info.memdevs[2].installed); /* empty slot */
    ASSERT_EQ_U64(0, info.memdevs[2].size_mb);
    ASSERT_EQ_STR("", info.memdevs[2].device_locator);

    ASSERT_EQ_U64(1, info.memdevs[3].size_mb); /* 512KB rounds to 1MB */

    ASSERT_TRUE(info.memdevs[4].installed);
    ASSERT_EQ_U64(UINT64_MAX, info.memdevs[4].size_mb); /* unknown */
}

static void test_mem_size_decode_table(void) {
    ASSERT_EQ_U64(0, rewsr_dmi_mem_size_mb(0, 0));
    ASSERT_EQ_U64(UINT64_MAX, rewsr_dmi_mem_size_mb(0xFFFF, 0));
    ASSERT_EQ_U64(8192, rewsr_dmi_mem_size_mb(8192, 0));
    ASSERT_EQ_U64(32766, rewsr_dmi_mem_size_mb(0x7FFE, 0));
    /* Escape to the extended dword, bit 31 must be masked. */
    ASSERT_EQ_U64(131072, rewsr_dmi_mem_size_mb(0x7FFF, 131072));
    ASSERT_EQ_U64(131072,
                  rewsr_dmi_mem_size_mb(0x7FFF, 0x80000000u | 131072));
    /* KB units. */
    ASSERT_EQ_U64(1, rewsr_dmi_mem_size_mb(0x8000 | 1024, 0));
    ASSERT_EQ_U64(1, rewsr_dmi_mem_size_mb(0x8000 | 512, 0));
    ASSERT_EQ_U64(4, rewsr_dmi_mem_size_mb(0x8000 | 4096, 0));
}

static void test_uuid_formatting(void) {
    char out[37];
    rewsr_dmi_format_uuid(k_uuid_raw, out);
    ASSERT_EQ_STR("03c8e5c4-2a17-4d3b-9a0f-1b2c3d4e5f60", out);

    /* All-zero and all-ones sentinels are rejected at the parse
     * level; the formatter itself is mechanical. */
    static const uint8_t seq[16] = { 0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                     0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                     0x0c, 0x0d, 0x0e, 0x0f };
    rewsr_dmi_format_uuid(seq, out);
    ASSERT_EQ_STR("03020100-0504-0706-0809-0a0b0c0d0e0f", out);
}

static void test_uuid_sentinels_not_present(void) {
    struct blob bl;
    bl.len = 0;
    uint8_t f[0x1B] = {0};
    f[0] = REWSR_DMI_TYPE_SYSTEM;
    f[0x04] = 1;
    memset(f + 0x08, 0xFF, 16); /* "set but not readable" sentinel */
    const char *s[] = { "Vendor" };
    blob_struct(&bl, f, sizeof(f), s, 1);
    fixture_end(&bl);

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));
    ASSERT_TRUE(info.system.present);
    ASSERT_TRUE(!info.system.uuid_present);
}

static void test_malformed_tables(void) {
    static struct rewsr_dmi_info info;

    /* Formatted length below the 4 byte header minimum. */
    uint8_t bad_len[] = { 4, 3, 0x00, 0x00, 0x00, 0x00 };
    ASSERT_EQ_INT(-EPROTO, rewsr_dmi_parse(bad_len, sizeof(bad_len), &info));

    /* Formatted area running past the buffer. */
    uint8_t overrun[] = { 4, 0x30, 0x00, 0x00, 0x00, 0x00 };
    ASSERT_EQ_INT(-EPROTO, rewsr_dmi_parse(overrun, sizeof(overrun), &info));

    /* String set never terminated. */
    uint8_t unterminated[] = { 1, 4, 0x00, 0x00, 'A', 'B', 'C' };
    ASSERT_EQ_INT(-EPROTO,
                  rewsr_dmi_parse(unterminated, sizeof(unterminated), &info));

    ASSERT_EQ_INT(-EINVAL, rewsr_dmi_parse(NULL, 100, &info));
    ASSERT_EQ_INT(-EINVAL, rewsr_dmi_parse(unterminated, 3, &info));
}

static void test_truncated_final_terminator(void) {
    /* One structure whose string set ends on a single NUL exactly at
     * the end of the blob; seen from firmware that drops the last
     * terminator byte. Must parse, not error. */
    struct blob bl;
    bl.len = 0;
    uint8_t f[0x0F] = {0};
    f[0] = REWSR_DMI_TYPE_BASEBOARD;
    f[0x04] = 1;
    const char *s[] = { "OnlyVendor" };
    blob_struct(&bl, f, sizeof(f), s, 1);
    bl.len -= 1; /* chop the final terminator NUL */

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));
    ASSERT_TRUE(info.baseboard.present);
    ASSERT_EQ_STR("OnlyVendor", info.baseboard.manufacturer);
}

static void test_zero_padding_treated_as_end(void) {
    /* Firmware pads the sysfs blob with zeros past Type 127 at times;
     * a zero type + zero length where a header should be is EOF, not
     * corruption. Build a table with no Type 127 and 16 zero bytes. */
    struct blob bl;
    bl.len = 0;
    uint8_t f[0x0F] = {0};
    f[0] = REWSR_DMI_TYPE_BASEBOARD;
    f[0x04] = 1;
    const char *s[] = { "PadVendor" };
    blob_struct(&bl, f, sizeof(f), s, 1);
    uint8_t pad[16] = {0};
    blob_append(&bl, pad, sizeof(pad));

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));
    ASSERT_EQ_U64(1, info.structure_count);
    ASSERT_EQ_STR("PadVendor", info.baseboard.manufacturer);
}

static void test_memdev_capacity_overflow_is_counted(void) {
    static struct blob bl;
    bl.len = 0;
    for (int i = 0; i < REWSR_DMI_MAX_MEMDEVS + 8; i++) {
        fixture_memdev(&bl, (uint16_t)(0x100 + i), 16384, 0, 3200, 3200,
                       1, NULL, NULL, 0);
    }
    fixture_end(&bl);

    static struct rewsr_dmi_info info;
    ASSERT_EQ_INT(0, rewsr_dmi_parse(bl.b, bl.len, &info));
    ASSERT_EQ_U64(REWSR_DMI_MAX_MEMDEVS, info.memdev_count);
    ASSERT_EQ_U64(8, info.memdevs_dropped);
}

static uint8_t checksum_fix(const uint8_t *buf, size_t len, size_t csum_off) {
    unsigned sum = 0;
    for (size_t i = 0; i < len; i++) {
        if (i != csum_off) {
            sum += buf[i];
        }
    }
    return (uint8_t)(0x100 - (sum & 0xFF));
}

static void test_entry_point_64(void) {
    uint8_t ep[0x18] = {0};
    memcpy(ep, "_SM3_", 5);
    ep[0x06] = 0x18; /* entry length */
    ep[0x07] = 3;    /* major */
    ep[0x08] = 4;    /* minor */
    ep[0x09] = 0;    /* docrev */
    ep[0x0A] = 0x01; /* entry point revision */
    /* table max size = 0x00003456 LE */
    ep[0x0C] = 0x56; ep[0x0D] = 0x34;
    /* table address = 0x000000007F540000 LE */
    ep[0x10] = 0x00; ep[0x11] = 0x00; ep[0x12] = 0x54; ep[0x13] = 0x7F;
    ep[0x05] = checksum_fix(ep, sizeof(ep), 0x05);

    struct rewsr_dmi_entry ent;
    ASSERT_EQ_INT(0, rewsr_dmi_parse_entry_point(ep, sizeof(ep), &ent));
    ASSERT_TRUE(ent.is_64bit);
    ASSERT_EQ_INT(3, ent.major);
    ASSERT_EQ_INT(4, ent.minor);
    ASSERT_EQ_U64(0x3456, ent.table_len);
    ASSERT_EQ_U64(0x7F540000ull, ent.table_addr);

    /* Corrupt one byte: checksum must catch it. */
    ep[0x08] = 9;
    ASSERT_EQ_INT(-EPROTO,
                  rewsr_dmi_parse_entry_point(ep, sizeof(ep), &ent));
}

static void test_entry_point_32(void) {
    uint8_t ep[0x1F] = {0};
    memcpy(ep, "_SM_", 4);
    ep[0x05] = 0x1F; /* entry length */
    ep[0x06] = 2;    /* major */
    ep[0x07] = 8;    /* minor */
    memcpy(ep + 0x10, "_DMI_", 5);
    /* structure table length 0x1A7C, address 0x000EB000, count 70 */
    ep[0x16] = 0x7C; ep[0x17] = 0x1A;
    ep[0x18] = 0x00; ep[0x19] = 0xB0; ep[0x1A] = 0x0E; ep[0x1B] = 0x00;
    ep[0x1C] = 70;
    ep[0x04] = checksum_fix(ep, sizeof(ep), 0x04);

    struct rewsr_dmi_entry ent;
    ASSERT_EQ_INT(0, rewsr_dmi_parse_entry_point(ep, sizeof(ep), &ent));
    ASSERT_TRUE(!ent.is_64bit);
    ASSERT_EQ_INT(2, ent.major);
    ASSERT_EQ_INT(8, ent.minor);
    ASSERT_EQ_U64(0x1A7C, ent.table_len);
    ASSERT_EQ_U64(0x000EB000ull, ent.table_addr);

    /* Unknown anchor. */
    uint8_t junk[0x20] = { 'X', 'Y', 'Z' };
    ASSERT_EQ_INT(-EPROTO,
                  rewsr_dmi_parse_entry_point(junk, sizeof(junk), &ent));
}

static void test_read_sysfs_platform_path(void) {
    static struct rewsr_dmi_info info;
#if defined(__linux__)
    /* On Linux, point the reader at a fixture tree carrying the same
     * blob and expect a full parse. */
    struct blob bl;
    build_fixture(&bl);
    char root[256];
    ASSERT_EQ_INT(0, rewsr_tutil_mkroot(root, sizeof(root)));
    ASSERT_EQ_INT(0, rewsr_tutil_write_bytes(
                         root, "sys/firmware/dmi/tables/DMI", bl.b, bl.len));
    ASSERT_EQ_INT(0, rewsr_dmi_read_sysfs(root, &info));
    ASSERT_EQ_STR("Supermicro", info.system.manufacturer);
    rewsr_tutil_rmtree(root);
#else
    /* Off Linux the guarded path must refuse cleanly, not pretend. */
    ASSERT_EQ_INT(-ENOTSUP, rewsr_dmi_read_sysfs("", &info));
#endif
}

REWSR_TEST_MAIN(
    RUN_TEST(test_walk_counts_structures);
    RUN_TEST(test_string_resolution);
    RUN_TEST(test_parse_bios_and_system);
    RUN_TEST(test_parse_baseboard_and_chassis);
    RUN_TEST(test_parse_processors);
    RUN_TEST(test_parse_memdevs);
    RUN_TEST(test_mem_size_decode_table);
    RUN_TEST(test_uuid_formatting);
    RUN_TEST(test_uuid_sentinels_not_present);
    RUN_TEST(test_malformed_tables);
    RUN_TEST(test_truncated_final_terminator);
    RUN_TEST(test_zero_padding_treated_as_end);
    RUN_TEST(test_memdev_capacity_overflow_is_counted);
    RUN_TEST(test_entry_point_64);
    RUN_TEST(test_entry_point_32);
    RUN_TEST(test_read_sysfs_platform_path);
)
