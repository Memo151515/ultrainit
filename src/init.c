/*
 * UltraInit 1.2.0 - init.c
 *
 * PID 1 cekirdegi. Gorevleri:
 *   1) Erken boot: /proc /sys /dev /run cgroup2 mount'lari, kok rw, hostname, lo
 *   2) Servis birimlerini (yerel .svc + eski init.d betikleri) yuklemek ve
 *      bagimlilik grafigini (need/want/after/before/conflicts) kurmak
 *   3) "Reconciler": hedef durum (calismali / durmali) ile gercek durumu
 *      surekli esitlemek - olay tabanli, dalga yok, paralel (max_parallel)
 *   4) Servisleri denetlemek (restart=no|on-failure|always, hiz sinirli)
 *   5) /run/ultrainit/control soketi uzerinden ult-service komutlarina cevap
 *   6) Duzgun kapanis: ters bagimlilik sirasi, TERM->KILL, unmount, reboot()
 *
 * Lisans: MFCL (bkz. LICENSE)
 */
#include "common.h"
#include "unit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <dirent.h>
#include <poll.h>
#include <pwd.h>
#include <grp.h>
#include <net/if.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/swap.h>
#include <sys/ioctl.h>

extern char **environ;

#define MAX_UNITS    256
#define MAX_CLIENTS  16
#define MAX_WAITERS  4
#define RESPAWN_CAP  16

typedef enum {
    ST_INACTIVE = 0, ST_STARTING, ST_RUNNING, ST_DONE,
    ST_FAILED, ST_STOPPING, ST_RESTARTING
} state_t;

static const char *const state_names[] = {
    "inactive", "starting", "running", "done", "failed", "stopping", "restarting"
};

typedef struct { int ci; int up; } waiter_t;

typedef struct {
    unit_def_t d;
    state_t    state;
    int        want_up;          /* hedef: 1 calismali, 0 durmali */
    int        from_runlevel;
    int        seen;             /* reload sirasinda kullanilir */
    int        restart_pending;
    pid_t      main_pid;         /* denetlenen surec */
    pid_t      job_pid;          /* start/stop isi (betik, oneshot, forking) */
    int        stop_phase;       /* 0 yok, 1 stop isi, 2 TERM gonderildi, 3 KILL gonderildi */
    double     deadline;         /* ms (monotonik), 0 = yok */
    double     restart_at;
    double     respawns[RESPAWN_CAP];
    int        respawn_idx;
    char       reason[96];
    waiter_t   waiters[MAX_WAITERS];
    int        nwaiters;
} unit_t;

typedef struct { int fd; char buf[256]; int len; } client_t;

/* ---------------- Genel durum ---------------- */

static unit_t       *g_units[MAX_UNITS];
static int           g_n = 0;
static unsigned char g_ord[MAX_UNITS][MAX_UNITS];   /* g_ord[a][b]: b, a'dan ONCE gelmeli */
static unsigned char g_hard[MAX_UNITS][MAX_UNITS];  /* g_hard[a][b]: a, b'ye "need" ile bagli */

static client_t g_clients[MAX_CLIENTS];
static int g_sfd = -1;
static int g_lfd = -1;
static int g_cg_ok = 0;
static int g_is_pid1 = 0;

static int    g_shutdown = 0;          /* 0 yok, 1 reboot, 2 halt, 3 poweroff */
static double g_shutdown_at = 0;

/* config */
static char g_default_rl[64] = "default";
static char g_getty_ttys[128] = "tty1";
static int  g_max_parallel = 16;
static int  g_respawn_window = 30;
static int  g_respawn_max = 5;
static int  g_shutdown_timeout = 30;

/* ---------------- Yardimcilar ---------------- */

static void copy_str(char *dst, size_t n, const char *src) { snprintf(dst, n, "%.*s", (int)(n - 1), src); }

static const char *shutdown_name(void) {
    return g_shutdown == 1 ? "yeniden baslatma" : (g_shutdown == 2 ? "durdurma" : "kapatma");
}

static int is_mountpoint(const char *path) {
    struct stat a, b;
    char parent[300];
    if (stat(path, &a) != 0) return 0;
    snprintf(parent, sizeof(parent), "%s/..", path);
    if (stat(parent, &b) != 0) return 0;
    return a.st_dev != b.st_dev || a.st_ino == b.st_ino;
}

static void mkdir_p1(const char *path, mode_t mode) {
    if (mkdir(path, mode) != 0 && errno != EEXIST)
        ult_log(ULT_LOG_WARN, "mkdir %s: %s", path, strerror(errno));
}

static void load_settings(void) {
    char v[256];
    if (ult_verify_safe_path(ULT_CONF_FILE) != 0) {
        ult_log(ULT_LOG_WARN, "ana config dogrulanamadi, varsayilanlar kullanilacak");
        return;
    }
    if (ult_conf_get(ULT_CONF_FILE, "default_runlevel", v, sizeof(v)) == 0 && ult_valid_name(v))
        copy_str(g_default_rl, sizeof(g_default_rl), v);
    if (ult_conf_get(ULT_CONF_FILE, "getty_ttys", g_getty_ttys, sizeof(g_getty_ttys)) != 0)
        copy_str(g_getty_ttys, sizeof(g_getty_ttys), "tty1");
    if (ult_conf_get(ULT_CONF_FILE, "max_parallel", v, sizeof(v)) == 0)
        g_max_parallel = atoi(v) < 1 ? 1 : (atoi(v) > 64 ? 64 : atoi(v));
    if (ult_conf_get(ULT_CONF_FILE, "respawn_window", v, sizeof(v)) == 0)
        g_respawn_window = atoi(v) < 1 ? 1 : (atoi(v) > 3600 ? 3600 : atoi(v));
    if (ult_conf_get(ULT_CONF_FILE, "respawn_max", v, sizeof(v)) == 0)
        g_respawn_max = atoi(v) < 1 ? 1 : (atoi(v) > RESPAWN_CAP ? RESPAWN_CAP : atoi(v));
    if (ult_conf_get(ULT_CONF_FILE, "shutdown_timeout", v, sizeof(v)) == 0)
        g_shutdown_timeout = atoi(v) < 5 ? 5 : (atoi(v) > 300 ? 300 : atoi(v));
}

/* ---------------- Erken boot (yalnizca gercek PID 1) ---------------- */

static void mount_one(const char *src, const char *tgt, const char *type,
                      unsigned long flags, const char *data) {
    if (is_mountpoint(tgt)) return;
    mkdir(tgt, 0755);
    if (mount(src, tgt, type, flags, data) != 0 && errno != EBUSY)
        ult_log(ULT_LOG_WARN, "mount %s -> %s: %s", type, tgt, strerror(errno));
}

