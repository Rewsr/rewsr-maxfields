/*
 * rewsr_dmi.h, SMBIOS/DMI structure table parsing.
 *
 * The kernel exposes the firmware tables verbatim at
 *   /sys/firmware/dmi/tables/smbios_entry_point  (anchor + metadata)
 *   /sys/firmware/dmi/tables/DMI                 (the structure table)
 * so instead of shelling out to dmidecode we parse the binary blob
 * ourselves. The parser here is pure buffer-in, structs-out and has no
 * platform dependencies; only rewsr_dmi_read_sysfs() at the bottom
 * touches Linux paths.
 *
 * Structure table wire format (SMBIOS spec 3.x, section 6.1):
 *   Each structure starts with a 4 byte header:
 *     u8  type      structure type (0 BIOS, 1 System, 4 Processor, ...)
 *     u8  length    length of the FORMATTED area, includes this header,
 *                   always >= 4
 *     u16 handle    little-endian unique id
 *   The formatted area is followed by the string set: a run of
 *   NUL-terminated strings closed by one extra NUL. A structure with
 *   no strings still carries the two terminating NUL bytes. String
 *   fields inside the formatted area are ONE-BASED indices into that
 *   set; index 0 means "no string".
 *   The table ends at Type 127 (End-of-Table) or at the end of the
 *   blob, whichever comes first.
 *
 * All multi-byte integers in the table are little-endian. We decode
 * them bytewise so the parser is correct on any host endianness.
 */
#ifndef REWSR_DMI_H
#define REWSR_DMI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Structure types we decode. Values are from the SMBIOS spec. */
#define REWSR_DMI_TYPE_BIOS        0
#define REWSR_DMI_TYPE_SYSTEM      1
#define REWSR_DMI_TYPE_BASEBOARD   2
#define REWSR_DMI_TYPE_CHASSIS     3
#define REWSR_DMI_TYPE_PROCESSOR   4
#define REWSR_DMI_TYPE_MEMDEV      17
#define REWSR_DMI_TYPE_END         127

/*
 * Fixed string capacity. SMBIOS strings have no formal limit but real
 * firmware keeps them short (part numbers, vendor names). Longer
 * strings are truncated, never overflowed.
 */
#define REWSR_DMI_STR_MAX 64

/* Capacities for repeated structures on big iron: 8 sockets, and
 * 2 DIMM slots per channel * 12 channels * 8 sockets leaves headroom
 * at 192. Overflow is counted, not dropped silently. */
#define REWSR_DMI_MAX_PROCESSORS 16
#define REWSR_DMI_MAX_MEMDEVS    192

/* Type 0, BIOS Information (spec section 7.1). */
struct rewsr_dmi_bios {
    int present;
    char vendor[REWSR_DMI_STR_MAX];
    char version[REWSR_DMI_STR_MAX];
    char release_date[REWSR_DMI_STR_MAX];
    /* 64K * (n+1) per spec; 0xFF in the raw byte means 16MB or more
     * and firmware then uses the extended size field we do not need
     * for inventory, so rom_kb is 0 in that case. */
    uint32_t rom_kb;
    /* System BIOS major.minor from offsets 0x14/0x15, 0xFF means not
     * reported (older tables are shorter and also yield 0xFF here). */
    uint8_t major_release;
    uint8_t minor_release;
};

/* Type 1, System Information (spec section 7.2). */
struct rewsr_dmi_system {
    int present;
    char manufacturer[REWSR_DMI_STR_MAX];
    char product[REWSR_DMI_STR_MAX];
    char version[REWSR_DMI_STR_MAX];
    char serial[REWSR_DMI_STR_MAX];
    char sku[REWSR_DMI_STR_MAX];
    char family[REWSR_DMI_STR_MAX];
    /* Raw 16 bytes at offset 0x08 plus the canonical text form.
     * Encoding note: since SMBIOS 2.6 the first three fields
     * (time_low u32, time_mid u16, time_hi u16) are little-endian,
     * the trailing 8 bytes are byte-for-byte. Before 2.6 the whole
     * thing was network order. rewsr_dmi_parse assumes >= 2.6, which
     * matches every server this project targets; the raw bytes are
     * kept so a caller can re-render if it knows better. */
    uint8_t uuid_raw[16];
    char uuid[37];
    int uuid_present; /* all-0x00 (not set) and all-0xFF (not settable)
                       * sentinels clear this flag */
};

