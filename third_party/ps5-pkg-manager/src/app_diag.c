/*
 * PKG Manager - System Diagnostics & Reporting
 *
 * Gathers system, storage, app database, and process diagnostic state.
 */

#include "app_diag.h"
#include "app_info.h"
#include "sqlite3.h"
#include "notification.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>

typedef struct {
    char *data;
    size_t len;
    size_t cap;
} diag_buf_t;

static void diag_init(diag_buf_t *b) {
    b->cap = 64 * 1024;
    b->data = (char *)malloc(b->cap);
    b->len = 0;
    if (b->data) {
        b->data[0] = '\0';
    }
}

static void diag_append(diag_buf_t *b, const char *fmt, ...) {
    if (!b->data) return;

    va_list ap, ap_copy;
    va_start(ap, fmt);
    va_copy(ap_copy, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);

    if (n <= 0) {
        va_end(ap_copy);
        return;
    }

    if (b->len + (size_t)n + 1 >= b->cap) {
        size_t new_cap = b->cap * 2;
        while (new_cap <= b->len + (size_t)n + 1) {
            new_cap *= 2;
        }
        char *p = (char *)realloc(b->data, new_cap);
        if (!p) {
            va_end(ap_copy);
            return;
        }
        b->data = p;
        b->cap = new_cap;
    }

    vsnprintf(b->data + b->len, (size_t)n + 1, fmt, ap_copy);
    va_end(ap_copy);
    b->len += (size_t)n;
}

static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void dump_sfo_keys(diag_buf_t *b, const char *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        diag_append(b, "      [Open failed: errno=%d (%s)]\n", errno, strerror(errno));
        return;
    }

    uint8_t sfo[32768];
    ssize_t n = read(fd, sfo, sizeof(sfo));
    close(fd);

    if (n < 20 || memcmp(sfo, "\x00PSF", 4) != 0) {
        diag_append(b, "      [Invalid SFO magic or read size %zd]\n", n);
        return;
    }

    size_t sfo_len = (size_t)n;
    uint32_t key_table_start = read_le32(sfo + 0x08);
    uint32_t data_table_start = read_le32(sfo + 0x0C);
    uint32_t entry_count = read_le32(sfo + 0x10);

    diag_append(b, "      [SFO: %u entries, key_tab=0x%X, data_tab=0x%X]\n",
                entry_count, key_table_start, data_table_start);

    if (key_table_start >= sfo_len || data_table_start >= sfo_len || entry_count > 1024) {
        diag_append(b, "      [Malformed table offsets]\n");
        return;
    }

    const uint8_t *entries = sfo + 20;
    for (uint32_t i = 0; i < entry_count; i++) {
        if ((size_t)(20 + (i + 1) * 16) > sfo_len) break;
        const uint8_t *e = entries + i * 16;
        uint16_t key_off = read_le16(e);
        uint32_t data_len = read_le32(e + 4);
        uint32_t data_off = read_le32(e + 12);

        if (key_table_start + key_off >= sfo_len) continue;
        const char *key = (const char *)(sfo + key_table_start + key_off);
        size_t max_key_len = sfo_len - (key_table_start + key_off);
        if (!memchr(key, '\0', max_key_len)) continue;

        if ((uint64_t)data_off > sfo_len || (uint64_t)data_len > sfo_len || (uint64_t)data_table_start + (uint64_t)data_off + (uint64_t)data_len > (uint64_t)sfo_len) continue;
        const char *data = (const char *)(sfo + data_table_start + data_off);

        char val_str[256] = {0};
        size_t copy_len = (data_len < sizeof(val_str) - 1) ? data_len : sizeof(val_str) - 1;
        while (copy_len > 0 && (data[copy_len - 1] == '\0' || data[copy_len - 1] == '\n' || data[copy_len - 1] == '\r')) {
            copy_len--;
        }
        memcpy(val_str, data, copy_len);
        val_str[copy_len] = '\0';

        /* Print interesting keys */
        if (strcmp(key, "APP_VER") == 0 || strcmp(key, "VERSION") == 0 ||
            strcmp(key, "TITLE") == 0 || strcmp(key, "TITLE_ID") == 0 ||
            strcmp(key, "CATEGORY") == 0 || strcmp(key, "CONTENT_ID") == 0 ||
            strcmp(key, "ATTRIBUTE") == 0 || strstr(key, "VER") != NULL) {
            diag_append(b, "        SFO Key [%s] = \"%s\"\n", key, val_str);
        }
    }
}