static void early_boot(void) {
    umask(0);
#ifndef ULT_TEST_MODE
    /* kok dosya sistemini yazilabilir yap (ro mount edilmis olabilir) */
    if (mount(NULL, "/", NULL, MS_REMOUNT, NULL) != 0)
        ult_log(ULT_LOG_WARN, "kok rw yapilamadi: %s", strerror(errno));
#endif

    mount_one("proc",     "/proc", "proc",     MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount_one("sysfs",    "/sys",  "sysfs",    MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);
    mount_one("devtmpfs", "/dev",  "devtmpfs", MS_NOSUID, "mode=0755");

    /* konsol: fd 0-2 gecerli olmali */
    if (fcntl(STDERR_FILENO, F_GETFD) < 0) {
        int fd = open("/dev/console", O_RDWR | O_NOCTTY);
        if (fd >= 0) {
            dup2(fd, 0); dup2(fd, 1); dup2(fd, 2);
            if (fd > 2) close(fd);
        }
    }

    mount_one("devpts", "/dev/pts", "devpts", MS_NOSUID | MS_NOEXEC, "mode=0620,ptmxmode=0666");
    mount_one("tmpfs",  "/dev/shm", "tmpfs",  MS_NOSUID | MS_NODEV,  "mode=1777");
    mount_one("tmpfs",  "/run",     "tmpfs",  MS_NOSUID | MS_NODEV,  "mode=0755");
    mount_one("cgroup2", ULT_CGROUP_ROOT, "cgroup2", MS_NOSUID | MS_NODEV | MS_NOEXEC, NULL);

    /* Ctrl-Alt-Del: kernel dogrudan reboot etmesin, bize SIGINT gondersin */
    reboot(RB_DISABLE_CAD);
}

static void setup_hostname(void) {
    FILE *fp = fopen("/etc/hostname", "re");
    if (!fp) return;
    char host[128] = "";
    if (fgets(host, sizeof(host), fp)) {
        host[strcspn(host, "\r\n")] = '\0';
        if (host[0] && sethostname(host, strlen(host)) != 0)
            ult_log(ULT_LOG_WARN, "hostname ayarlanamadi: %s", strerror(errno));
    }
    fclose(fp);
}

static void setup_loopback(void) {
    int s = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (s < 0) return;
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    snprintf(ifr.ifr_name, IFNAMSIZ, "lo");

    struct sockaddr_in *sin = (struct sockaddr_in *)&ifr.ifr_addr;
    sin->sin_family = AF_INET;
    inet_pton(AF_INET, "127.0.0.1", &sin->sin_addr);
    ioctl(s, SIOCSIFADDR, &ifr);
    inet_pton(AF_INET, "255.0.0.0", &sin->sin_addr);
    ioctl(s, SIOCSIFNETMASK, &ifr);

    if (ioctl(s, SIOCGIFFLAGS, &ifr) == 0) {
        ifr.ifr_flags |= IFF_UP | IFF_RUNNING;
        if (ioctl(s, SIOCSIFFLAGS, &ifr) != 0)
            ult_log(ULT_LOG_WARN, "lo etkinlestirilemedi: %s", strerror(errno));
    }
    close(s);
}

/* ---------------- cgroup (v2) ---------------- */

static void cg_init(void) {
    char probe[256];
    snprintf(probe, sizeof(probe), "%s/cgroup.controllers", ULT_CGROUP_ROOT);
    if (access(probe, F_OK) != 0) return;
    snprintf(probe, sizeof(probe), "%s/ultrainit", ULT_CGROUP_ROOT);
    if (mkdir(probe, 0755) != 0 && errno != EEXIST) return;
    g_cg_ok = 1;
}

static void cg_path(const unit_t *u, const char *file, char *out, size_t n) {
    if (file) snprintf(out, n, "%s/ultrainit/%s/%s", ULT_CGROUP_ROOT, u->d.name, file);
    else      snprintf(out, n, "%s/ultrainit/%s", ULT_CGROUP_ROOT, u->d.name);
}

/* Cocuk surecte, exec oncesi: kendini birimin cgroup'una tasir */
static void cg_join(const unit_t *u) {
    if (!g_cg_ok) return;
    char p[320];
    cg_path(u, NULL, p, sizeof(p));
    if (mkdir(p, 0755) != 0 && errno != EEXIST) return;
    cg_path(u, "cgroup.procs", p, sizeof(p));
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd < 0) return;
    char pid[24];
    int n = snprintf(pid, sizeof(pid), "%d", (int)getpid());
    if (write(fd, pid, (size_t)n) < 0) { /* en iyi caba */ }
    close(fd);
}

/* Birimin cgroup'undaki TUM surecleri oldurur (arta kalan cocuklar dahil) */
static void cg_kill(const unit_t *u) {
    if (!g_cg_ok) return;
    char p[320];
    cg_path(u, "cgroup.kill", p, sizeof(p));
    int fd = open(p, O_WRONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (write(fd, "1", 1) < 0) { /* yoksay */ }
        close(fd);
    } else {
        cg_path(u, "cgroup.procs", p, sizeof(p));
        FILE *fp = fopen(p, "re");
        if (fp) {
            long pid;
            while (fscanf(fp, "%ld", &pid) == 1)
                if (pid > 1) kill((pid_t)pid, SIGKILL);
            fclose(fp);
        }
    }
    cg_path(u, NULL, p, sizeof(p));
    rmdir(p);   /* bossa silinir; degilse sonraki seferde denenir */
}

/* ---------------- Sureç baslatma ---------------- */

static void child_fail(int efd, int e) {
    if (write(efd, &e, sizeof(e)) < 0) { /* yoksay */ }
    _exit(127);
}

static void resolve_exec(const char *path, char *out, size_t n) {
    snprintf(out, n, "%s", path);
    if (access(path, X_OK) == 0) return;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    static const char *const dirs[] = { "/usr/local/sbin", "/usr/local/bin", "/usr/sbin",
                                        "/usr/bin", "/sbin", "/bin", NULL };
    for (int i = 0; dirs[i]; i++) {
        char cand[512];
        snprintf(cand, sizeof(cand), "%s/%s", dirs[i], base);
        if (access(cand, X_OK) == 0) { snprintf(out, n, "%s", cand); return; }
    }
}

/*
 * Servisin stdout/stderr'i icin journald'e akis baglantisi acar. Baslik:
 * "UNIT <ad> <oncelik>\n", sonrasi ham bayt akisi. Engellemesiz connect:
 * journald yoksa/mesgulse hemen vazgecilir (PID 1 bu sirada exec hatasi borusunu
 * bekler, asla takilmamali).
 */
static int journal_connect(const char *unit, int prio) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", ULT_JOURNAL_SOCK);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }
    char hdr[128];
    int n = snprintf(hdr, sizeof(hdr), "UNIT %s %d\n", unit, prio);
    if (write(fd, hdr, (size_t)n) != n) { close(fd); return -1; }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) & ~O_NONBLOCK);   /* servis normal (bloklayan) yazsin */
    return fd;
}

