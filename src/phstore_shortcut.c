/*
 * PH Store shortcut metadata/file preparation. Native shortcut registration
 * is delegated to the vendored ps5-pkg-manager install_service/helper engine.
 */
#include "phstore_config.h"
#include "phstore_notification.h"
#include "phstore_shortcut.h"
#include "phstore_assets.h"
#include "phstore_install.h"
#include "install_service.h"
#include "app_info.h"
#include "sqlite3.h"

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#define PHSTORE_TITLE_ID "PHST00002"
#define PHSTORE_REGISTER_ROOT "/user/app/"
#define PHSTORE_APP_DIR PHSTORE_REGISTER_ROOT PHSTORE_TITLE_ID
#define PHSTORE_SCE_SYS PHSTORE_APP_DIR "/sce_sys"
#define PHSTORE_PARAM_PATH PHSTORE_SCE_SYS "/param.json"
#define PHSTORE_ICON_PATH PHSTORE_SCE_SYS "/icon0.png"
#define PHSTORE_REGISTERED_PATH PHSTORE_SCE_SYS "/.phstore-registered"

static pthread_mutex_t shortcut_mutex = PTHREAD_MUTEX_INITIALIZER;
/* A marker alone is insufficient; a new daemon also checks the shell database. */
static atomic_int registered_this_run = 0;

static uint64_t shortcut_ms(void) {
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;
    return (uint64_t)t.tv_sec*1000u+(uint64_t)t.tv_nsec/1000000u;
}

static int shortcut_shell_registered(void) {
    /* Shortcut tiles may have size=0, unlike installed game packages. Require
     * an actual healthy shell row, never the /user/app files alone. Any unreadable
     * or unsupported database schema falls back to normal helper registration. */
    sqlite3 *db=NULL;sqlite3_stmt *stmt=NULL;int found=0;
    pthread_mutex_lock(&g_appinfo_db_mutex);
    if(sqlite3_open_v2("/system_data/priv/mms/app.db",&db,
                      SQLITE_OPEN_READONLY|SQLITE_OPEN_FULLMUTEX,NULL)==SQLITE_OK) {
        sqlite3_busy_timeout(db,100);
        if(sqlite3_prepare_v2(db,"SELECT titleId FROM tbl_contentinfo WHERE titleId=? AND installStatus=0 AND contentStatus=0 LIMIT 1",
                             -1,&stmt,NULL)==SQLITE_OK) {
            sqlite3_bind_text(stmt,1,PHSTORE_TITLE_ID,-1,SQLITE_STATIC);
            found=sqlite3_step(stmt)==SQLITE_ROW;
        }
    }
    if(stmt)sqlite3_finalize(stmt);
    if(db)sqlite3_close(db);
    pthread_mutex_unlock(&g_appinfo_db_mutex);
    return found;
}

static void shortcut_log(const char *format, ...) {
    int saved_errno = errno;
    char message[768];
    va_list args;
    va_start(args, format);
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);
    fprintf(stderr, "[PHSTORE/SHORTCUT] %s\n", message);
    phstore_install_detailed_log("SHORTCUT", message);
    errno = saved_errno;
}

static int ensure_directory(const char *path) {
    if (mkdir(path, 0755) == 0) return 0;
    if (errno != EEXIST) return -1;
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISDIR(info.st_mode)) { errno = ENOTDIR; return -1; }
    return 0;
}

static int file_matches(const char *path, const unsigned char *expected, size_t size) {
    struct stat info;
    if (stat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 0 ||
        (uint64_t)info.st_size != (uint64_t)size) return 0;
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    unsigned char chunk[4096];
    size_t offset = 0;
    int matches = 1;
    while (offset < size) {
        size_t amount = size - offset;
        if (amount > sizeof(chunk)) amount = sizeof(chunk);
        if (fread(chunk, 1, amount, file) != amount || memcmp(chunk, expected + offset, amount) != 0) {
            matches = 0; break;
        }
        offset += amount;
    }
    fclose(file);
    return matches;
}

static uint32_t fnv1a32(const unsigned char *bytes, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; i++) hash = (hash ^ bytes[i]) * 16777619u;
    return hash;
}

static size_t registration_marker(unsigned char *buffer, size_t capacity) {
    int length = snprintf((char *)buffer, capacity, "PHST00002 registered %08X %08X\n",
        fnv1a32(phstore_shortcut_param_json, phstore_shortcut_param_json_size),
        fnv1a32(phstore_shortcut_icon, phstore_shortcut_icon_size));
    return length > 0 && (size_t)length < capacity ? (size_t)length : 0;
}

static int refuse_foreign_metadata(void) {
    FILE *file = fopen(PHSTORE_PARAM_PATH, "rb");
    if (!file) return errno == ENOENT ? 0 : -1;
    char buffer[4096];
    size_t count = fread(buffer, 1, sizeof(buffer) - 1, file);
    int read_error = ferror(file), has_more = !feof(file);
    fclose(file);
    if (read_error || has_more) { errno = EFBIG; return -1; }
    buffer[count] = '\0';
    const char *field = strstr(buffer, "\"titleId\"");
    if (!field) { errno = EEXIST; return -1; }
    field += sizeof("\"titleId\"") - 1;
    while (*field == ' ' || *field == '\t' || *field == '\r' || *field == '\n') field++;
    if (*field++ != ':') { errno = EEXIST; return -1; }
    while (*field == ' ' || *field == '\t' || *field == '\r' || *field == '\n') field++;
    if (*field++ != '"' || strncmp(field, PHSTORE_TITLE_ID, sizeof(PHSTORE_TITLE_ID) - 1) != 0 ||
        field[sizeof(PHSTORE_TITLE_ID) - 1] != '"') { errno = EEXIST; return -1; }
    return 0;
}

