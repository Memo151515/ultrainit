/*
 * UltraInit 1.2.0 - journalult.c
 * ult-journald gunlugunu okur (journalctl'in hafif karsiligi).
 *
 *   journalult                      son 100 kayit
 *   journalult -u sshd -n 50        sshd'nin son 50 kaydi   (-u ad*  : on ek eslesmesi)
 *   journalult -u ultrainit         init'in kendi logu
 *   journalult -p err               err ve daha ciddi (0-7 ya da emerg..debug)
 *   journalult -k                   yalnizca cekirdek mesajlari
 *   journalult --since 10m          son 10 dakika (30s, 2h, 1d, "2026-10-07 12:00")
 *   journalult -f                   canli takip
 *   journalult -o json | cat        cikti bicimi
 *   journalult -g hata              mesajda arama (buyuk/kucuk harf duyarsiz)
 *
 * Lisans: MFCL (bkz. LICENSE)
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <time.h>
#include <sys/stat.h>

typedef struct {
    long long ts;
    int       prio;
    char     *unit;
    int       pid;
    char     *msg;
    char     *buf;       /* sahip olunan bellek (unit/msg buraya isaret eder) */
} rec_t;

typedef struct { char *buf; size_t len, cap; } linebuf_t;

typedef enum { OUT_SHORT = 0, OUT_CAT, OUT_JSON } out_t;

static struct {
    long   n;                 /* 0 = hepsi */
    int    follow;
    char   unit[64];
    int    unit_prefix;
    int    max_prio;          /* -1: sinirsiz */
    int    kernel;
    int    init_log;
    int    list_units;
    long long since, until;   /* 0 = yok */
    char   grep[128];
    out_t  out;
    int    color;
} opt = { 100, 0, "", 0, -1, 0, 0, 0, 0, 0, "", OUT_SHORT, 0 };

static rec_t *g_ring = NULL;
static long   g_cap = 0, g_count = 0, g_head = 0;
static int    g_collecting = 1;      /* 1: halkaya topla, 0: dogrudan yazdir (follow) */
static char   g_units[512][64];
static int    g_nunits = 0;

static void usage(void) {
    fprintf(stderr,
        "UltraInit %s - journalult\n\n"
        "Kullanim: journalult [secenekler]\n"
        "  -n N | -n all     son N kayit (varsayilan 100)\n"
        "  -u BIRIM[*]       yalnizca bu birim ('ultrainit': init'in kendi logu)\n"
        "  -p SEVIYE         emerg|alert|crit|err|warning|notice|info|debug ya da 0-7\n"
        "  -k                yalnizca cekirdek mesajlari\n"
        "  -g METIN          mesajda ara (buyuk/kucuk harf duyarsiz)\n"
        "  --since ZAMAN     orn. 10m, 2h, 1d, today, \"2026-10-07 12:00\"\n"
        "  --until ZAMAN\n"
        "  -f                canli takip\n"
        "  -o short|cat|json cikti bicimi\n"
        "  --list-units      kayitli birimleri listele\n"
        "  --disk-usage      gunluklerin disk kullanimi\n", ULT_VERSION);
}

/* ---------------- yardimcilar ---------------- */

static int parse_prio(const char *s) {
    static const char *const names[] = { "emerg", "alert", "crit", "err", "warning", "notice", "info", "debug" };
    if (s[0] >= '0' && s[0] <= '7' && s[1] == '\0') return s[0] - '0';
    for (int i = 0; i < 8; i++) if (!strcasecmp(s, names[i])) return i;
    if (!strcasecmp(s, "error")) return 3;
    if (!strcasecmp(s, "warn")) return 4;
    return -1;
}

