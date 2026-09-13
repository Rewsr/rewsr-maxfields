#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include "util.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

int rewsr_path_join(char *out, size_t cap, const char *root, const char *rel)
{
    int n;

    if (out == NULL || root == NULL || rel == NULL || cap == 0)
        return -EINVAL;
    n = snprintf(out, cap, "%s/%s", root, rel);
    if (n < 0 || (size_t)n >= cap)
        return -ENAMETOOLONG;
    return 0;
}

int rewsr_strlcpy(char *dst, size_t cap, const char *src)
{
    size_t n;

    if (dst == NULL || src == NULL || cap == 0)
        return -EINVAL;
    n = strlen(src);
    if (n >= cap) {
        memcpy(dst, src, cap - 1);
        dst[cap - 1] = '\0';
        return -ENAMETOOLONG;
    }
    memcpy(dst, src, n + 1);
    return 0;
}

static int is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\v' || c == '\f';
}

char *rewsr_trim(char *s)
{
    size_t n;
    char *p;

    if (s == NULL)
        return NULL;
    p = s;
    while (is_space(*p))
        p++;
    n = strlen(p);
    while (n > 0 && is_space(p[n - 1]))
        n--;
    if (p != s)
        memmove(s, p, n);
    s[n] = '\0';
    return s;
}

int rewsr_read_file(const char *path, char *buf, size_t cap)
{
    int fd;
    size_t off = 0;

    if (path == NULL || buf == NULL || cap == 0)
        return -EINVAL;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    while (off < cap - 1) {
        ssize_t n = read(fd, buf + off, cap - 1 - off);

        if (n < 0) {
            int e = errno;

            if (e == EINTR)
                continue;
            close(fd);
            return -e;
        }
        if (n == 0)
            break;
        off += (size_t)n;
    }
    close(fd);
    buf[off] = '\0';
    return (int)off;
}

int rewsr_slurp(const char *path, char **out, size_t *out_len)
{
    int fd;
    char *buf;
    size_t cap = 4096;
    size_t len = 0;

    if (path == NULL || out == NULL || out_len == NULL)
        return -EINVAL;
    fd = open(path, O_RDONLY);
    if (fd < 0)
        return -errno;
    buf = malloc(cap);
    if (buf == NULL) {
        close(fd);
        return -ENOMEM;
    }
    for (;;) {
        ssize_t n;

        if (len + 1 >= cap) {
            char *grown;

            cap *= 2;
            grown = realloc(buf, cap);
            if (grown == NULL) {
                free(buf);
                close(fd);
                return -ENOMEM;
            }
            buf = grown;
        }
        n = read(fd, buf + len, cap - len - 1);
        if (n < 0) {
            int e = errno;

            if (e == EINTR)
                continue;
            free(buf);
            close(fd);
            return -e;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }
    close(fd);
    buf[len] = '\0';
    *out = buf;
    *out_len = len;
    return 0;
}

int rewsr_read_attr(const char *root, const char *rel, char *buf, size_t cap)
{
    char path[REWSR_UTIL_PATH_MAX];
    int rc;

    rc = rewsr_path_join(path, sizeof(path), root, rel);
    if (rc != 0)
        return rc;
    rc = rewsr_read_file(path, buf, cap);
    if (rc < 0)
        return rc;
    rewsr_trim(buf);
    return (int)strlen(buf);
}

int rewsr_parse_u64(const char *s, uint64_t *out)
{
    unsigned long long v;
    char *end;

    if (s == NULL || out == NULL)
        return -EINVAL;
    while (*s == ' ' || *s == '\t')
        s++;
    if (*s < '0' || *s > '9')
        return -EINVAL;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno == ERANGE)
        return -ERANGE;
    while (is_space(*end))
        end++;
    if (*end != '\0')
        return -EINVAL;
    *out = (uint64_t)v;
    return 0;
}

int rewsr_parse_i64(const char *s, int64_t *out)
{
    long long v;
    char *end;
    const char *digits;

    if (s == NULL || out == NULL)
        return -EINVAL;
    while (*s == ' ' || *s == '\t')
        s++;
    digits = (*s == '-') ? s + 1 : s;
    if (*digits < '0' || *digits > '9')
        return -EINVAL;
    errno = 0;
    v = strtoll(s, &end, 10);
    if (errno == ERANGE)
        return -ERANGE;
    while (is_space(*end))
        end++;
    if (*end != '\0')
        return -EINVAL;
    *out = (int64_t)v;
    return 0;
}

int rewsr_parse_u32(const char *s, uint32_t *out)
{
    uint64_t v;
    int rc;

    if (out == NULL)
        return -EINVAL;
    rc = rewsr_parse_u64(s, &v);
    if (rc != 0)
        return rc;
    if (v > 0xffffffffULL)
        return -ERANGE;
    *out = (uint32_t)v;
    return 0;
}

int rewsr_read_attr_u64(const char *root, const char *rel, uint64_t *out)
{
    char buf[64];
    int rc;

    rc = rewsr_read_attr(root, rel, buf, sizeof(buf));
    if (rc < 0)
        return rc;
    return rewsr_parse_u64(buf, out);
}

int rewsr_read_attr_i64(const char *root, const char *rel, int64_t *out)
{
    char buf[64];
    int rc;

    rc = rewsr_read_attr(root, rel, buf, sizeof(buf));
    if (rc < 0)
        return rc;
    return rewsr_parse_i64(buf, out);
}

int rewsr_read_attr_u32(const char *root, const char *rel, uint32_t *out)
{
    char buf[64];
    int rc;

    rc = rewsr_read_attr(root, rel, buf, sizeof(buf));
    if (rc < 0)
        return rc;
    return rewsr_parse_u32(buf, out);
}

static int name_cmp(const void *a, const void *b)
{
    return strcmp((const char *)a, (const char *)b);
}

int rewsr_list_dir(const char *path, char (*names)[REWSR_UTIL_NAME_MAX],
                   size_t cap)
{
    DIR *d;
    struct dirent *de;
    size_t stored = 0;
    size_t total = 0;

    if (path == NULL || (names == NULL && cap > 0))
        return -EINVAL;
    d = opendir(path);
    if (d == NULL)
        return -errno;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        total++;
        if (stored < cap &&
            rewsr_strlcpy(names[stored], REWSR_UTIL_NAME_MAX, de->d_name) == 0)
            stored++;
    }
    closedir(d);
    if (stored > 1)
        qsort(names, stored, REWSR_UTIL_NAME_MAX, name_cmp);
    if (total > (size_t)0x7fffffff)
        return -EOVERFLOW;
    return (int)total;
}

int rewsr_all_digits(const char *s)
{
    if (s == NULL || *s == '\0')
        return 0;
    for (; *s; s++) {
        if (*s < '0' || *s > '9')
            return 0;
    }
    return 1;
}

int rewsr_dir_exists(const char *root, const char *rel)
{
    char path[REWSR_UTIL_PATH_MAX];
    struct stat st;

    if (rewsr_path_join(path, sizeof(path), root, rel) != 0)
        return 0;
    if (stat(path, &st) != 0)
        return 0;
    return S_ISDIR(st.st_mode);
}
