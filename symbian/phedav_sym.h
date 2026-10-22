// Copyright The Phorepoc Authors
// SPDX-License-Identifier: Apache-2.0
#ifndef PHEDAV_SYM_H
#define PHEDAV_SYM_H
#include "phedav.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHEDAV_DIR "C:\\data\\phedav"
#define PHEDAV_LOG PHEDAV_DIR "\\phedav.log"

/* plat.cpp */
void plat_log_open(void);
int plat_net_init(void);                       /* default access point (iap.txt) */
phedav_net plat_make_net(void);                /* plain TCP; with PHEDAV_WITH_MBEDTLS also TLS (ca.pem / pin.txt) */

/* store.cpp: journalled file-backed sync state and the per-contact signatures */
int fstore_open(const char *coll, const char *tag);             /* loads (and compacts) the state of one collection */
void fstore_close(void);
phedav_store fstore_store(void);
int fsig_get(const char *id, unsigned long *sig);                 /* 1 = present */
void fsig_set(const char *id, unsigned long sig);
void fsig_del(const char *id);
void fsig_each(int (*cb)(void *ctx, const char *id, unsigned long sig), void *ctx);

/* contacts.cpp */
int contacts_open(const char *dbname);   /* NULL = the default (real) database */
void contacts_close(void);
phedav_local contacts_local(void);

/* calendar.cpp: dbname = calendar file, e.g. c:phedavtest.cal (created if missing); NULL = the default calendar */
int calendar_open(const char *dbname);
void calendar_close(void);
phedav_local calendar_local(void);

#ifdef __cplusplus
}
#endif
#endif