static int parse_time(const char *s, long long *out) {
    time_t now = time(NULL);
    if (!strcmp(s, "now")) { *out = (long long)now * 1000; return 0; }
    struct tm tmv;
    if (!strcmp(s, "today") || !strcmp(s, "yesterday")) {
        localtime_r(&now, &tmv);
        tmv.tm_hour = tmv.tm_min = tmv.tm_sec = 0;
        tmv.tm_isdst = -1;
        time_t t = mktime(&tmv);
        if (!strcmp(s, "yesterday")) t -= 86400;
        *out = (long long)t * 1000;
        return 0;
    }
    const char *p = s;
    if (*p == '-') p++;
    char *end;
    long v = strtol(p, &end, 10);
    if (end != p && v >= 0 && end[0] && !end[1]) {
        long mult = 0;
        switch (end[0]) { case 's': mult = 1; break; case 'm': mult = 60; break;
                          case 'h': mult = 3600; break; case 'd': mult = 86400; break;
                          case 'w': mult = 604800; break; default: break; }
        if (mult) { *out = ((long long)now - (long long)v * mult) * 1000; return 0; }
    }
    int Y, M, D, h = 0, m = 0, sec = 0;
    int n = sscanf(s, "%d-%d-%d %d:%d:%d", &Y, &M, &D, &h, &m, &sec);
    if (n >= 3) {
        memset(&tmv, 0, sizeof(tmv));
        tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
        tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = sec; tmv.tm_isdst = -1;
        time_t t = mktime(&tmv);
        if (t != (time_t)-1) { *out = (long long)t * 1000; return 0; }
    }
    return -1;
}

static int contains_ci(const char *hay, const char *needle) {
    if (!*needle) return 1;
    size_t nl = strlen(needle);
    for (; *hay; hay++) if (!strncasecmp(hay, needle, nl)) return 1;
    return 0;
}

/* "ts\tprio\tbirim\tpid\tmesaj" -> rec (satir yerinde bolunur) */
static int parse_rec(char *line, rec_t *r) {
    char *f[5];
    char *p = line;
    for (int i = 0; i < 4; i++) {
        char *tab = strchr(p, '\t');
        if (!tab) return -1;
        *tab = '\0';
        f[i] = p;
        p = tab + 1;
    }
    f[4] = p;
    r->ts = atoll(f[0]);
    r->prio = atoi(f[1]);
    r->unit = f[2];
    r->pid = atoi(f[3]);
    r->msg = f[4];
    return 0;
}

static int passes(const rec_t *r) {
    if (opt.kernel && strcmp(r->unit, "kernel") != 0) return 0;
    if (opt.unit[0] && !opt.init_log) {
        if (opt.unit_prefix) { if (strncmp(r->unit, opt.unit, strlen(opt.unit)) != 0) return 0; }
        else if (strcmp(r->unit, opt.unit) != 0) return 0;
    }
    if (opt.max_prio >= 0 && r->prio > opt.max_prio) return 0;
    if (opt.since && r->ts < opt.since) return 0;
    if (opt.until && r->ts > opt.until) return 0;
    if (opt.grep[0] && !contains_ci(r->msg, opt.grep)) return 0;
    return 1;
}

/* ---------------- cikti ---------------- */

static void print_rec(const rec_t *r) {
    if (opt.out == OUT_CAT) { printf("%s\n", r->msg); return; }
    if (opt.out == OUT_JSON) {
        printf("{\"timestamp_ms\":%lld,\"priority\":%d,\"unit\":\"%s\",\"pid\":%d,\"message\":\"",
               r->ts, r->prio, r->unit, r->pid);
        for (const unsigned char *c = (const unsigned char *)r->msg; *c; c++) {
            if (*c == '"' || *c == '\\') printf("\\%c", *c);
            else if (*c < 0x20) printf("\\u%04x", *c);
            else putchar(*c);
        }
        printf("\"}\n");
        return;
    }
    static const char *const mon[] = { "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec" };
    time_t t = (time_t)(r->ts / 1000);
    struct tm tmv;
    localtime_r(&t, &tmv);
    const char *c0 = "", *c1 = "";
    if (opt.color) {
        if (r->prio <= 3) { c0 = "\033[31m"; c1 = "\033[0m"; }
        else if (r->prio == 4) { c0 = "\033[33m"; c1 = "\033[0m"; }
    }
    if (r->pid > 0)
        printf("%s%s %02d %02d:%02d:%02d %s[%d]: %s%s\n", c0, mon[tmv.tm_mon % 12], tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min, tmv.tm_sec, r->unit, r->pid, r->msg, c1);
    else
        printf("%s%s %02d %02d:%02d:%02d %s: %s%s\n", c0, mon[tmv.tm_mon % 12], tmv.tm_mday,
               tmv.tm_hour, tmv.tm_min, tmv.tm_sec, r->unit, r->msg, c1);
}

