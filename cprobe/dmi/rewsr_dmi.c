/*
 * rewsr_dmi.c, portable SMBIOS/DMI structure table parser.
 *
 * Everything in this file operates on a caller-provided buffer and is
 * exercised by test/test_dmi.c against a hand-built table, so it runs
 * identically on the macOS dev box and on the Linux targets. The only
 * platform-bound piece (reading the blob out of sysfs) lives in
 * rewsr_dmi_linux.c.
 */
#include "rewsr_dmi.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Little-endian field readers. The table format is LE regardless of
 * host, and bytewise assembly also sidesteps alignment traps on
 * strict architectures (the formatted area is packed, words land on
 * odd offsets all the time).
 */
static uint16_t dmi_u16(const uint8_t *p) {
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t dmi_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t dmi_u64(const uint8_t *p) {
    return (uint64_t)dmi_u32(p) | ((uint64_t)dmi_u32(p + 4) << 32);
}

void rewsr_dmi_cursor_init(struct rewsr_dmi_cursor *cur,
                           const uint8_t *buf, size_t len) {
    cur->buf = buf;
    cur->len = len;
    cur->off = 0;
    cur->done = (buf == NULL || len == 0);
}

int rewsr_dmi_next(struct rewsr_dmi_cursor *cur,
                   struct rewsr_dmi_struct *out) {
    if (cur->done) {
        return 0;
    }
    /* A structure needs at least its 4 byte header plus the two NUL
     * bytes of an empty string set. Firmware pads the sysfs blob with
     * trailing zeros sometimes; a bare NUL run where a header should
     * be is treated as end of table rather than corruption. */
    if (cur->off + 4 > cur->len) {
        cur->done = 1;
        return 0;
    }
    const uint8_t *p = cur->buf + cur->off;
    uint8_t type = p[0];
    uint8_t flen = p[1];
    if (type == 0 && flen == 0) {
        /* Zero padding, not a real structure (real Type 0 has
         * length >= 0x12). */
        cur->done = 1;
        return 0;
    }
    if (flen < 4) {
        return -EPROTO;
    }
    if (cur->off + flen > cur->len) {
        return -EPROTO;
    }

    /* Scan the string set: it ends at the first double NUL at or after
     * the formatted area. An empty set is exactly "\0\0". A lone NUL
     * between two nonzero runs just separates adjacent strings. */
    size_t soff = cur->off + flen;
    size_t send = soff;
    int terminated = 0;
    while (send < cur->len) {
        if (cur->buf[send] != 0) {
            send++;
            continue;
        }
        if (send + 1 < cur->len && cur->buf[send + 1] == 0) {
            send += 2;
            terminated = 1;
            break;
        }
        if (send + 1 == cur->len) {
            /* Table ends exactly on the last structure's final NUL,
             * one terminator byte short. Some firmware emits this on
             * the very last structure; accept it and finish. */
            send += 1;
            terminated = 1;
            cur->done = 1;
            break;
        }
        send++;
    }
    if (!terminated) {
        return -EPROTO;
    }

    out->type = type;
    out->formatted_len = flen;
    out->handle = dmi_u16(p + 2);
    out->formatted = p;
    out->strings = cur->buf + cur->off + flen;
    out->strings_len = send - (cur->off + flen);

    cur->off = send;
    if (type == REWSR_DMI_TYPE_END) {
        cur->done = 1;
    }
    return 1;
}

const char *rewsr_dmi_string(const struct rewsr_dmi_struct *st,
                             uint8_t index) {
    if (index == 0 || st->strings_len < 2) {
        return NULL;
    }
    const uint8_t *p = st->strings;
    const uint8_t *end = st->strings + st->strings_len;
    uint8_t cur = 1;
    while (p < end && *p != 0) {
        if (cur == index) {
            return (const char *)p;
        }
        /* Skip to the byte after this string's NUL. */
        while (p < end && *p != 0) {
            p++;
        }
        if (p < end) {
            p++;
        }
        cur++;
    }
    return NULL;
}

/*
 * Copy a resolved string reference into a fixed field. Missing
 * references (index 0, dangling index) become the empty string, which
 * downstream code treats as "not reported". Also trims trailing
 * spaces: DMI strings are routinely space-padded by BIOS vendors
 * ("HPE                 " is a classic) and the padding is never
 * signal.
 */
static void dmi_copy_string(char *dst, size_t cap,
                            const struct rewsr_dmi_struct *st,
                            const uint8_t *field) {
    dst[0] = '\0';
    if (field == NULL) {
        return;
    }
    const char *s = rewsr_dmi_string(st, *field);
    if (s == NULL) {
        return;
    }
    size_t n = strlen(s);
    if (n >= cap) {
        n = cap - 1;
    }
    while (n > 0 && s[n - 1] == ' ') {
        n--;
    }
    memcpy(dst, s, n);
    dst[n] = '\0';
}

/* Formatted-area accessor: NULL when the structure is too short to
 * carry the field, so pre-3.0 tables with shorter Type 4/17 records
 * decode as far as they go instead of reading garbage. */
static const uint8_t *dmi_field(const struct rewsr_dmi_struct *st,
                                size_t off, size_t width) {
    if (off + width > st->formatted_len) {
        return NULL;
    }
    return st->formatted + off;
}

static uint16_t dmi_field_u16(const struct rewsr_dmi_struct *st, size_t off,
                              uint16_t fallback) {
    const uint8_t *p = dmi_field(st, off, 2);
    return p ? dmi_u16(p) : fallback;
}

static uint8_t dmi_field_u8(const struct rewsr_dmi_struct *st, size_t off,
                            uint8_t fallback) {
    const uint8_t *p = dmi_field(st, off, 1);
    return p ? *p : fallback;
}

uint64_t rewsr_dmi_mem_size_mb(uint16_t raw_size, uint32_t ext_size) {
    if (raw_size == 0) {
        return 0; /* slot present, nothing installed */
    }
    if (raw_size == 0xFFFF) {
        return UINT64_MAX; /* installed, size unknown to firmware */
    }
    if (raw_size == 0x7FFF) {
        /* Size >= 32GB - 1MB, real size is the extended dword,
         * bits 30:0, always in MB. */
        return (uint64_t)(ext_size & 0x7FFFFFFFu);
    }
    if (raw_size & 0x8000) {
        /* Bit 15 set: value is in KB. Round up so a 512KB DIMM (never
         * seen on our fleet, but the spec allows it) does not report
         * as zero MB. */
        uint32_t kb = raw_size & 0x7FFF;
        return (kb + 1023) / 1024;
    }
    return raw_size; /* already MB */
}

void rewsr_dmi_format_uuid(const uint8_t raw[16], char out[37]) {
    /* Mixed-endian per SMBIOS >= 2.6: time_low, time_mid, time_hi are
     * little-endian on the wire, clock_seq and node are big-endian
     * (byte order as written). dmidecode does the same swap. */
    snprintf(out, 37,
             "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
             "%02x%02x%02x%02x%02x%02x",
             raw[3], raw[2], raw[1], raw[0],
             raw[5], raw[4],
             raw[7], raw[6],
             raw[8], raw[9],
             raw[10], raw[11], raw[12], raw[13], raw[14], raw[15]);
}

static void dmi_decode_bios(const struct rewsr_dmi_struct *st,
                            struct rewsr_dmi_bios *b) {
    b->present = 1;
    dmi_copy_string(b->vendor, sizeof(b->vendor), st, dmi_field(st, 0x04, 1));
    dmi_copy_string(b->version, sizeof(b->version), st,
                    dmi_field(st, 0x05, 1));
    dmi_copy_string(b->release_date, sizeof(b->release_date), st,
                    dmi_field(st, 0x08, 1));
    uint8_t rom = dmi_field_u8(st, 0x09, 0xFF);
    /* ROM size byte n encodes 64K * (n + 1); 0xFF means the extended
     * field holds it and we do not carry that for inventory. */
    b->rom_kb = (rom == 0xFF) ? 0 : (uint32_t)(rom + 1) * 64;
    b->major_release = dmi_field_u8(st, 0x14, 0xFF);
    b->minor_release = dmi_field_u8(st, 0x15, 0xFF);
}

static void dmi_decode_system(const struct rewsr_dmi_struct *st,
                              struct rewsr_dmi_system *s) {
    s->present = 1;
    dmi_copy_string(s->manufacturer, sizeof(s->manufacturer), st,
                    dmi_field(st, 0x04, 1));
    dmi_copy_string(s->product, sizeof(s->product), st,
                    dmi_field(st, 0x05, 1));
    dmi_copy_string(s->version, sizeof(s->version), st,
                    dmi_field(st, 0x06, 1));
    dmi_copy_string(s->serial, sizeof(s->serial), st,
                    dmi_field(st, 0x07, 1));
    dmi_copy_string(s->sku, sizeof(s->sku), st, dmi_field(st, 0x19, 1));
    dmi_copy_string(s->family, sizeof(s->family), st,
                    dmi_field(st, 0x1A, 1));

    const uint8_t *uuid = dmi_field(st, 0x08, 16);
    if (uuid != NULL) {
        memcpy(s->uuid_raw, uuid, 16);
        int all0 = 1, allf = 1;
        for (int i = 0; i < 16; i++) {
            if (uuid[i] != 0x00) all0 = 0;
            if (uuid[i] != 0xFF) allf = 0;
        }
        /* The spec reserves all-zero (not set) and all-ones (set but
         * not readable) as non-values. */
        s->uuid_present = !(all0 || allf);
        if (s->uuid_present) {
            rewsr_dmi_format_uuid(uuid, s->uuid);
        }
    }
}

static void dmi_decode_baseboard(const struct rewsr_dmi_struct *st,
                                 struct rewsr_dmi_baseboard *b) {
    b->present = 1;
    dmi_copy_string(b->manufacturer, sizeof(b->manufacturer), st,
                    dmi_field(st, 0x04, 1));
    dmi_copy_string(b->product, sizeof(b->product), st,
                    dmi_field(st, 0x05, 1));
    dmi_copy_string(b->version, sizeof(b->version), st,
                    dmi_field(st, 0x06, 1));
    dmi_copy_string(b->serial, sizeof(b->serial), st,
                    dmi_field(st, 0x07, 1));
    dmi_copy_string(b->asset_tag, sizeof(b->asset_tag), st,
                    dmi_field(st, 0x08, 1));
}

static void dmi_decode_chassis(const struct rewsr_dmi_struct *st,
                               struct rewsr_dmi_chassis *c) {
    c->present = 1;
    dmi_copy_string(c->manufacturer, sizeof(c->manufacturer), st,
                    dmi_field(st, 0x04, 1));
    uint8_t raw = dmi_field_u8(st, 0x05, 0x02 /* Unknown */);
    c->type = raw & 0x7F;
    c->has_lock = (raw & 0x80) != 0;
    dmi_copy_string(c->version, sizeof(c->version), st,
                    dmi_field(st, 0x06, 1));
    dmi_copy_string(c->serial, sizeof(c->serial), st,
                    dmi_field(st, 0x07, 1));
    dmi_copy_string(c->asset_tag, sizeof(c->asset_tag), st,
                    dmi_field(st, 0x08, 1));
}

static void dmi_decode_processor(const struct rewsr_dmi_struct *st,
                                 struct rewsr_dmi_processor *p) {
    p->handle = st->handle;
    dmi_copy_string(p->socket, sizeof(p->socket), st,
                    dmi_field(st, 0x04, 1));
    dmi_copy_string(p->manufacturer, sizeof(p->manufacturer), st,
                    dmi_field(st, 0x07, 1));
    dmi_copy_string(p->version, sizeof(p->version), st,
                    dmi_field(st, 0x10, 1));
    const uint8_t *id = dmi_field(st, 0x08, 8);
    p->id = id ? dmi_u64(id) : 0;
    p->max_speed_mhz = dmi_field_u16(st, 0x14, 0);
    p->current_speed_mhz = dmi_field_u16(st, 0x16, 0);
    /* Status byte, bit 6 = CPU socket populated. Structures old
     * enough to lack it are from populated sockets in practice. */
    uint8_t status = dmi_field_u8(st, 0x18, 0x40);
    p->populated = (status & 0x40) != 0;

    /* SMBIOS 2.5 added the byte counts, 3.0 added the word-sized
     * Count 2 fields for parts with more than 254 cores. A byte value
     * of 0xFF is the escape saying "use Count 2". */
    uint8_t cores8 = dmi_field_u8(st, 0x23, 0);
    uint8_t enabled8 = dmi_field_u8(st, 0x24, 0);
    uint8_t threads8 = dmi_field_u8(st, 0x25, 0);
    p->core_count = (cores8 == 0xFF) ? dmi_field_u16(st, 0x2A, 0) : cores8;
    p->cores_enabled =
        (enabled8 == 0xFF) ? dmi_field_u16(st, 0x2C, 0) : enabled8;
    p->thread_count =
        (threads8 == 0xFF) ? dmi_field_u16(st, 0x2E, 0) : threads8;
}

static void dmi_decode_memdev(const struct rewsr_dmi_struct *st,
                              struct rewsr_dmi_memdev *m) {
    m->handle = st->handle;
    uint16_t raw_size = dmi_field_u16(st, 0x0C, 0);
    const uint8_t *ext = dmi_field(st, 0x1C, 4);
    m->size_mb = rewsr_dmi_mem_size_mb(raw_size, ext ? dmi_u32(ext) : 0);
    m->installed = raw_size != 0;

    uint16_t tw = dmi_field_u16(st, 0x08, 0xFFFF);
    uint16_t dw = dmi_field_u16(st, 0x0A, 0xFFFF);
    m->total_width_bits = (tw == 0xFFFF) ? 0 : tw;
    m->data_width_bits = (dw == 0xFFFF) ? 0 : dw;

    m->form_factor = dmi_field_u8(st, 0x0E, 0x02 /* Unknown */);
    m->memory_type = dmi_field_u8(st, 0x12, 0x02 /* Unknown */);
    m->speed_mts = dmi_field_u16(st, 0x15, 0);
    m->configured_speed_mts = dmi_field_u16(st, 0x20, 0);
    m->rank = dmi_field_u8(st, 0x1B, 0) & 0x0F;

    dmi_copy_string(m->device_locator, sizeof(m->device_locator), st,
                    dmi_field(st, 0x10, 1));
    dmi_copy_string(m->bank_locator, sizeof(m->bank_locator), st,
                    dmi_field(st, 0x11, 1));
    dmi_copy_string(m->manufacturer, sizeof(m->manufacturer), st,
                    dmi_field(st, 0x17, 1));
    dmi_copy_string(m->serial, sizeof(m->serial), st,
                    dmi_field(st, 0x18, 1));
    dmi_copy_string(m->part_number, sizeof(m->part_number), st,
                    dmi_field(st, 0x1A, 1));
}

int rewsr_dmi_parse(const uint8_t *buf, size_t len,
                    struct rewsr_dmi_info *out) {
    memset(out, 0, sizeof(*out));
    if (buf == NULL || len < 6) {
        return -EINVAL;
    }

    struct rewsr_dmi_cursor cur;
    struct rewsr_dmi_struct st;
    rewsr_dmi_cursor_init(&cur, buf, len);

    int rc;
    while ((rc = rewsr_dmi_next(&cur, &st)) == 1) {
        out->structure_count++;
        switch (st.type) {
        case REWSR_DMI_TYPE_BIOS:
            /* First one wins; duplicate Type 0 records exist on some
             * multi-node firmware and the first is the boot BIOS. */
            if (!out->bios.present) {
                dmi_decode_bios(&st, &out->bios);
            }
            break;
        case REWSR_DMI_TYPE_SYSTEM:
            if (!out->system.present) {
                dmi_decode_system(&st, &out->system);
            }
            break;
        case REWSR_DMI_TYPE_BASEBOARD:
            if (!out->baseboard.present) {
                dmi_decode_baseboard(&st, &out->baseboard);
            }
            break;
        case REWSR_DMI_TYPE_CHASSIS:
            if (!out->chassis.present) {
                dmi_decode_chassis(&st, &out->chassis);
            }
            break;
        case REWSR_DMI_TYPE_PROCESSOR:
            if (out->processor_count < REWSR_DMI_MAX_PROCESSORS) {
                dmi_decode_processor(
                    &st, &out->processors[out->processor_count++]);
            } else {
                out->processors_dropped++;
            }
            break;
        case REWSR_DMI_TYPE_MEMDEV:
            if (out->memdev_count < REWSR_DMI_MAX_MEMDEVS) {
                dmi_decode_memdev(&st, &out->memdevs[out->memdev_count++]);
            } else {
                out->memdevs_dropped++;
            }
            break;
        default:
            break;
        }
    }
    return rc < 0 ? rc : 0;
}

int rewsr_dmi_parse_entry_point(const uint8_t *buf, size_t len,
                                struct rewsr_dmi_entry *out) {
    memset(out, 0, sizeof(*out));
    if (buf == NULL || len < 5) {
        return -EINVAL;
    }
    if (memcmp(buf, "_SM3_", 5) == 0) {
        /* SMBIOS 3.x 64-bit entry point, 0x18 bytes:
         *   0x00 anchor "_SM3_"
         *   0x05 checksum
         *   0x06 entry length (0x18)
         *   0x07 major, 0x08 minor, 0x09 docrev
         *   0x0A entry point revision (0x01)
         *   0x0C table maximum size, u32 LE
         *   0x10 table address, u64 LE */
        if (len < 0x18) {
            return -EPROTO;
        }
        uint8_t elen = buf[6];
        if (elen < 0x18 || elen > len) {
            return -EPROTO;
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < elen; i++) {
            sum = (uint8_t)(sum + buf[i]);
        }
        if (sum != 0) {
            return -EPROTO;
        }
        out->is_64bit = 1;
        out->major = buf[7];
        out->minor = buf[8];
        out->table_len = dmi_u32(buf + 0x0C);
        out->table_addr = dmi_u64(buf + 0x10);
        return 0;
    }
    if (memcmp(buf, "_SM_", 4) == 0) {
        /* SMBIOS 2.x 32-bit entry point, 0x1F bytes:
         *   0x00 anchor "_SM_"
         *   0x04 checksum (covers the full 0x1F)
         *   0x05 entry length (0x1F)
         *   0x06 major, 0x07 minor
         *   0x10 intermediate anchor "_DMI_"
         *   0x15 intermediate checksum
         *   0x16 structure table length, u16 LE
         *   0x18 structure table address, u32 LE
         *   0x1C number of structures, u16 LE */
        if (len < 0x1F) {
            return -EPROTO;
        }
        uint8_t elen = buf[5];
        if (elen < 0x1F || elen > len) {
            return -EPROTO;
        }
        uint8_t sum = 0;
        for (size_t i = 0; i < elen; i++) {
            sum = (uint8_t)(sum + buf[i]);
        }
        if (sum != 0) {
            return -EPROTO;
        }
        if (memcmp(buf + 0x10, "_DMI_", 5) != 0) {
            return -EPROTO;
        }
        out->is_64bit = 0;
        out->major = buf[6];
        out->minor = buf[7];
        out->table_len = dmi_u16(buf + 0x16);
        out->table_addr = dmi_u32(buf + 0x18);
        return 0;
    }
    return -EPROTO;
}

const char *rewsr_dmi_chassis_type_name(uint8_t type) {
    /* SMBIOS spec table 17, System Enclosure or Chassis Types. */
    switch (type) {
    case 0x01: return "Other";
    case 0x02: return "Unknown";
    case 0x03: return "Desktop";
    case 0x04: return "Low Profile Desktop";
    case 0x05: return "Pizza Box";
    case 0x06: return "Mini Tower";
    case 0x07: return "Tower";
    case 0x08: return "Portable";
    case 0x09: return "Laptop";
    case 0x0A: return "Notebook";
    case 0x0B: return "Hand Held";
    case 0x0C: return "Docking Station";
    case 0x0D: return "All In One";
    case 0x0E: return "Sub Notebook";
    case 0x0F: return "Space-saving";
    case 0x10: return "Lunch Box";
    case 0x11: return "Main Server Chassis";
    case 0x12: return "Expansion Chassis";
    case 0x13: return "SubChassis";
    case 0x14: return "Bus Expansion Chassis";
    case 0x15: return "Peripheral Chassis";
    case 0x16: return "RAID Chassis";
    case 0x17: return "Rack Mount Chassis";
    case 0x18: return "Sealed-case PC";
    case 0x19: return "Multi-system Chassis";
    case 0x1A: return "Compact PCI";
    case 0x1B: return "Advanced TCA";
    case 0x1C: return "Blade";
    case 0x1D: return "Blade Enclosure";
    case 0x1E: return "Tablet";
    case 0x1F: return "Convertible";
    case 0x20: return "Detachable";
    case 0x21: return "IoT Gateway";
    case 0x22: return "Embedded PC";
    case 0x23: return "Mini PC";
    case 0x24: return "Stick PC";
    default:   return NULL;
    }
}

const char *rewsr_dmi_memory_type_name(uint8_t type) {
    /* SMBIOS spec table 77, Memory Device Type. Only entries a server
     * fleet can plausibly report; NULL for the rest. */
    switch (type) {
    case 0x01: return "Other";
    case 0x02: return "Unknown";
    case 0x03: return "DRAM";
    case 0x07: return "RAM";
    case 0x08: return "ROM";
    case 0x09: return "Flash";
    case 0x0F: return "SDRAM";
    case 0x12: return "DDR";
    case 0x13: return "DDR2";
    case 0x14: return "DDR2 FB-DIMM";
    case 0x18: return "DDR3";
    case 0x19: return "FBD2";
    case 0x1A: return "DDR4";
    case 0x1B: return "LPDDR";
    case 0x1C: return "LPDDR2";
    case 0x1D: return "LPDDR3";
    case 0x1E: return "LPDDR4";
    case 0x1F: return "Logical non-volatile device";
    case 0x20: return "HBM";
    case 0x21: return "HBM2";
    case 0x22: return "DDR5";
    case 0x23: return "LPDDR5";
    case 0x24: return "HBM3";
    default:   return NULL;
    }
}

const char *rewsr_dmi_form_factor_name(uint8_t ff) {
    /* SMBIOS spec table 76, Memory Device Form Factor. */
    switch (ff) {
    case 0x01: return "Other";
    case 0x02: return "Unknown";
    case 0x03: return "SIMM";
    case 0x04: return "SIP";
    case 0x05: return "Chip";
    case 0x06: return "DIP";
    case 0x07: return "ZIP";
    case 0x08: return "Proprietary Card";
    case 0x09: return "DIMM";
    case 0x0A: return "TSOP";
    case 0x0B: return "Row of chips";
    case 0x0C: return "RIMM";
    case 0x0D: return "SODIMM";
    case 0x0E: return "SRIMM";
    case 0x0F: return "FB-DIMM";
    case 0x10: return "Die";
    case 0x11: return "CAMM";
    default:   return NULL;
    }
}
