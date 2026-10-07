/*
 * UltraInit 1.2.0 - ult-service.c
 * Servis yonetim araci. PID 1 ile kontrol soketi uzerinden konusur.
 *
 *   ult-service <servis> start|stop|restart|status|enable|disable [runlevel]
 *   ult-service start|stop|restart|status|enable|disable <servis>
 *   ult-service list | reload | poweroff | reboot | halt
 *
 * Init calismiyorsa (chroot/kurulum), eski tip init.d betikleri dogrudan
 * calistirilabilir (bagimlilik cozumu yapilmaz).
 *
 * Lisans: MFCL (bkz. LICENSE)
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>

#define OUT_MAX 65536

#ifndef ULT_JOURNALULT_PATH
#define ULT_JOURNALULT_PATH "/usr/bin/journalult"
#endif

static const char *const verbs[] = { "start", "stop", "restart", "status", "enable", "disable", NULL };

static int is_verb(const char *s) {
    for (int i = 0; verbs[i]; i++) if (!strcmp(s, verbs[i])) return 1;
    return 0;
}

static void usage(void) {
    fprintf(stderr,
        "UltraInit %s - ult-service\n\n"
        "Kullanim:\n"
        "  ult-service <servis> start|stop|restart|status|enable|disable [runlevel]\n"
        "  ult-service start|stop|restart|status|enable|disable <servis>\n"
        "  ult-service list        servisleri ve durumlarini listele\n"
        "  ult-service reload      runlevel/yapilandirmayi yeniden oku\n"
        "  ult-service poweroff | reboot | halt\n", ULT_VERSION);
}

/* Init'e tek satir komut gonderir, cevabin tamamini okur. -1: init'e ulasilamadi */
static int ctl_request(const char *line, char *out, size_t outlen) {
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", ULT_SOCK_PATH);
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { close(fd); return -1; }

    char msg[300];
    int n = snprintf(msg, sizeof(msg), "%s\n", line);
    if (send(fd, msg, (size_t)n, MSG_NOSIGNAL) != n) { close(fd); return -1; }

    size_t len = 0;
    for (;;) {
        ssize_t r = read(fd, out + len, outlen - 1 - len);
        if (r < 0) { if (errno == EINTR) continue; break; }
        if (r == 0) break;
        len += (size_t)r;
        if (len >= outlen - 1) break;
    }
    out[len] = '\0';
    close(fd);
    return 0;
}

/* ---------------- init yokken: eski tip betikleri dogrudan calistir ---------------- */

static int run_script_direct(const char *name, const char *action) {
    char path[300];
    snprintf(path, sizeof(path), "%s/%s", ULT_INITD_DIR, name);
    if (access(path, F_OK) != 0) {
        fprintf(stderr, "ultrainit calismiyor ve '%s' eski tip betik degil; dogrudan calistirilamaz\n", name);
        return 1;
    }
    if (ult_verify_safe_path(path) != 0) return 1;

    char wrapper[3072];
    ult_build_wrapper(wrapper, sizeof(wrapper), path, action, name);
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid == 0) {
        ult_sanitize_environment();
        umask(022);
        execl("/bin/sh", "sh", "-c", wrapper, (char *)NULL);
        _exit(127);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 1;
}

/* ---------------- enable / disable ---------------- */

static void default_runlevel(char *out, size_t n) {
    char v[64];
    if (ult_conf_get(ULT_CONF_FILE, "default_runlevel", v, sizeof(v)) == 0 && ult_valid_name(v))
        snprintf(out, n, "%s", v);
    else
        snprintf(out, n, "default");
}

static int find_source(const char *name, char *out, size_t n) {
    snprintf(out, n, "%s/%s.svc", ULT_UNITS_DIR, name);
    if (access(out, F_OK) == 0) return 0;
    const char *at = strchr(name, '@');
    if (at && at[1]) {
        snprintf(out, n, "%s/%.*s@.svc", ULT_UNITS_DIR, (int)(at - name), name);
        if (access(out, F_OK) == 0) return 0;
    }
    snprintf(out, n, "%s/%s", ULT_INITD_DIR, name);
    return access(out, F_OK) == 0 ? 0 : -1;
}