static void child_exec(const unit_t *u, int is_script, const char *arg, int efd) {
    setsid();
    sigset_t empty;
    sigemptyset(&empty);
    sigprocmask(SIG_SETMASK, &empty, NULL);

    cg_join(u);

    ult_sanitize_environment();
    setenv("ULT_SERVICE", u->d.name, 1);
    umask(u->d.umask_val >= 0 ? (mode_t)u->d.umask_val : 022);

    /* stdin her zaman /dev/null */
    int nul = open("/dev/null", O_RDWR | O_CLOEXEC);
    if (nul >= 0) dup2(nul, STDIN_FILENO);

    if (is_script) {
        char wrapper[3072];
        ult_build_wrapper(wrapper, sizeof(wrapper), u->d.path, arg, u->d.name);
        char *argv[] = { "/bin/sh", "-c", wrapper, NULL };
        execve("/bin/sh", argv, environ);
        child_fail(efd, errno);
    }

    for (int i = 0; i < u->d.nenv; i++) {
        char tmp[128];
        copy_str(tmp, sizeof(tmp), u->d.env[i]);
        char *eq = strchr(tmp, '=');
        if (eq) { *eq = '\0'; setenv(tmp, eq + 1, 1); }
    }

    /* stdout/stderr */
    const char *logmode = u->d.log;
    if (!strcmp(logmode, "journal")) {
        int jo = journal_connect(u->d.name, 6);   /* stdout: info */
        int je = (jo >= 0) ? journal_connect(u->d.name, 3) : -1;   /* stderr: err */
        if (jo >= 0 && je >= 0) {
            dup2(jo, STDOUT_FILENO);
            dup2(je, STDERR_FILENO);
            logmode = "journal-ok";
        } else {
            if (jo >= 0) close(jo);
            logmode = "file";      /* journald hazir degil: dosyaya dus */
        }
    }
    if (!strcmp(logmode, "console") || !strcmp(logmode, "journal-ok")) {
        /* console: init'in konsolunu miras al; journal-ok: zaten baglandi */
    } else {
        int fd = -1;
        if (!strcmp(logmode, "file")) {
            char lp[320];
            snprintf(lp, sizeof(lp), "%s/%s.log", ULT_SVC_LOG_DIR, u->d.name);
            fd = open(lp, O_WRONLY | O_CREAT | O_APPEND | O_NOFOLLOW | O_CLOEXEC, 0640);
        }
        if (fd < 0) fd = nul;
        if (fd >= 0) { dup2(fd, STDOUT_FILENO); dup2(fd, STDERR_FILENO); }
    }

    if (u->d.user[0] || u->d.group[0]) {
        struct passwd *pw = u->d.user[0] ? getpwnam(u->d.user) : NULL;
        if (u->d.user[0] && !pw) child_fail(efd, ESRCH);
        gid_t gid = pw ? pw->pw_gid : 0;
        if (u->d.group[0]) {
            struct group *gr = getgrnam(u->d.group);
            if (!gr) child_fail(efd, ESRCH);
            gid = gr->gr_gid;
        }
        if (pw && initgroups(pw->pw_name, gid) != 0) child_fail(efd, errno);
        if (setgid(gid) != 0) child_fail(efd, errno);
        if (pw) {
            if (setuid(pw->pw_uid) != 0) child_fail(efd, errno);
            setenv("HOME", pw->pw_dir, 1);
            setenv("USER", pw->pw_name, 1);
        }
    }
    if (u->d.workdir[0] && chdir(u->d.workdir) != 0) child_fail(efd, errno);

    char cmd[512];
    copy_str(cmd, sizeof(cmd), arg);
    char *argv[32];
    if (ult_split_args(cmd, argv, 32) == 0) child_fail(efd, EINVAL);
    char path[512];
    resolve_exec(argv[0], path, sizeof(path));
    execve(path, argv, environ);
    child_fail(efd, errno);
}

/*
 * fork + exec. exec basarisiz olursa (CLOEXEC borusu uzerinden) hata kodu
 * ebeveyne iletilir; -1 doner ve *err doldurulur.
 */
static pid_t spawn_child(const unit_t *u, int is_script, const char *arg, int *err) {
    if (is_script && ult_verify_safe_path(u->d.path) != 0) { *err = EACCES; return -1; }

    int ep[2];
    if (pipe2(ep, O_CLOEXEC) != 0) { *err = errno; return -1; }
    pid_t pid = fork();
    if (pid < 0) { *err = errno; close(ep[0]); close(ep[1]); return -1; }
    if (pid == 0) {
        close(ep[0]);
        child_exec(u, is_script, arg, ep[1]);
        _exit(127);
    }
    close(ep[1]);
    int e = 0;
    ssize_t n;
    do { n = read(ep[0], &e, sizeof(e)); } while (n < 0 && errno == EINTR);
    close(ep[0]);
    if (n == (ssize_t)sizeof(e)) {
        waitpid(pid, NULL, 0);
        *err = e;
        return -1;
    }
    return pid;
}

/* Surec grubunu (native servisler setsid ile lider olur) ya da tek sureci sinyalle */
static void signal_pid(pid_t pid, int sig) {
    if (pid <= 1) return;
    if (kill(-pid, sig) != 0) kill(pid, sig);
}

/* ---------------- Istemciler (kontrol soketi) ---------------- */

static void client_reply(int ci, const char *fmt, ...) {
    if (ci < 0 || ci >= MAX_CLIENTS || g_clients[ci].fd < 0) return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (send(g_clients[ci].fd, buf, strlen(buf), MSG_NOSIGNAL) < 0) { /* istemci gitmis olabilir */ }
}

static void client_close(int ci) {
    if (ci < 0 || ci >= MAX_CLIENTS || g_clients[ci].fd < 0) return;
    for (int i = 0; i < g_n; i++) {
        unit_t *u = g_units[i];
        int keep = 0;
        for (int k = 0; k < u->nwaiters; k++)
            if (u->waiters[k].ci != ci) u->waiters[keep++] = u->waiters[k];
        u->nwaiters = keep;
    }
    close(g_clients[ci].fd);
    g_clients[ci].fd = -1;
    g_clients[ci].len = 0;
}

static int state_active(state_t s) {
    return s == ST_STARTING || s == ST_RUNNING || s == ST_DONE ||
           s == ST_STOPPING || s == ST_RESTARTING;
}

static void notify_waiters(unit_t *u) {
    int dci[MAX_WAITERS], dok[MAX_WAITERS], nd = 0, keep = 0;
    for (int k = 0; k < u->nwaiters; k++) {
        waiter_t w = u->waiters[k];
        int done = 0, ok = 0;
        if (w.up) {
            if (u->state == ST_RUNNING || u->state == ST_DONE) { done = 1; ok = 1; }
            else if (u->state == ST_FAILED)                    { done = 1; ok = 0; }
            else if (u->state == ST_INACTIVE && !u->want_up && !u->restart_pending) { done = 1; ok = 1; }
        } else {
            if (u->state == ST_INACTIVE || u->state == ST_FAILED) { done = 1; ok = 1; }
        }
        if (done) { dci[nd] = w.ci; dok[nd] = ok; nd++; }
        else u->waiters[keep++] = w;
    }
    u->nwaiters = keep;
    for (int d = 0; d < nd; d++) {
        client_reply(dci[d], "%s %s %s\n", dok[d] ? "OK" : "ERR", state_names[u->state],
                     u->reason[0] ? u->reason : "-");
        client_close(dci[d]);
    }
}

/* ---------------- Birim yonetimi ---------------- */

static void set_state(unit_t *u, state_t s, const char *reason) {
    state_t old = u->state;
    u->state = s;
    if (reason) copy_str(u->reason, sizeof(u->reason), reason);
    else if (s == ST_STARTING || s == ST_RUNNING || s == ST_DONE) u->reason[0] = '\0';

    if (old != s) {
        switch (s) {
            case ST_RUNNING:
                if (u->main_pid > 0) ult_log(ULT_LOG_OK, "%s: calisiyor (pid %d)", u->d.name, (int)u->main_pid);
                else                 ult_log(ULT_LOG_OK, "%s: calisiyor", u->d.name);
                break;
            case ST_DONE:       ult_log(ULT_LOG_OK, "%s: tamamlandi", u->d.name); break;
            case ST_INACTIVE:   if (old == ST_STOPPING || old == ST_RESTARTING)
                                    ult_log(ULT_LOG_INFO, "%s: durduruldu", u->d.name);
                                break;
            case ST_RESTARTING: ult_log(ULT_LOG_WARN, "%s: yeniden baslatilacak (%s)", u->d.name, u->reason); break;
            default: break;
        }
    }
    notify_waiters(u);
}

