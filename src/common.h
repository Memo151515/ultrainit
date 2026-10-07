/*
 * UltraInit 1.2.0 - common.h
 * Ortak tipler, yollar, loglama ve guvenlik yardimcilari.
 *
 * Lisans: MFCL - MEMO Free Code License (bkz. LICENSE)
 * Telif: (c) 2026 Memo151515
 */
#ifndef ULTRAINIT_COMMON_H
#define ULTRAINIT_COMMON_H

#include <stdio.h>
#include <stddef.h>
#include <sys/types.h>

#define ULT_VERSION "1.2.0"

/* Yollar. Test derlemeleri bunlari -D ile gecersiz kilar. */
#ifndef ULT_CONF_DIR
#define ULT_CONF_DIR       "/etc/ultrainit"
#endif
#ifndef ULT_RUN_DIR
#define ULT_RUN_DIR        "/run/ultrainit"
#endif
#ifndef ULT_LOG_FILE
#define ULT_LOG_FILE       "/var/log/ultrainit.log"
#endif
#ifndef ULT_SVC_LOG_DIR
#define ULT_SVC_LOG_DIR    "/var/log/ultrainit"
#endif
#ifndef ULT_CGROUP_ROOT
#define ULT_CGROUP_ROOT    "/sys/fs/cgroup"
#endif

#ifndef ULT_JOURNAL_DIR
#define ULT_JOURNAL_DIR    ULT_SVC_LOG_DIR "/journal"
#endif
#ifndef ULT_JOURNAL_SOCK
#define ULT_JOURNAL_SOCK   ULT_RUN_DIR "/journal.stream"
#endif
#ifndef ULT_SYSLOG_SOCK
#define ULT_SYSLOG_SOCK    "/dev/log"
#endif

#define ULT_UNITS_DIR      ULT_CONF_DIR "/services"
#define ULT_INITD_DIR      ULT_CONF_DIR "/init.d"
#define ULT_RUNLEVELS_DIR  ULT_CONF_DIR "/runlevels"
#define ULT_CONF_FILE      ULT_CONF_DIR "/ultrainit.conf"
#define ULT_SOCK_PATH      ULT_RUN_DIR "/control"

typedef enum {
    ULT_LOG_INFO = 0,
    ULT_LOG_OK,
    ULT_LOG_WARN,
    ULT_LOG_ERR
} ult_log_level_t;

/* Hem konsola (renkli) hem log dosyasina yazar. */
void ult_log(ult_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/*
 * GUVENLIK: Dosyayi O_NOFOLLOW ile acar, ayni fd uzerinden fstat ile
 * dogrular (root'a ait, normal dosya, grup/diger yazamaz) ve dosyanin
 * ust dizin zincirinin (/ dahil) root'a ait ve grup/diger tarafindan
 * yazilamaz oldugunu denetler (sticky dizinler root'a aitse kabul edilir).
 * Donus: guvenli ise acik fd (>=0), degilse -1 (nedeni loglanir).
 */
int ult_open_verified(const char *path);
int ult_verify_safe_path(const char *path);

/* Servis adi gecerli mi? [A-Za-z0-9_.@:-], 1..63, '.' ya da '-' ile baslamaz, ".." yok */
int ult_valid_name(const char *name);

/* /run/ultrainit/<isim>.pid */
void ult_pidfile_path(const char *name, char *out, size_t outlen);

/* key=value config okuyucu. Bulunursa 0, bulunamazsa -1 doner; deger out'a kopyalanir. */
int ult_conf_get(const char *file, const char *key, char *out, size_t outlen);

/* Ortami temizleyip minimal/guvenli bir set kurar. (umask'a dokunmaz.) */
void ult_sanitize_environment(void);

/* Yerinde bosluk ayirici arguman parcalayici ("..." ve '...' destekler). */
int ult_split_args(char *s, char **argv, int max);

/* Eski tip (init.d) betik icin `sh -c` sarmalayici satirini olusturur. */
void ult_build_wrapper(char *out, size_t outlen, const char *script,
                       const char *action, const char *name);

/* Monotonik zaman (ms) */
double ult_now_ms(void);

#endif /* ULTRAINIT_COMMON_H */