static int write_atomic(const char *path, const unsigned char *bytes, size_t size) {
    char temporary[320];
    int length = snprintf(temporary, sizeof(temporary), "%s.phstore-tmp", path);
    if (length < 0 || (size_t)length >= sizeof(temporary)) { errno = ENAMETOOLONG; return -1; }
    FILE *file = fopen(temporary, "wb");
    if (!file) return -1;
    int good = fwrite(bytes, 1, size, file) == size && fflush(file) == 0;
    if (good && fsync(fileno(file)) != 0) good = 0;
    int saved_error = errno;
    if (fclose(file) != 0) good = 0;
    if (!good || rename(temporary, path) != 0) {
        if (good) saved_error = errno;
        unlink(temporary);
        errno = saved_error ? saved_error : EIO;
        return -1;
    }
    return 0;
}

static int shortcut_is_current(void) {
    unsigned char marker[80];
    size_t marker_size = registration_marker(marker, sizeof(marker));
    return file_matches(PHSTORE_PARAM_PATH, phstore_shortcut_param_json, phstore_shortcut_param_json_size) &&
        file_matches(PHSTORE_ICON_PATH, phstore_shortcut_icon, phstore_shortcut_icon_size) &&
        marker_size && file_matches(PHSTORE_REGISTERED_PATH, marker, marker_size);
}

static int run_install(int force, uint32_t *error_code) {
    if (error_code) *error_code = 0;
    pthread_mutex_lock(&shortcut_mutex);
    uint64_t began=shortcut_ms();
    if (!force && shortcut_is_current() &&
        (atomic_load(&registered_this_run) || shortcut_shell_registered())) {
        atomic_store(&registered_this_run,1);
        shortcut_log("skip: matching assets/marker and confirmed registration elapsed_ms=%llu",(unsigned long long)(shortcut_ms()-began));
        phstore_notify("PHStore2\nKısayol hazır.");
        pthread_mutex_unlock(&shortcut_mutex); return 0;
    }
    atomic_store(&registered_this_run, 0);
    shortcut_log("begin force=%d stage=%d title=%s register_root=%s metadata=%s icon=%s",
        force, PHSTORE_INSTALL_DIAGNOSTIC_STAGE, PHSTORE_TITLE_ID,
        PHSTORE_REGISTER_ROOT, PHSTORE_PARAM_PATH, PHSTORE_ICON_PATH);
    phstore_notify("PHStore2\nKısayol ana ekrana yükleniyor...");
    if (ensure_directory(PHSTORE_APP_DIR) != 0 || ensure_directory(PHSTORE_SCE_SYS) != 0 ||
        refuse_foreign_metadata() != 0 ||
        (!file_matches(PHSTORE_PARAM_PATH, phstore_shortcut_param_json, phstore_shortcut_param_json_size) &&
         write_atomic(PHSTORE_PARAM_PATH, phstore_shortcut_param_json, phstore_shortcut_param_json_size) != 0) ||
        (!file_matches(PHSTORE_ICON_PATH, phstore_shortcut_icon, phstore_shortcut_icon_size) &&
         write_atomic(PHSTORE_ICON_PATH, phstore_shortcut_icon, phstore_shortcut_icon_size) != 0)) {
        int error = errno ? errno : EIO;
        if (error_code) *error_code = (uint32_t)-error;
        shortcut_log("metadata preparation failed errno=%d", error);
        phstore_notify("PH Store\nKısayol kurulamadı: 0x%08X", (unsigned)-error);
        pthread_mutex_unlock(&shortcut_mutex);
        return -1;
    }

    shortcut_log("metadata ready param_bytes=%zu icon_bytes=%zu; BEFORE helper registration",
        phstore_shortcut_param_json_size, phstore_shortcut_icon_size);
    /* Match pkg-manager app_installer.c: API takes the parent application
     * root, while metadata/icon are written under root/title_id/sce_sys. */
    int result = install_service_shortcut(PHSTORE_TITLE_ID, PHSTORE_REGISTER_ROOT);
    shortcut_log("AFTER helper registration rc=0x%08X elapsed_ms=%llu", (unsigned)result,(unsigned long long)(shortcut_ms()-began));
    if (result != 0) {
        if (error_code) *error_code = (uint32_t)result;
        shortcut_log("upstream install_service_shortcut rc=0x%08X", (unsigned)result);
        phstore_notify("PH Store\nKısayol kurulamadı: 0x%08X", (unsigned)result);
        pthread_mutex_unlock(&shortcut_mutex);
        return -1;
    }
    unsigned char marker[80];
    size_t marker_size = registration_marker(marker, sizeof(marker));
    if (!marker_size || write_atomic(PHSTORE_REGISTERED_PATH, marker, marker_size) != 0) {
        int error = errno ? errno : EIO;
        if (error_code) *error_code = (uint32_t)-error;
        shortcut_log("registration marker write failed errno=%d", error);
        phstore_notify("PH Store\nKısayol durumu yazılamadı: 0x%08X", (unsigned)-error);
        pthread_mutex_unlock(&shortcut_mutex);
        return -1;
    }
    atomic_store(&registered_this_run, 1);
    shortcut_log("upstream helper registered home-screen shortcut title=%s name=PH Store", PHSTORE_TITLE_ID);
    phstore_notify("PHStore2\nKısayol kuruldu.");
    pthread_mutex_unlock(&shortcut_mutex);
    return 0;
}

int phstore_shortcut_install_if_needed(uint32_t *error_code) { return run_install(0, error_code); }
int phstore_shortcut_force_install(uint32_t *error_code) { return run_install(1, error_code); }
int phstore_shortcut_is_current(void) {
    return atomic_load(&registered_this_run) && shortcut_is_current();
}