static void fail_unit(unit_t *u, const char *fmt, ...) {
    char r[96];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(r, sizeof(r), fmt, ap);
    va_end(ap);
    ult_log(ULT_LOG_ERR, "%s: basarisiz: %s", u->d.name, r);
    if (u->job_pid > 0) signal_pid(u->job_pid, SIGKILL);
    if (u->main_pid > 0) signal_pid(u->main_pid, SIGKILL);
    cg_kill(u);
    u->main_pid = 0;
    u->job_pid = 0;
    u->deadline = 0;
    u->stop_phase = 0;
    set_state(u, ST_FAILED, r);
}

static int find_unit(const char *name) {
    for (int i = 0; i < g_n; i++)
        if (!strcmp(g_units[i]->d.name, name)) return i;
    for (int i = 0; i < g_n; i++)
        for (int k = 0; k < g_units[i]->d.provides.n; k++)
            if (!strcmp(g_units[i]->d.provides.v[k], name)) return i;
    return -1;
}

static int add_unit(const char *name, int from_rl) {
    int i = find_unit(name);
    if (i >= 0) { if (from_rl) g_units[i]->from_runlevel = 1; return i; }
    if (g_n >= MAX_UNITS) { ult_log(ULT_LOG_ERR, "birim siniri asildi (%d)", MAX_UNITS); return -1; }
    unit_t *u = calloc(1, sizeof(*u));
    if (!u) return -1;
    int rc = unit_load(name, &u->d);
    if (rc != 0) { free(u); return -1; }
    u->state = ST_INACTIVE;
    u->from_runlevel = from_rl;
    g_units[g_n++] = u;
    return g_n - 1;
}

/*
 * Bagimlilik grafigi. g_ord[a][b]=1: b, a'dan once ayaga kalkmali (need/want/after
 * ve karsi taraftaki before). Dongu varsa Kahn algoritmasiyla tespit edilir,
 * loglanir ve dongudeki birimler arasi siralama yoksayilir.
 */
static void rebuild_graph(void) {
    memset(g_ord, 0, sizeof(g_ord));
    memset(g_hard, 0, sizeof(g_hard));
    for (int i = 0; i < g_n; i++) {
        const unit_def_t *d = &g_units[i]->d;
        for (int k = 0; k < d->need.n; k++) {
            int j = find_unit(d->need.v[k]);
            if (j >= 0 && j != i) { g_ord[i][j] = 1; g_hard[i][j] = 1; }
        }
        for (int k = 0; k < d->want.n; k++) {
            int j = find_unit(d->want.v[k]);
            if (j >= 0 && j != i) g_ord[i][j] = 1;
        }
        for (int k = 0; k < d->after.n; k++) {
            int j = find_unit(d->after.v[k]);
            if (j >= 0 && j != i) g_ord[i][j] = 1;
        }
        for (int k = 0; k < d->before.n; k++) {
            int j = find_unit(d->before.v[k]);
            if (j >= 0 && j != i) g_ord[j][i] = 1;
        }
    }

    int indeg[MAX_UNITS], done[MAX_UNITS], processed = 0;
    for (int i = 0; i < g_n; i++) {
        indeg[i] = 0; done[i] = 0;
        for (int j = 0; j < g_n; j++) indeg[i] += g_ord[i][j];
    }
    int progress = 1;
    while (progress) {
        progress = 0;
        for (int i = 0; i < g_n; i++) {
            if (done[i] || indeg[i] != 0) continue;
            done[i] = 1; processed++; progress = 1;
            for (int k = 0; k < g_n; k++) if (g_ord[k][i]) indeg[k]--;
        }
    }
    if (processed < g_n) {
        char names[160] = "";
        for (int i = 0; i < g_n; i++) {
            if (done[i]) continue;
            if (strlen(names) + strlen(g_units[i]->d.name) + 2 < sizeof(names)) {
                strcat(names, g_units[i]->d.name);
                strcat(names, " ");
            }
        }
        ult_log(ULT_LOG_WARN, "bagimlilik dongusu tespit edildi, siralama yoksayiliyor: %s", names);
        for (int a = 0; a < g_n; a++) {
            if (done[a]) continue;
            for (int b = 0; b < g_n; b++) if (!done[b]) { g_ord[a][b] = 0; g_hard[a][b] = 0; }
        }
    }
}

/* ---------------- Hedef durum istekleri ---------------- */

static void request_stop(int i, int restart);

static void request_start(int i, int depth) {
    if (depth > 32) return;
    unit_t *u = g_units[i];
    if (u->state == ST_FAILED) { u->state = ST_INACTIVE; u->reason[0] = '\0'; }
    if (u->want_up) return;
    u->want_up = 1;
    u->restart_pending = 0;
    u->respawn_idx = 0;
    memset(u->respawns, 0, sizeof(u->respawns));

    const dlist_t *lists[2] = { &u->d.need, &u->d.want };
    for (int l = 0; l < 2; l++) {
        for (int k = 0; k < lists[l]->n; k++) {
            const char *dep = lists[l]->v[k];
            int j = find_unit(dep);
            if (j < 0) j = add_unit(dep, 0);
            if (j < 0) {
                ult_log(ULT_LOG_WARN, "%s: bagimlilik bulunamadi: %s (yoksayildi)", u->d.name, dep);
                continue;
            }
            if (j != i) request_start(j, depth + 1);
        }
    }
    for (int k = 0; k < u->d.conflicts.n; k++) {
        int j = find_unit(u->d.conflicts.v[k]);
        if (j >= 0 && j != i && g_units[j]->want_up) {
            ult_log(ULT_LOG_INFO, "%s: celisen servis durduruluyor: %s", u->d.name, g_units[j]->d.name);
            request_stop(j, 0);
        }
    }
}

static void request_stop(int i, int restart) {
    unit_t *u = g_units[i];
    int was_up = u->want_up;
    u->want_up = 0;
    if (restart && (was_up || state_active(u->state))) u->restart_pending = 1;
    else if (!restart) u->restart_pending = 0;
    for (int j = 0; j < g_n; j++)
        if (g_hard[j][i] && g_units[j]->want_up) request_stop(j, restart);
}

/* ---------------- Servis yasam dongusu ---------------- */

static void describe_status(int st, char *out, size_t n) {
    if (WIFEXITED(st))        snprintf(out, n, "cikis kodu %d", WEXITSTATUS(st));
    else if (WIFSIGNALED(st)) snprintf(out, n, "sinyal %d ile oldu", WTERMSIG(st));
    else                      snprintf(out, n, "bilinmeyen durum");
}

static int adopt_pidfile(const unit_t *u, pid_t *out) {
    char p[256];
    if (u->d.pidfile[0]) copy_str(p, sizeof(p), u->d.pidfile);
    else ult_pidfile_path(u->d.name, p, sizeof(p));
    FILE *fp = fopen(p, "re");
    if (!fp) return -1;
    long v = 0;
    int ok = (fscanf(fp, "%ld", &v) == 1);
    fclose(fp);
    if (!ok || v <= 1 || v > 4194304 || (pid_t)v == getpid()) return -1;
    if (kill((pid_t)v, 0) != 0) return -1;
    *out = (pid_t)v;
    return 0;
}

static int respawn_allowed(unit_t *u) {
    double now = ult_now_ms();
    int count = 0;
    for (int k = 0; k < g_respawn_max; k++)
        if (u->respawns[k] > 0 && now - u->respawns[k] < g_respawn_window * 1000.0) count++;
    if (count >= g_respawn_max) return 0;
    u->respawns[u->respawn_idx % g_respawn_max] = now;
    u->respawn_idx++;
    return 1;
}

