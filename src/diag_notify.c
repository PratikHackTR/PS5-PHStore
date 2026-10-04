/*
 * Diagnostic B notification adaptation from itsPLK/ps5-pkg-manager.
 * This file is distributed under GPL-3.0-only; see licenses/GPL-3.0.txt and
 * docs/diag-b-notification-license.md. The separate diagnostic A is unchanged.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define PHSTORE_DIAG_PORT 1905
#define PHSTORE_DIAG_ADDRESS "127.0.0.1"
#define REQUEST_MAX 4096

extern int sceNetInit(void);
extern const unsigned char phstore_diag_html[];
extern const size_t phstore_diag_html_size;

typedef struct {
    char useless1[45];
    char message[3075];
} phstore_notify_request_t;

extern int sceKernelSendNotificationRequest(int, phstore_notify_request_t *, size_t, int);

static volatile int phstore_notification_sent;

static void raw_marker(const char *message, size_t size) {
    (void)write(STDOUT_FILENO, message, size);
}

static int send_all(int fd, const char *data, size_t size) {
    while (size > 0) {
        ssize_t written = send(fd, data, size, 0);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return -1;
        data += written;
        size -= (size_t)written;
    }
    return 0;
}

static void respond(int fd, int status, const char *reason, const char *type,
                    const char *body, size_t body_size, int include_body) {
    char header[384];
    int length = snprintf(header, sizeof(header),
        "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
        "Connection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n",
        status, reason, type, body_size);
    if (length > 0 && (size_t)length < sizeof(header) && send_all(fd, header, (size_t)length) == 0 && include_body)
        (void)send_all(fd, body, body_size);
}

static void handle_client(int fd) {
    char request[REQUEST_MAX + 1];
    struct timeval timeout = {.tv_sec = 5, .tv_usec = 0};
    (void)setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    size_t used = 0;
    while (used < REQUEST_MAX) {
        ssize_t count = recv(fd, request + used, REQUEST_MAX - used, 0);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return;
        used += (size_t)count;
        request[used] = '\0';
        if (strstr(request, "\r\n\r\n") || strstr(request, "\n\n")) break;
    }
    char method[8] = {0}, path[128] = {0};
    if (sscanf(request, "%7s %127s", method, path) != 2) {
        static const char body[] = "Bad Request\n";
        respond(fd, 400, "Bad Request", "text/plain; charset=utf-8", body, sizeof(body) - 1, 1);
    } else if (strcmp(method, "GET") == 0 && strcmp(path, "/") == 0) {
        respond(fd, 200, "OK", "text/html; charset=utf-8", (const char *)phstore_diag_html,
                phstore_diag_html_size, 1);
    } else if ((strcmp(method, "GET") == 0 || strcmp(method, "HEAD") == 0) &&
               strcmp(path, "/api/health") == 0) {
        static const char body[] = "{\"ok\":true,\"version\":\"diag-baseline\",\"loopback_only\":true}";
        respond(fd, 200, "OK", "application/json", body, sizeof(body) - 1, strcmp(method, "HEAD") != 0);
    } else {
        static const char body[] = "Not Found\n";
        respond(fd, 404, "Not Found", "text/plain; charset=utf-8", body, sizeof(body) - 1, 1);
    }
}

static void *client_worker(void *argument) {
    int fd = (int)(intptr_t)argument;
    handle_client(fd);
    if (__sync_lock_test_and_set(&phstore_notification_sent, 1) == 0) {
        static const char before[] = "[PHSTORE] BEFORE NOTIFY\n";
        static const char message[] = "PH Store Debug\n127.0.0.1:1905 hazır";
        phstore_notify_request_t request;
        memset(&request, 0, sizeof(request));
        for (size_t i = 0; i < sizeof(message) - 1; i++) request.message[i] = message[i];
        raw_marker(before, sizeof(before) - 1);
        int result = sceKernelSendNotificationRequest(0, &request, sizeof(request), 0);
        char after[80];
        int length = snprintf(after, sizeof(after), "[PHSTORE] AFTER NOTIFY result=0x%08X\n", (unsigned)result);
        if (length > 0) raw_marker(after, (size_t)length < sizeof(after) ? (size_t)length : sizeof(after) - 1);
    }
    (void)shutdown(fd, SHUT_RDWR);
    (void)close(fd);
    return NULL;
}

int main(void) {
    static const char entered[] = "[PHSTORE] ENTERED MAIN\n";
    (void)write(STDOUT_FILENO, entered, sizeof(entered) - 1);
    signal(SIGPIPE, SIG_IGN);

    static const char before_net[] = "[PHSTORE] BEFORE NET INIT\n";
    raw_marker(before_net, sizeof(before_net) - 1);
    int net_result = sceNetInit();
    static const char after_net[] = "[PHSTORE] AFTER NET INIT\n";
    raw_marker(after_net, sizeof(after_net) - 1);
    if (net_result != 0) {
        char line[96];
        int n = snprintf(line, sizeof(line), "[PHSTORE] sceNetInit returned 0x%08x\n", (unsigned)net_result);
        if (n > 0) raw_marker(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        return 1;
    }

    static const char before_socket[] = "[PHSTORE] BEFORE SOCKET\n";
    raw_marker(before_socket, sizeof(before_socket) - 1);
    int server = socket(AF_INET, SOCK_STREAM, 0);
    if (server < 0) {
        char line[96];
        int n = snprintf(line, sizeof(line), "[PHSTORE] socket failed errno=%d\n", errno);
        if (n > 0) raw_marker(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        return 2;
    }
    int yes = 1;
    (void)setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PHSTORE_DIAG_PORT);
    if (inet_pton(AF_INET, PHSTORE_DIAG_ADDRESS, &address.sin_addr) != 1 ||
        bind(server, (struct sockaddr *)&address, sizeof(address)) != 0) {
        char line[96];
        int n = snprintf(line, sizeof(line), "[PHSTORE] bind failed errno=%d\n", errno);
        if (n > 0) raw_marker(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        close(server);
        return 3;
    }
    static const char before_listen[] = "[PHSTORE] BEFORE LISTEN\n";
    raw_marker(before_listen, sizeof(before_listen) - 1);
    if (listen(server, 16) != 0) {
        char line[96];
        int n = snprintf(line, sizeof(line), "[PHSTORE] listen failed errno=%d\n", errno);
        if (n > 0) raw_marker(line, (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1);
        close(server);
        return 4;
    }
    static const char ready[] = "[PHSTORE] HTTP LISTEN READY 127.0.0.1:1905\n";
    raw_marker(ready, sizeof(ready) - 1);
    static const char accept_loop[] = "[PHSTORE] ENTERING ACCEPT LOOP\n";
    raw_marker(accept_loop, sizeof(accept_loop) - 1);

    for (;;) {
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            continue;
        }
        pthread_t worker;
        if (pthread_create(&worker, NULL, client_worker, (void *)(intptr_t)client) == 0)
            (void)pthread_detach(worker);
        else {
            handle_client(client);
            (void)close(client);
        }
    }
}
