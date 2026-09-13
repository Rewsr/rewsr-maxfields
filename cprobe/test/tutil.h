/*
 * tutil.h, fixture helpers shared by the cprobe tests.
 *
 * The sysfs and pci probes take a root prefix precisely so tests can
 * point them at a throwaway directory tree instead of a live /sys.
 * These helpers build that tree: mkdtemp a root, then drop files at
 * relative paths, creating intermediate directories as needed.
 * POSIX only (mkdtemp, mkdir), which covers macOS and Linux.
 */
#ifndef REWSR_TUTIL_H
#define REWSR_TUTIL_H

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

/*
 * Create a unique scratch directory under TMPDIR (or /tmp). Returns 0
 * and fills out_path (must hold at least 256 bytes) on success,
 * negative errno on failure. Caller cleans up with
 * rewsr_tutil_rmtree, or leaves it for the OS tmp reaper.
 */
static inline int rewsr_tutil_mkroot(char *out_path, size_t cap) {
    const char *tmp = getenv("TMPDIR");
    if (tmp == NULL || tmp[0] == '\0') {
        tmp = "/tmp";
    }
    int n = snprintf(out_path, cap, "%s/rewsr-cprobe-XXXXXX", tmp);
    if (n < 0 || (size_t)n >= cap) {
        return -ENAMETOOLONG;
    }
    if (mkdtemp(out_path) == NULL) {
        return -errno;
    }
    return 0;
}

/*
 * mkdir -p for every directory component of path (the final component
 * is treated as a file name and not created). Ignores EEXIST so it is
 * safe to call repeatedly while laying out a fixture tree.
 */
static inline int rewsr_tutil_mkparents(const char *path) {
    char buf[1024];
    size_t len = strlen(path);
    if (len >= sizeof(buf)) {
        return -ENAMETOOLONG;
    }
    memcpy(buf, path, len + 1);
    for (size_t i = 1; i < len; i++) {
        if (buf[i] != '/') {
            continue;
        }
        buf[i] = '\0';
        if (mkdir(buf, 0755) != 0 && errno != EEXIST) {
            return -errno;
        }
        buf[i] = '/';
    }
    return 0;
}

/*
 * Write contents (a NUL-terminated string, written without the NUL)
 * to root/relpath, creating parent directories. Returns 0 or negative
 * errno.
 */
static inline int rewsr_tutil_write_file(const char *root,
                                         const char *relpath,
                                         const char *contents) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int rc = rewsr_tutil_mkparents(path);
    if (rc != 0) {
        return rc;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -errno;
    }
    size_t len = strlen(contents);
    if (len > 0 && fwrite(contents, 1, len, f) != len) {
        int saved = errno ? errno : EIO;
        fclose(f);
        return -saved;
    }
    if (fclose(f) != 0) {
        return -errno;
    }
    return 0;
}

/* Binary variant for fixtures like a raw DMI blob. */
static inline int rewsr_tutil_write_bytes(const char *root,
                                          const char *relpath,
                                          const void *data, size_t len) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int rc = rewsr_tutil_mkparents(path);
    if (rc != 0) {
        return rc;
    }
    FILE *f = fopen(path, "wb");
    if (f == NULL) {
        return -errno;
    }
    if (len > 0 && fwrite(data, 1, len, f) != len) {
        int saved = errno ? errno : EIO;
        fclose(f);
        return -saved;
    }
    if (fclose(f) != 0) {
        return -errno;
    }
    return 0;
}

/*
 * Create an empty directory at root/relpath (plus parents). Needed for
 * fixture net interfaces that have a directory but no populated files.
 */
static inline int rewsr_tutil_mkdir(const char *root, const char *relpath) {
    char path[1024];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relpath);
    if (n < 0 || (size_t)n >= sizeof(path)) {
        return -ENAMETOOLONG;
    }
    int rc = rewsr_tutil_mkparents(path);
    if (rc != 0) {
        return rc;
    }
    if (mkdir(path, 0755) != 0 && errno != EEXIST) {
        return -errno;
    }
    return 0;
}

/*
 * Best-effort recursive delete of a fixture tree. Shells out to rm -rf
 * would be simpler but keeps a subprocess out of the tests; a bounded
 * manual walk is enough for the shallow trees we build. Returns 0 on
 * success, negative errno on first failure.
 */
static inline int rewsr_tutil_rmtree(const char *path);

#include <dirent.h>

static inline int rewsr_tutil_rmtree(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        return errno == ENOENT ? 0 : -errno;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(path) == 0 ? 0 : -errno;
    }
    DIR *d = opendir(path);
    if (d == NULL) {
        return -errno;
    }
    struct dirent *ent;
    int rc = 0;
    while (rc == 0 && (ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        char child[1024];
        int n = snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (n < 0 || (size_t)n >= sizeof(child)) {
            rc = -ENAMETOOLONG;
            break;
        }
        rc = rewsr_tutil_rmtree(child);
    }
    closedir(d);
    if (rc != 0) {
        return rc;
    }
    return rmdir(path) == 0 ? 0 : -errno;
}

#endif /* REWSR_TUTIL_H */