/* Type 2, Baseboard Information (spec section 7.3). */
struct rewsr_dmi_baseboard {
    int present;
    char manufacturer[REWSR_DMI_STR_MAX];
    char product[REWSR_DMI_STR_MAX];
    char version[REWSR_DMI_STR_MAX];
    char serial[REWSR_DMI_STR_MAX];
    char asset_tag[REWSR_DMI_STR_MAX];
};

/* Type 3, System Enclosure (spec section 7.4). */
struct rewsr_dmi_chassis {
    int present;
    char manufacturer[REWSR_DMI_STR_MAX];
    /* Bits 6:0 of the raw type byte, bit 7 is the chassis-lock flag. */
    uint8_t type;
    int has_lock;
    char version[REWSR_DMI_STR_MAX];
    char serial[REWSR_DMI_STR_MAX];
    char asset_tag[REWSR_DMI_STR_MAX];
};

/* Type 4, Processor Information (spec section 7.5). */
struct rewsr_dmi_processor {
    uint16_t handle;
    char socket[REWSR_DMI_STR_MAX];
    char manufacturer[REWSR_DMI_STR_MAX];
    char version[REWSR_DMI_STR_MAX];
    /* Raw 8-byte processor ID (on x86 this is CPUID leaf 1 EAX in the
     * low dword and leaf 1 EDX in the high dword). */
    uint64_t id;
    uint16_t max_speed_mhz;
    uint16_t current_speed_mhz;
    /* Byte counts saturate at 0xFF, in which case SMBIOS 3.0 tables
     * carry the real value in the Count 2 word fields; we resolve
     * that here so callers see one number. 0 means unknown. */
    uint16_t core_count;
    uint16_t cores_enabled;
    uint16_t thread_count;
    /* Status byte bit 6 = socket populated. */
    int populated;
};

/* Type 17, Memory Device (spec section 7.18). */
struct rewsr_dmi_memdev {
    uint16_t handle;
    /* 0 when the slot is empty; UINT64_MAX when firmware says the size
     * is unknown (raw 0xFFFF). Extended-size dword is honored. */
    uint64_t size_mb;
    int installed;
    uint16_t speed_mts;            /* 0 = unknown */
    uint16_t configured_speed_mts; /* 0 = unknown or short table */
    uint8_t memory_type;           /* raw enum, see rewsr_dmi_memory_type_name */
    uint8_t form_factor;           /* raw enum, see rewsr_dmi_form_factor_name */
    /* Attributes byte bits 3:0; 0 = unknown rank. */
    uint8_t rank;
    uint16_t total_width_bits; /* 0xFFFF (unknown) normalized to 0 */
    uint16_t data_width_bits;
    char device_locator[REWSR_DMI_STR_MAX];
    char bank_locator[REWSR_DMI_STR_MAX];
    char manufacturer[REWSR_DMI_STR_MAX];
    char serial[REWSR_DMI_STR_MAX];
    char part_number[REWSR_DMI_STR_MAX];
};

/*
 * Aggregate decode of one structure table. Plain value type, no heap
 * ownership: callers may memcpy or stack-allocate it freely (it is a
 * few tens of KB, prefer heap or static storage for deep stacks).
 */
struct rewsr_dmi_info {
    struct rewsr_dmi_bios bios;
    struct rewsr_dmi_system system;
    struct rewsr_dmi_baseboard baseboard;
    struct rewsr_dmi_chassis chassis;
    struct rewsr_dmi_processor processors[REWSR_DMI_MAX_PROCESSORS];
    size_t processor_count;
    struct rewsr_dmi_memdev memdevs[REWSR_DMI_MAX_MEMDEVS];
    size_t memdev_count;
    /* Structures seen but beyond the fixed capacity above. */
    size_t processors_dropped;
    size_t memdevs_dropped;
    /* Total structures walked, including types we do not decode.
     * Useful as a sanity signal that the blob was really a table. */
    size_t structure_count;
};

/*
 * Low-level walk cursor. Exposed so tests (and future decoders for
 * other types) can iterate raw structures without buying into the
 * aggregate decode above.
 */