/* ---------------- satir isleme ---------------- */

static void keep_rec(char *normalized) {
    char *dup = strdup(normalized);
    if (!dup) return;
    rec_t r;
    r.buf = dup;
    if (parse_rec(dup, &r) != 0 || !passes(&r)) { free(dup); return; }

    if (opt.n == 0) {                                   /* hepsi: buyuyen dizi */
        if (g_count == g_cap) {
            g_cap = g_cap ? g_cap * 2 : 1024;
            rec_t *nr = realloc(g_ring, (size_t)g_cap * sizeof(rec_t));
            if (!nr) { free(dup); return; }
            g_ring = nr;
        }
        g_ring[g_count++] = r;
    } else {                                            /* son N: dairesel */
        if (!g_ring) {
            g_cap = opt.n;
            g_ring = calloc((size_t)g_cap, sizeof(rec_t));
            if (!g_ring) { free(dup); return; }
        }
        if (g_count < g_cap) g_ring[g_count++] = r;
        else { free(g_ring[g_head].buf); g_ring[g_head] = r; g_head = (g_head + 1) % g_cap; }
    }
}

/* init'in kendi logu: "YYYY-MM-DD HH:MM:SS [TAG] mesaj" -> normal kayit */
static int convert_init_line(const char *line, char *out, size_t n) {
    int Y, M, D, h, m, s;
    char tag[8];
    int used = 0;
    if (sscanf(line, "%d-%d-%d %d:%d:%d [%7[^]]]%n", &Y, &M, &D, &h, &m, &s, tag, &used) != 7 || used == 0)
        return -1;
    struct tm tmv;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_year = Y - 1900; tmv.tm_mon = M - 1; tmv.tm_mday = D;
    tmv.tm_hour = h; tmv.tm_min = m; tmv.tm_sec = s; tmv.tm_isdst = -1;
    time_t t = mktime(&tmv);
    int prio = !strcmp(tag, "FAIL") ? 3 : (!strcmp(tag, "WARN") ? 4 : 6);
    const char *msg = line + used;
    while (*msg == ' ') msg++;
    snprintf(out, n, "%lld\t%d\tultrainit\t0\t%s", (long long)t * 1000, prio, msg);
    return 0;
}

static void add_unit_name(const char *u) {
    for (int i = 0; i < g_nunits; i++) if (!strcmp(g_units[i], u)) return;
    if (g_nunits < 512) snprintf(g_units[g_nunits++], 64, "%.63s", u);
}

static void handle_line(char *line, int is_init) {
    char conv[8300];
    char *use = line;
    if (is_init) {
        if (convert_init_line(line, conv, sizeof(conv)) != 0) return;
        use = conv;
    }
    if (opt.list_units) {
        char *dup = strdup(use);
        if (!dup) return;
        rec_t r;
        if (parse_rec(dup, &r) == 0 && passes(&r)) add_unit_name(r.unit);
        free(dup);
        return;
    }
    if (g_collecting) { keep_rec(use); return; }

    char *dup = strdup(use);
    if (!dup) return;
    rec_t r;
    r.buf = dup;
    if (parse_rec(dup, &r) == 0 && passes(&r)) { print_rec(&r); fflush(stdout); }
    free(dup);
}