static void dump_json_preview(diag_buf_t *b, const char *filepath) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) {
        diag_append(b, "      [Open failed: errno=%d (%s)]\n", errno, strerror(errno));
        return;
    }

    char buf[8192];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);

    if (n <= 0) {
        diag_append(b, "      [Empty or read failed]\n");
        return;
    }
    buf[n] = '\0';

    /* Print lines containing version or title */
    diag_append(b, "      [JSON content snippet (first 1KB)]:\n");
    char snippet[1024];
    size_t snip_len = (size_t)n < sizeof(snippet) - 1 ? (size_t)n : sizeof(snippet) - 1;
    memcpy(snippet, buf, snip_len);
    snippet[snip_len] = '\0';
    diag_append(b, "        %s\n", snippet);
}

static void test_file(diag_buf_t *b, const char *filepath) {
    struct stat st;
    if (stat(filepath, &st) == 0) {
        diag_append(b, "    FOUND: %s (size: %lld bytes, mtime: %ld)\n",
                    filepath, (long long)st.st_size, (long)st.st_mtime);
        if (strstr(filepath, ".sfo") != NULL) {
            dump_sfo_keys(b, filepath);
        } else if (strstr(filepath, ".json") != NULL) {
            dump_json_preview(b, filepath);
        }
    } else {
        if (errno != ENOENT) {
            diag_append(b, "    NO ACCESS: %s (errno=%d: %s)\n", filepath, errno, strerror(errno));
        }
    }
}

static void inspect_title_id(diag_buf_t *b, const char *tid) {
    diag_append(b, "\n--- Inspecting Title ID: %s ---\n", tid);

    char path[512];

    /* 1. Patch directories (where updates live) */
    snprintf(path, sizeof(path), "/user/patch/%s/sce_sys/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/patch/%s/sce_sys/param.json", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/patch/%s/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/patch/%s/param.json", tid);
    test_file(b, path);

    snprintf(path, sizeof(path), "/user/patch0/%s/sce_sys/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/patch0/%s/sce_sys/param.json", tid);
    test_file(b, path);

    /* 2. Base package directories */
    snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/app/%s/sce_sys/param.json", tid);
    test_file(b, path);

    /* 3. App metadata directories */
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/param.json", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/patch/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/%s/patch/param.json", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/external/%s/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/system_data/priv/appmeta/external/%s/param.json", tid);
    test_file(b, path);

    /* 4. User appmeta */
    snprintf(path, sizeof(path), "/user/appmeta/%s/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/appmeta/%s/param.json", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/appmeta/%s/patch/param.sfo", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/user/appmeta/%s/patch/param.json", tid);
    test_file(b, path);

    /* 5. system_ex */
    snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.json", tid);
    test_file(b, path);
    snprintf(path, sizeof(path), "/system_ex/app/%s/sce_sys/param.sfo", tid);
    test_file(b, path);
}

static void list_directory(diag_buf_t *b, const char *dir_path, char tids[][32], int *tid_count, int max_tids) {
    diag_append(b, "\n[Listing Directory: %s]\n", dir_path);
    DIR *d = opendir(dir_path);
    if (!d) {
        diag_append(b, "  FAILED to open directory: errno=%d (%s)\n", errno, strerror(errno));
        return;
    }

    struct dirent *de;
    int count = 0;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        diag_append(b, "  entry: %s (type=%d)\n", de->d_name, (int)de->d_type);
        count++;

        /* Check if looks like a title id (CUSAxxxxx or PPSAxxxxx) */
        if ((strncmp(de->d_name, "CUSA", 4) == 0 || strncmp(de->d_name, "PPSA", 4) == 0 ||
             strncmp(de->d_name, "cusa", 4) == 0 || strncmp(de->d_name, "ppsa", 4) == 0) &&
            strlen(de->d_name) <= 16) {
            /* Add to unique tids */
            int already = 0;
            for (int i = 0; i < *tid_count; i++) {
                if (strcasecmp(tids[i], de->d_name) == 0) {
                    already = 1;
                    break;
                }
            }
            if (!already && *tid_count < max_tids) {
                strncpy(tids[*tid_count], de->d_name, sizeof(tids[*tid_count]) - 1);
                (*tid_count)++;
            }
        }
    }
    closedir(d);
    diag_append(b, "  Total entries: %d\n", count);
}

