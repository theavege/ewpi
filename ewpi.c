#define _DEFAULT_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#ifdef _WIN32
# include <windows.h>
# ifdef PATH_MAX
#  undef  PATH_MAX
# endif
# define PATH_MAX 32767
#else
# include <linux/limits.h>
# include <sys/mman.h>
# include <unistd.h>
# include <fcntl.h>
#endif

#include "ewpi_map.h"
#include "ewpi_spawn.h"

#define EWPI_DEBUG 0

#define EWPI_NAME(it) \
    (it[0] == 'n') && \
    (it[1] == 'a') && \
    (it[2] == 'm') && \
    (it[3] == 'e') && \
    (it[4] == ':') && \
    (it[5] == ' ')

#define EWPI_VERSION(it) \
    (it[0] == 'v') && \
    (it[1] == 'e') && \
    (it[2] == 'r') && \
    (it[3] == 's') && \
    (it[4] == 'i') && \
    (it[5] == 'o') && \
    (it[6] == 'n') && \
    (it[7] == ':') && \
    (it[8] == ' ')

#define EWPI_URL(it) \
    (it[0] == 'u') && \
    (it[1] == 'r') && \
    (it[2] == 'l') && \
    (it[3] == ':') && \
    (it[4] == ' ')

/* Build a shell command safely: no raw string concatenation, all dynamic
 * values are single-quoted so they cannot break out of the shell word. */
static int
_ew_cmd(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    int ret;

    va_start(ap, fmt);
    ret = vsnprintf(buf, size, fmt, ap);
    va_end(ap);

    if ((ret < 0) || ((size_t)ret >= size))
    {
        printf("error: command too long, buffer overflow prevented\n");
        fflush(stdout);
        return 0;
    }

    return 1;
}

/* Quote a string for safe use inside a /bin/sh command line. */
static int
_ew_sh_quote(char *out, size_t size, const char *in)
{
    size_t o = 0;
    const char *i;

    if (size < 3)
        return 0;

    out[o++] = '\'';
    for (i = in; *i; i++)
    {
        if (*i == '\'')
        {
            /* close quote, escaped quote, reopen quote */
            if ((o + 4) >= size)
                return 0;
            out[o++] = '\'';
            out[o++] = '\\';
            out[o++] = '\'';
            out[o++] = '\'';
        }
        else
        {
            if ((o + 2) >= size)
                return 0;
            out[o++] = *i;
        }
    }
    out[o++] = '\'';
    out[o] = '\0';

    return 1;
}

/* TLS certificate verification can only be disabled on explicit request,
 * via the EWPI_INSECURE environment variable. Downloads are always
 * integrity-checked against the pinned sha512sum when present. */
static const char *
_ew_wget_insecure_opt(void)
{
    static const char *cached = NULL;
    static int checked = 0;

    if (!checked)
    {
        checked = 1;
        cached = getenv("EWPI_INSECURE");
        if (cached && *cached && strcmp(cached, "0") != 0)
        {
            printf("warning: EWPI_INSECURE is set, TLS certificate checks are DISABLED\n");
            fflush(stdout);
            cached = "--no-check-certificate ";
        }
        else
        {
            cached = "";
        }
    }

    return cached;
}

#define EWPI_SHA512SUM(it) \
    (it[0] == 's') && \
    (it[1] == 'h') && \
    (it[2] == 'a') && \
    (it[3] == '5') && \
    (it[4] == '1') && \
    (it[5] == '2') && \
    (it[6] == 'S') && \
    (it[7] == 'U') && \
    (it[8] == 'M') && \
    (it[9] == ':') && \
    (it[10] == ' ')

#define EWPI_DEPS(it) \
    (it[0] == 'd') && \
    (it[1] == 'e') && \
    (it[2] == 'p') && \
    (it[3] == 's') && \
    (it[4] == ':')

typedef struct
{
    char *name;
    char *version;
    int vmaj;
    int vmin;
    int vmic;
    int vrev;
    char *url;
    char *tarname;
    char *sha512sum;
    int deps_count;
    char **deps;
    unsigned int downloaded : 1;
    unsigned int extracted : 1;
    unsigned int installed : 1;
    unsigned int is_git : 1;
} Package;

/* Bounded string helpers: ew_strlcpy / ew_strlcat return the total length
 * they tried to create; if that is >= the buffer size, truncation occurred
 * and the caller must handle it instead of overflowing the buffer. */
static size_t
ew_strlcpy(char *dst, const char *src, size_t size)
{
    size_t src_len = strlen(src);
    size_t copy_len = (src_len >= size) ? (size - 1) : src_len;

    if (size == 0)
        return src_len + 1; /* force caller to detect truncation */

    if (copy_len)
        memcpy(dst, src, copy_len);
    dst[copy_len] = '\0';

    return src_len;
}

static size_t
ew_strlcat(char *dst, const char *src, size_t size)
{
    size_t dst_len = strnlen(dst, size);
    size_t src_len = strlen(src);

    if (dst_len >= size) /* unterminated dst */
        return size + src_len;

    if (src_len >= size - dst_len)
    {
        /* would truncate: copy what fits, keep NUL, signal via return */
        memcpy(dst + dst_len, src, size - dst_len - 1);
        dst[size - 1] = '\0';
        return dst_len + src_len;
    }

    memcpy(dst + dst_len, src, src_len + 1);

    return dst_len + src_len;
}

#define EW_PATH_SIZE 4096

/* Join up to 3 components into buf with '/' separators. Returns buf on
 * success, NULL on truncation. */
static char *
ew_path_join(char *buf, size_t size, const char *a, const char *b, const char *c)
{
    size_t n;

    if (ew_strlcpy(buf, a, size) >= size)
        return NULL;
    n = ew_strlcat(buf, "/", size);
    n = ew_strlcat(buf, b, size);
    if (n >= size)
        return NULL;
    if (c)
    {
        n = ew_strlcat(buf, "/", size);
        n = ew_strlcat(buf, c, size);
        if (n >= size)
            return NULL;
    }

    return buf;
}

static const int _ew_vmaj = 1;
static const int _ew_vmin = 3;

static char *_ew_package_dir_git = NULL;
static char *_ew_package_dir_dst = NULL;
static int _ew_package_count_total = 0;
static int _ew_package_deps_dst_count = 0;
static int *_ew_package_index = NULL;
/* Number of packages (within the computed dependency tree) that still need
 * to be downloaded / extracted / installed.  These are recomputed from the
 * dependency tree in _ew_packages_status_set(), so they only ever cover the
 * packages actually selected for this run. */
static int _ew_package_count_not_downloaded = 0;
static int _ew_package_count_not_extracted = 0;
static int _ew_package_count_not_installed = 0;
static int _ew_package_name_size_max = 0;

static Package *_ewpi_pkgs = NULL;

static void *
_ew_str_get(const unsigned char *start, const unsigned char *end)
{
    char *str;

    size_t len = (size_t)(end - start);

    str = (char *)malloc(len + 1);
    memcpy(str, start, len);
    str[len] = '\0';

    return str;
}