static void start_unit(unit_t *u) {
    int err = 0;
    u->main_pid = 0;
    u->job_pid = 0;
    u->stop_phase = 0;
    u->deadline = 0;
    u->reason[0] = '\0';

    if (u->d.is_script) {
        pid_t p = spawn_child(u, 1, "start", &err);
        if (p < 0) { fail_unit(u, "baslatilamadi: %s", strerror(err)); return; }
        u->job_pid = p;
        if (u->d.timeout_start) u->deadline = ult_now_ms() + u->d.timeout_start * 1000.0;
        set_state(u, ST_STARTING, NULL);
        return;
    }

    if (u->d.ready_path[0]) {
        /* onceki calismadan kalan bayat SOKETI sil (yalnizca soket: baska dosyaya dokunma) */
        struct stat st;
        if (lstat(u->d.ready_path, &st) == 0 && S_ISSOCK(st.st_mode)) unlink(u->d.ready_path);
    }
    pid_t p = spawn_child(u, 0, u->d.exec, &err);
    if (p < 0) { fail_unit(u, "exec basarisiz: %s", strerror(err)); return; }

    if (u->d.type == UT_SIMPLE) {
        u->main_pid = p;
        if (u->d.ready_path[0]) {         /* hazir olana kadar "starting" */
            if (u->d.timeout_start) u->deadline = ult_now_ms() + u->d.timeout_start * 1000.0;
            set_state(u, ST_STARTING, NULL);
        } else {
            set_state(u, ST_RUNNING, NULL);
        }
    } else {
        u->job_pid = p;
        if (u->d.timeout_start) u->deadline = ult_now_ms() + u->d.timeout_start * 1000.0;
        set_state(u, ST_STARTING, NULL);
    }
}

static void finalize_stop(unit_t *u) {
    cg_kill(u);
    u->main_pid = 0;
    u->job_pid = 0;
    u->stop_phase = 0;
    u->deadline = 0;
    set_state(u, ST_INACTIVE, NULL);
}

static void stop_continue(unit_t *u) {
    if (u->main_pid > 0 && kill(u->main_pid, 0) == 0) {
        signal_pid(u->main_pid, SIGTERM);
        u->stop_phase = 2;
        u->deadline = ult_now_ms() + u->d.timeout_stop * 1000.0;
    } else {
        finalize_stop(u);
    }
}

static void begin_stop(unit_t *u) {
    int err = 0;
    set_state(u, ST_STOPPING, NULL);
    u->stop_phase = 0;
    pid_t p = -1;
    if (u->d.is_script)        p = spawn_child(u, 1, "stop", &err);
    else if (u->d.stop_exec[0]) p = spawn_child(u, 0, u->d.stop_exec, &err);
    if (p > 0) {
        u->job_pid = p;
        u->stop_phase = 1;
        u->deadline = ult_now_ms() + u->d.timeout_stop * 1000.0;
        return;
    }
    stop_continue(u);
}

static void handle_job_exit(unit_t *u, int st) {
    int ok = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    u->job_pid = 0;

    if (u->state == ST_STARTING) {
        u->deadline = 0;
        if (!ok) {
            char d[48];
            describe_status(st, d, sizeof(d));
            if (u->reason[0]) fail_unit(u, "%s", u->reason);
            else fail_unit(u, "start betigi/komutu basarisiz (%s)", d);
            return;
        }
        if (u->d.is_script || u->d.type == UT_FORKING) {
            pid_t pp;
            if (adopt_pidfile(u, &pp) == 0) {
                u->main_pid = pp;
                set_state(u, ST_RUNNING, NULL);
            } else if (u->d.is_script) {
                set_state(u, ST_DONE, NULL);
            } else {
                ult_log(ULT_LOG_WARN, "%s: pidfile okunamadi, surec denetlenmeyecek", u->d.name);
                set_state(u, ST_RUNNING, NULL);
            }
        } else {
            set_state(u, ST_DONE, NULL);   /* oneshot */
        }
    } else if (u->state == ST_STOPPING && u->stop_phase == 1) {
        u->deadline = 0;
        stop_continue(u);
    }
}

static void handle_main_exit(unit_t *u, int st) {
    u->main_pid = 0;

    if (u->state == ST_STOPPING) {
        if (u->job_pid > 0) return;     /* stop isi bitince finalize edilecek */
        finalize_stop(u);
        return;
    }
    if (u->state == ST_STARTING) {       /* ready_path beklerken cikti */
        char d[48];
        describe_status(st, d, sizeof(d));
        fail_unit(u, "hazir olmadan cikti (%s)", d);
        return;
    }
    if (u->state != ST_RUNNING) return;

    int clean = WIFEXITED(st) && WEXITSTATUS(st) == 0;
    char d[48];
    describe_status(st, d, sizeof(d));
    cg_kill(u);                          /* arta kalan cocuklari temizle */

    int want_restart = (u->d.restart == RS_ALWAYS) ||
                       (u->d.restart == RS_ON_FAILURE && !clean);
    if (u->want_up && want_restart && !g_shutdown) {
        if (!respawn_allowed(u)) {
            u->want_up = 0;
            fail_unit(u, "cok sik coktu, yeniden baslatma durduruldu (%s)", d);
            return;
        }
        u->restart_at = ult_now_ms() + u->d.restart_sec * 1000.0;
        set_state(u, ST_RESTARTING, d);
        return;
    }
    if (clean) {
        u->want_up = 0;
        set_state(u, ST_INACTIVE, "sona erdi");
    } else {
        fail_unit(u, "%s", d);
    }
}

static void reap_children(void) {
    int st;
    pid_t p;
    while ((p = waitpid(-1, &st, WNOHANG)) > 0) {
        for (int i = 0; i < g_n; i++) {
            unit_t *u = g_units[i];
            if (u->job_pid == p)  { handle_job_exit(u, st);  break; }
            if (u->main_pid == p) { handle_main_exit(u, st); break; }
        }
        /* bilinmeyen (sahipsiz) surecler: zombi birakmamak icin toplanmasi yeterli */
    }
}

static void check_timers(void) {
    double now = ult_now_ms();
    for (int i = 0; i < g_n; i++) {
        unit_t *u = g_units[i];
        if (u->deadline <= 0 || now < u->deadline) continue;

        if (u->state == ST_STARTING && u->job_pid == 0 && u->main_pid > 0) {
            fail_unit(u, "hazir olmadi (ready_path zaman asimi)");
        } else if (u->state == ST_STARTING && u->job_pid > 0) {
            ult_log(ULT_LOG_ERR, "%s: baslatma zaman asimi", u->d.name);
            copy_str(u->reason, sizeof(u->reason), "baslatma zaman asimi");
            signal_pid(u->job_pid, SIGKILL);
            u->deadline = 0;                      /* is bitince fail_unit calisir */
        } else if (u->state == ST_STOPPING) {
            if (u->stop_phase == 1) {             /* stop isi takildi */
                if (u->job_pid > 0) signal_pid(u->job_pid, SIGKILL);
                u->deadline = 0;
            } else if (u->stop_phase == 2) {      /* TERM yetmedi -> KILL */
                ult_log(ULT_LOG_WARN, "%s: TERM'e cevap vermedi, KILL gonderiliyor", u->d.name);
                if (u->main_pid > 0) signal_pid(u->main_pid, SIGKILL);
                cg_kill(u);
                u->stop_phase = 3;
                u->deadline = now + 3000.0;
            } else if (u->stop_phase == 3) {      /* KILL'e ragmen toplanmadi */
                ult_log(ULT_LOG_ERR, "%s: surec sonlanmadi, zorla birakiliyor", u->d.name);
                finalize_stop(u);
            }
        }
    }
}