static void dump_sqlite_tables(diag_buf_t *b, const char *db_path, char tids[][32], int tid_count) {
    diag_append(b, "\n========================================\n");
    diag_append(b, "[SQLite Inspection: %s]\n", db_path);
    diag_append(b, "========================================\n");

    if (access(db_path, R_OK) != 0) {
        diag_append(b, "access() failed for %s: errno=%d (%s)\n", db_path, errno, strerror(errno));
    }

    pthread_mutex_lock(&g_appinfo_db_mutex);
    sqlite3 *db = NULL;
    int rc = sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL);
    if (rc != SQLITE_OK) {
        diag_append(b, "sqlite3_open_v2 failed: rc=%d, err='%s'\n", rc, db ? sqlite3_errmsg(db) : "NULL");
        if (db) sqlite3_close(db);
        pthread_mutex_unlock(&g_appinfo_db_mutex);
        return;
    }

    diag_append(b, "sqlite3_open_v2 SUCCESS! Inspecting schema...\n\n");

    /* 1. Dump all tables and their CREATE statements */
    sqlite3_stmt *stmt = NULL;
    const char *sql_tables = "SELECT name, sql FROM sqlite_master WHERE type='table' ORDER BY name;";
    if (sqlite3_prepare_v2(db, sql_tables, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *name = (const char *)sqlite3_column_text(stmt, 0);
            const char *create_sql = (const char *)sqlite3_column_text(stmt, 1);
            diag_append(b, "TABLE: %s\n  %s\n\n", name ? name : "NULL", create_sql ? create_sql : "NULL");
        }
        sqlite3_finalize(stmt);
    } else {
        diag_append(b, "Failed to query sqlite_master: %s\n", sqlite3_errmsg(db));
    }

    /* 2. For each table, dump contents */
    const char *candidate_tables[] = {
        "tbl_contentinfo",
        "tbl_appinfo",
        "tbl_patchinfo",
        "tbl_addcontinfo",
        "tbl_savedata",
        NULL
    };

    for (int t = 0; candidate_tables[t] != NULL; t++) {
        const char *tname = candidate_tables[t];
        char query[512];
        snprintf(query, sizeof(query), "SELECT * FROM %s LIMIT 20;", tname);
        diag_append(b, "\n--- Sample Rows from [%s] ---\n", tname);

        if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
            int cols = sqlite3_column_count(stmt);
            diag_append(b, "Columns (%d): ", cols);
            for (int c = 0; c < cols; c++) {
                diag_append(b, "[%s] ", sqlite3_column_name(stmt, c));
            }
            diag_append(b, "\n");

            int row = 0;
            while (sqlite3_step(stmt) == SQLITE_ROW && row < 20) {
                diag_append(b, "Row %d:\n", ++row);
                for (int c = 0; c < cols; c++) {
                    const char *val = (const char *)sqlite3_column_text(stmt, c);
                    if (val && val[0] != '\0') {
                        diag_append(b, "  %s = \"%s\"\n", sqlite3_column_name(stmt, c), val);
                    }
                }
            }
            sqlite3_finalize(stmt);
        } else {
            diag_append(b, "Query failed for %s: %s\n", tname, sqlite3_errmsg(db));
        }

        /* 3. Query specifically for detected Title IDs */
        for (int i = 0; i < tid_count; i++) {
            snprintf(query, sizeof(query), "SELECT * FROM %s WHERE titleId = ? LIMIT 5;", tname);
            int qrc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
            if (qrc != SQLITE_OK) {
                snprintf(query, sizeof(query), "SELECT * FROM %s WHERE title_id = ? LIMIT 5;", tname);
                qrc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
            }
            if (qrc == SQLITE_OK) {
                sqlite3_bind_text(stmt, 1, tids[i], -1, SQLITE_STATIC);
                int cols = sqlite3_column_count(stmt);
                int found_rows = 0;
                while (sqlite3_step(stmt) == SQLITE_ROW) {
                    if (found_rows == 0) {
                        diag_append(b, "  Matched rows in [%s] for Title ID '%s':\n", tname, tids[i]);
                    }
                    found_rows++;
                    for (int c = 0; c < cols; c++) {
                        const char *val = (const char *)sqlite3_column_text(stmt, c);
                        if (val && val[0] != '\0') {
                            diag_append(b, "    %s = \"%s\"\n", sqlite3_column_name(stmt, c), val);
                        }
                    }
                }
                sqlite3_finalize(stmt);
            }
        }
    }

    sqlite3_close(db);
    pthread_mutex_unlock(&g_appinfo_db_mutex);
}