static int cmd_enable(const char *name, const char *rl, int enable) {
    char dir[300], link[400], src[300];
    snprintf(dir, sizeof(dir), "%s/%s", ULT_RUNLEVELS_DIR, rl);
    snprintf(link, sizeof(link), "%s/%s", dir, name);

    if (enable) {
        if (find_source(name, src, sizeof(src)) != 0) {
            fprintf(stderr, "servis bulunamadi: %s\n", name);
            return 1;
        }
        if (ult_verify_safe_path(src) != 0) {
            fprintf(stderr, "'%s' guvenlik denetimini gecemedi, etkinlestirilmedi\n", src);
            return 1;
        }
        if (mkdir(ULT_RUNLEVELS_DIR, 0755) != 0 && errno != EEXIST) { perror(ULT_RUNLEVELS_DIR); return 1; }
        if (mkdir(dir, 0755) != 0 && errno != EEXIST) { perror(dir); return 1; }
        if (symlink(src, link) != 0) {
            if (errno == EEXIST) { printf("%s zaten '%s' runlevel'inde etkin\n", name, rl); return 0; }
            perror("symlink");
            return 1;
        }
        printf("%s '%s' runlevel'inde etkinlestirildi (hemen uygulamak icin: ult-service reload)\n", name, rl);
        return 0;
    }
    if (unlink(link) != 0) {
        if (errno == ENOENT) { printf("%s '%s' runlevel'inde zaten etkin degil\n", name, rl); return 0; }
        perror("unlink");
        return 1;
    }
    printf("%s '%s' runlevel'inden cikarildi (hemen uygulamak icin: ult-service reload)\n", name, rl);
    return 0;
}

/* ---------------- list ---------------- */


static int cmp_str(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

static int add_names(char names[][64], int n, int max, const char *dir, int strip_svc) {
    DIR *d = opendir(dir);
    if (!d) return n;
    struct dirent *e;
    while ((e = readdir(d)) && n < max) {
        char nm[64];
        if (strlen(e->d_name) > 63) continue;
        snprintf(nm, sizeof(nm), "%.63s", e->d_name);
        size_t l = strlen(nm);
        if (strip_svc) {
            if (l < 5 || strcmp(nm + l - 4, ".svc") != 0) continue;
            nm[l - 4] = '\0';
        }
        if (!ult_valid_name(nm)) continue;
        int dup = 0;
        for (int i = 0; i < n; i++) if (!strcmp(names[i], nm)) { dup = 1; break; }
        if (!dup) snprintf(names[n++], 64, "%s", nm);
    }
    closedir(d);
    return n;
}

static int cmd_list(void) {
    static char names[512][64];
    int n = 0;
    n = add_names(names, n, 512, ULT_UNITS_DIR, 1);
    n = add_names(names, n, 512, ULT_INITD_DIR, 0);
    qsort(names, (size_t)n, sizeof(names[0]), cmp_str);

    static char out[OUT_MAX];
    int live = (ctl_request("dump", out, sizeof(out)) == 0);

    char rl[64];
    default_runlevel(rl, sizeof(rl));

    printf("%-26s %-11s %-8s %-8s %s\n", "SERVIS", "DURUM", "ETKIN", "PID", "TUR");
    for (int i = 0; i < n; i++) {
        char state[16] = "inactive", pid[16] = "-";
        if (live) {
            char key[100];
            snprintf(key, sizeof(key), "U %.63s ", names[i]);
            char *p = strstr(out, key);
            while (p && p != out && p[-1] != '\n') p = strstr(p + 1, key);
            if (p) {
                char nm[64], st[16];
                int pd = 0, up = 0;
                if (sscanf(p, "U %63s %15s %d %d", nm, st, &pd, &up) == 4) {
                    snprintf(state, sizeof(state), "%s", st);
                    if (pd > 0) snprintf(pid, sizeof(pid), "%d", pd);
                }
            }
        }
        char link[400], src[300];
        snprintf(link, sizeof(link), "%s/%s/%.63s", ULT_RUNLEVELS_DIR, rl, names[i]);
        struct stat sb;
        int enabled = (lstat(link, &sb) == 0);
        const char *kind = "betik";
        if (find_source(names[i], src, sizeof(src)) == 0 && strstr(src, ".svc")) kind = "yerel";
        const char *nmshow = names[i];
        char tmp[80];
        size_t l = strlen(names[i]);
        if (l > 0 && names[i][l-1] == '@') { snprintf(tmp, sizeof(tmp), "%.63s(sablon)", names[i]); nmshow = tmp; }
        printf("%-26s %-11s %-8s %-8s %s\n", nmshow, state, enabled ? "evet" : "-", pid, kind);
    }
    if (!live) fprintf(stderr, "\n(ultrainit calismiyor: durumlar bilinmiyor)\n");
    return 0;
}

/* ---------------- init uzerinden servis komutlari ---------------- */

/* "status" sonrasi: servisin son log satirlarini (varsa) goster */
static void show_recent_logs(const char *svc) {
    if (access(ULT_JOURNALULT_PATH, X_OK) != 0) return;
    char cmd[400];
    snprintf(cmd, sizeof(cmd), "%s -u '%s' -n 5 2>/dev/null", ULT_JOURNALULT_PATH, svc);
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char line[600];
    int shown = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (!shown) { printf("\nSon loglar:\n"); shown = 1; }
        printf("  %s", line);
    }
    pclose(fp);
}