/* ---------------- Reconciler ---------------- */

static int settled(const unit_t *p) {
    return p->state == ST_RUNNING || p->state == ST_DONE || p->state == ST_FAILED ||
           (p->state == ST_INACTIVE && !p->want_up);
}

/* i durdurulabilir mi: durmasi beklenen ardil birimler (j) zaten durmus olmali */
static int successors_down(int i) {
    for (int j = 0; j < g_n; j++)
        if (g_ord[j][i] && !g_units[j]->want_up && state_active(g_units[j]->state)) return 0;
    return 1;
}

static void reconcile(void) {
    int changed;
    int guard = 0;
    do {
        changed = 0;
        int starting = 0;
        for (int i = 0; i < g_n; i++)
            if (g_units[i]->state == ST_STARTING && g_units[i]->job_pid > 0) starting++;

        for (int i = 0; i < g_n; i++) {
            unit_t *u = g_units[i];

            if ((u->state == ST_INACTIVE || u->state == ST_FAILED) && u->restart_pending && !g_shutdown) {
                int n0 = g_n;
                u->restart_pending = 0;
                request_start(i, 0);
                if (g_n != n0) rebuild_graph();
                changed = 1;
            }

            if (u->want_up) {
                if (u->state != ST_INACTIVE && u->state != ST_RESTARTING) continue;
                if (u->state == ST_RESTARTING && ult_now_ms() < u->restart_at) continue;

                int ready = 1, bad = -1;
                for (int j = 0; j < g_n; j++) {
                    if (!g_ord[i][j]) continue;
                    const unit_t *p = g_units[j];
                    if (!settled(p)) { ready = 0; break; }
                    if (g_hard[i][j] && p->state != ST_RUNNING && p->state != ST_DONE) bad = j;
                }
                if (!ready) continue;
                if (bad >= 0) {
                    u->want_up = 0;
                    fail_unit(u, "bagimlilik basarisiz: %s", g_units[bad]->d.name);
                    changed = 1;
                    continue;
                }
                if (starting >= g_max_parallel) continue;
                start_unit(u);
                if (u->state == ST_STARTING && u->job_pid > 0) starting++;
                changed = 1;
            } else {
                switch (u->state) {
                    case ST_RUNNING:
                    case ST_DONE:
                        if (successors_down(i)) { begin_stop(u); changed = 1; }
                        break;
                    case ST_RESTARTING:
                        set_state(u, ST_INACTIVE, NULL);
                        changed = 1;
                        break;
                    default: break;
                }
            }
        }
    } while (changed && ++guard < 64);
}

/* ---------------- Runlevel / reload ---------------- */

static int runlevel_names(char names[][ULT_NAME_LEN], int max) {
    int n = 0;
    char dir[300];
    snprintf(dir, sizeof(dir), "%s/%s", ULT_RUNLEVELS_DIR, g_default_rl);
    DIR *d = opendir(dir);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d)) && n < max) {
            if (!ult_valid_name(e->d_name)) continue;
            copy_str(names[n++], ULT_NAME_LEN, e->d_name);
        }
        closedir(d);
    } else {
        ult_log(ULT_LOG_WARN, "runlevel dizini acilamadi: %s", dir);
    }

    /* getty@<tty> ornekleri (sablon varsa) */
    char tpl[300];
    snprintf(tpl, sizeof(tpl), "%s/getty@.svc", ULT_UNITS_DIR);
    if (access(tpl, F_OK) == 0) {
        char ttys[128];
        copy_str(ttys, sizeof(ttys), g_getty_ttys);
        char *sp = NULL;
        for (char *t = strtok_r(ttys, ", \t", &sp); t && n < max; t = strtok_r(NULL, ", \t", &sp)) {
            if (strncmp(t, "tty", 3) != 0 || !ult_valid_name(t)) continue;
            snprintf(names[n++], ULT_NAME_LEN, "getty@%s", t);
        }
    }
    return n;
}

/* Calisma durumunu bozmadan runlevel'i yeniden esitler (SIGHUP / reload). */
static void sync_runlevel(void) {
    static char names[MAX_UNITS][ULT_NAME_LEN];
    int cnt = runlevel_names(names, MAX_UNITS);

    for (int i = 0; i < g_n; i++) {
        g_units[i]->seen = 0;
        /* bosta olan birimlerin tanimini tazele (dosya degismis olabilir) */
        unit_t *u = g_units[i];
        if (!u->want_up && (u->state == ST_INACTIVE || u->state == ST_FAILED)) {
            unit_def_t nd;
            if (unit_load(u->d.name, &nd) == 0) u->d = nd;
        }
    }
    int is_new[MAX_UNITS] = { 0 };
    for (int k = 0; k < cnt; k++) {
        int prev = find_unit(names[k]);
        int was_enabled = (prev >= 0 && g_units[prev]->from_runlevel);
        int i = add_unit(names[k], 1);
        if (i < 0) {
            ult_log(ULT_LOG_WARN, "runlevel girisi yuklenemedi: %s", names[k]);
            continue;
        }
        g_units[i]->seen = 1;
        /* Sadece yeni etkinlestirilenler baslatilir: reload, elle durdurulmus
         * servislerin hedef durumunu (stop) geri almamali. */
        if (!was_enabled) is_new[i] = 1;
    }
    rebuild_graph();
    for (int i = 0; i < g_n; i++) {
        unit_t *u = g_units[i];
        if (is_new[i]) request_start(i, 0);
        else if (!u->seen && u->from_runlevel) {
            u->from_runlevel = 0;
            ult_log(ULT_LOG_INFO, "%s: runlevel'den cikarildi, durduruluyor", u->d.name);
            request_stop(i, 0);
        }
    }
    rebuild_graph();
}

static void reload_config(void) {
    ult_log(ULT_LOG_INFO, "yapilandirma yeniden okunuyor");
    load_settings();
    sync_runlevel();
}

/* ---------------- Kapanis ---------------- */

static void request_shutdown(int kind) {
    if (g_shutdown) return;
    g_shutdown = kind;
    g_shutdown_at = ult_now_ms();
    ult_log(ULT_LOG_INFO, "sistem %s icin servisler durduruluyor", shutdown_name());
    for (int i = 0; i < g_n; i++) {
        g_units[i]->want_up = 0;
        g_units[i]->restart_pending = 0;
    }
}

static int all_units_down(void) {
    for (int i = 0; i < g_n; i++)
        if (state_active(g_units[i]->state)) return 0;
    return 1;
}

static void reap_for_ms(int ms, int stop_on_echild) {
    double end = ult_now_ms() + ms;
    while (ult_now_ms() < end) {
        pid_t p = waitpid(-1, NULL, WNOHANG);
        if (p < 0 && errno == ECHILD && stop_on_echild) return;
        if (p <= 0) usleep(50000);
    }
}