static void
_ew_usage(const char *argv0)
{
    printf("Usage: %s [OPTION]\n", argv0);
    printf("\n");
    printf("Compile and install the EFL dependencies.\n");
    printf("\n");
    printf("Optional arguments:\n");
    printf("  --help        show this help message and exit\n");
    printf("  --version     show the Ewpi version and exit\n");
    printf("  --prefix=DIR  install in  DIR (must be an absolute path)\n");
    printf("                  [default=$HOME/ewpi_$arch] $arch=32|64 base on\n");
    printf("                  host value\n");
    printf("  --host=VAL    host triplet, either i686-w64-mingw32 or x86_64-w64-mingw32\n");
    printf("                  [default=x86_64-w64-mingw32]\n");
    printf("  --arch=VAL    value passed to -march and -mtune gcc options\n");
    printf("                  [default=i686|x86-64], depending on host value\n");
    printf("  --winver=VAL  requested Windows version, win7 or win10 [default=win10]\n");
    printf("  --verbose     verbose mode\n");
    printf("  --strip       strip DLL\n");
    printf("  --nsis        strip DLL and create the NSIS installer\n");
    printf("\n");
    printf("Environment:\n");
    printf("  EWPI_INSECURE  if set (and not '0'), disable TLS certificate\n");
    printf("                 verification for downloads (NOT recommended;\n");
    printf("                 pinned sha512sum: entries still apply)\n");
    printf("  --efl         install the EFL\n");
    printf("  --jobs=VAL    maximum number of used jobs [default=maximum]\n");
    printf("  --clean       remove the archives and the created directories\n");
    printf("                  (not removed by default)\n");
    printf("\n");
    printf("Examples:\n");
    printf("  ./ewpi --prefix=/opt/ewpi_32 --host=i686-w64-mingw32\n");
    printf("  ./ewpi --host=x86_64-w64-mingw32 --efl --jobs=4\n");
    printf("\n");
    fflush(stdout);
}

static int
_ew_path_is_absolute(const char *path)
{
#ifdef _WIN32
        if (!((strlen(path) >= 2) &&
              (((path[0] >= 'a') && (path[0] <= 'z')) ||
               ((path[0] >= 'A') && (path[0] <= 'Z'))) &&
              (path[1] == ':')))
        {
            printf("The path must be an absolute directory\n");
            fflush(stdout);
            return 0;
        }
#else
        if (*path != '/')
        {
            printf("The path must be an absolute directory\n");
            fflush(stdout);
            return 0;
        }
#endif
        return 1;
}

static int
_ew_path_exists(const char *path)
{
    struct stat buf;

    if (stat(path, &buf) != 0)
        return 0;

    return S_ISDIR(buf.st_mode);
}

static ssize_t
_ew_file_exists(const char *file)
{
    struct stat buf;

    if ((stat(file, &buf) == 0) && S_ISREG(buf.st_mode))
        return (ssize_t)buf.st_size;

    return 0;
}

static int
_ew_mkdir(const char *pathname, int mode)
{
#if _WIN32
    return mkdir(pathname);
    (void)mode;
#else
    return mkdir(pathname, (mode_t)mode);
#endif
}

