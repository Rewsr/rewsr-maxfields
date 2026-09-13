/* fixture.h: temp-dir sysfs/procfs fixture builder for cstore tests.
 *
 * Include this header before any system header in the test file: on Linux
 * it must define feature macros first. Tests build a throwaway directory
 * tree that stands in for /sys or /proc, then hand its path to the
 * root-prefix readers.
 */
#ifndef REWSR_FIXTURE_H
#define REWSR_FIXTURE_H

#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define FX_PATH_MAX 1024

/* Create a fresh temp dir under $TMPDIR (or /tmp) and store its absolute
 * path in buf. Returns 0 or -errno. */
static inline int fx_mkroot(char *buf, size_t cap)
{
    char tmpl[FX_PATH_MAX];
    const char *tmp = getenv("TMPDIR");
    size_t n;
    int sep;

    if (buf == NULL || cap == 0)
        return -EINVAL;
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    n = strlen(tmp);
    sep = (n > 0 && tmp[n - 1] == '/') ? 0 : 1;
    if (snprintf(tmpl, sizeof(tmpl), "%s%scstore.XXXXXX",
                 tmp, sep ? "/" : "") >= (int)sizeof(tmpl))
        return -ENAMETOOLONG;
    if (mkdtemp(tmpl) == NULL)
        return -errno;
    if (strlen(tmpl) + 1 > cap) {
        rmdir(tmpl);
        return -ENAMETOOLONG;
    }
    strcpy(buf, tmpl);
    return 0;
}

/* Write content to root/relpath, creating intermediate directories.
 * relpath uses forward slashes and must be relative. Returns 0 or -errno. */
static inline int fx_write(const char *root, const char *relpath,
                           const char *content)
{
    char path[FX_PATH_MAX];
    size_t i, rootlen;
    FILE *f;

    if (root == NULL || relpath == NULL || content == NULL)
        return -EINVAL;
    if (relpath[0] == '/' || relpath[0] == '\0')
        return -EINVAL;
    if (snprintf(path, sizeof(path), "%s/%s", root, relpath) >=
        (int)sizeof(path))
        return -ENAMETOOLONG;

    rootlen = strlen(root);
    for (i = rootlen + 1; path[i] != '\0'; i++) {
        if (path[i] != '/')
            continue;
        path[i] = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST)
            return -errno;
        path[i] = '/';
    }

    f = fopen(path, "w");
    if (f == NULL)
        return -errno;
    if (fputs(content, f) < 0) {
        fclose(f);
        return -EIO;
    }
    if (fclose(f) != 0)
        return -errno;
    return 0;
}

/* Create an empty directory at root/relpath (with parents). 0 or -errno. */
static inline int fx_mkdir(const char *root, const char *relpath)
{
    char path[FX_PATH_MAX];
    size_t i, rootlen;

    if (root == NULL || relpath == NULL)
        return -EINVAL;
    if (relpath[0] == '/' || relpath[0] == '\0')
        return -EINVAL;
    if (snprintf(path, sizeof(path), "%s/%s", root, relpath) >=
        (int)sizeof(path))
        return -ENAMETOOLONG;

    rootlen = strlen(root);
    for (i = rootlen + 1; ; i++) {
        if (path[i] != '/' && path[i] != '\0')
            continue;
        if (path[i] == '\0') {
            if (mkdir(path, 0755) != 0 && errno != EEXIST)
                return -errno;
            break;
        }
        path[i] = '\0';
        if (mkdir(path, 0755) != 0 && errno != EEXIST)
            return -errno;
        path[i] = '/';
    }
    return 0;
}

/* Recursive delete rooted at path. Symlinks are not followed.
 * Missing path counts as success. Returns 0 or -errno. */
static inline int fx_rmtree(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *de;

    if (path == NULL || path[0] == '\0')
        return -EINVAL;
    if (lstat(path, &st) != 0)
        return errno == ENOENT ? 0 : -errno;
    if (!S_ISDIR(st.st_mode))
        return unlink(path) == 0 ? 0 : -errno;

    d = opendir(path);
    if (d == NULL)
        return -errno;
    while ((de = readdir(d)) != NULL) {
        char child[FX_PATH_MAX];
        int rc;

        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0)
            continue;
        if (snprintf(child, sizeof(child), "%s/%s", path, de->d_name) >=
            (int)sizeof(child)) {
            closedir(d);
            return -ENAMETOOLONG;
        }
        rc = fx_rmtree(child);
        if (rc != 0) {
            closedir(d);
            return rc;
        }
    }
    closedir(d);
    return rmdir(path) == 0 ? 0 : -errno;
}

#endif