/* fd'den okunabilen her seyi oku, tam satirlari isle, yarim satiri tamponda birak */
static void feed(int fd, linebuf_t *lb, int is_init) {
    char tmp[65536];
    for (;;) {
        ssize_t n = read(fd, tmp, sizeof(tmp));
        if (n <= 0) break;
        if (lb->len + (size_t)n + 1 > lb->cap) {
            size_t nc = lb->cap ? lb->cap * 2 : 131072;
            while (nc < lb->len + (size_t)n + 1) nc *= 2;
            char *nb = realloc(lb->buf, nc);
            if (!nb) return;
            lb->buf = nb; lb->cap = nc;
        }
        memcpy(lb->buf + lb->len, tmp, (size_t)n);
        lb->len += (size_t)n;

        size_t start = 0;
        for (size_t i = 0; i < lb->len; i++) {
            if (lb->buf[i] == '\n') {
                lb->buf[i] = '\0';
                handle_line(lb->buf + start, is_init);
                start = i + 1;
            }
        }
        if (start) { memmove(lb->buf, lb->buf + start, lb->len - start); lb->len -= start; }
        if (lb->len > (1u << 20)) lb->len = 0;           /* asiri uzun satir: at */
    }
}

/* ---------------- kaynaklar ---------------- */

static int cmp_desc(const void *a, const void *b) { return *(const int *)b - *(const int *)a; }

static int list_journal_files(int *idx, int max) {
    DIR *d = opendir(ULT_JOURNAL_DIR);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        int i;
        if (!strcmp(e->d_name, "journal.log")) idx[n++] = 0;
        else if (sscanf(e->d_name, "journal.%d.log", &i) == 1 && i > 0 && i < 1000) idx[n++] = i;
    }
    closedir(d);
    qsort(idx, (size_t)n, sizeof(int), cmp_desc);      /* en eskiden en yeniye */
    return n;
}

static int       g_fd = -1;
static linebuf_t g_lb = { NULL, 0, 0 };
static char      g_path[320];
static ino_t     g_ino = 0;

static void jpath(int idx, char *out, size_t n) {
    if (idx == 0) snprintf(out, n, "%s/journal.log", ULT_JOURNAL_DIR);
    else          snprintf(out, n, "%s/journal.%d.log", ULT_JOURNAL_DIR, idx);
}