static int
_ew_mkdir_p(const char *path)
{
    char *p;
    char *iter;

    p = strdup(path);
    if (!p)
        return 0;

    iter = strchr(p, '/');
    if (!iter)
        goto free_p;
    /* Windows : "*:/" and Unix : "/" so we skip first "/" */
    iter++;

    while (1)
    {
        iter = strchr(iter, '/');
        if (!iter)
        {
            _ew_mkdir(p, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
            break;
        }

        *iter = '\0';
        if (!_ew_path_exists(p))
        {
            if (_ew_mkdir(p, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH))
                goto free_p;
        }
        *iter = '/';
        iter++;
    }

    free(p);

    return 1;

  free_p:
    free(p);

    return 0;
}

static const char *_ew_req_host[] =
{
    "gcc",
    "g++",
    "ar",
    "dlltool",
    "nm",
    "ranlib",
    "strip",
    "windres",
    NULL
};

static const char *_ew_req[][2] =
{
    { "make",     "--version" },
    { "cmake",    "--version" },
    { "python",   "--version" },
    { "perl",     "--version" },
    { "meson",    "--version" },
    { "ninja",    "--version" },
    { "yasm",     "--version" },
    { "nasm",     "--version" },
    { "gperf",    "--version" },
    { "wget",     "--version" },
    { "bison",    "--version" },
    { "flex",     "--version" },
    { "itstool",  "--version" },
    { "makensis", "-VERSION" },
    { NULL, NULL }
};

static int
_ew_requirements(const char *host)
{
    int ret;

    for (int i = 0; _ew_req_host[i]; i++)
    {
#ifdef _WIN32
        char old_file[4096];
        char new_file[4096];
        HMODULE mod;
        char *last;
        DWORD len;
        BOOL res;

        mod = LoadLibrary("gcc.exe");
        if (!mod)
            return 0;

        len = GetModuleFileName(mod, old_file, sizeof(old_file));
        FreeLibrary(mod);

        if (!len)
            return 0;

        old_file[len] = 0; /* not necessary, but i'm paranoid */
        last = strrchr(old_file, '\\');
        *(last + 1) = 0;
        ew_strlcpy(new_file, old_file, sizeof(new_file));

        if (i >= 2)
        {
            size_t n;

            n = ew_strlcat(old_file, _ew_req_host[i], sizeof(old_file));
            n += ew_strlcat(old_file, ".exe", sizeof(old_file));

            n = ew_strlcat(new_file, host, sizeof(new_file));
            n = ew_strlcat(new_file, "-", sizeof(new_file));
            n = ew_strlcat(new_file, _ew_req_host[i], sizeof(new_file));
            n = ew_strlcat(new_file, ".exe", sizeof(new_file));
            if (n >= sizeof(old_file) || n >= sizeof(new_file))
                return 0;

            res = CopyFile(old_file, new_file, FALSE);
            if (!res) return 0;
        }
#endif

        ret = ewpi_spawnv(host, _ew_req_host[i], "--version");
        printf("  %s : %s\n", _ew_req_host[i], ret ? "yes" : "no");
        fflush(stdout);
        if (!ret) return 0;
    }

    for (int i = 0; _ew_req[i][0]; i++)
    {
        ret = ewpi_spawn(NULL, _ew_req[i][0], _ew_req[i][1]);
        printf("  %s : %s\n", _ew_req[i][0], ret ? "yes" : "no");
        fflush(stdout);
        if (ret == 0) return 0;
    }

    return 1;
}

static int
_ew_packages_dir_set(const char *prefix)
{
    char buf[PATH_MAX];
    char *iter;

    /* first, package in the git repo */
    if (!getcwd(buf, PATH_MAX))
        return 0;

    iter = buf;
    while (*iter)
    {
        if (*iter == '\\') *iter = '/';
        iter++;
    }

    if (!_ew_cmd(buf, sizeof(buf), "%s/packages", buf))
        return 0;

    _ew_package_dir_git = strdup(buf);

    /* then, package in the destination */
    if (!_ew_cmd(buf, sizeof(buf), "%s/share/ewpi/packages", prefix))
        return 0;

    _ew_package_dir_dst = strdup(buf);

    _ew_mkdir_p(_ew_package_dir_dst);

    return 1;
}

static void
_ew_packages_count_total()
{
    DIR *dir;
    struct dirent *f;

    dir = opendir(_ew_package_dir_git);
    if (!dir)
        return;

    while ((f = readdir(dir)))
    {
        if ((strcmp(f->d_name, ".") == 0) ||
            (strcmp(f->d_name, "..") == 0))
            continue;

        _ew_package_count_total++;
    }

    closedir(dir);
}

static void
_ew_version_get(char *ver, int *maj, int *min, int *mic, int *rev)
{
    char buf[128];
    char *start;
    char *iter;

    *maj = 0;
    *min = 0;
    *mic = 0;
    *rev = 0;

    if (ew_strlcpy(buf, ver, sizeof(buf)) >= sizeof(buf))
        return;
    start = buf;

    iter = strchr(start, '-');
    if (iter)
    {
        *iter = '\0';
        iter++;
        *rev = atoi(iter);
    }

    iter = strchr(start, '.');
    if (iter)
    {
        *iter = '\0';
        *maj = atoi(start);
        start = iter + 1;
        iter = strchr(start, '.');
        if (iter)
        {
            *iter = '\0';
            *min = atoi(start);
            start = iter + 1;
            if (*start != '\0')
            {
                *mic = atoi(start);
            }
        }
        else
            *min = atoi(start);
    }
    else
        *maj = atoi(start);
}

static void
_ew_packages_fill(Map *map, Package *pkg)
{
    const unsigned char *iter;

    iter = map->base;

    while ((int)(iter - map->base) < (int)map->length)
    {
        if (EWPI_NAME(iter))
        {
            const unsigned char *iter2;

            iter += 6;
            iter2 = iter;
            while (*iter != '\n') iter++;
            pkg->name = _ew_str_get(iter2, iter);
        }
        else if (EWPI_VERSION(iter))
        {
            const unsigned char *iter2;

            iter += 9;
            iter2 = iter;
            while (*iter != '\n') iter++;
            pkg->version = _ew_str_get(iter2, iter);
            _ew_version_get(pkg->version, &pkg->vmaj, &pkg->vmin, &pkg->vmic, &pkg->vrev);
        }
        else if (EWPI_URL(iter))
        {
            const unsigned char *iter2;
            char *tarname;

            iter += 5;
            iter2 = iter;
            while (*iter != '\n') iter++;
            pkg->url = _ew_str_get(iter2, iter);
            tarname = strrchr(pkg->url, '/');
            tarname++;
            pkg->tarname = strdup(tarname);
            tarname = strrchr(tarname, '.');
            tarname++;
            if (strcmp(tarname, "git") == 0)
                pkg->is_git = 1;
        }
        else if (EWPI_SHA512SUM(iter))
        {
            const unsigned char *iter2;

            iter += 11;
            iter2 = iter;
            while (*iter != '\n') iter++;
            pkg->sha512sum = _ew_str_get(iter2, iter);
        }
        else if (EWPI_DEPS(iter))
        {
            iter += 5;
            if (*iter == '\n')
            {
                pkg->deps_count = 0;
                pkg->deps = NULL;
            }
            else
            {
                const unsigned char *iter2;
                int j;

                iter2 = iter;
                pkg->deps_count = 0;
                while (*iter != '\n')
                {
                    if (*iter == ' ')
                        pkg->deps_count++;
                    iter++;
                }
                pkg->deps = (char **)malloc((size_t)pkg->deps_count * sizeof(char *));
                j = 0;
                iter2++;
                iter = iter2;
                while (*iter != '\n')
                {
                    if (*iter == ' ')
                    {
                        size_t dlen = (size_t)(iter - iter2);

                        pkg->deps[j] = (char *)malloc(dlen + 1);
                        memcpy(pkg->deps[j], iter2, dlen);
                        pkg->deps[j][dlen] = '\0';
                        j++;
                        iter2 = iter + 1;
                    }
                    iter++;
                }
                {
                    size_t dlen = (size_t)(iter - iter2);

                    pkg->deps[j] = (char *)malloc(dlen + 1);
                    memcpy(pkg->deps[j], iter2, dlen);
                    pkg->deps[j][dlen] = '\0';
                }
            }
        }

        iter++;
    }
}

static int
_ew_packages_get_git(void)
{
    char buf[PATH_MAX];
    DIR *dir;
    struct dirent *f;
    Package *iter;

    _ewpi_pkgs = (Package *)calloc((size_t)_ew_package_count_total, sizeof(Package));
    if (!_ewpi_pkgs)
        return 0;

    dir = opendir(_ew_package_dir_git);
    if (!dir)
        goto free_pkgs;

    iter = _ewpi_pkgs;
    while ((f = readdir(dir)))
    {
        Map map;

        if ((strcmp(f->d_name, ".") == 0) ||
            (strcmp(f->d_name, "..") == 0))
            continue;
        if (!_ew_cmd(buf, sizeof(buf), "%s/%s/%s.ewpi",
                     _ew_package_dir_git, f->d_name, f->d_name))
            continue;

        if (!ewpi_map_new(&map, buf))
            continue;

        /* calloc() gives us a zeroed Package: every optional field
         * (deps_count, deps, sha512sum, flags...) starts at 0/NULL so a
         * recipe missing keys cannot leave garbage that would later be
         * free()'d twice. */
        _ew_packages_fill(&map, iter);

        ewpi_map_del(&map);

#if EWPI_DEBUG
        printf(" name: %s\n", iter->name);
        printf(" version: %s\n", iter->version);
        printf(" url: %s\n", iter->url);
        printf(" deps:");
        int j;
        for (j = 0; j < iter->deps_count; j++)
            printf(" %s", iter->deps[j]);
        printf("\n");
        printf(" inst: %d\n", iter->installed);
#endif
        iter++;
    }

    closedir(dir);

    return 1;

  free_pkgs:
    /* Keep _ewpi_pkgs allocated (zeroed by calloc) so the cleanup loop in
     * main() can free it exactly once; setting it to NULL here would make
     * main() free an already-freed pointer. */
    return 0;
}

static int
_ew_copy(const char *path_git, const char *path_dst, const char *filename)
{
    char f_git[EW_PATH_SIZE];
    char f_dst[EW_PATH_SIZE];
    char *buf_git = NULL;
    FILE *file;
    size_t size;
    size_t sz;

    if (!ew_path_join(f_git, sizeof(f_git), path_git, filename, NULL))
        return 0;

    /* get f_git size */
    {
        ssize_t fsize = _ew_file_exists(f_git);

        if (fsize <= 0)
            return 0;
        size = (size_t)fsize;
    }

    buf_git = (char *)malloc(size);
    if (!buf_git)
        return 0;

    file = fopen(f_git, "rb");
    if (!file)
        goto free_buf_git;

    sz = fread(buf_git, 1, size, file);
    if (sz != size)
        goto close_file;

    fclose(file);

    if (!ew_path_join(f_dst, sizeof(f_dst), path_dst, filename, NULL))
    {
        free(buf_git);
        return 0;
    }

    file = fopen(f_dst, "wb");
    if (!file)
        goto free_buf_git;

    sz = fwrite(buf_git, 1, size, file);

    fclose(file);
    free(buf_git);

    if (sz != size)
    {
        printf(" size mismatch %d %d\n", (int)size, (int)sz);
        fflush(stdout);
        unlink(f_dst);
        return 0;
    }

    return 1;

  close_file:
    fclose(file);
  free_buf_git:
    free(buf_git);

    return 0;
}

/* Forward declarations: the dependency tree is built before the status
 * scan, and both steps reference each other. */
static void
_ew_packages_status_set(void);

static void
_ew_packages_tree(const char *pkg_name);

static void
_ew_packages_dst_set(const char *prefix)
{
    char buf_git[PATH_MAX];
    char buf_dst[PATH_MAX];

    for (int i = 0; i < _ew_package_count_total; i++)
    {
        char buf_ewpi[PATH_MAX];

        if (!ew_path_join(buf_git, sizeof(buf_git), _ew_package_dir_git,
                          _ewpi_pkgs[i].name, NULL) ||
            !ew_path_join(buf_dst, sizeof(buf_dst), _ew_package_dir_dst,
                          _ewpi_pkgs[i].name, NULL))
            continue;

        if (ew_strlcpy(buf_ewpi, _ewpi_pkgs[i].name, sizeof(buf_ewpi)) >= sizeof(buf_ewpi) ||
            ew_strlcat(buf_ewpi, ".ewpi", sizeof(buf_ewpi)) >= sizeof(buf_ewpi))
            continue;

        if (_ew_path_exists(buf_dst))
        {
            /*
             * if directory exists, we compare versions from git and dst
             * and if git is newer, we suppress the 'installed' file
             */
            char buf_ewpi_dst[PATH_MAX];
            Package pkg;
            Map map;

            if (!ew_path_join(buf_ewpi_dst, sizeof(buf_ewpi_dst), buf_dst,
                              buf_ewpi, NULL))
                continue;
            if (ewpi_map_new(&map, buf_ewpi_dst))
            {
                _ew_packages_fill(&map, &pkg);
                /* if version in git is greater, we remove "installed" file */
                if ((_ewpi_pkgs[i].vmaj > pkg.vmaj) ||
                    ((_ewpi_pkgs[i].vmaj == pkg.vmaj) &&
                     (_ewpi_pkgs[i].vmin > pkg.vmin)) ||
                    ((_ewpi_pkgs[i].vmaj == pkg.vmaj) &&
                     (_ewpi_pkgs[i].vmin == pkg.vmin) &&
                     (_ewpi_pkgs[i].vmic > pkg.vmic)) ||
                    ((_ewpi_pkgs[i].vmaj == pkg.vmaj) &&
                     (_ewpi_pkgs[i].vmin == pkg.vmin) &&
                     (_ewpi_pkgs[i].vmic == pkg.vmic) &&
                     (_ewpi_pkgs[i].vrev > pkg.vrev)))
                {
                    char buf[PATH_MAX];

                    if (ew_path_join(buf, sizeof(buf), buf_dst, "downloaded", NULL))
                        unlink(buf);
                    if (ew_path_join(buf, sizeof(buf), buf_dst, "extracted", NULL))
                        unlink(buf);
                    if (ew_path_join(buf, sizeof(buf), buf_dst, "installed", NULL))
                        unlink(buf);
                }
                ewpi_map_del(&map);
            }
        }
        else
        {
            /* Otherwise, the dst directory does not exist, so we create it. */
            _ew_mkdir(buf_dst, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
        }

        /*
         * we always copy all the files, in case:
         * a new package is installed
         * or a build system has changed
         * or something else :-)
         */
        _ew_copy(buf_git, buf_dst, buf_ewpi);
        _ew_copy(buf_git, buf_dst, "install.sh");
        _ew_copy(buf_git, buf_dst, "cross_toolchain.txt");
    }

    if (!getcwd(buf_git, PATH_MAX))
        return;

    if (ew_strlcpy(buf_dst, prefix, sizeof(buf_dst)) >= sizeof(buf_dst) ||
        ew_strlcat(buf_dst, "/share/ewpi", sizeof(buf_dst)) >= sizeof(buf_dst))
        return;
    _ew_copy(buf_git, buf_dst, "common.sh");
}

static void
_ew_packages_status_set(void)
{
    /* Walk the packages selected in the dependency tree (not every recipe
     * on disk) and check their download/extract/install markers. */
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        char buf[PATH_MAX];
        char buf_pkg[PATH_MAX];
        Package *iter = _ewpi_pkgs + _ew_package_index[i];

        if (!ew_path_join(buf_pkg, sizeof(buf_pkg), _ew_package_dir_dst,
                          iter->name, NULL))
            continue;

        if (ew_strlcpy(buf, buf_pkg, sizeof(buf)) < sizeof(buf) &&
            ew_strlcat(buf, "/downloaded", sizeof(buf)) < sizeof(buf))
        {
            if (_ew_file_exists(buf) && !iter->sha512sum)
                iter->downloaded = 1;
            else
                _ew_package_count_not_downloaded++;
        }
        else
        {
            _ew_package_count_not_downloaded++;
        }

        if (ew_path_join(buf, sizeof(buf), buf_pkg, "extracted", NULL))
        {
            if (_ew_file_exists(buf))
                iter->extracted = 1;
            else
                _ew_package_count_not_extracted++;
        }
        else
        {
            _ew_package_count_not_extracted++;
        }

        if (ew_path_join(buf, sizeof(buf), buf_pkg, "installed", NULL))
        {
            if (_ew_file_exists(buf))
                iter->installed = 1;
            else
                _ew_package_count_not_installed++;
        }
        else
        {
            _ew_package_count_not_installed++;
        }
    }
}

static int
_ew_packages_find(const char *pkg_name)
{
    for (int idx = 0; idx < _ew_package_count_total; idx++)
    {
        if (_ewpi_pkgs[idx].name && strcmp(pkg_name, _ewpi_pkgs[idx].name) == 0)
            return idx;
    }

    return -1;
}

static void
_ew_packages_tree(const char *pkg_name)
{
    int idx;
    int already_in = 0;

    /* get the index of the package */
    idx = _ew_packages_find(pkg_name);
    if (idx < 0)
    {
        printf("error: unknown dependency '%s', exiting...\n", pkg_name);
        fflush(stdout);
        exit(1);
    }

    /* guard against dependency loops */
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        if (_ew_package_index[i] == idx)
            already_in = 1;
    }
    if (already_in)
        return;

    _ew_package_index[_ew_package_deps_dst_count] = idx;
    _ew_package_deps_dst_count++;

    for (int i = 0; i < _ewpi_pkgs[idx].deps_count; i++)
        _ew_packages_tree(_ewpi_pkgs[idx].deps[i]);
}

