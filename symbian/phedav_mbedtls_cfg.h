/* Copyright The Phorepoc Authors
 * SPDX-License-Identifier: Apache-2.0 */
/* mbedTLS 3.6 configuration for the Open C build: the default configuration minus what the platform lacks or we do not need. */
#include "mbedtls/mbedtls_config.h"

#undef MBEDTLS_NET_C                 /* the host supplies the stream */
#undef MBEDTLS_TIMING_C
#undef MBEDTLS_FS_IO
#undef MBEDTLS_PSA_ITS_FILE_C
#undef MBEDTLS_PSA_CRYPTO_STORAGE_C
#undef MBEDTLS_THREADING_C
#undef MBEDTLS_THREADING_PTHREAD
#undef MBEDTLS_SELF_TEST
#undef MBEDTLS_AESNI_C
#undef MBEDTLS_AESCE_C
#undef MBEDTLS_PADLOCK_C
#undef MBEDTLS_HAVE_ASM              /* portable C: no inline assembly for the ARM9/11 or Cortex core */
#undef MBEDTLS_SSL_SRV_C             /* client only */
#undef MBEDTLS_SSL_CACHE_C
#undef MBEDTLS_SSL_COOKIE_C
#undef MBEDTLS_SSL_TICKET_C
#undef MBEDTLS_DEBUG_C

#define MBEDTLS_NO_PLATFORM_ENTROPY          /* no /dev/urandom */
#define MBEDTLS_ENTROPY_HARDWARE_ALT         /* mbedtls_hardware_poll() in plat.cpp */
#define MBEDTLS_PLATFORM_MS_TIME_ALT         /* mbedtls_ms_time() in plat.cpp */
#define MBEDTLS_NO_UDBL_DIVISION             /* no 128-bit division (__udivti3) on a 64-bit-limb build (the x32 test image): the call would hit an unresolved stub */