__attribute__((unused)) static void umount_all(void) {
    static char mp[256][256];
    int n = 0;
    FILE *fp = fopen("/proc/self/mounts", "re");
    if (fp) {
        char line[1024];
        while (fgets(line, sizeof(line), fp) && n < 256) {
            char dev[256], dir[256];
            if (sscanf(line, "%255s %255s", dev, dir) == 2) copy_str(mp[n++], sizeof(mp[0]), dir);
        }
        fclose(fp);
    }
    static const char *const keep[] = { "/proc", "/sys", "/dev", "/run", NULL };
    for (int i = n - 1; i >= 0; i--) {
        if (!strcmp(mp[i], "/")) continue;
        int skip = 0;
        for (int k = 0; keep[k]; k++) {
            size_t l = strlen(keep[k]);
            if (!strncmp(mp[i], keep[k], l) && (mp[i][l] == '\0' || mp[i][l] == '/')) skip = 1;
        }
        if (skip) continue;
        if (umount2(mp[i], 0) != 0) umount2(mp[i], MNT_DETACH);
    }
}

__attribute__((unused)) static void swap_off_all(void) {
    FILE *fp = fopen("/proc/swaps", "re");
    if (!fp) return;
    char line[512];
    if (!fgets(line, sizeof(line), fp)) { fclose(fp); return; }  /* baslik */
    while (fgets(line, sizeof(line), fp)) {
        char path[256];
        if (sscanf(line, "%255s", path) == 1) swapoff(path);
    }
    fclose(fp);
}

static void final_stage(void) {
    ult_log(ULT_LOG_INFO, "servisler durdu, sistem %s", shutdown_name());
    if (g_lfd >= 0) { close(g_lfd); unlink(ULT_SOCK_PATH); g_lfd = -1; }

    if (!g_is_pid1) {
        ult_log(ULT_LOG_INFO, "PID 1 degil: sadece cikiliyor");
        exit(0);
    }

    kill(-1, SIGTERM);
    reap_for_ms(2000, 1);
    kill(-1, SIGKILL);
    reap_for_ms(500, 1);

    sync();
#ifndef ULT_TEST_MODE   /* test derlemesi gercek dosya sistemlerine dokunmaz */
    swap_off_all();
    umount_all();
    mount(NULL, "/", NULL, MS_REMOUNT | MS_RDONLY, NULL);
#endif
    sync();

    int cmd = g_shutdown == 1 ? RB_AUTOBOOT : (g_shutdown == 2 ? RB_HALT_SYSTEM : RB_POWER_OFF);
    reboot(cmd);
    /* reboot() donerse (ornegin pid namespace icinde) sonsuza kadar bekle */
    for (;;) pause();
}

/* ---------------- Kontrol komutlari ---------------- */

static int add_waiter(unit_t *u, int ci, int up) {
    if (u->nwaiters >= MAX_WAITERS) return -1;
    u->waiters[u->nwaiters].ci = ci;
    u->waiters[u->nwaiters].up = up;
    u->nwaiters++;
    return 0;
}

static int pid_of(const unit_t *u) { return u->main_pid > 0 ? (int)u->main_pid : (int)u->job_pid; }

static void handle_command(int ci, char *line) {
    char *sp = NULL;
    char *cmd = strtok_r(line, " \t\r", &sp);
    char *arg = strtok_r(NULL, " \t\r", &sp);
    if (!cmd) { client_reply(ci, "ERR bos komut\n"); client_close(ci); return; }

    if (!strcmp(cmd, "ping")) { client_reply(ci, "OK pong\n"); client_close(ci); return; }

    if (!strcmp(cmd, "poweroff") || !strcmp(cmd, "reboot") || !strcmp(cmd, "halt")) {
        request_shutdown(!strcmp(cmd, "reboot") ? 1 : (!strcmp(cmd, "halt") ? 2 : 3));
        client_reply(ci, "OK kapanis basladi\n");
        client_close(ci);
        return;
    }
    if (!strcmp(cmd, "reload")) {
        if (g_shutdown) client_reply(ci, "ERR sistem kapaniyor\n");
        else { reload_config(); client_reply(ci, "OK yeniden okundu\n"); }
        client_close(ci);
        return;
    }
    if (!strcmp(cmd, "dump")) {
        for (int i = 0; i < g_n; i++) {
            const unit_t *u = g_units[i];
            client_reply(ci, "U %s %s %d %d\n", u->d.name, state_names[u->state], pid_of(u), u->want_up);
        }
        client_reply(ci, "END\n");
        client_close(ci);
        return;
    }

    if (!arg || !ult_valid_name(arg)) { client_reply(ci, "ERR gecersiz servis adi\n"); client_close(ci); return; }

    if (!strcmp(cmd, "status")) {
        int i = find_unit(arg);
        if (i < 0) {
            unit_def_t tmp;
            if (unit_load(arg, &tmp) == 0) client_reply(ci, "STATUS %s inactive 0 %s\n", arg, tmp.description[0] ? tmp.description : "-");
            else client_reply(ci, "ERR bilinmeyen servis\n");
        } else {
            const unit_t *u = g_units[i];
            const char *info = u->reason[0] ? u->reason : (u->d.description[0] ? u->d.description : "-");
            client_reply(ci, "STATUS %s %s %d %s\n", u->d.name, state_names[u->state], pid_of(u), info);
        }
        client_close(ci);
        return;
    }

    if (!strcmp(cmd, "start") || !strcmp(cmd, "restart")) {
        if (g_shutdown) { client_reply(ci, "ERR sistem kapaniyor\n"); client_close(ci); return; }
        int i = find_unit(arg);
        if (i < 0) i = add_unit(arg, 0);
        if (i < 0) { client_reply(ci, "ERR bilinmeyen servis\n"); client_close(ci); return; }
        unit_t *u = g_units[i];
        if (!strcmp(cmd, "start")) {
            request_start(i, 0);
        } else {
            if (u->state == ST_FAILED) u->state = ST_INACTIVE;
            request_stop(i, 1);
        }
        rebuild_graph();
        if (add_waiter(u, ci, 1) != 0) { client_reply(ci, "ERR cok fazla bekleyen istek\n"); client_close(ci); return; }
        notify_waiters(u);
        return;
    }

    if (!strcmp(cmd, "stop")) {
        int i = find_unit(arg);
        if (i < 0) { client_reply(ci, "OK inactive -\n"); client_close(ci); return; }
        unit_t *u = g_units[i];
        request_stop(i, 0);
        if (add_waiter(u, ci, 0) != 0) { client_reply(ci, "ERR cok fazla bekleyen istek\n"); client_close(ci); return; }
        notify_waiters(u);
        return;
    }

    client_reply(ci, "ERR bilinmeyen komut\n");
    client_close(ci);
}

static void client_read(int ci) {
    client_t *c = &g_clients[ci];
    ssize_t n = read(c->fd, c->buf + c->len, sizeof(c->buf) - 1 - (size_t)c->len);
    if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR)) { client_close(ci); return; }
    if (n < 0) return;
    c->len += (int)n;
    c->buf[c->len] = '\0';

    char *nl;
    while (c->fd >= 0 && (nl = strchr(c->buf, '\n'))) {
        *nl = '\0';
        char lineb[256];
        copy_str(lineb, sizeof(lineb), c->buf);
        memmove(c->buf, nl + 1, strlen(nl + 1) + 1);
        c->len = (int)strlen(c->buf);
        handle_command(ci, lineb);
    }
    if (c->fd >= 0 && c->len >= (int)sizeof(c->buf) - 1) {
        client_reply(ci, "ERR satir cok uzun\n");
        client_close(ci);
    }
}

