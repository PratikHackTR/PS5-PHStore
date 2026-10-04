#include "phstore_notification.h"
#include "phstore_config.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    char reserved[45];
    char message[3075];
} phstore_notification_request_t;

extern int sceKernelSendNotificationRequest(int, phstore_notification_request_t *, size_t, int);

void phstore_notify(const char *format, ...) {
    if (!format) return;
    phstore_notification_request_t request;
    memset(&request, 0, sizeof(request));
    va_list args;
    va_start(args, format);
    vsnprintf(request.message, sizeof(request.message), format, args);
    va_end(args);
#ifdef PHSTORE_DEBUG_BUILD
    int result = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    if (result != 0) fprintf(stderr, "sceKernelSendNotificationRequest failed: 0x%08x\n", (unsigned)result);
#else
    if (strncmp(request.message,"PH Store",8) && strncmp(request.message,"PHStore",7)) return;
    int result = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
    if (result != 0) fprintf(stderr, "sceKernelSendNotificationRequest failed: 0x%08x\n", (unsigned)result);
#endif
}
