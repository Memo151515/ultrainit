/*
 * UltraInit 1.2.0 - journald.c  (ult-journald)
 *
 * Hafif merkezi log toplayici. Uc kaynagi tek, zaman damgali, oncelikli ve
 * dondurulen (rotasyonlu) bir METIN gunlugune toplar:
 *
 *   1) Servis ciktisi : UltraInit, log=journal olan servislerin stdout/stderr'ini
 *                       dogrudan bu daemon'in akis soketine baglar. PID, SO_PEERCRED
 *                       ile cekirdekten alinir (servis yalan soyleyemez).
 *   2) syslog         : /dev/log (datagram) - syslog(3), logger(1) vb.
 *   3) Cekirdek       : /dev/kmsg (yeniden baslatmada tekrar kaydetmez).
 *
 * Kayit bicimi (satir basina bir kayit, sekmeyle ayrilmis):
 *     <epoch_ms> \t <oncelik 0-7> \t <birim> \t <pid> \t <mesaj>
 * Mesajdaki kontrol karakterleri ('?' ile degistirilir) terminal kacis dizisi
 * enjeksiyonunu engeller. Okumak icin: journalult.
 *
 * Lisans: MFCL (bkz. LICENSE)
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/signalfd.h>

#define MAX_CLIENTS   256
#define LINE_CAP      4096
#define OBUF_SIZE     65536

typedef struct {
    int    fd;
    int    got_header;
    int    prio;
    pid_t  pid;
    char   unit[64];
    char   buf[LINE_CAP];
    size_t len;
} client_t;

static client_t *g_cl[MAX_CLIENTS];

static long g_max_file = 4096L * 1024;   /* bayt */
static int  g_max_files = 8;
static int  g_no_kmsg = 0;

static int    g_cur_fd = -1;
static off_t  g_cur_size = 0;
static char   g_obuf[OBUF_SIZE];
static size_t g_olen = 0;
static int    g_write_err_reported = 0;

static int    g_kfd = -1;
static unsigned long long g_last_seq = 0;
static int    g_have_seq = 0;
static int    g_seq_dirty = 0;
static double g_seq_saved_ms = 0;
static char   g_boot_id[64] = "";

/* ---------------- yardimcilar ---------------- */

static long long realtime_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static long long monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void journal_path(int idx, char *out, size_t n) {
    if (idx == 0) snprintf(out, n, "%s/journal.log", ULT_JOURNAL_DIR);
    else          snprintf(out, n, "%s/journal.%d.log", ULT_JOURNAL_DIR, idx);
}

/* Birim adi: yalnizca [A-Za-z0-9_.@:-]; digerleri '_' olur */
static void clean_unit(char *dst, size_t n, const char *src) {
    size_t o = 0;
    for (; *src && o + 1 < n && o < 63; src++) {
        char c = *src;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                 c == '_' || c == '.' || c == '@' || c == ':' || c == '-';
        dst[o++] = ok ? c : '_';
    }
    dst[o] = '\0';
    if (o == 0) snprintf(dst, n, "unknown");
}

/* ---------------- yazim / rotasyon ---------------- */

static int open_current(void) {
    char p[300];
    journal_path(0, p, sizeof(p));
    int fd = open(p, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0640);
    if (fd < 0) return -1;
    struct stat st;
    g_cur_size = (fstat(fd, &st) == 0) ? st.st_size : 0;
    g_cur_fd = fd;
    return 0;
}

static void rotate(void) {
    if (g_cur_fd >= 0) { close(g_cur_fd); g_cur_fd = -1; }
    char a[300], b[300];
    journal_path(g_max_files - 1, a, sizeof(a));
    unlink(a);
    for (int i = g_max_files - 2; i >= 0; i--) {
        journal_path(i, a, sizeof(a));
        journal_path(i + 1, b, sizeof(b));
        rename(a, b);
    }
    open_current();
}

