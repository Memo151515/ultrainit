/*
 * UltraInit 1.2.0 - common.c
 * Lisans: MFCL (bkz. LICENSE)
 */
#include "common.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>

static FILE *ult_logfp = NULL;
static int   ult_log_disabled = 0;

/*
 * Log dosyasi O_NOFOLLOW ile acilir: /var/log/ultrainit.log bir symlink'e
 * cevrilmisse sessizce takip edilmez, dosya loglama o oturum icin kapatilir
 * (stderr'e yazma her zaman devam eder).
 */
static void ult_open_log(void) {
    if (ult_logfp || ult_log_disabled) return;

    int fd = open(ULT_LOG_FILE, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd < 0) {
        ult_log_disabled = 1;
        if (errno == ELOOP)
            fprintf(stderr, "\033[31m[FAIL]\033[0m guvenlik: %s bir symlink, dosya loglama devre disi\n",
                    ULT_LOG_FILE);
        return;
    }
    ult_logfp = fdopen(fd, "a");
    if (!ult_logfp) close(fd);
}

void ult_log(ult_log_level_t level, const char *fmt, ...) {
    ult_open_log();

    const char *tag, *color;
    switch (level) {
        case ULT_LOG_OK:   tag = " OK "; color = "\033[32m"; break;
        case ULT_LOG_WARN: tag = "WARN"; color = "\033[33m"; break;
        case ULT_LOG_ERR:  tag = "FAIL"; color = "\033[31m"; break;
        default:           tag = "INFO"; color = "\033[36m"; break;
    }

    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    char timebuf[32];
    strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M:%S", &tmv);

    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    fprintf(stderr, "%s[%s]\033[0m %s\n", color, tag, msg);
    if (ult_logfp) {
        fprintf(ult_logfp, "%s [%s] %s\n", timebuf, tag, msg);
        fflush(ult_logfp);
    }
}

/* ---------------- Guvenlik dogrulamasi ---------------- */

static int dir_is_safe(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return 0;
    if (st.st_uid != 0) return 0;
    /* grup/diger yazabiliyorsa sadece sticky bit varsa kabul (orn. /tmp) */
    if ((st.st_mode & (S_IWGRP | S_IWOTH)) && !(st.st_mode & S_ISVTX)) return 0;
    return 1;
}

static int ancestors_safe(const char *path) {
    char buf[PATH_MAX];
    if (path[0] != '/') return 0;
    snprintf(buf, sizeof(buf), "%s", path);
    for (;;) {
        char *slash = strrchr(buf, '/');
        if (!slash) return 0;
        if (slash == buf) {
            buf[1] = '\0';
            return dir_is_safe(buf);
        }
        *slash = '\0';
        if (!dir_is_safe(buf)) {
            ult_log(ULT_LOG_ERR, "guvenlik: dizin guvensiz (root'a ait degil ya da grup/diger yazabilir): %s", buf);
            return 0;
        }
    }
}

int ult_open_verified(const char *path) {
    if (!ancestors_safe(path)) return -1;

    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        if (errno == ELOOP)
            ult_log(ULT_LOG_ERR, "guvenlik: '%s' bir sembolik link, reddedildi", path);
        else if (errno != ENOENT)
            ult_log(ULT_LOG_ERR, "guvenlik: '%s' acilamadi (%s)", path, strerror(errno));
        return -1;
    }
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        ult_log(ULT_LOG_ERR, "guvenlik: '%s' normal bir dosya degil", path);
        close(fd); return -1;
    }
    if (st.st_uid != 0) {
        ult_log(ULT_LOG_ERR, "guvenlik: '%s' root'a ait degil (uid=%d)", path, (int)st.st_uid);
        close(fd); return -1;
    }
    if (st.st_mode & (S_IWGRP | S_IWOTH)) {
        ult_log(ULT_LOG_ERR, "guvenlik: '%s' grup/diger tarafindan yazilabilir, reddedildi", path);
        close(fd); return -1;
    }
    return fd;
}

int ult_verify_safe_path(const char *path) {
    int fd = ult_open_verified(path);
    if (fd < 0) return -1;
    close(fd);
    return 0;
}

int ult_valid_name(const char *name) {
    size_t n = name ? strlen(name) : 0;
    if (n == 0 || n > 63) return 0;
    if (name[0] == '.' || name[0] == '-') return 0;
    if (strstr(name, "..")) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '_' || c == '.' || c == '@' || c == ':' || c == '-'))
            return 0;
    }
    return 1;
}

void ult_pidfile_path(const char *name, char *out, size_t outlen) {
    snprintf(out, outlen, "%s/%s.pid", ULT_RUN_DIR, name);
}

/*
 * ESKI HATA DUZELTMESI: eski surum statik bir tamponun isaretcisini
 * donduruyordu; ikinci cagri ilk degeri eziyordu (default_runlevel
 * "tty1,tty2" olabiliyordu). Artik cagiran tampon verir.
 */
int ult_conf_get(const char *file, const char *key, char *out, size_t outlen) {
    FILE *fp = fopen(file, "re");
    if (!fp) return -1;

    char line[1024];
    int found = -1;
    size_t keylen = strlen(key);

    while (fgets(line, sizeof(line), fp)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\0') continue;
        if (strncmp(p, key, keylen) == 0 && p[keylen] == '=') {
            char *v = p + keylen + 1;
            size_t len = strlen(v);
            while (len > 0 && (v[len-1] == '\n' || v[len-1] == '\r' || v[len-1] == ' ' || v[len-1] == '\t'))
                v[--len] = '\0';
            snprintf(out, outlen, "%s", v);
            found = 0;
            break;
        }
    }
    fclose(fp);
    return found;
}

void ult_sanitize_environment(void) {
    clearenv();
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("HOME", "/root", 1);
    setenv("SHELL", "/bin/sh", 1);
    setenv("ULTRAINIT_VERSION", ULT_VERSION, 1);
}

int ult_split_args(char *s, char **argv, int max) {
    int n = 0;
    char *p = s;
    while (*p && n < max - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char *o = p;
        argv[n++] = o;
        char q = 0;
        while (*p) {
            if (q) {
                if (*p == q) { q = 0; p++; continue; }
                *o++ = *p++;
            } else if (*p == '"' || *p == '\'') {
                q = *p++;
            } else if (*p == ' ' || *p == '\t') {
                p++;
                break;
            } else {
                *o++ = *p++;
            }
        }
        *o = '\0';
    }
    argv[n] = NULL;
    return n;
}

/*
 * Servis adi ult_valid_name ile dogrulandigi icin (tirnak/slash icermez)
 * betik yolu tek tirnak icinde guvenle gomulebilir.
 */
void ult_build_wrapper(char *out, size_t outlen, const char *script,
                       const char *action, const char *name) {
    char pf[256];
    ult_pidfile_path(name, pf, sizeof(pf));
    snprintf(out, outlen,
        "ult_log(){ echo \"[ultrainit] $*\" >&2; }; "
        "ult_ok(){ echo \"[  ok  ] $*\" >&2; }; "
        "ult_fail(){ echo \"[ fail ] $*\" >&2; }; "
        "ULT_SERVICE='%s'; ULT_PIDFILE='%s'; ULT_RUN_DIR='%s'; "
        "export ULT_SERVICE ULT_PIDFILE ULT_RUN_DIR; "
        ". '%s'; "
        "if command -v %s >/dev/null 2>&1; then %s; else "
        "echo \"[ultrainit] '%s' bu betikte tanimli degil\" >&2; exit 3; fi",
        name, pf, ULT_RUN_DIR, script, action, action, action);
}

double ult_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}