static void
_ew_packages_not_installed_disp(void)
{

    printf("\nPackages (%d)\n", _ew_package_count_not_installed);
    fflush(stdout);
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        int idx = _ew_package_index[i];
        if (!_ewpi_pkgs[idx].installed)
            printf("  %s-%s\n", _ewpi_pkgs[idx].name, _ewpi_pkgs[idx].version);
    }
    printf("\n");
    fflush(stdout);
}

/* Portable SHA-512 implementation (FIPS 180-4 reference style).
 * Used to verify downloads against the sha512sum pinned in each recipe.
 * Kept in-tree so ewpi has no build-time dependency on OpenSSL. */
#define EW_SHA512_BLOCK_BYTES 128
#define EW_SHA512_DIGEST_LEN  64

typedef struct
{
    unsigned long long state[8];
    unsigned long long bitlen;
    unsigned char data[EW_SHA512_BLOCK_BYTES];
    size_t datalen;
} EwSha512Ctx;

static const unsigned long long _ew_sha512_k[80] = {
    0x428a2f98d728ae22ULL, 0x7137449123ef65cdULL, 0xb5c0fbcfec4d3b2fULL,
    0xe9b5dba58189dbbcULL, 0x3956c25bf348b538ULL, 0x59f111f1b605d019ULL,
    0x923f82a4af194f9bULL, 0xab1c5ed5da6d8118ULL, 0xd807aa98a3030242ULL,
    0x12835b0145706fbeULL, 0x243185be4ee4b28cULL, 0x550c7dc3d5ffb4e1ULL,
    0x72be5d74f27b896fULL, 0x80deb1fe3b1696b1ULL, 0x9bdc06a725c71235ULL,
    0xc19bf174cf692694ULL, 0xe49b69c19ef14ad2ULL, 0xefbe4786384f25e3ULL,
    0x0fc19dc68b8cd5b5ULL, 0x240ca1cc77ac9c65ULL, 0x5ce975e6276df135ULL,
    0x76a98da64b71938dULL, 0x983e5152ee66dfabULL, 0xa831c66d2db43210ULL,
    0xb00327c898fb213fULL, 0xbf597fc7beef0ee4ULL, 0xc6e00bf33da88fc2ULL,
    0xd5a79147930aa725ULL, 0x06ca6351e003826fULL, 0x142929670a0e6e70ULL,
    0x27b70a8546d22ffcULL, 0x2e1b21385c26c926ULL, 0x4d2c6dfc5ac42aedULL,
    0x53380d139d95b3dfULL, 0x650a73548baf63deULL, 0x766a0abb3c77b2a8ULL,
    0x81c1979bca4487a7ULL, 0x92722c851482353bULL, 0xa2bfe8a14cf10364ULL,
    0xa81a664bbc423001ULL, 0xc24b8b70d0f89791ULL, 0xc76c51a30654be30ULL,
    0xd192e819d6ef5218ULL, 0xd69906245565a910ULL, 0xf40e308557712045ULL,
    0x106aa07032bbd1b8ULL, 0x19a4c116b8d2d0c8ULL, 0x1e376c085141ab53ULL,
    0x2748774cdf8eeb99ULL, 0x34b0bcb5e19b48a8ULL, 0x391c0cb3c5c95a63ULL,
    0x4ed8aa4ae3418acbULL, 0x5b9cca4f7763e373ULL, 0x682e6ff3d6b7b8a3ULL,
    0x748f82ee5defb2fcULL, 0x78a5636f43172f60ULL, 0x84c87814a1f0ab72ULL,
    0x8cc702081a6439ecULL, 0x90befffa23631e28ULL, 0xa4506cebde82bde9ULL,
    0xbef9a3f7b2c67915ULL, 0xc67178f2e372532aULL, 0xca273eceea26619cULL,
    0xd186b8c721c0c207ULL, 0xedeada4828825b2bULL, 0xf57d4f7f6eed4f57ULL,
    0x06f067aa72176fbaULL, 0x0a637dc5a2cfeaefULL, 0x113f9804bef90daeULL,
    0x1b710b35131c471bULL, 0x28db77f523047d84ULL, 0x32caab7140c72493ULL,
    0x3c9ebe0a15c9bebcULL, 0x431d67c49c100d4cULL, 0x4cc5d4becb3e42b6ULL,
    0x597f299cfc657e2aULL, 0x5fcb6fab3ad6faecULL, 0x6c44198c4a475817ULL
};

