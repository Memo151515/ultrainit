/*
 * UltraInit 1.2.0 - unit.c
 * Lisans: MFCL (bkz. LICENSE)
 */
#include "unit.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

#define UNIT_FILE_MAX 16384

static char *trim(char *s) {
    while (*s == ' ' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n > 0 && (s[n-1] == ' ' || s[n-1] == '\t' || s[n-1] == '\r' || s[n-1] == '\n'))
        s[--n] = '\0';
    return s;
}

static void dlist_add(dlist_t *l, const char *name) {
    if (!ult_valid_name(name) || l->n >= ULT_MAX_DEPS) return;
    for (int i = 0; i < l->n; i++)
        if (strcmp(l->v[i], name) == 0) return;
    snprintf(l->v[l->n++], ULT_NAME_LEN, "%s", name);
}

static void add_words(dlist_t *l, char *value) {
    char *sp = NULL;
    for (char *t = strtok_r(value, " \t,", &sp); t; t = strtok_r(NULL, " \t,", &sp))
        dlist_add(l, t);
}

const char *unit_restart_name(restart_t r) {
    return r == RS_ALWAYS ? "always" : (r == RS_ON_FAILURE ? "on-failure" : "no");
}

/* %i -> ornek adi, %n -> tam ad */
static void substitute(const char *in, char *out, size_t outlen,
                       const char *inst, const char *name) {
    size_t o = 0;
    for (; *in && o + 1 < outlen; in++) {
        if (in[0] == '%' && (in[1] == 'i' || in[1] == 'n')) {
            const char *rep = (in[1] == 'i') ? inst : name;
            in++;
            while (*rep && o + 1 < outlen) out[o++] = *rep++;
        } else {
            out[o++] = *in;
        }
    }
    out[o] = '\0';
}

/* Asiri uzun degerleri sessizce kirpmak yerine reddeder. */
static int setstr(char *dst, size_t n, const char *src, const char *unit, const char *key) {
    if (strlen(src) >= n) {
        ult_log(ULT_LOG_ERR, "%s: '%s' degeri cok uzun (en fazla %zu karakter)", unit, key, n - 1);
        return -1;
    }
    memcpy(dst, src, strlen(src) + 1);
    return 0;
}
#define SETSTR(field) do { if (setstr(field, sizeof(field), val, u->name, key) != 0) return -1; } while (0)

static int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* ---------------- Yerel .svc birimi ---------------- */