static void accept_clients(void) {
    for (;;) {
        int fd = accept4(g_lfd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        struct ucred cr;
        socklen_t l = sizeof(cr);
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &l) != 0 || cr.uid != 0) {
            if (send(fd, "ERR yetki yok\n", 14, MSG_NOSIGNAL) < 0) { /* yoksay */ }
            close(fd);
            continue;
        }
        int slot = -1;
        for (int i = 0; i < MAX_CLIENTS; i++) if (g_clients[i].fd < 0) { slot = i; break; }
        if (slot < 0) {
            if (send(fd, "ERR mesgul\n", 11, MSG_NOSIGNAL) < 0) { /* yoksay */ }
            close(fd);
            continue;
        }
        g_clients[slot].fd = fd;
        g_clients[slot].len = 0;
    }
}

static int setup_control_socket(void) {
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    if (strlen(ULT_SOCK_PATH) >= sizeof(sa.sun_path)) {
        ult_log(ULT_LOG_ERR, "kontrol soketi yolu cok uzun");
        return -1;
    }
    copy_str(sa.sun_path, sizeof(sa.sun_path), ULT_SOCK_PATH);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    unlink(ULT_SOCK_PATH);
    mode_t old = umask(0177);
    int rc = bind(fd, (struct sockaddr *)&sa, sizeof(sa));
    umask(old);
    if (rc != 0 || listen(fd, 8) != 0) {
        ult_log(ULT_LOG_ERR, "kontrol soketi kurulamadi: %s", strerror(errno));
        close(fd);
        return -1;
    }
    chmod(ULT_SOCK_PATH, 0600);
    g_lfd = fd;
    return 0;
}

/* ---------------- Ana dongu ---------------- */

static int ready_waiting(void);

static int next_timeout_ms(void) {
    double now = ult_now_ms(), best = -1;
    for (int i = 0; i < g_n; i++) {
        const unit_t *u = g_units[i];
        double t = -1;
        if (u->deadline > 0) t = u->deadline;
        if (u->state == ST_RESTARTING && u->want_up && (t < 0 || u->restart_at < t)) t = u->restart_at;
        if (t >= 0 && (best < 0 || t < best)) best = t;
    }
    if (g_shutdown) {
        double t = g_shutdown_at + g_shutdown_timeout * 1000.0;
        if (best < 0 || t < best) best = t;
    }
    if (ready_waiting() && (best < 0 || now + 20 < best)) best = now + 20;
    if (best < 0) return -1;
    double d = best - now;
    if (d < 0) d = 0;
    return (int)d + 1;
}

/* ready_path bekleyen servisleri kisa araliklarla yokla */
static int ready_waiting(void) {
    for (int i = 0; i < g_n; i++) {
        const unit_t *u = g_units[i];
        if (u->state == ST_STARTING && u->job_pid == 0 && u->main_pid > 0 && u->d.ready_path[0]) return 1;
    }
    return 0;
}

static void poll_ready(void) {
    for (int i = 0; i < g_n; i++) {
        unit_t *u = g_units[i];
        if (u->state == ST_STARTING && u->job_pid == 0 && u->main_pid > 0 && u->d.ready_path[0] &&
            access(u->d.ready_path, F_OK) == 0) {
            u->deadline = 0;
            set_state(u, ST_RUNNING, NULL);
        }
    }
}

static void handle_signals(void) {
    struct signalfd_siginfo si;
    int reap = 0;
    while (read(g_sfd, &si, sizeof(si)) == (ssize_t)sizeof(si)) {
        switch (si.ssi_signo) {
            case SIGCHLD: reap = 1; break;
            case SIGTERM:
            case SIGINT:  request_shutdown(1); break;   /* reboot (Ctrl-Alt-Del dahil) */
            case SIGUSR1: request_shutdown(2); break;   /* halt */
            case SIGUSR2:
            case SIGPWR:  request_shutdown(3); break;   /* poweroff */
            case SIGHUP:  if (!g_shutdown) reload_config(); break;
            default: break;
        }
    }
    if (reap) reap_children();
}

static void main_loop(void) {
    for (;;) {
        reconcile();

        if (g_shutdown) {
            if (all_units_down() ||
                ult_now_ms() >= g_shutdown_at + g_shutdown_timeout * 1000.0) {
                if (!all_units_down())
                    ult_log(ULT_LOG_WARN, "kapanis zaman asimi: kalan servisler zorla sonlandirilacak");
                final_stage();
            }
        }

        struct pollfd fds[2 + MAX_CLIENTS];
        int map[2 + MAX_CLIENTS];
        int nf = 0;
        fds[nf].fd = g_sfd; fds[nf].events = POLLIN; map[nf++] = -1;
        if (g_lfd >= 0) { fds[nf].fd = g_lfd; fds[nf].events = POLLIN; map[nf++] = -2; }
        for (int i = 0; i < MAX_CLIENTS; i++)
            if (g_clients[i].fd >= 0) { fds[nf].fd = g_clients[i].fd; fds[nf].events = POLLIN; map[nf++] = i; }

        int r = poll(fds, (nfds_t)nf, next_timeout_ms());
        if (r < 0 && errno != EINTR) { ult_log(ULT_LOG_ERR, "poll: %s", strerror(errno)); usleep(100000); }

        if (r > 0) {
            for (int k = 0; k < nf; k++) {
                if (!(fds[k].revents & (POLLIN | POLLHUP | POLLERR))) continue;
                if (map[k] == -1)      handle_signals();
                else if (map[k] == -2) accept_clients();
                else if (g_clients[map[k]].fd == fds[k].fd) client_read(map[k]);
            }
        }
        poll_ready();
        check_timers();
    }
}

static void setup_signals(void) {
    sigset_t m;
    sigemptyset(&m);
    sigaddset(&m, SIGCHLD); sigaddset(&m, SIGTERM); sigaddset(&m, SIGINT);
    sigaddset(&m, SIGHUP);  sigaddset(&m, SIGUSR1); sigaddset(&m, SIGUSR2);
    sigaddset(&m, SIGPWR);
    sigprocmask(SIG_BLOCK, &m, NULL);
    g_sfd = signalfd(-1, &m, SFD_NONBLOCK | SFD_CLOEXEC);
    if (g_sfd < 0) {
        ult_log(ULT_LOG_ERR, "signalfd olusturulamadi: %s", strerror(errno));
        _exit(1);
    }
}

int main(void) {
    g_is_pid1 = (getpid() == 1);
    for (int i = 0; i < MAX_CLIENTS; i++) g_clients[i].fd = -1;

    umask(0027);
    if (g_is_pid1) early_boot();
    umask(0027);
    ult_sanitize_environment();

    ult_log(ULT_LOG_INFO, "UltraInit %s basliyor (pid %d%s)", ULT_VERSION, (int)getpid(),
            g_is_pid1 ? "" : ", test modu");

    load_settings();
    mkdir_p1(ULT_RUN_DIR, 0755);
    mkdir_p1(ULT_SVC_LOG_DIR, 0750);

    if (prctl(PR_SET_CHILD_SUBREAPER, 1, 0, 0, 0) != 0)
        ult_log(ULT_LOG_WARN, "subreaper ayarlanamadi: %s", strerror(errno));

    setup_signals();
    cg_init();
    if (g_cg_ok) ult_log(ULT_LOG_INFO, "cgroup v2 destegi etkin");
    if (setup_control_socket() != 0)
        ult_log(ULT_LOG_WARN, "kontrol soketi yok: ult-service ile yonetim calismayacak");

    if (g_is_pid1) { setup_hostname(); setup_loopback(); }

    sync_runlevel();
    ult_log(ULT_LOG_OK, "runlevel '%s' hazirlaniyor (%d birim yuklendi)", g_default_rl, g_n);

    main_loop();
    return 0;
}