static inline unsigned long long
_ew_rotr64(unsigned long long x, unsigned int n)
{
    return (x >> n) | (x << (64 - n));
}

static void
_ew_sha512_transform(EwSha512Ctx *ctx, const unsigned char *data)
{
    unsigned long long w[80];
    unsigned long long a, b, c, d, e, f, g, h;
    int t;

    for (t = 0; t < 16; t++)
    {
        w[t] = ((unsigned long long)data[t * 8] << 56) |
               ((unsigned long long)data[t * 8 + 1] << 48) |
               ((unsigned long long)data[t * 8 + 2] << 40) |
               ((unsigned long long)data[t * 8 + 3] << 32) |
               ((unsigned long long)data[t * 8 + 4] << 24) |
               ((unsigned long long)data[t * 8 + 5] << 16) |
               ((unsigned long long)data[t * 8 + 6] << 8) |
               ((unsigned long long)data[t * 8 + 7]);
    }
    for (t = 16; t < 80; t++)
    {
        unsigned long long s0 = _ew_rotr64(w[t - 15], 1) ^
                                _ew_rotr64(w[t - 15], 8) ^ (w[t - 15] >> 7);
        unsigned long long s1 = _ew_rotr64(w[t - 2], 19) ^
                                _ew_rotr64(w[t - 2], 61) ^ (w[t - 2] >> 6);
        w[t] = w[t - 16] + s0 + w[t - 7] + s1;
    }

    a = ctx->state[0]; b = ctx->state[1]; c = ctx->state[2]; d = ctx->state[3];
    e = ctx->state[4]; f = ctx->state[5]; g = ctx->state[6]; h = ctx->state[7];

    for (t = 0; t < 80; t++)
    {
        unsigned long long s1 = _ew_rotr64(e, 14) ^ _ew_rotr64(e, 18) ^
                                _ew_rotr64(e, 41);
        unsigned long long ch = (e & f) ^ (~e & g);
        unsigned long long t1 = h + s1 + ch + _ew_sha512_k[t] + w[t];
        unsigned long long s0 = _ew_rotr64(a, 28) ^ _ew_rotr64(a, 34) ^
                                _ew_rotr64(a, 39);
        unsigned long long maj = (a & b) ^ (a & c) ^ (b & c);
        unsigned long long t2 = s0 + maj;

        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }

    ctx->state[0] += a; ctx->state[1] += b; ctx->state[2] += c;
    ctx->state[3] += d; ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void
_ew_sha512_init(EwSha512Ctx *ctx)
{
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667f3bcc908ULL;
    ctx->state[1] = 0xbb67ae8584caa73bULL;
    ctx->state[2] = 0x3c6ef372fe94f82bULL;
    ctx->state[3] = 0xa54ff53a5f1d36f1ULL;
    ctx->state[4] = 0x510e527fade682d1ULL;
    ctx->state[5] = 0x9b05688c2b3e6c1fULL;
    ctx->state[6] = 0x1f83d9abfb41bd6bULL;
    ctx->state[7] = 0x5be0cd19137e2179ULL;
}

static void
_ew_sha512_update(EwSha512Ctx *ctx, const unsigned char *data, size_t len)
{
    size_t i;

    for (i = 0; i < len; i++)
    {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == EW_SHA512_BLOCK_BYTES)
        {
            _ew_sha512_transform(ctx, ctx->data);
            ctx->bitlen += 1024;
            ctx->datalen = 0;
        }
    }
}

static void
_ew_sha512_final(EwSha512Ctx *ctx, unsigned char *digest)
{
    unsigned long long bitlen;
    size_t pad_start;
    int i;

    /* Save the true length before padding mutates the state. */
    bitlen = ctx->bitlen + (unsigned long long)ctx->datalen * 8;

    ctx->data[ctx->datalen++] = 0x80;
    if (ctx->datalen > EW_SHA512_BLOCK_BYTES - 16)
    {
        while (ctx->datalen < EW_SHA512_BLOCK_BYTES)
            ctx->data[ctx->datalen++] = 0;
        _ew_sha512_transform(ctx, ctx->data);
        ctx->datalen = 0;
    }
    pad_start = ctx->datalen;
    while (ctx->datalen < EW_SHA512_BLOCK_BYTES)
        ctx->data[ctx->datalen++] = 0;
    for (i = 0; i < 8; i++)
        ctx->data[EW_SHA512_BLOCK_BYTES - 1 - (size_t)i] =
            (unsigned char)(bitlen >> (8 * i));
    (void)pad_start;
    _ew_sha512_transform(ctx, ctx->data);

    for (i = 0; i < 8; i++)
    {
        int j;
        for (j = 0; j < 8; j++)
            digest[i * 8 + j] = (unsigned char)(ctx->state[i] >> (56 - 8 * j));
    }
}

static int
_ew_packages_verify(Package *pkg, const char *dir)
{
    char path[4096];
    char shafile[4096];
    FILE *f;
    size_t n;
    EwSha512Ctx ctx;
    unsigned char digest[EW_SHA512_DIGEST_LEN];
    char hex[2 * EW_SHA512_DIGEST_LEN + 1];
    size_t i;

    if (!_ew_cmd(shafile, sizeof(shafile), "%s/.sha512sum", dir))
        return 0;

    /* No pinned hash: accept the download but do not mark it verified. */
    if (!pkg->sha512sum)
        return 0;

    /* Fast path: previously verified against the same pinned hash. */
    f = fopen(shafile, "r");
    if (f)
    {
        char prev[129];

        if (fgets(prev, sizeof(prev), f) &&
            strncmp(prev, pkg->sha512sum, 128) == 0)
        {
            fclose(f);
            return 1;
        }
        fclose(f);
    }

    /* If the archive is missing, there is nothing to verify yet. */
    if (!_ew_cmd(path, sizeof(path), "%s/%s", dir, pkg->tarname))
        return 0;
    if (!_ew_file_exists(path))
        return 0;

    /* Compute the SHA-512 of the downloaded archive. */
    f = fopen(path, "rb");
    if (!f)
        return 0;
    _ew_sha512_init(&ctx);
    while ((n = fread(path, 1, sizeof(path), f)) > 0)
        _ew_sha512_update(&ctx, (const unsigned char *)path, n);
    if (ferror(f))
    {
        fclose(f);
        return 0;
    }
    fclose(f);
    _ew_sha512_final(&ctx, digest);
    for (i = 0; i < sizeof(digest); i++)
        sprintf(hex + (2 * i), "%02x", digest[i]);
    hex[sizeof(hex) - 1] = '\0';

    if (strcmp(hex, pkg->sha512sum) == 0)
    {
        /* Cache the result so re-runs skip recomputation. */
        f = fopen(shafile, "w");
        if (f)
        {
            fprintf(f, "%s\n", hex);
            fclose(f);
        }
        return 1;
    }

    printf("checksum mismatch for package %s, exiting...\n", pkg->name);
    printf("  expected: %s\n", pkg->sha512sum);
    printf("  actual:   %s\n", hex);
    fflush(stdout);
    remove(shafile);
    unlink(path);
    exit(1);

    return 0;
}

