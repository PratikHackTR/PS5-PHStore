#ifndef PHSTORE_CONFIG_H
#define PHSTORE_CONFIG_H

#define PHSTORE_HTTP_PORT 1903
#define PHSTORE_STREAM_PORT 1925
#define PHSTORE_IPC_PORT 1926
#define PHSTORE_BIND_IPV4 "127.0.0.1"
#define PHSTORE_VERSION "0.1.0"
#define PHSTORE_BUILD_ID "phstore2-faststart-ph-ps5-20261004"

/* 0=normal installer, 1=A, 2=B, 3=C, 4=D, 5=Stage E real package install. */
#ifndef PHSTORE_INSTALL_DIAGNOSTIC_STAGE
#define PHSTORE_INSTALL_DIAGNOSTIC_STAGE 5
#endif

/* Stage E uses the real helper, as does normal operation. Only the isolated
 * A-D installer diagnostics must suppress shortcut helper sessions. */
#define PHSTORE_SHORTCUT_ENABLED (PHSTORE_INSTALL_DIAGNOSTIC_STAGE == 0 || PHSTORE_INSTALL_DIAGNOSTIC_STAGE == 5)

#ifndef PHSTORE_BOOTSTRAP_URL
#define PHSTORE_BOOTSTRAP_URL "https://phstore.invalid/bootstrap.json"
#endif

#endif