static int parse_svc(char *text, unit_def_t *u, const char *inst) {
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = trim(line);
        if (*t == '\0' || *t == '#' || *t == ';') continue;
        char *eq = strchr(t, '=');
        if (!eq) continue;
        *eq = '\0';
        char *key = trim(t);
        char val[512];
        substitute(trim(eq + 1), val, sizeof(val), inst, u->name);

        if      (!strcmp(key, "description")) SETSTR(u->description);
        else if (!strcmp(key, "exec"))        SETSTR(u->exec);
        else if (!strcmp(key, "stop_exec"))   SETSTR(u->stop_exec);
        else if (!strcmp(key, "pidfile"))     SETSTR(u->pidfile);
        else if (!strcmp(key, "ready_path"))  SETSTR(u->ready_path);
        else if (!strcmp(key, "user"))        SETSTR(u->user);
        else if (!strcmp(key, "group"))       SETSTR(u->group);
        else if (!strcmp(key, "workdir"))     SETSTR(u->workdir);
        else if (!strcmp(key, "log"))         SETSTR(u->log);
        else if (!strcmp(key, "type")) {
            if      (!strcmp(val, "simple"))  u->type = UT_SIMPLE;
            else if (!strcmp(val, "forking")) u->type = UT_FORKING;
            else if (!strcmp(val, "oneshot")) u->type = UT_ONESHOT;
            else { ult_log(ULT_LOG_ERR, "%s: gecersiz type '%s'", u->name, val); return -1; }
        } else if (!strcmp(key, "restart")) {
            if      (!strcmp(val, "no"))         u->restart = RS_NO;
            else if (!strcmp(val, "on-failure")) u->restart = RS_ON_FAILURE;
            else if (!strcmp(val, "always"))     u->restart = RS_ALWAYS;
            else { ult_log(ULT_LOG_ERR, "%s: gecersiz restart '%s'", u->name, val); return -1; }
        }
        else if (!strcmp(key, "restart_sec"))    u->restart_sec   = clamp(atoi(val), 0, 3600);
        else if (!strcmp(key, "timeout_start"))  u->timeout_start = clamp(atoi(val), 0, 3600);
        else if (!strcmp(key, "timeout_stop"))   u->timeout_stop  = clamp(atoi(val), 1, 3600);
        else if (!strcmp(key, "umask"))          u->umask_val     = (int)(strtol(val, NULL, 8) & 0777);
        else if (!strcmp(key, "env")) {
            if (u->nenv < ULT_MAX_ENV && strchr(val, '='))
                { SETSTR(u->env[u->nenv]); u->nenv++; }
        }
        else if (!strcmp(key, "need"))      add_words(&u->need, val);
        else if (!strcmp(key, "want"))      add_words(&u->want, val);
        else if (!strcmp(key, "after"))     add_words(&u->after, val);
        else if (!strcmp(key, "before"))    add_words(&u->before, val);
        else if (!strcmp(key, "conflicts")) add_words(&u->conflicts, val);
        else if (!strcmp(key, "provides"))  add_words(&u->provides, val);
        else ult_log(ULT_LOG_WARN, "%s: bilinmeyen anahtar '%s' yoksayildi", u->name, key);
    }

    if (u->exec[0] != '/') {
        ult_log(ULT_LOG_ERR, "%s: exec mutlak yol olmali ('%s')", u->name, u->exec);
        return -1;
    }
    if (u->stop_exec[0] && u->stop_exec[0] != '/') {
        ult_log(ULT_LOG_ERR, "%s: stop_exec mutlak yol olmali", u->name);
        return -1;
    }
    if (strcmp(u->log, "null") && strcmp(u->log, "console") &&
        strcmp(u->log, "file") && strcmp(u->log, "journal")) {
        ult_log(ULT_LOG_ERR, "%s: gecersiz log '%s' (null|console|file|journal)", u->name, u->log);
        return -1;
    }
    if (u->ready_path[0] && (u->ready_path[0] != '/' || u->type != UT_SIMPLE)) {
        ult_log(ULT_LOG_ERR, "%s: ready_path mutlak yol olmali ve yalnizca type=simple ile kullanilir", u->name);
        return -1;
    }
    /* log=journal: journald hazir olduktan SONRA baslat (yoksa dosyaya duser) */
    if (!strcmp(u->log, "journal") && strcmp(u->name, ULT_JOURNAL_UNIT) != 0) {
        dlist_add(&u->want, ULT_JOURNAL_UNIT);
        dlist_add(&u->after, ULT_JOURNAL_UNIT);
    }
    if (u->pidfile[0] && u->pidfile[0] != '/') {
        ult_log(ULT_LOG_ERR, "%s: pidfile mutlak yol olmali", u->name);
        return -1;
    }
    return 0;
}

/* ---------------- Eski tip betik: depend() statik okuma ---------------- */

static void dep_stmt(unit_def_t *u, char *stmt) {
    char *sp = NULL;
    char *w = strtok_r(stmt, " \t", &sp);
    if (!w) return;
    char *rest = sp ? sp : (char *)"";
    if      (!strcmp(w, "need"))    add_words(&u->need, rest);
    else if (!strcmp(w, "want") || !strcmp(w, "use")) add_words(&u->want, rest);
    else if (!strcmp(w, "after"))   add_words(&u->after, rest);
    else if (!strcmp(w, "before"))  add_words(&u->before, rest);
    else if (!strcmp(w, "provide")) add_words(&u->provides, rest);
    else if (!strcmp(w, "conflict") || !strcmp(w, "conflicts")) add_words(&u->conflicts, rest);
}