static void
_ew_packages_download(void)
{
    char buf[4096];
    Package *iter;
    int count;

    count = 0;
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        char dir[4096];

        iter = _ewpi_pkgs + _ew_package_index[i];
        if (!_ew_cmd(dir, sizeof(dir), "%s/%s", _ew_package_dir_dst, iter->name))
            continue;
        if (!_ew_cmd(buf, sizeof(buf), "%s/downloaded", dir))
            continue;
        if (!_ew_file_exists(buf))
            count++;
        else if (!iter->sha512sum || _ew_packages_verify(iter, dir))
            iter->downloaded = 1;
        else
            count++;
    }

    if (count == 0)
        return;

    printf(":: Download sources...\n");
    fflush(stdout);
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        int ret;
        char dir[4096];
        char qdir[2 * 4096];
        char qurl[2 * 4096];

        iter = _ewpi_pkgs + _ew_package_index[i];
        if (iter->downloaded)
            continue;

        if (!_ew_cmd(dir, sizeof(dir), "%s/%s", _ew_package_dir_dst, iter->name))
            exit(1);
        if (!_ew_sh_quote(qdir, sizeof(qdir), dir))
            exit(1);

        if (iter->is_git)
        {
            if (iter->downloaded)
            {
                if (!_ew_cmd(buf, sizeof(buf), "cd %s && git pull", qdir))
                    exit(1);
            }
            else
            {
                if (!_ew_sh_quote(qurl, sizeof(qurl), iter->url))
                    exit(1);
                if (!_ew_cmd(buf, sizeof(buf), "cd %s && git clone %s", qdir, qurl))
                    exit(1);
            }
        }
        else
        {
            if (!_ew_sh_quote(qurl, sizeof(qurl), iter->url))
                exit(1);
            if (!_ew_cmd(buf, sizeof(buf), "cd %s && wget -q --show-progress %s%s",
                         qdir, _ew_wget_insecure_opt(), qurl))
                exit(1);
        }
        fflush(stdout);
        ret = system(buf);
        if (ret != 0)
        {
            printf("error while downloading package %s (ret = %d)\n", iter->name, ret);
            exit(1);
        }
        else
        {
            /* Verify the freshly downloaded archive against the pinned
             * sha512sum before accepting it (exits on mismatch). */
            if (!iter->is_git && iter->sha512sum &&
                !_ew_packages_verify(iter, dir))
                exit(1);
            if (!_ew_cmd(buf, sizeof(buf), "echo 1 > %s/downloaded", qdir))
                exit(1);
            ret = system(buf);
            if (ret != 0)
            {
                printf("error while writing downloaded marker for package %s\n", iter->name);
                exit(1);
            }
            if (iter->is_git)
            {
                if (!_ew_cmd(buf, sizeof(buf), "echo 1 > %s/extracted", qdir))
                    exit(1);
                ret = system(buf);
                if (ret != 0)
                {
                    printf("error while writing extracted marker for package %s\n", iter->name);
                    exit(1);
                }
            }
        }
    }
}

static void
_ew_packages_longest_name()
{
    Package *iter;

    /* compute the largest name (including version) of the packages */
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        iter = _ewpi_pkgs + _ew_package_index[i];
        {
            int len = (int)(strlen(iter->name) + 1 + strlen(iter->version));

            if (len > _ew_package_name_size_max)
                _ew_package_name_size_max = len;
        }
    }
}

static void
_ew_packages_status_disp(int i, int count, const char *name, const char* version)
{
    char buf[4096];
    char *iter;
    size_t len;
    size_t len2;
    int nbr_sharp = 40;
    int sharp;
    int percent;
    size_t j;

    snprintf(buf, 4095, "(%2d/%d) ", i + 1, count);
    len = strlen(buf);
    iter = buf + len;
    if (name)
    {
        len = strlen(name);
        for (j = 0; j < len; j++)
            *iter++ = name[j];
        *iter++ = '-';
        len2 = strlen(version);
        for (j = 0; j < len2; j++)
            *iter++ = version[j];
        {
            size_t pad = (_ew_package_name_size_max > (int)(len + 1 + len2)) ?
                         (size_t)(_ew_package_name_size_max - (int)(len + 1 + len2)) : 0;

            for (j = 0; j < pad; j++)
                *iter++ = ' ';
        }
    }
    else
    {
        for (j = 0; j < (size_t)_ew_package_name_size_max; j++)
            *iter++ = ' ';
    }
    *iter++ = ' ';
    *iter++ = '[';
    sharp = (nbr_sharp * (i + 1)) / count;
    percent = (100 * (i + 1)) / count;
    for (j = 0; j < (size_t)sharp; j++, iter++)
        *iter = '#';
    for (; j < (size_t)nbr_sharp; j++, iter++)
        *iter = ' ';
    *iter++ = ']';
    *iter++ = ' ';
    if (percent < 10)
    {
        *iter++ = ' ';
        *iter++ = ' ';
        *iter++ = (char)(percent + '0');
    }
    else if (percent < 100)
    {
        *iter++ = ' ';
        *iter++ = (char)((percent / 10) + '0');
        *iter++ = (char)((percent % 10) + '0');
    }
    else
    {
        *iter++ = '1';
        *iter++ = '0';
        *iter++ = '0';
    }
    *iter++ = '%';
    *iter++ = '\0';
    printf("\r%s", buf);
    fflush(stdout);
}

static void
_ew_packages_extract(int verbose)
{
    char buf[4096];
    Package *iter;
    int count;
    int c;

    count = 0;
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        iter = _ewpi_pkgs + _ew_package_index[i];
        if (!ew_path_join(buf, sizeof(buf), _ew_package_dir_dst, iter->name,
                          "extracted"))
            continue;
        if (!_ew_file_exists(buf))
            count++;
        else
            iter->extracted = 1;
    }

    if (count == 0)
        return;

    printf(":: Extraction of sources...\n");
    fflush(stdout);

    c = 0;
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        const char *name;
        const char *tarname;
        const char *ext;
        char taropt[5];
        int idx;
        int ret;
        char qdir[2 * 4096];
        char qtname[2 * 4096];

        iter = _ewpi_pkgs + _ew_package_index[i];
        if (iter->extracted)
            continue;

        name = iter->name;
        tarname = iter->tarname;
        ext = strrchr(tarname, '.');
        ext++;

        idx = 0;
        if (verbose)
            taropt[idx++] = 'v';
        if ((strcmp(ext, "gz") == 0) || (strcmp(ext, "tgz") == 0))
            taropt[idx++] = 'z';
        else if (strcmp(ext, "bz2") == 0)
            taropt[idx++] = 'j';
        else
        {
            taropt[idx++] = 'J';
            taropt[idx++] = 'h';
        }
        taropt[idx++] = 'f';
        taropt[idx++] = '\0';

        _ew_packages_status_disp(c, count, name, iter->version);

        if (!_ew_cmd(buf, sizeof(buf), "%s/%s", _ew_package_dir_dst, name) ||
            !_ew_sh_quote(qdir, sizeof(qdir), buf) ||
            !_ew_sh_quote(qtname, sizeof(qtname), tarname))
            exit(1);
        if (!_ew_cmd(buf, sizeof(buf), "cd %s && tar x%s %s",
                     qdir, taropt, qtname))
            exit(1);
        fflush(stdout);
        ret = system(buf);
        if (ret != 0)
        {
            printf(" Can not extract %s\n", tarname);
            exit(1);
        }
        else
        {
            if (!_ew_cmd(buf, sizeof(buf), "echo 1 > %s/extracted", qdir))
                exit(1);
            ret = system(buf);
            if (ret != 0)
            {
                printf(" error while writing extracted marker for %s\n", name);
                exit(1);
            }
        }

        c++;
    }

    _ew_packages_status_disp(count - 1, count, NULL, NULL);
    printf("\n");
}

