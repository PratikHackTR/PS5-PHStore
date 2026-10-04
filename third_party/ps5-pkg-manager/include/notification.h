#ifndef NOTIFICATION_H
#define NOTIFICATION_H

#include <stddef.h>

typedef struct notify_request {
    char useless1[45];
    char message[3075];
} notify_request_t;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Sends a PS5 on-screen system notification.
 */
void ps5_notify(const char *fmt, ...);

#ifdef __cplusplus
}
#endif

#endif /* NOTIFICATION_H */