static void flush_out(void) {
    if (g_olen == 0) return;
    if (g_cur_fd < 0 && open_current() != 0) {
        if (!g_write_err_reported) {
            fprintf(stderr, "ult-journald: gunluk dosyasi acilamadi: %s\n", strerror(errno));
            g_write_err_reported = 1;
        }
        g_olen = 0;     /* diske yazamiyoruz: tamponu at, bellegi sisirme */
        return;
    }
    size_t off = 0;
    while (off < g_olen) {
        ssize_t n = write(g_cur_fd, g_obuf + off, g_olen - off);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (!g_write_err_reported) {
                fprintf(stderr, "ult-journald: yazma hatasi: %s\n", strerror(errno));
                g_write_err_reported = 1;
            }
            break;
        }
        off += (size_t)n;
    }
    g_cur_size += (off_t)off;
    g_olen = 0;
    if (g_cur_size >= g_max_file) rotate();
}

static void emit(long long ts_ms, int prio, const char *unit, int pid, const char *msg, size_t mlen) {
    while (mlen > 0 && (msg[mlen - 1] == '\r' || msg[mlen - 1] == '\n')) mlen--;
    if (mlen > LINE_CAP) mlen = LINE_CAP;

    char hdr[128];
    int hl = snprintf(hdr, sizeof(hdr), "%lld\t%d\t%.63s\t%d\t", ts_ms, prio & 7, unit, pid);
    if (g_olen + (size_t)hl + mlen + 1 > sizeof(g_obuf)) flush_out();

    memcpy(g_obuf + g_olen, hdr, (size_t)hl);
    g_olen += (size_t)hl;
    for (size_t i = 0; i < mlen; i++) {
        unsigned char c = (unsigned char)msg[i];
        if (c == '\t') c = ' ';
        else if (c < 0x20 || c == 0x7f) c = '?';
        g_obuf[g_olen++] = (char)c;
    }
    g_obuf[g_olen++] = '\n';
}

/* ---------------- kmsg durumu (yeniden baslatmada tekrar yok) ---------------- */

static void state_path(char *out, size_t n) { snprintf(out, n, "%s/kmsg.state", ULT_JOURNAL_DIR); }

static void load_state(void) {
    FILE *fp = fopen("/proc/sys/kernel/random/boot_id", "re");
    if (fp) {
        if (fgets(g_boot_id, sizeof(g_boot_id), fp)) g_boot_id[strcspn(g_boot_id, "\r\n")] = '\0';
        fclose(fp);
    }
    char p[300];
    state_path(p, sizeof(p));
    fp = fopen(p, "re");
    if (!fp) return;
    char id[64] = "";
    unsigned long long seq = 0;
    if (fscanf(fp, "%63s %llu", id, &seq) == 2 && g_boot_id[0] && !strcmp(id, g_boot_id)) {
        g_last_seq = seq;
        g_have_seq = 1;
    }
    fclose(fp);
}

static void save_state(int force) {
    if (!g_seq_dirty || !g_boot_id[0]) return;
    double now = (double)monotonic_ms();
    if (!force && now - g_seq_saved_ms < 1000) return;
    char p[300], tmp[320];
    state_path(p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *fp = fopen(tmp, "we");
    if (!fp) return;
    fprintf(fp, "%s %llu\n", g_boot_id, g_last_seq);
    fclose(fp);
    chmod(tmp, 0640);
    rename(tmp, p);
    g_seq_dirty = 0;
    g_seq_saved_ms = now;
}

/* ---------------- kaynak: akis (servis stdout/stderr) ---------------- */

static void client_free(int i) {
    if (!g_cl[i]) return;
    close(g_cl[i]->fd);
    free(g_cl[i]);
    g_cl[i] = NULL;
}

static void client_lines(client_t *c) {
    for (;;) {
        char *nl = memchr(c->buf, '\n', c->len);
        size_t linelen;
        if (nl) linelen = (size_t)(nl - c->buf);
        else if (c->len >= LINE_CAP) linelen = c->len;          /* cok uzun satir: parcala */
        else break;

        emit(realtime_ms(), c->prio, c->unit, (int)c->pid, c->buf, linelen);
        size_t consumed = nl ? linelen + 1 : linelen;
        memmove(c->buf, c->buf + consumed, c->len - consumed);
        c->len -= consumed;
    }
}

static void client_read(int i) {
    client_t *c = g_cl[i];
    ssize_t n = read(c->fd, c->buf + c->len, LINE_CAP - c->len);
    if (n < 0) {
        if (errno == EAGAIN || errno == EINTR) return;
        n = 0;
    }
    if (n == 0) {                                   /* EOF: kalan yarim satiri da yaz */
        if (c->got_header && c->len > 0) emit(realtime_ms(), c->prio, c->unit, (int)c->pid, c->buf, c->len);
        client_free(i);
        return;
    }
    c->len += (size_t)n;

    if (!c->got_header) {
        char *nl = memchr(c->buf, '\n', c->len);
        if (!nl) {
            if (c->len >= 128) client_free(i);      /* gecersiz istemci */
            return;
        }
        *nl = '\0';
        char name[96] = "";
        int prio = 6;
        if (sscanf(c->buf, "UNIT %95s %d", name, &prio) < 1) { client_free(i); return; }
        clean_unit(c->unit, sizeof(c->unit), name);
        c->prio = (prio < 0 || prio > 7) ? 6 : prio;
        c->got_header = 1;
        size_t consumed = (size_t)(nl - c->buf) + 1;
        memmove(c->buf, c->buf + consumed, c->len - consumed);
        c->len -= consumed;
    }
    client_lines(c);
}

static void accept_clients(int lfd) {
    for (;;) {
        int fd = accept4(lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) if (!g_cl[i]) { slot = i; break; }
        if (slot < 0) { close(fd); continue; }
        client_t *c = calloc(1, sizeof(*c));
        if (!c) { close(fd); continue; }
        struct ucred cr;
        socklen_t l = sizeof(cr);
        c->fd = fd;
        c->pid = (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) == 0) ? cr.pid : 0;
        g_cl[slot] = c;
    }
}