static void
_ew_packages_install(const char *prefix, const char *host, const char *arch, const char *jobopt, int verbose, const char *winver)
{
    char buf[4096];
    Package *iter;
    int c;
    FILE *f;

    if (_ew_package_count_not_installed == 0)
        return;

    printf("\n:: Installation of packages...\n");
    fflush(stdout);

    c = 0;
    for (int i = 0; i < _ew_package_deps_dst_count; i++)
    {
        const char *name;
        const char *tarname;
        int ret;
        char qdir[2 * 4096];
        char qtname[2 * 4096];
        char qprefix[2 * 4096];
        char qhost[2 * 4096];
        char qarch[2 * 4096];
        char qjobopt[2 * 4096];
        char qwinver[2 * 4096];

        iter = _ewpi_pkgs + _ew_package_index[i];
        if (iter->installed)
            continue;

        name = iter->name;
        tarname = iter->tarname;

        _ew_packages_status_disp(c, _ew_package_count_not_installed, name, iter->version);

        if (!_ew_cmd(buf, sizeof(buf), "%s/%s", _ew_package_dir_dst, name) ||
            !_ew_sh_quote(qdir, sizeof(qdir), buf) ||
            !_ew_sh_quote(qtname, sizeof(qtname), tarname) ||
            !_ew_sh_quote(qprefix, sizeof(qprefix), prefix) ||
            !_ew_sh_quote(qhost, sizeof(qhost), host) ||
            !_ew_sh_quote(qarch, sizeof(qarch), arch) ||
            !_ew_sh_quote(qjobopt, sizeof(qjobopt), (*jobopt == 0) ? "no" : jobopt) ||
            !_ew_sh_quote(qwinver, sizeof(qwinver), winver))
            exit(1);
        if (!_ew_cmd(buf, sizeof(buf),
                     "cd %s && sh ./install.sh %s %s %s %s %s %s %s",
                     qdir, qarch, qtname, qprefix, qhost, qjobopt,
                     verbose ? "yes" : "no", qwinver))
            exit(1);
        ret = system(buf);
        if (ret != 0)
        {
            char logfile[4096];

            printf(" Can not install %s\n", name);
            snprintf(logfile, sizeof(logfile), "%s/%s/make.log",
                     _ew_package_dir_dst, name);
            f = fopen(logfile, "r");
            if (f)
            {
                size_t n;

                printf("--- last lines of %s ---\n", logfile);
                n = fread(buf, 1, sizeof(buf) - 1, f);
                buf[n] = '\0';
                fputs(buf, stdout);
                fclose(f);
                printf("--- end of log ---\n");
            }
            fflush(stdout);
            exit(1);
        }
        else
        {
            if (!_ew_cmd(buf, sizeof(buf), "echo 1 > %s/installed", qdir))
                exit(1);
            ret = system(buf);
            if (ret != 0)
            {
                printf(" error while writing installed marker for %s\n", name);
                exit(1);
            }
        }

        c++;
    }

    _ew_packages_status_disp(_ew_package_count_not_installed - 1, _ew_package_count_not_installed, NULL, NULL);
    printf("\n");
}

static void
_ew_recursive_rm(const char *path)
{
    if (_ew_path_exists(path))
    {
        DIR *dir;
        struct dirent *f;

        dir = opendir(path);
        if (dir)
        {
            while ((f = readdir(dir)))
            {
                char buf[4096];

                if ((strcmp(f->d_name, ".") == 0) ||
                    (strcmp(f->d_name, "..") == 0))
                    continue;

                snprintf(buf, 4095, "%s/%s", path, f->d_name);
                _ew_recursive_rm(buf);
            }

            closedir(dir);
        }
        rmdir(path);
    }
    else
    {
        unlink(path);
    }
}

static void
_ew_packages_clean_directories(char *pkg_dir)
{
    DIR *dir;
    struct dirent *f;

    dir = opendir(pkg_dir);
    if (dir)
    {
        char buf2[4096];

        while ((f = readdir(dir)))
        {
            if ((strcmp(f->d_name, ".") == 0) ||
                (strcmp(f->d_name, "..") == 0))
                continue;

            if (!ew_path_join(buf2, sizeof(buf2), pkg_dir, f->d_name, NULL))
                continue;
            if (_ew_path_exists(buf2))
                _ew_recursive_rm(buf2);
        }
        closedir(dir);
    }
}

static void
_ew_packages_clean(void)
{
    char buf[4096];
    Package *iter;

    printf("\n:: Cleaning...\n");
    fflush(stdout);

    for (int i = 0; i < _ew_package_count_total; i++)
    {
        const char *name;
        const char *version;
        const char *tarname;

        iter = _ewpi_pkgs + _ew_package_index[i];
        name = iter->name;
        version = iter->version;
        tarname = iter->tarname;

        _ew_packages_status_disp(i, _ew_package_count_total, name, version);

        if (!ew_path_join(buf, sizeof(buf), _ew_package_dir_dst, name, NULL))
            continue;
        _ew_packages_clean_directories(buf);
        if (ew_strlcat(buf, "/", sizeof(buf)) < sizeof(buf) &&
            ew_strlcat(buf, tarname, sizeof(buf)) < sizeof(buf))
            unlink(buf);
    }

    _ew_packages_status_disp(_ew_package_count_total - 1, _ew_package_count_total, NULL, NULL);
    printf("\n");
}

static void
_ew_recurse_strip(const char *path, const char *strip)
{
    DIR *dir;
    struct dirent *dp;
    size_t l;

    if (!path || !*path)
        return;

    dir = opendir(path);
    if (!dir)
        return;

    l = strlen(path);

    do
    {
        dp = readdir(dir);
        if (dp)
        {
            char file[4096];
            struct stat buf;
            size_t len;

            if ((*dp->d_name == '.') ||
                (*(dp->d_name + 1) == '.'))
                continue;

            len = strlen(dp->d_name);
            if ((l + 1 + len + 1) >= 4096)
                continue;

            memcpy(file, path, l);
            memcpy(file + l, "/", 1);
            memcpy(file + l + 1, dp->d_name, len + 1);
            if (stat(file, &buf))
                continue;

            if (S_ISDIR(buf.st_mode))
            {
                _ew_recurse_strip(file, strip);
            }
            else
            {
                char *ext;
                ext = strrchr(file, '.');
                if (ext)
                {
                    ext++;
                    if (strcmp(ext, "dll") == 0)
                    {
                        char strip_cmd[4096];
                        char qstrip[2 * 4096];
                        char qfile[2 * 4096];
                        int ret;

                        if (!_ew_sh_quote(qstrip, sizeof(qstrip), strip) ||
                            !_ew_sh_quote(qfile, sizeof(qfile), file))
                            continue;
                        if (!_ew_cmd(strip_cmd, sizeof(strip_cmd), "%s %s",
                                     qstrip, qfile))
                            continue;
                        printf("  %s\n", file);
                        fflush(stdout);
                        ret = system(strip_cmd);
                        if (ret != 0)
                        {
                            printf("can not strip '%s\n'\n", file);
                            fflush(stdout);
                        }
                    }
                }
            }
        }
    } while (dp);

    closedir(dir);
}

static void
_ew_packages_strip(const char *prefix, const char *host)
{
    char strip[4096];
    char path[4096];

    printf("\n:: Stripping DLL...\n");
    fflush(stdout);

    snprintf(strip, sizeof(strip), "%s-strip", host);

    snprintf(path, sizeof(path), "%s/bin", prefix);
    _ew_recurse_strip(path, strip);

    snprintf(path, sizeof(path), "%s/lib", prefix);
    _ew_recurse_strip(path, strip);

    printf("\n");
}

