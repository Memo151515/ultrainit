/*
 * UltraInit 1.2.0 - unit.h
 * Servis tanimlari: yerel .svc birimleri ve eski tip init.d betikleri.
 *
 * Lisans: MFCL (bkz. LICENSE)
 */
#ifndef ULTRAINIT_UNIT_H
#define ULTRAINIT_UNIT_H

#include "common.h"

#define ULT_NAME_LEN 64
#define ULT_MAX_DEPS 16
#define ULT_MAX_ENV  8
#define ULT_JOURNAL_UNIT "journald"

typedef enum { UT_SIMPLE = 0, UT_FORKING, UT_ONESHOT } ut_type_t;
typedef enum { RS_NO = 0, RS_ON_FAILURE, RS_ALWAYS } restart_t;

typedef struct {
    char v[ULT_MAX_DEPS][ULT_NAME_LEN];
    int  n;
} dlist_t;

typedef struct {
    char      name[ULT_NAME_LEN];
    int       is_script;          /* 1: eski tip init.d betigi */
    char      path[256];
    char      description[128];
    ut_type_t type;
    restart_t restart;
    int       restart_sec;
    int       timeout_start;      /* sn, 0 = sinirsiz */
    int       timeout_stop;       /* sn */
    int       umask_val;          /* -1 = varsayilan (022) */
    char      exec[512];
    char      stop_exec[512];
    char      pidfile[128];
    char      ready_path[128];    /* simple: bu yol olusana kadar "starting" */
    char      user[32];
    char      group[32];
    char      workdir[128];
    char      log[16];            /* null | console | file */
    char      env[ULT_MAX_ENV][128];
    int       nenv;
    dlist_t   need, want, after, before, conflicts, provides;
} unit_def_t;

/*
 * Birimi yukler. Arama sirasi:
 *   1) <services>/<ad>.svc            (yerel birim)
 *   2) <services>/<on-ek>@.svc        (sablon; ad "on-ek@ornek" ise, %i = ornek)
 *   3) <init.d>/<ad>                  (eski tip betik; depend() statik okunur)
 * Donus: 0 basarili, -1 bulunamadi, -2 guvensiz/gecersiz (nedeni loglanir).
 */
int unit_load(const char *name, unit_def_t *u);

const char *unit_restart_name(restart_t r);

#endif /* ULTRAINIT_UNIT_H */
