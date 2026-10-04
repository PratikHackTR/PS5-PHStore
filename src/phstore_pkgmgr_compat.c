/* Explicitly disabled upstream subsystems for the catalog-source build.
 * Installer orchestration and the stream server remain the vendored code;
 * only SMB discovery and browser-pushed live-upload inputs are omitted. */
#include "phstore_notification.h"
#include "pkg_cache.h"
#include "pkg_scanner.h"
#include "smb_client.h"
#include "ws_stream.h"
#include "ws_upload.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void ps5_notify(const char *format, ...) {
    char message[512];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    phstore_notify("%s", message);
}

void pkg_cache_get_settings(app_settings_t *settings) {
    if (settings) memset(settings, 0, sizeof(*settings));
}

int pkg_scanner_find_part(const uint8_t *uuid, const char *filename,
                          uint32_t part, char *path, size_t capacity) {
    (void)uuid; (void)filename; (void)part;
    if (path && capacity) path[0] = '\0';
    return -1;
}

int pkg_scanner_find_part_ex(const uint8_t *uuid, const char *filename,
                             uint32_t part, char *path, size_t capacity,
                             uint32_t *detected_part) {
    if (detected_part) *detected_part = 0;
    return pkg_scanner_find_part(uuid, filename, part, path, capacity);
}

struct smb_file_session;
smb_file_session_t *smb_file_session_open(const char *url) { (void)url; return NULL; }
ssize_t smb_file_session_read(smb_file_session_t *session, void *buffer, size_t count, uint64_t offset) {
    (void)session; (void)buffer; (void)count; (void)offset; return -1;
}
uint64_t smb_file_session_get_size(smb_file_session_t *session) { (void)session; return 0; }
void smb_file_session_close(smb_file_session_t *session) { (void)session; }
ssize_t smb_client_pread(const char *url, void *buffer, size_t count, uint64_t offset) {
    (void)url; (void)buffer; (void)count; (void)offset; return -1;
}
int smb_client_parse_pkg(const char *url, pkg_detail_t *detail) { (void)url; (void)detail; return -1; }
int smb_client_get_icon(const char *url, uint8_t **data, size_t *size) {
    (void)url; if (data) *data = NULL; if (size) *size = 0; return -1;
}

int ws_live_check_id(const char *id) { (void)id; return 0; }
uint64_t ws_live_get_total(void) { return 0; }
int ws_live_attach(void) { return -1; }
void ws_live_detach(void) {}
long ws_live_read(uint64_t offset, void *buffer, size_t length) {
    (void)offset; (void)buffer; (void)length; return -1;
}
int ws_live_wait_header(int timeout) { (void)timeout; return -1; }
size_t ws_live_get_header(uint8_t *buffer, size_t length) { (void)buffer; (void)length; return 0; }
void ws_live_abort(void) {}
void ws_live_destroy(void) {}
int ws_direct_get_metadata(const char *sid, char *title, size_t title_max,
                           char *title_id, size_t id_max, char *version,
                           size_t version_max, char *kind, size_t kind_max) {
    (void)sid;
    if (title && title_max) title[0] = '\0';
    if (title_id && id_max) title_id[0] = '\0';
    if (version && version_max) version[0] = '\0';
    if (kind && kind_max) kind[0] = '\0';
    return -1;
}