/* ---------------- kaynak: syslog ---------------- */

static void handle_syslog_dgram(char *msg, size_t len, pid_t cred_pid) {
    while (len > 0 && (msg[len - 1] == '\n' || msg[len - 1] == '\0')) len--;
    if (len == 0) return;
    msg[len] = '\0';

    int pri = 13;                                   /* user.notice */
    char *p = msg;
    if (*p == '<') {
        char *e = strchr(p, '>');
        if (e && e - p <= 5) { pri = atoi(p + 1); p = e + 1; }
    }
    /* RFC3164 zaman damgasi: "Mmm dd hh:mm:ss " */
    if (strlen(p) >= 16 && p[3] == ' ' && p[6] == ' ' && p[9] == ':' && p[12] == ':') p += 16;

    char tag[64] = "";
    int pid = (int)cred_pid;          /* cekirdek tarafindan dogrulanmis */
    char *q = p;
    size_t t = 0;
    while (*q && *q != ':' && *q != '[' && *q != ' ' && t < sizeof(tag) - 1) tag[t++] = *q++;
    tag[t] = '\0';
    if (t > 0 && (*q == ':' || *q == '[')) {            /* gecerli etiket */
        if (*q == '[') {
            int v = atoi(q + 1);
            if (v > 0 && pid == 0) pid = v;   /* iddia edilen PID yalnizca kimlik bilgisi yoksa */
            char *e = strchr(q, ']');
            q = e ? e + 1 : q + strlen(q);
        }
        if (*q == ':') q++;
        while (*q == ' ') q++;
        p = q;
    } else {
        tag[0] = '\0';                                  /* etiketsiz: hepsi mesaj */
    }

    char unit[64];
    clean_unit(unit, sizeof(unit), tag[0] ? tag : "syslog");
    long long now = realtime_ms();
    char *save = NULL;
    for (char *line = strtok_r(p, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        emit(now, (pri < 0 ? 5 : pri) & 7, unit, pid, line, strlen(line));
}

static void syslog_read(int sfd) {
    for (int i = 0; i < 256; i++) {
        char buf[8192], ctl[256];
        struct iovec iov = { buf, sizeof(buf) - 1 };
        struct msghdr mh;
        memset(&mh, 0, sizeof(mh));
        mh.msg_iov = &iov; mh.msg_iovlen = 1;
        mh.msg_control = ctl; mh.msg_controllen = sizeof(ctl);
        ssize_t n = recvmsg(sfd, &mh, MSG_DONTWAIT);
        if (n < 0) return;
        buf[n] = '\0';
        pid_t pid = 0;
        for (struct cmsghdr *cm = CMSG_FIRSTHDR(&mh); cm; cm = CMSG_NXTHDR(&mh, cm)) {
            if (cm->cmsg_level == SOL_SOCKET && cm->cmsg_type == SCM_CREDENTIALS) {
                struct ucred cr;
                memcpy(&cr, CMSG_DATA(cm), sizeof(cr));
                pid = cr.pid;
            }
        }
        handle_syslog_dgram(buf, (size_t)n, pid);
    }
}

/* ---------------- kaynak: cekirdek (/dev/kmsg) ---------------- */

static void kmsg_read(void) {
    long long offset = realtime_ms() - monotonic_ms();      /* kmsg zamani CLOCK_MONOTONIC */
    for (int guard = 0; guard < 4096; guard++) {
        char buf[8192];
        ssize_t n = read(g_kfd, buf, sizeof(buf) - 1);
        if (n < 0) {
            if (errno == EPIPE || errno == EINVAL) continue;    /* tampon tasti / kisa tampon */
            break;                                              /* EAGAIN vb. */
        }
        buf[n] = '\0';
        char *semi = strchr(buf, ';');
        if (!semi) continue;
        *semi = '\0';
        unsigned pri;
        unsigned long long seq, ts;
        if (sscanf(buf, "%u,%llu,%llu", &pri, &seq, &ts) != 3) continue;
        if (g_have_seq && seq <= g_last_seq) continue;           /* onceden kaydedilmis */
        g_last_seq = seq; g_have_seq = 1; g_seq_dirty = 1;

        char *msg = semi + 1;
        char *nl = strchr(msg, '\n');
        if (nl) *nl = '\0';                                      /* devam satirlari (ek alanlar) yok say */
        emit(offset + (long long)(ts / 1000), (int)(pri & 7), "kernel", 0, msg, strlen(msg));
    }
}

/* ---------------- soket kurulumu ---------------- */

static int make_stream_socket(void) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    char tmp[sizeof(sa.sun_path)];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", ULT_JOURNAL_SOCK) >= (int)sizeof(tmp)) return -1;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", tmp);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(tmp);
    mode_t old = umask(0177);
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old);
    if (rc != 0 || listen(fd, 128) != 0) { close(fd); return -1; }
    chmod(tmp, 0600);
    /* atomik yayinla: yol artik "dinleyen" bir sokete aittir (UltraInit ready_path) */
    if (rename(tmp, ULT_JOURNAL_SOCK) != 0) { close(fd); return -1; }
    return fd;
}