char *app_diag_generate_report(void) {
    diag_buf_t b;
    diag_init(&b);

    time_t now = time(NULL);
    struct tm *tm_info = localtime(&now);
    char time_str[64] = {0};
    if (tm_info) strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

    diag_append(&b, "=================================================================\n");
    diag_append(&b, "            PKG MANAGER & SYSTEM APP INFO DIAGNOSTIC             \n");
    diag_append(&b, "=================================================================\n");
    diag_append(&b, "Version: %s (%s, %s)\n", PKGMGR_VERSION, PKGMGR_BUILD_COMMIT, PKGMGR_BUILD_DATE);
    diag_append(&b, "Timestamp: %s\n", time_str);
    diag_append(&b, "Process: PID=%d, UID=%d, EUID=%d, GID=%d, EGID=%d\n",
                getpid(), getuid(), geteuid(), getgid(), getegid());

    /* Check access permissions to key system root paths */
    diag_append(&b, "\n[Path Access Tests]\n");
    const char *paths_to_check[] = {
        "/system_data/priv/mms/app.db",
        "/system_data/priv/appmeta",
        "/system_data/priv/mms",
        "/user/app",
        "/user/patch",
        "/user/patch0",
        "/user/appmeta",
        "/user/addcont",
        "/system_ex/app",
        "/mnt/usb0",
        "/mnt/usb1",
        "/mnt/disc",
        NULL
    };

    for (int i = 0; paths_to_check[i] != NULL; i++) {
        const char *p = paths_to_check[i];
        int r_ok = access(p, R_OK);
        int f_ok = access(p, F_OK);
        struct stat st;
        int st_ok = stat(p, &st);
        diag_append(&b, "  %-35s : F_OK=%s, R_OK=%s, stat=%s (mode=0%o, size=%lld)\n",
                    p,
                    (f_ok == 0) ? "YES" : "NO",
                    (r_ok == 0) ? "YES" : "NO",
                    (st_ok == 0) ? "YES" : "NO",
                    (st_ok == 0) ? (unsigned int)(st.st_mode & 0777) : 0,
                    (st_ok == 0) ? (long long)st.st_size : -1);
    }

    /* List directories and collect Title IDs */
    char tids[128][32];
    int tid_count = 0;

    list_directory(&b, "/user/app", tids, &tid_count, 128);
    list_directory(&b, "/user/patch", tids, &tid_count, 128);
    list_directory(&b, "/user/patch0", tids, &tid_count, 128);
    list_directory(&b, "/user/appmeta", tids, &tid_count, 128);
    list_directory(&b, "/system_data/priv/appmeta", tids, &tid_count, 128);
    list_directory(&b, "/system_data/priv/mms", tids, &tid_count, 128);

    diag_append(&b, "\n[Discovered Title IDs (%d total)]:\n", tid_count);
    for (int i = 0; i < tid_count; i++) {
        diag_append(&b, "  [%d] %s\n", i + 1, tids[i]);
    }

    /* Inspect metadata for all Title IDs */
    for (int i = 0; i < tid_count; i++) {
        inspect_title_id(&b, tids[i]);
    }

    /* SQLite inspection */
    dump_sqlite_tables(&b, "/system_data/priv/mms/app.db", tids, tid_count);

    /* Check for other databases in /system_data/priv/mms */
    DIR *d = opendir("/system_data/priv/mms");
    if (d) {
        struct dirent *de;
        while ((de = readdir(d)) != NULL) {
            if (strstr(de->d_name, ".db") != NULL && strcmp(de->d_name, "app.db") != 0) {
                char db_full[512];
                snprintf(db_full, sizeof(db_full), "/system_data/priv/mms/%s", de->d_name);
                dump_sqlite_tables(&b, db_full, tids, tid_count);
            }
        }
        closedir(d);
    }

    diag_append(&b, "\n=================================================================\n");
    diag_append(&b, "                      END OF DIAGNOSTIC REPORT                   \n");
    diag_append(&b, "=================================================================\n");

    /* Also save report directly to local files */
    const char *save_paths[] = {
        "/data/app_debug.txt",
        "/mnt/usb0/app_debug.txt",
        "/mnt/usb1/app_debug.txt",
        NULL
    };
    for (int i = 0; save_paths[i] != NULL; i++) {
        FILE *f = fopen(save_paths[i], "wb");
        if (f) {
            fwrite(b.data, 1, b.len, f);
            fclose(f);
        }
    }

    return b.data;
}