/*
 * HIZ + GUVENLIK: depend() blogu betik CALISTIRILMADAN okunur. Boot sirasinda
 * her servis icin ayri bir shell calistirmaya gerek kalmaz.
 */
static void parse_script_deps(char *text, unit_def_t *u) {
    int in = 0;
    char *save = NULL;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *t = trim(line);
        char *hash = strchr(t, '#');
        if (hash) { *hash = '\0'; t = trim(t); }
        if (!in) {
            if (strncmp(t, "depend()", 8) != 0) continue;
            char *brace = strchr(t, '{');
            if (!brace) { in = 1; continue; }
            char *close = strchr(brace, '}');
            char *body = brace + 1;
            if (close) *close = '\0'; else in = 1;
            char *sp = NULL;
            for (char *s = strtok_r(body, ";", &sp); s; s = strtok_r(NULL, ";", &sp))
                dep_stmt(u, trim(s));
            continue;
        }
        if (*t == '}') { in = 0; continue; }
        size_t n = strlen(t);
        if (n && t[n-1] == ';') t[n-1] = '\0';
        dep_stmt(u, t);
    }
}

/* ---------------- Yukleme ---------------- */

static int read_fd(int fd, char *buf, size_t max) {
    size_t len = 0;
    for (;;) {
        ssize_t n = read(fd, buf + len, max - 1 - len);
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        if (n == 0) break;
        len += (size_t)n;
        if (len >= max - 1) return -1;   /* cok buyuk */
    }
    buf[len] = '\0';
    return 0;
}

int unit_load(const char *name, unit_def_t *u) {
    if (!ult_valid_name(name)) return -1;

    memset(u, 0, sizeof(*u));
    snprintf(u->name, sizeof(u->name), "%s", name);
    u->type = UT_SIMPLE;
    u->restart = RS_NO;
    u->restart_sec = 1;
    u->timeout_start = 30;
    u->timeout_stop = 10;
    u->umask_val = -1;
    snprintf(u->log, sizeof(u->log), "file");

    char path[256], inst[ULT_NAME_LEN] = "";
    int native = 0;

    snprintf(path, sizeof(path), "%s/%s.svc", ULT_UNITS_DIR, name);
    if (access(path, F_OK) == 0) {
        native = 1;
    } else {
        const char *at = strchr(name, '@');
        if (at && at[1]) {
            snprintf(inst, sizeof(inst), "%s", at + 1);
            snprintf(path, sizeof(path), "%s/%.*s@.svc", ULT_UNITS_DIR, (int)(at - name), name);
            if (access(path, F_OK) == 0) native = 1;
        }
    }
    if (!native) {
        snprintf(path, sizeof(path), "%s/%s", ULT_INITD_DIR, name);
        if (access(path, F_OK) != 0) return -1;
        u->is_script = 1;
        /* eski tip betikler: oneshot gibi baslar, pidfile varsa denetlenir */
        u->restart = RS_ALWAYS;
        u->timeout_start = 60;
        u->timeout_stop = 20;
    }
    snprintf(u->path, sizeof(u->path), "%s", path);

    int fd = ult_open_verified(path);
    if (fd < 0) return -2;
    char *text = malloc(UNIT_FILE_MAX);
    if (!text) { close(fd); return -2; }
    int rc = read_fd(fd, text, UNIT_FILE_MAX);
    close(fd);
    if (rc != 0) {
        ult_log(ULT_LOG_ERR, "%s: dosya okunamadi ya da cok buyuk", path);
        free(text);
        return -2;
    }

    rc = native ? parse_svc(text, u, inst) : (parse_script_deps(text, u), 0);
    free(text);
    return rc == 0 ? 0 : -2;
}
