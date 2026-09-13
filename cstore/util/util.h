/* util.h: shared sysfs/procfs plumbing for cstore modules.
 *
 * Everything here is portable. Readers take caller-supplied paths, so a
 * test can point them at a fixture tree instead of a live /sys or /proc.
 * Error convention across cstore: 0 or a positive count on success,
 * negative errno on failure.
 */
#ifndef REWSR_CSTORE_UTIL_H
#define REWSR_CSTORE_UTIL_H

#include <stddef.h>
#include <stdint.h>

#define REWSR_UTIL_NAME_MAX 64
#define REWSR_UTIL_PATH_MAX 1024

/* Join root + "/" + rel into out. Returns 0 or -ENAMETOOLONG. */
int rewsr_path_join(char *out, size_t cap, const char *root, const char *rel);

/* Copy src into dst, always NUL-terminating. Returns 0, or -ENAMETOOLONG
 * if src was truncated to fit. */
int rewsr_strlcpy(char *dst, size_t cap, const char *src);

/* Trim leading and trailing ASCII whitespace in place. Returns s. */
char *rewsr_trim(char *s);

/* Read at most cap-1 bytes of the file into buf and NUL-terminate.
 * Larger files are silently truncated: sysfs attributes are one page.
 * Returns byte count or -errno. */
int rewsr_read_file(const char *path, char *buf, size_t cap);

/* Read the whole file through a growing heap buffer. On success *out is a
 * NUL-terminated malloc'd buffer the caller frees and *out_len its length.
 * Needed for /proc/mounts and friends, which exceed fixed guesses on
 * overlay-heavy hosts. Returns 0 or -errno. */
int rewsr_slurp(const char *path, char **out, size_t *out_len);

/* Read root/rel and trim surrounding whitespace, including the trailing
 * newline every sysfs attribute carries. Returns trimmed length or -errno. */
int rewsr_read_attr(const char *root, const char *rel, char *buf, size_t cap);

/* Strict decimal parsers. Leading spaces and trailing whitespace are
 * accepted, anything else is -EINVAL; overflow is -ERANGE. */
int rewsr_parse_u64(const char *s, uint64_t *out);
int rewsr_parse_i64(const char *s, int64_t *out);
int rewsr_parse_u32(const char *s, uint32_t *out);

/* read_attr + parse in one step. */
int rewsr_read_attr_u64(const char *root, const char *rel, uint64_t *out);
int rewsr_read_attr_i64(const char *root, const char *rel, int64_t *out);
int rewsr_read_attr_u32(const char *root, const char *rel, uint32_t *out);

/* Sorted directory listing, skipping "." and "..". Stores the first
 * min(total, cap) names and returns the total number of entries seen, so
 * a return greater than cap means the caller's array was too small.
 * Entries longer than REWSR_UTIL_NAME_MAX-1 are counted but not stored. */
int rewsr_list_dir(const char *path, char (*names)[REWSR_UTIL_NAME_MAX],
                   size_t cap);

/* Nonzero if s is non-empty and all decimal digits. */
int rewsr_all_digits(const char *s);

/* Nonzero if root/rel exists and is a directory. */
int rewsr_dir_exists(const char *root, const char *rel);

#endif