static int open_follow(void) {
    int fd = open(g_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    if (fstat(fd, &st) == 0) g_ino = st.st_ino;
    return fd;
}

static int read_all_sources(void) {
    if (opt.init_log) {
        snprintf(g_path, sizeof(g_path), "%s", ULT_LOG_FILE);
        g_fd = open_follow();
        if (g_fd < 0) { fprintf(stderr, "%s okunamadi: %s\n", g_path, strerror(errno)); return -1; }
        feed(g_fd, &g_lb, 1);
        return 0;
    }
    int idx[1000];
    int n = list_journal_files(idx, 1000);
    if (n < 0) {
        if (errno == EACCES) fprintf(stderr, "izin yok: gunluk yalnizca root'a acik (sudo journalult ...)\n");
        else fprintf(stderr, "gunluk bulunamadi (%s): ult-journald calismiyor mu?\n", ULT_JOURNAL_DIR);
        return -1;
    }
    for (int k = 0; k < n; k++) {
        char p[320];
        jpath(idx[k], p, sizeof(p));
        int fd = open(p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) continue;
        if (idx[k] == 0) {                                  /* guncel dosya: takip icin acik tut */
            snprintf(g_path, sizeof(g_path), "%s", p);
            struct stat st;
            if (fstat(fd, &st) == 0) g_ino = st.st_ino;
            g_fd = fd;
            feed(fd, &g_lb, 0);
        } else {
            linebuf_t lb = { NULL, 0, 0 };
            feed(fd, &lb, 0);
            free(lb.buf);
            close(fd);
        }
    }
    if (g_fd < 0) jpath(0, g_path, sizeof(g_path));         /* henuz yoksa takipte aranir */
    return 0;
}

static void follow_loop(void) {
    g_collecting = 0;
    for (;;) {
        struct timespec ts = { 0, 200 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        struct stat st;
        int is_init = opt.init_log;
        if (stat(g_path, &st) == 0) {
            if (g_fd < 0) { g_fd = open_follow(); g_lb.len = 0; }
            else if (st.st_ino != g_ino || st.st_size < lseek(g_fd, 0, SEEK_CUR)) {
                feed(g_fd, &g_lb, is_init);                 /* eski dosyanin kalani */
                close(g_fd);
                g_fd = open_follow();
                g_lb.len = 0;
            }
        }
        if (g_fd >= 0) feed(g_fd, &g_lb, is_init);
    }
}

static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

int main(int argc, char **argv) {
    opt.color = isatty(STDOUT_FILENO) && !getenv("NO_COLOR");
    int disk_usage = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        const char *v = (i + 1 < argc) ? argv[i + 1] : NULL;
        if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(); return 0; }
        else if (!strcmp(a, "--version")) { printf("journalult %s\n", ULT_VERSION); return 0; }
        else if (!strcmp(a, "-f")) opt.follow = 1;
        else if (!strcmp(a, "-k")) opt.kernel = 1;
        else if (!strcmp(a, "--no-color")) opt.color = 0;
        else if (!strcmp(a, "--list-units")) opt.list_units = 1;
        else if (!strcmp(a, "--disk-usage")) disk_usage = 1;
        else if (!strcmp(a, "--init")) opt.init_log = 1;
        else if (!strcmp(a, "-n") && v) {
            i++;
            if (!strcmp(v, "all")) opt.n = 0;
            else { opt.n = atol(v); if (opt.n < 0) opt.n = 0; if (opt.n > 1000000) opt.n = 1000000; }
        }
        else if (!strcmp(a, "-u") && v) {
            i++;
            size_t l = strlen(v);
            if (l == 0 || l >= sizeof(opt.unit)) { fprintf(stderr, "gecersiz birim adi\n"); return 2; }
            snprintf(opt.unit, sizeof(opt.unit), "%s", v);
            if (opt.unit[l - 1] == '*') { opt.unit[l - 1] = '\0'; opt.unit_prefix = 1; }
            if (!strcmp(opt.unit, "ultrainit")) opt.init_log = 1;
        }
        else if (!strcmp(a, "-p") && v) {
            i++;
            opt.max_prio = parse_prio(v);
            if (opt.max_prio < 0) { fprintf(stderr, "gecersiz oncelik: %s\n", v); return 2; }
        }
        else if (!strcmp(a, "-g") && v) { i++; snprintf(opt.grep, sizeof(opt.grep), "%s", v); }
        else if (!strcmp(a, "-o") && v) {
            i++;
            if (!strcmp(v, "short")) opt.out = OUT_SHORT;
            else if (!strcmp(v, "cat")) opt.out = OUT_CAT;
            else if (!strcmp(v, "json")) opt.out = OUT_JSON;
            else { fprintf(stderr, "gecersiz cikti bicimi: %s\n", v); return 2; }
        }
        else if (!strcmp(a, "--since") && v) {
            i++;
            if (parse_time(v, &opt.since) != 0) { fprintf(stderr, "gecersiz zaman: %s\n", v); return 2; }
        }
        else if (!strcmp(a, "--until") && v) {
            i++;
            if (parse_time(v, &opt.until) != 0) { fprintf(stderr, "gecersiz zaman: %s\n", v); return 2; }
        }
        else { fprintf(stderr, "bilinmeyen secenek: %s\n", a); usage(); return 2; }
    }

    if (disk_usage) {
        int idx[1000];
        int n = list_journal_files(idx, 1000);
        if (n < 0) { fprintf(stderr, "gunluk okunamadi: %s\n", strerror(errno)); return 1; }
        long long total = 0;
        for (int k = 0; k < n; k++) {
            char p[320];
            struct stat st;
            jpath(idx[k], p, sizeof(p));
            if (stat(p, &st) == 0) total += st.st_size;
        }
        printf("%lld KiB (%d dosya)\n", total / 1024, n);
        return 0;
    }

    if (read_all_sources() != 0) return 1;

    if (opt.list_units) {
        qsort(g_units, (size_t)g_nunits, sizeof(g_units[0]), cmp_str);
        for (int i = 0; i < g_nunits; i++) puts(g_units[i]);
        return 0;
    }

    long start = (opt.n != 0 && g_count == g_cap) ? g_head : 0;
    for (long i = 0; i < g_count; i++) print_rec(&g_ring[(start + i) % (g_cap ? g_cap : 1)]);
    fflush(stdout);

    if (opt.follow) follow_loop();
    return 0;
}