static int via_init(const char *verb, const char *svc) {
    char line[160], out[2048];
    snprintf(line, sizeof(line), "%s %s", verb, svc);
    if (ctl_request(line, out, sizeof(out)) != 0) return -1;

    char tag[16] = "", st[24] = "", rest[512] = "";
    int pid = 0;
    if (sscanf(out, "STATUS %*s %23s %d %511[^\n]", st, &pid, rest) >= 2) {
        int up = !strcmp(st, "running") || !strcmp(st, "done");
        if (pid > 0) printf("%s: %s (pid %d)", svc, st, pid); else printf("%s: %s", svc, st);
        if (strcmp(rest, "-") != 0 && rest[0]) printf(" - %s", rest);
        printf("\n");
        show_recent_logs(svc);
        return up ? 0 : 3;
    }
    if (sscanf(out, "%15s %23s %511[^\n]", tag, st, rest) >= 1) {
        int ok = !strcmp(tag, "OK");
        const char *extra = (strcmp(rest, "-") != 0) ? rest : "";
        if (ok) {
            printf("\033[32m[ ok ]\033[0m %s: %s%s%s\n", svc, st[0] ? st : "tamam", extra[0] ? " - " : "", extra);
            return 0;
        }
        fprintf(stderr, "\033[31m[FAIL]\033[0m %s: %s%s%s\n", svc, st, extra[0] ? " - " : "", extra);
        return 1;
    }
    fprintf(stderr, "beklenmeyen cevap: %s\n", out);
    return 1;
}

static int simple_cmd(const char *cmd) {
    char out[512];
    if (ctl_request(cmd, out, sizeof(out)) != 0) {
        fprintf(stderr, "ultrainit'e ulasilamadi (%s)\n", ULT_SOCK_PATH);
        return 1;
    }
    fputs(out, out[0] == 'E' ? stderr : stdout);
    return out[0] == 'O' ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 1; }
    if (!strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) { usage(); return 0; }
    if (!strcmp(argv[1], "--version")) { printf("ult-service %s\n", ULT_VERSION); return 0; }

    if (!strcmp(argv[1], "list"))     return cmd_list();
    if (!strcmp(argv[1], "reload"))   return simple_cmd("reload");
    if (!strcmp(argv[1], "poweroff")) return simple_cmd("poweroff");
    if (!strcmp(argv[1], "reboot"))   return simple_cmd("reboot");
    if (!strcmp(argv[1], "halt"))     return simple_cmd("halt");

    const char *svc, *verb;
    if (argc >= 3 && is_verb(argv[1]) && !is_verb(argv[2])) { verb = argv[1]; svc = argv[2]; }
    else if (argc >= 3)                                      { svc = argv[1]; verb = argv[2]; }
    else { usage(); return 1; }

    if (!is_verb(verb)) { fprintf(stderr, "bilinmeyen komut: %s\n", verb); usage(); return 1; }
    if (!ult_valid_name(svc)) { fprintf(stderr, "gecersiz servis adi: %s\n", svc); return 1; }

    if (!strcmp(verb, "enable") || !strcmp(verb, "disable")) {
        char rl[64];
        if (argc >= 4) {
            if (!ult_valid_name(argv[3])) { fprintf(stderr, "gecersiz runlevel adi\n"); return 1; }
            snprintf(rl, sizeof(rl), "%s", argv[3]);
        } else default_runlevel(rl, sizeof(rl));
        return cmd_enable(svc, rl, !strcmp(verb, "enable"));
    }

    int rc = via_init(verb, svc);
    if (rc >= 0) return rc;

    /* init yok: eski tip betikler icin dogrudan calistirma */
    fprintf(stderr, "(ultrainit calismiyor: betik dogrudan calistiriliyor, bagimlilik cozumu yok)\n");
    if (!strcmp(verb, "restart")) {
        run_script_direct(svc, "stop");
        return run_script_direct(svc, "start");
    }
    return run_script_direct(svc, verb);
}