static int make_syslog_socket(void) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(ULT_SYSLOG_SOCK) >= sizeof(sa.sun_path)) return -1;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", ULT_SYSLOG_SOCK);

    int fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(ULT_SYSLOG_SOCK);
    mode_t old = umask(0);
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old);
    if (rc != 0) { close(fd); return -1; }
    chmod(ULT_SYSLOG_SOCK, 0666);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_PASSCRED, &one, sizeof(one));
    return fd;
}

static void load_config(void) {
    char v[64];
    if (ult_verify_safe_path(ULT_CONF_FILE) != 0) return;
    if (ult_conf_get(ULT_CONF_FILE, "journal_max_file_kb", v, sizeof(v)) == 0) {
        long kb = atol(v);
        if (kb < 64) kb = 64;
        if (kb > 1048576) kb = 1048576;
        g_max_file = kb * 1024;
    }
    if (ult_conf_get(ULT_CONF_FILE, "journal_max_files", v, sizeof(v)) == 0) {
        int n = atoi(v);
        g_max_files = n < 2 ? 2 : (n > 64 ? 64 : n);
    }
}

/* ---------------- ana dongu ---------------- */

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--no-kmsg")) g_no_kmsg = 1;
        else if (!strcmp(argv[i], "--version")) { printf("ult-journald %s\n", ULT_VERSION); return 0; }
        else { fprintf(stderr, "kullanim: ult-journald [--no-kmsg]\n"); return 1; }
    }

    umask(0027);
    load_config();

    /* gunluk dizini: root'a ait, 0750 */
    mkdir(ULT_SVC_LOG_DIR, 0750);
    if (mkdir(ULT_JOURNAL_DIR, 0750) != 0 && errno != EEXIST) {
        fprintf(stderr, "ult-journald: %s olusturulamadi: %s\n", ULT_JOURNAL_DIR, strerror(errno));
        return 1;
    }
    struct stat dst;
    if (stat(ULT_JOURNAL_DIR, &dst) == 0 && dst.st_uid != 0)
        fprintf(stderr, "ult-journald: UYARI: %s root'a ait degil\n", ULT_JOURNAL_DIR);
    /* journal_max_files dusurulduyse artan eski dosyalar diskte kalmasin */
    for (int i = g_max_files; i < g_max_files + 64; i++) {
        char old[300];
        journal_path(i, old, sizeof(old));
        unlink(old);
    }
    if (open_current() != 0) {
        fprintf(stderr, "ult-journald: gunluk acilamadi: %s\n", strerror(errno));
        return 1;
    }

    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGTERM); sigaddset(&m, SIGINT); sigaddset(&m, SIGHUP); sigaddset(&m, SIGUSR1);
    sigprocmask(SIG_BLOCK, &m, NULL);
    int sig_fd = signalfd(-1, &m, SFD_NONBLOCK | SFD_CLOEXEC);

    mkdir(ULT_RUN_DIR, 0755);
    int lfd = make_stream_socket();
    if (lfd < 0) { fprintf(stderr, "ult-journald: akis soketi kurulamadi: %s\n", strerror(errno)); return 1; }
    int sfd = make_syslog_socket();
    if (sfd < 0) fprintf(stderr, "ult-journald: syslog soketi kurulamadi (%s): %s\n", ULT_SYSLOG_SOCK, strerror(errno));

    load_state();
    if (!g_no_kmsg) {
        g_kfd = open("/dev/kmsg", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (g_kfd < 0) fprintf(stderr, "ult-journald: /dev/kmsg acilamadi: %s\n", strerror(errno));
    }

    int running = 1;
    while (running) {
        if (g_kfd >= 0) kmsg_read();            /* aciliste mevcut halkayi da tuketir */

        struct pollfd fds[4 + MAX_CLIENTS];
        int map[4 + MAX_CLIENTS];
        int nf = 0;
        fds[nf].fd = sig_fd; fds[nf].events = POLLIN; map[nf++] = -1;
        fds[nf].fd = lfd;    fds[nf].events = POLLIN; map[nf++] = -2;
        if (sfd >= 0)    { fds[nf].fd = sfd;   fds[nf].events = POLLIN; map[nf++] = -3; }
        if (g_kfd >= 0)  { fds[nf].fd = g_kfd; fds[nf].events = POLLIN; map[nf++] = -4; }
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_cl[i]) { fds[nf].fd = g_cl[i]->fd; fds[nf].events = POLLIN; map[nf++] = i; }

        flush_out();
        save_state(0);
        int r = poll(fds, (nfds_t)nf, 1000);
        if (r < 0 && errno != EINTR) break;
        if (r <= 0) continue;

        for (int k = 0; k < nf; k++) {
            if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            switch (map[k]) {
                case -1: {
                    struct signalfd_siginfo si;
                    while (read(sig_fd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
                        if (si.ssi_signo == SIGTERM || si.ssi_signo == SIGINT) running = 0;
                        else if (si.ssi_signo == SIGHUP) { flush_out(); if (g_cur_fd >= 0) { close(g_cur_fd); g_cur_fd = -1; } open_current(); }
                        else if (si.ssi_signo == SIGUSR1) { flush_out(); rotate(); }
                    }
                    break;
                }
                case -2: accept_clients(lfd); break;
                case -3: syslog_read(sfd); break;
                case -4: kmsg_read(); break;
                default:
                    if (g_cl[map[k]] && g_cl[map[k]]->fd == fds[k].fd) client_read(map[k]);
                    break;
            }
        }
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (!g_cl[i]) continue;
        if (g_cl[i]->got_header && g_cl[i]->len > 0)
            emit(realtime_ms(), g_cl[i]->prio, g_cl[i]->unit, (int)g_cl[i]->pid, g_cl[i]->buf, g_cl[i]->len);
        client_free(i);
    }
    flush_out();
    save_state(1);
    unlink(ULT_JOURNAL_SOCK);
    return 0;
}