static void
_ew_packages_nsis(const char *prefix, const char *host, const char *winver, int efl)
{
    char buf[4096];
    char qprefix[2 * 4096];
    char qwinver[2 * 4096];
    const char *arch;
    const char *arch_suf;
    int ret;

    _ew_packages_strip(prefix, host);

    if (strcmp(host, "i686-w64-mingw32") == 0)
    {
        arch = "i686";
        arch_suf = "32";
    }
    else
    {
        arch = "x86_64";
        arch_suf = "64";
    }

    if (!_ew_sh_quote(qprefix, sizeof(qprefix), prefix) ||
        !_ew_sh_quote(qwinver, sizeof(qwinver), winver))
        return;

    if (efl)
    {
        printf("\n:: Create EFL NSIS installer...\n");
        fflush(stdout);
        if (!_ew_cmd(buf, sizeof(buf),
                     "sh ./efl_nsis.sh %s %d.%d %s %s %s",
                     qprefix, _ew_vmaj, _ew_vmin, arch, arch_suf, qwinver))
            return;
    }
    else
    {
        printf("\n:: Create EWPI NSIS installer...\n");
        fflush(stdout);
        if (!_ew_cmd(buf, sizeof(buf),
                     "sh ./ewpi_nsis.sh %s %d.%d %s %s %s",
                     qprefix, _ew_vmaj, _ew_vmin, arch, arch_suf, qwinver))
            return;
    }
    ret = system(buf);
    if (ret != 0)
    {
        printf(" Can not create NSIS installer\n");
        fflush(stdout);
    }

    printf("\n");
}

int main(int argc, char *argv[])
{
    char *prefix = NULL;
    char *host = "x86_64-w64-mingw32";
    char *arch =  NULL;;
    char *jobopt = "";
    char *winver = "win10";
    size_t len;
    int strip = 0;
    int nsis = 0;
    int verbose = 0;
    int efl = 0;
    int cleaning = 0;
    int ret;

    for (int i = 1; i < argc; i++)
    {
        if (strcmp(argv[i], "--help") == 0)
        {
            _ew_usage(argv[0]);
            exit(0);
        }
        if (strcmp(argv[i], "--version") == 0)
        {
            printf("Ewpi version %d.%d\n", _ew_vmaj, _ew_vmin);
            exit(0);
        }
        else if (strncmp(argv[i], "--prefix=", strlen("--prefix=")) == 0)
        {
            prefix = argv[i] + strlen("--prefix=");
        }
        else if (strncmp(argv[i], "--host=", strlen("--host=")) == 0)
        {
            char *opt;

            opt = argv[i] + strlen("--host=");
            if (strcmp(opt, "i686-w64-mingw32") == 0)
                host = "i686-w64-mingw32";
            else if (strcmp(opt, "x86_64-w64-mingw32") == 0)
                host = "x86_64-w64-mingw32";
            else
            {
                _ew_usage(argv[0]);
                exit(1);
            }
        }
        else if (strncmp(argv[i], "--arch=", strlen("--arch=")) == 0)
        {
            arch = argv[i] + strlen("--arch=");
        }
        else if (strncmp(argv[i], "--winver=", strlen("--winver=")) == 0)
        {
            char *opt;

            opt = argv[i] + strlen("--winver=");
            if (strcmp(opt, "win7") == 0)
                winver = "win7";
            else if (strcmp(opt, "win10") == 0)
                winver = "win10";
            else
            {
                _ew_usage(argv[0]);
                exit(1);
            }
        }
        else if (strcmp(argv[i], "--strip") == 0)
        {
            strip = 1;
        }
        else if (strcmp(argv[i], "--nsis") == 0)
        {
            nsis = 1;
        }
        else if (strcmp(argv[i], "--verbose") == 0)
        {
            verbose = 1;
        }
        else if (strcmp(argv[i], "--efl") == 0)
        {
            efl = 1;
        }
        else if (strncmp(argv[i], "--jobs=", strlen("--jobs=")) == 0)
        {
            jobopt = argv[i] + strlen("--jobs=");
        }
        else if (strcmp(argv[i], "--clean") == 0)
        {
            cleaning = 1;
        }
        else
        {
            _ew_usage(argv[0]);
            exit(1);
        }
    }

    if (!prefix)
    {
        char buf[PATH_MAX];
        const char *home = getenv("HOME");

        if (!home || !ew_path_join(buf, sizeof(buf), home,
                                   (strcmp(host, "i686-w64-mingw32") == 0) ?
                                   "ewpi_32" : "ewpi_64", NULL))
        {
            fprintf(stderr, "error: cannot determine default prefix (is HOME set?)\n");
            exit(1);
        }
        prefix = strdup(buf);
    }

    if (!arch)
    {
        if (strcmp(host, "i686-w64-mingw32") == 0)
            arch = "i686";
        else
            arch = "x86-64";
    }

    /* use slash in prefix, not backslash */
    {
        char *iter = prefix;
        while (*iter)
        {
            if (*iter == '\\') *iter = '/';
            iter++;
        }
    }

    /* remove possible trailing slash */
    len = strlen(prefix);
    if (prefix[len - 1] == '/')
        prefix[len - 1] = '\0';

    /* prefix must be absolute */
    if (!_ew_path_is_absolute(prefix))
    {
        printf("prefix must be an absolute path, exiting...\n");
        return 1;
    }

    printf(":: Configuration...\n");
    printf("  prefix:    %s\n", prefix);
    printf("  host:      %s\n", host);
    printf("  arch:      %s\n", arch);
    printf("  strip:     %s\n", strip ? "yes" : "no");
    printf("  installer: %s\n", nsis ? "yes" : "no");
    printf("  verbose:   %s\n", verbose ? "yes" : "no");
    printf("  efl:       %s\n", efl ? "yes" : "no");
    printf("  jobs:      %s\n", jobopt);
    printf("\n");
    fflush(stdout);

    printf(":: Checking requirements...\n");
    ret = _ew_requirements(host);
    if (!ret)
    {
        printf("one of the requirement is not found, exiting...\n");
        return 1;
    }

    printf(":: Prepare directories in %s...\n", prefix);
    _ew_packages_dir_set(prefix);

    _ew_packages_count_total();
    _ew_packages_get_git();
    _ew_packages_dst_set(prefix);

    printf(":: Build the dependency tree...\n");
    fflush(stdout);
    _ew_package_index = (int *)malloc((size_t)_ew_package_count_total * sizeof(int));
    if (!_ew_package_index)
        return 1;

    /* Build the dependency tree from every recipe found in packages/ by
     * default; the 'efl' meta-package is only pulled in when it was
     * explicitly requested with --efl. Previously the tree was rooted at
     * "efl" unconditionally, which made every run without --efl abort
     * with "error: unknown dependency 'efl'" as soon as the efl recipe
     * was not part of the loaded list (e.g. after it had been dropped,
     * or when the copy under $prefix/share/ewpi/packages was stale). */
    for (int i = 0; i < _ew_package_count_total; i++)
    {
        const Package *p = &_ewpi_pkgs[i];

        if (!p->name)
            continue;
        if (strcmp(p->name, "efl") == 0 && !efl)
            continue;
        _ew_packages_tree(p->name);
    }

    printf(":: Check which package is not installed...\n");
    fflush(stdout);
    /* Counters are recomputed here from the packages in the dependency
     * tree only (not from every recipe on disk). */
    _ew_packages_status_set();

    _ew_packages_not_installed_disp();
    _ew_packages_download();
    _ew_packages_longest_name();
    _ew_packages_extract(verbose);
    _ew_packages_install(prefix, host, arch, jobopt, verbose, winver);
    if (strip && !nsis)
        _ew_packages_strip(prefix, host);
    if (nsis)
    {
        _ew_packages_nsis(prefix, host, winver, efl);
    }
    if (cleaning)
        _ew_packages_clean();

    free(_ew_package_index);
    for (int i = 0; i < _ew_package_count_total; i++)
    {
        free(_ewpi_pkgs[i].name);
        free(_ewpi_pkgs[i].version);
        free(_ewpi_pkgs[i].url);
        free(_ewpi_pkgs[i].tarname);
        free(_ewpi_pkgs[i].sha512sum);
        for (int j = 0; j < _ewpi_pkgs[i].deps_count; j++)
            free(_ewpi_pkgs[i].deps[j]);
        free(_ewpi_pkgs[i].deps);
    }
    free(_ewpi_pkgs);

    return 0;
}