struct rewsr_dmi_cursor {
    const uint8_t *buf;
    size_t len;
    size_t off;
    int done;
};

/* One raw structure as located by the walker. */
struct rewsr_dmi_struct {
    uint8_t type;
    uint8_t formatted_len; /* includes the 4 byte header */
    uint16_t handle;
    const uint8_t *formatted; /* points at the header inside buf */
    const uint8_t *strings;   /* first byte after the formatted area */
    size_t strings_len;       /* bytes up to and including the double NUL */
};

void rewsr_dmi_cursor_init(struct rewsr_dmi_cursor *cur,
                           const uint8_t *buf, size_t len);

/*
 * Advance to the next structure. Returns 1 and fills *out when a
 * structure was produced, 0 at clean end of table (Type 127 or end of
 * buffer), negative errno on a malformed table (header shorter than 4,
 * formatted area or string set running past the buffer).
 */
int rewsr_dmi_next(struct rewsr_dmi_cursor *cur, struct rewsr_dmi_struct *out);

/*
 * Resolve a one-based string reference against a structure's string
 * set. Returns a pointer into the table buffer (valid as long as the
 * buffer is) or NULL when index is 0 or out of range. The returned
 * string is NUL-terminated by construction of the wire format.
 */
const char *rewsr_dmi_string(const struct rewsr_dmi_struct *st, uint8_t index);

/*
 * Parse a full structure table into *out. The buffer is only read.
 * Returns 0 on success (including tables that simply lack some types,
 * check the present flags), negative errno when the table is
 * structurally invalid. *out is fully zeroed first, so it is safe to
 * inspect even after a failure.
 */
int rewsr_dmi_parse(const uint8_t *buf, size_t len, struct rewsr_dmi_info *out);

/*
 * Entry point metadata. Handles both anchors:
 *   "_SM3_" 64-bit entry (SMBIOS 3.x): table max size u32 at 0x0C
 *   "_SM_"  32-bit entry (SMBIOS 2.x): table length u16 at 0x16
 * Verified with the spec checksum (all entry bytes sum to 0 mod 256).
 */
struct rewsr_dmi_entry {
    uint8_t major;
    uint8_t minor;
    int is_64bit;
    /* For _SM3_ this is the maximum table size, for _SM_ the exact
     * table length. Either way it bounds the DMI blob. */
    uint32_t table_len;
    uint64_t table_addr;
};

int rewsr_dmi_parse_entry_point(const uint8_t *buf, size_t len,
                                struct rewsr_dmi_entry *out);

/*
 * Helpers, exposed for tests and for callers that walk raw structures
 * themselves.
 */

/* Decode the Type 17 size word (+ extended size dword) into MB.
 * raw_size is the word at offset 0x0C, ext_size the dword at 0x1C
 * (pass 0 if the table is too short to carry it).
 *   returns 0            empty slot
 *           UINT64_MAX   firmware reports unknown
 *           otherwise    size in MB */
uint64_t rewsr_dmi_mem_size_mb(uint16_t raw_size, uint32_t ext_size);

/* Render the 16 raw UUID bytes in canonical 8-4-4-4-12 text form using
 * the SMBIOS >= 2.6 mixed-endian encoding. out must hold 37 bytes. */
void rewsr_dmi_format_uuid(const uint8_t raw[16], char out[37]);

/* Enum-to-name maps. Return a static string, "unknown (0xNN)" style
 * fallbacks come from an internal static buffer only when names is
 * NULL, so instead unknown values return NULL and the caller decides
 * how to render them. */
const char *rewsr_dmi_chassis_type_name(uint8_t type);
const char *rewsr_dmi_memory_type_name(uint8_t type);
const char *rewsr_dmi_form_factor_name(uint8_t ff);

/*
 * Linux only: slurp /sys/firmware/dmi/tables/DMI (relative to root,
 * pass "" for the real filesystem) and parse it. Returns 0 on
 * success, -ENOTSUP when built on a non-Linux platform, other
 * negative errno on read or parse failure.
 */
int rewsr_dmi_read_sysfs(const char *root, struct rewsr_dmi_info *out);

#ifdef __cplusplus
}
#endif

#endif /* REWSR_DMI_H */
