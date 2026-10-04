/*
 * PKG Manager - Installed Application Database & Version Inspector
 *
 * Queries appinfo.db and appmeta directory trees to inspect installed
 * titles, verify versions, and prevent duplicate installations.
 */

#include "app_info.h"
#include "sqlite3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>

pthread_mutex_t g_appinfo_db_mutex = PTHREAD_MUTEX_INITIALIZER;

#if defined(__Prospero__) || defined(PS5_BUILD)
extern int sceAppInstUtilInitialize(void);
extern int sceAppInstUtilAppExists(const char *title_id);
extern int sceAppInstUtilGetAddcontInstalledStatus(const char *content_id, int *status);

static void ensure_appinstutil_init(void) {
    static int inited = 0;
    if (!inited) {
        sceAppInstUtilInitialize();
        inited = 1;
    }
}
#endif

/* Endian helpers */
static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Helper to check if file or directory exists using stat (uses EUID) */
static int path_exists(const char *path) {
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

void app_info_normalize_version(const char *in, char *out, size_t max_out) {
    if (!out || max_out == 0) return;
    out[0] = '\0';
    if (!in || in[0] == '\0') return;

    while (*in == 'v' || *in == 'V' || isspace((unsigned char)*in)) in++;
    if (*in == '\0') {
        strncpy(out, "v1.00", max_out - 1);
        out[max_out - 1] = '\0';
        return;
    }

    int maj = 0, min = 0, patch = 0;
    int matched = sscanf(in, "%d.%d.%d", &maj, &min, &patch);
    if (matched == 3) {
        if (min == 0 && patch > 0) {
            snprintf(out, max_out, "v%d.%02d", maj, patch);
        } else if (patch == 0) {
            snprintf(out, max_out, "v%d.%02d", maj, min);
        } else {
            snprintf(out, max_out, "v%d.%d.%d", maj, min, patch);
        }
    } else if (matched == 2) {
        if (in[0] == '0' || (in[1] != '.' && in[1] != '\0' && in[2] == '.')) {
            snprintf(out, max_out, "v%02d.%02d", maj, min);
        } else {
            snprintf(out, max_out, "v%d.%02d", maj, min);
        }
    } else if (matched == 1) {
        snprintf(out, max_out, "v%d.00", maj);
    } else {
        snprintf(out, max_out, "v%s", in);
    }
}

static void parse_version_parts(const char *v, int *maj, int *min, int *patch) {
    *maj = 0;
    *min = 0;
    *patch = 0;
    if (!v) return;
    while (*v == 'v' || *v == 'V' || isspace((unsigned char)*v)) v++;
    if (*v == '\0') return;

    int a = 0, b = 0, c = 0;
    int matched = sscanf(v, "%d.%d.%d", &a, &b, &c);
    if (matched == 3) {
        *maj = a;
        if (b == 0 && c > 0) {
            *min = c;
            *patch = 0;
        } else {
            *min = b;
            *patch = c;
        }
    } else if (matched == 2) {
        *maj = a;
        *min = b;
        *patch = 0;
    } else if (matched == 1) {
        *maj = a;
        *min = 0;
        *patch = 0;
    }
}

int app_info_compare_versions(const char *ver_a, const char *ver_b) {
    int maj_a = 0, min_a = 0, patch_a = 0;
    int maj_b = 0, min_b = 0, patch_b = 0;
    parse_version_parts(ver_a, &maj_a, &min_a, &patch_a);
    parse_version_parts(ver_b, &maj_b, &min_b, &patch_b);

    if (maj_a != maj_b) return maj_a - maj_b;
    if (min_a != min_b) return min_a - min_b;
    return patch_a - patch_b;
}

/* Helper to extract key value from json */
static int extract_json_str(const char *json, size_t json_len, const char *key, char *out, size_t out_max) {
    char pattern[128];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *p = json;
    const char *end = json + json_len;

    while (p < end) {
        const char *found = strstr(p, pattern);
        if (!found || found >= end) return -1;

        const char *colon = strchr(found + strlen(pattern), ':');
        if (!colon || colon >= end) return -1;

        const char *q = colon + 1;
        while (q < end && isspace((unsigned char)*q)) q++;

        if (q < end && *q == '"') {
            q++;
            size_t idx = 0;
            while (q < end && *q != '"') {
                if (*q == '\\' && (q + 1) < end) q++;
                if (idx + 1 < out_max) out[idx++] = *q;
                q++;
            }
            out[idx] = '\0';
            return 0;
        }
        p = found + strlen(pattern);
    }
    return -1;
}

/* Helper to parse version from param.json file */
static int read_param_json_version(const char *filepath, char *out_ver, size_t max_ver) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -1;

    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char ver[64] = {0};
    if (extract_json_str(buf, n, "contentVersion", ver, sizeof(ver)) == 0 ||
        extract_json_str(buf, n, "appVersion", ver, sizeof(ver)) == 0 ||
        extract_json_str(buf, n, "version", ver, sizeof(ver)) == 0) {
        if (ver[0] != '\0') {
            app_info_normalize_version(ver, out_ver, max_ver);
            return 0;
        }
    }
    return -1;
}

/* Helper to parse version from param.sfo file */
static int read_param_sfo_version(const char *filepath, char *out_ver, size_t max_ver) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -1;

    uint8_t sfo[16384];
    ssize_t n = read(fd, sfo, sizeof(sfo));
    close(fd);
    if (n < 20 || memcmp(sfo, "\x00PSF", 4) != 0) return -1;

    size_t sfo_len = (size_t)n;
    uint32_t key_table_start = read_le32(sfo + 0x08);
    uint32_t data_table_start = read_le32(sfo + 0x0C);
    uint32_t entry_count = read_le32(sfo + 0x10);

    if (key_table_start >= sfo_len || data_table_start >= sfo_len || entry_count > 1024) {
        return -1;
    }

    char app_ver[32] = {0};
    char version[32] = {0};

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

        if (strcmp(key, "APP_VER") == 0 && app_ver[0] == '\0') {
            size_t copy_len = data_len < sizeof(app_ver) ? data_len : sizeof(app_ver) - 1;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(app_ver, data, copy_len);
            app_ver[copy_len] = '\0';
        } else if (strcmp(key, "VERSION") == 0 && version[0] == '\0') {
            size_t copy_len = data_len < sizeof(version) ? data_len : sizeof(version) - 1;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            strncpy(version, data, copy_len);
            version[copy_len] = '\0';
        }
    }

    const char *best = (app_ver[0] != '\0') ? app_ver : version;

    if (best && best[0] != '\0') {
        app_info_normalize_version(best, out_ver, max_ver);
        return 0;
    }

    return -1;
}

/* Helper to check if file in SFO format has matching content_id */
static int sfo_has_content_id(const char *filepath, const char *target_cid) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return 0;

    uint8_t sfo[16384];
    ssize_t n = read(fd, sfo, sizeof(sfo));
    close(fd);
    if (n < 20 || memcmp(sfo, "\x00PSF", 4) != 0) return 0;

    size_t sfo_len = (size_t)n;
    uint32_t key_table_start = read_le32(sfo + 0x08);
    uint32_t data_table_start = read_le32(sfo + 0x0C);
    uint32_t entry_count = read_le32(sfo + 0x10);

    if (key_table_start >= sfo_len || data_table_start >= sfo_len || entry_count > 1024) {
        return 0;
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

        if (strcmp(key, "CONTENT_ID") == 0) {
            size_t copy_len = data_len < 64 ? data_len : 63;
            while (copy_len > 0 && data[copy_len - 1] == '\0') copy_len--;
            char cid[64] = {0};
            strncpy(cid, data, copy_len);
            cid[copy_len] = '\0';
            if (strcmp(cid, target_cid) == 0) return 1;
        }
    }
    return 0;
}

/* Helper to check if file in JSON format has matching content_id */
static int json_has_content_id(const char *filepath, const char *target_cid) {
    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return 0;

    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';

    char cid[64] = {0};
    if (extract_json_str(buf, n, "contentId", cid, sizeof(cid)) == 0) {
        if (strcmp(cid, target_cid) == 0) return 1;
    }
    return 0;
}

/* Helper to extract a value for a specific "key":"NAME" in AppInfoJson */
static int extract_appinfo_json_key(const char *json, size_t json_len, const char *key_name, char *out, size_t max_out) {
    if (!json || !key_name || !out || max_out == 0) return -1;
    out[0] = '\0';

    char target[128];
    snprintf(target, sizeof(target), "\"key\":\"%s\"", key_name);

    const char *p = json;
    const char *end = json + json_len;

    while (p < end) {
        const char *found = strstr(p, target);
        if (!found || found >= end) break;

        /* Find the start and end of this JSON object { ... } */
        const char *obj_start = found;
        while (obj_start > json && *obj_start != '{') obj_start--;

        const char *obj_end = found;
        while (obj_end < end && *obj_end != '}') obj_end++;

        if (*obj_start == '{' && *obj_end == '}') {
            /* Within this object, find "data": */
            const char *d = strstr(obj_start, "\"data\":");
            if (d && d < obj_end) {
                const char *val = d + 7;
                while (val < obj_end && isspace((unsigned char)*val)) val++;
                if (val < obj_end && *val == '"') {
                    val++;
                    size_t idx = 0;
                    while (val < obj_end && *val != '"' && idx + 1 < max_out) {
                        if (*val == '\\' && (val + 1) < obj_end) val++;
                        out[idx++] = *val++;
                    }
                    out[idx] = '\0';
                    return 0;
                } else if (val < obj_end && (isdigit((unsigned char)*val) || *val == '.')) {
                    size_t idx = 0;
                    while (val < obj_end && (isdigit((unsigned char)*val) || *val == '.') && idx + 1 < max_out) {
                        out[idx++] = *val++;
                    }
                    out[idx] = '\0';
                    return 0;
                }
            }
        }
        p = found + strlen(target);
    }
    return -1;
}

/* Query app.db for Title ID */
static int check_app_db_for_title(const char *db_path, const char *title_id, char *out_version, size_t max_ver) {
    if (!db_path || !title_id || title_id[0] == '\0') return 0;

    pthread_mutex_lock(&g_appinfo_db_mutex);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        pthread_mutex_unlock(&g_appinfo_db_mutex);
        return 0;
    }
    sqlite3_busy_timeout(db, 100);

    int found = 0;
    int has_contentinfo = 0;
    sqlite3_stmt *stmt = NULL;

    /* 1. Try tbl_contentinfo (contains AppInfoJson, metaDataPath, installStatus, contentStatus, and size on PS5) */
    const char *sql1 = "SELECT titleId, AppInfoJson, metaDataPath, installStatus, contentStatus, size FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
    int rc = sqlite3_prepare_v2(db, sql1, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sql1 = "SELECT titleId, AppInfoJson, metaDataPath, installStatus FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
        rc = sqlite3_prepare_v2(db, sql1, -1, &stmt, NULL);
    }
    if (rc != SQLITE_OK) {
        sql1 = "SELECT titleId, AppInfoJson, metaDataPath FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
        rc = sqlite3_prepare_v2(db, sql1, -1, &stmt, NULL);
    }
    if (rc == SQLITE_OK) {
        has_contentinfo = 1;
        sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int install_status = 0;
            if (sqlite3_column_count(stmt) >= 4 && sqlite3_column_type(stmt, 3) != SQLITE_NULL) {
                install_status = sqlite3_column_int(stmt, 3);
            }

            int content_status = 0;
            if (sqlite3_column_count(stmt) >= 5 && sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
                content_status = sqlite3_column_int(stmt, 4);
            }

            int64_t size_val = -1;
            if (sqlite3_column_count(stmt) >= 6 && sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
                size_val = sqlite3_column_int64(stmt, 5);
            }

            const char *json = (const char *)sqlite3_column_text(stmt, 1);
            int install_sub_status = 1;
            if (json) {
                char sub_buf[32] = {0};
                if (extract_appinfo_json_key(json, strlen(json), "_install_sub_status", sub_buf, sizeof(sub_buf)) == 0) {
                    install_sub_status = atoi(sub_buf);
                }
            }

            /* Incomplete or aborted installs have contentStatus != 0, size == 0, or _install_sub_status == 2 */
            int is_partially_installed = 0;
            if (content_status != 0 || size_val == 0 || install_sub_status == 2) {
                is_partially_installed = 1;
            }

            if (install_status == 0 && !is_partially_installed) {
                found = 1;
                if (json && out_version && max_ver > 0) {
                    size_t jlen = strlen(json);
                    char vbuf[64] = {0};
                    if (extract_appinfo_json_key(json, jlen, "APP_VER", vbuf, sizeof(vbuf)) == 0 ||
                        extract_appinfo_json_key(json, jlen, "contentVersion", vbuf, sizeof(vbuf)) == 0 ||
                        extract_appinfo_json_key(json, jlen, "appVersion", vbuf, sizeof(vbuf)) == 0 ||
                        extract_appinfo_json_key(json, jlen, "VERSION", vbuf, sizeof(vbuf)) == 0) {
                        if (vbuf[0] != '\0') {
                            app_info_normalize_version(vbuf, out_version, max_ver);
                        }
                    }
                }
                const char *meta_path = (const char *)sqlite3_column_text(stmt, 2);
                if (meta_path && meta_path[0] != '\0' && out_version && max_ver > 0) {
                    char sfo_p[512], json_p[512], cand[64];
                    snprintf(sfo_p, sizeof(sfo_p), "%s/param.sfo", meta_path);
                    if (read_param_sfo_version(sfo_p, cand, sizeof(cand)) == 0 && cand[0] != '\0') {
                        if (out_version[0] == '\0' || app_info_compare_versions(cand, out_version) > 0) {
                            strncpy(out_version, cand, max_ver - 1);
                            out_version[max_ver - 1] = '\0';
                        }
                    }
                    snprintf(json_p, sizeof(json_p), "%s/param.json", meta_path);
                    if (read_param_json_version(json_p, cand, sizeof(cand)) == 0 && cand[0] != '\0') {
                        if (out_version[0] == '\0' || app_info_compare_versions(cand, out_version) > 0) {
                            strncpy(out_version, cand, max_ver - 1);
                            out_version[max_ver - 1] = '\0';
                        }
                    }
                }
            }
        }
        sqlite3_finalize(stmt);
    } else {
        const char *sql1_alt = "SELECT titleId, version FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql1_alt, -1, &stmt, NULL) == SQLITE_OK) {
            has_contentinfo = 1;
            sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                found = 1;
                const char *v = (const char *)sqlite3_column_text(stmt, 1);
                if (v && v[0] != '\0' && out_version && max_ver > 0 && out_version[0] == '\0') {
                    app_info_normalize_version(v, out_version, max_ver);
                }
            }
            sqlite3_finalize(stmt);
        } else {
            const char *sql1b = "SELECT titleId FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
            if (sqlite3_prepare_v2(db, sql1b, -1, &stmt, NULL) == SQLITE_OK) {
                has_contentinfo = 1;
                sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
                if (sqlite3_step(stmt) == SQLITE_ROW) found = 1;
                sqlite3_finalize(stmt);
            }
        }
    }

    /* 2. Try tbl_appinfo only on systems where tbl_contentinfo does not exist (PS4 legacy) */
    if (!has_contentinfo && (!found || (out_version && out_version[0] == '\0'))) {
        /* 2a. Key-value table variant */
        const char *sql_kv = "SELECT val FROM tbl_appinfo WHERE titleId = ? AND (key = 'APP_VER' OR key = 'version' OR key = 'contentVersion') LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql_kv, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                found = 1;
                const char *v = (const char *)sqlite3_column_text(stmt, 0);
                if (v && v[0] != '\0' && out_version && max_ver > 0 && out_version[0] == '\0') {
                    app_info_normalize_version(v, out_version, max_ver);
                }
            }
            sqlite3_finalize(stmt);
        }

        /* 2b. Wide column table variant */
        const char *sql2 = "SELECT * FROM tbl_appinfo WHERE titleId = ? LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql2, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) {
                found = 1;
                int cols = sqlite3_column_count(stmt);
                const char *best_val = NULL;
                for (int c = 0; c < cols; c++) {
                    const char *cname = sqlite3_column_name(stmt, c);
                    if (!cname) continue;
                    if (strcasecmp(cname, "appVer") == 0 || strcasecmp(cname, "app_ver") == 0 ||
                        strcasecmp(cname, "version") == 0 || strcasecmp(cname, "contentVersion") == 0) {
                        const char *txt = (const char *)sqlite3_column_text(stmt, c);
                        if (txt && txt[0] != '\0') {
                            best_val = txt;
                            break;
                        }
                    }
                }
                if (best_val && out_version && max_ver > 0 && out_version[0] == '\0') {
                    app_info_normalize_version(best_val, out_version, max_ver);
                }
            }
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(db);
    pthread_mutex_unlock(&g_appinfo_db_mutex);
    return found;
}

/* Query app.db for DLC Content ID */
static int check_app_db_for_dlc(const char *db_path, const char *title_id, const char *content_id) {
    (void)title_id;
    if (!db_path || !content_id || content_id[0] == '\0') return 0;

    pthread_mutex_lock(&g_appinfo_db_mutex);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        pthread_mutex_unlock(&g_appinfo_db_mutex);
        return 0;
    }
    sqlite3_busy_timeout(db, 500);

    int found = 0;
    sqlite3_stmt *stmt = NULL;

    const char *sql1 = "SELECT contentId FROM tbl_contentinfo WHERE contentId = ? LIMIT 1;";
    if (sqlite3_prepare_v2(db, sql1, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, content_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) found = 1;
        sqlite3_finalize(stmt);
    }

    if (!found) {
        const char *sql2 = "SELECT contentId FROM tbl_addcontinfo WHERE contentId = ? LIMIT 1;";
        if (sqlite3_prepare_v2(db, sql2, -1, &stmt, NULL) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, content_id, -1, SQLITE_STATIC);
            if (sqlite3_step(stmt) == SQLITE_ROW) found = 1;
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(db);
    pthread_mutex_unlock(&g_appinfo_db_mutex);
    return found;
}

int app_info_check_installed(const char *title_id, char *out_version, size_t max_ver_len) {
    if (!title_id || title_id[0] == '\0') return 0;
    if (out_version && max_ver_len > 0) out_version[0] = '\0';
    /* Validate to avoid path traversal (title_id comes from PKG headers). */
    {
        size_t tlen = strlen(title_id);
        if (tlen < 9 || tlen > 16) return 0;
        for (size_t i = 0; i < tlen; i++) {
            char c = title_id[i];
            if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') || c == '_' || c == '-')) return 0;
        }
        if (strchr(title_id, '/') || strstr(title_id, "..")) return 0;
    }

    /* If the app is registered as partially installed / aborted, it is NOT fully installed */
    if (app_info_check_partially_installed(title_id, NULL, 0)) {
        return 0;
    }

    int found = 0;

    /* 1. Check app.db for registered base title */
    const char *env_db = getenv("PKG_APP_DB_PATH");
    const char *db_path = env_db ? env_db : "/system_data/priv/mms/app.db";
    if (check_app_db_for_title(db_path, title_id, out_version, max_ver_len)) {
        found = 1;
    }

    /* 3. Base app filesystem check (actual base package executable or metadata only!)
     * NOTE: Do NOT check appmeta or user/patch here! On PS4/PS5, appmeta survives application
     * uninstallation for UI/trophy metadata cache, and patch metadata alone does not equal
     * an installed base package. */
    const char *env_user_app = getenv("PKG_USER_APP_DIR");
    const char *user_app_base = env_user_app ? env_user_app : "/user/app";

    char base_paths[10][512];
    int num_base = 0;
    _Static_assert(sizeof(base_paths) / sizeof(base_paths[0]) == 10, "base_paths size");

#define ADD_BASE_PATH(fmt, ...) do { \
        if (num_base < (int)(sizeof(base_paths) / sizeof(base_paths[0]))) \
            snprintf(base_paths[num_base++], sizeof(base_paths[0]), fmt, ##__VA_ARGS__); \
    } while (0)

    ADD_BASE_PATH("%s/%s/sce_sys/param.sfo", user_app_base, title_id);
    ADD_BASE_PATH("%s/%s/sce_sys/param.json", user_app_base, title_id);
    ADD_BASE_PATH("%s/%s/eboot.bin", user_app_base, title_id);
    ADD_BASE_PATH("/system_ex/app/%s/sce_sys/param.json", title_id);
    ADD_BASE_PATH("/system_ex/app/%s/sce_sys/param.sfo", title_id);
    ADD_BASE_PATH("/system_ex/app/%s/eboot.bin", title_id);
    ADD_BASE_PATH("/mnt/ext0/user/app/%s/sce_sys/param.sfo", title_id);
    ADD_BASE_PATH("/mnt/ext0/user/app/%s/sce_sys/param.json", title_id);
    ADD_BASE_PATH("/mnt/ext1/user/app/%s/sce_sys/param.sfo", title_id);
    ADD_BASE_PATH("/mnt/ext1/user/app/%s/sce_sys/param.json", title_id);
#undef ADD_BASE_PATH

    for (int i = 0; i < num_base; i++) {
        const char *p = base_paths[i];
        if (strstr(p, "eboot.bin") != NULL) {
            if (path_exists(p)) {
                found = 1;
            }
        } else {
            char cand_ver[64] = {0};
            int parsed = -1;
            if (strstr(p, ".sfo") != NULL) {
                parsed = read_param_sfo_version(p, cand_ver, sizeof(cand_ver));
            } else if (strstr(p, ".json") != NULL) {
                parsed = read_param_json_version(p, cand_ver, sizeof(cand_ver));
            }

            if (parsed == 0) {
                found = 1;
                if (cand_ver[0] != '\0' && out_version && max_ver_len > 0) {
                    if (out_version[0] == '\0') {
                        strncpy(out_version, cand_ver, max_ver_len - 1);
                        out_version[max_ver_len - 1] = '\0';
                    } else if (app_info_compare_versions(cand_ver, out_version) > 0) {
                        strncpy(out_version, cand_ver, max_ver_len - 1);
                        out_version[max_ver_len - 1] = '\0';
                    }
                }
            }
        }
    }

    /* If the base application is NOT installed, do not proceed to check patches! */
    if (!found) {
        return 0;
    }

    /* 4. Base package is confirmed installed! Now scan patch and appmeta directories
     * to discover if a newer update/patch version is installed. */
    const char *env_appmeta = getenv("PKG_APPMETA_DIR");
    const char *appmeta_base = env_appmeta ? env_appmeta : "/system_data/priv/appmeta";

    const char *env_user_appmeta = getenv("PKG_USER_APPMETA_DIR");
    const char *user_appmeta_base = env_user_appmeta ? env_user_appmeta : "/user/appmeta";

    const char *env_user_patch = getenv("PKG_USER_PATCH_DIR");
    const char *user_patch_base = env_user_patch ? env_user_patch : "/user/patch";

    char patch_paths[16][512];
    int num_patch = 0;
    _Static_assert(sizeof(patch_paths) / sizeof(patch_paths[0]) == 16, "patch_paths size");

#define ADD_PATCH_PATH(fmt, ...) do { \
        if (num_patch < (int)(sizeof(patch_paths) / sizeof(patch_paths[0]))) \
            snprintf(patch_paths[num_patch++], sizeof(patch_paths[0]), fmt, ##__VA_ARGS__); \
    } while (0)

    /* User patch directories */
    ADD_PATCH_PATH("%s/%s/sce_sys/param.sfo", user_patch_base, title_id);
    ADD_PATCH_PATH("%s/%s/sce_sys/param.json", user_patch_base, title_id);
    ADD_PATCH_PATH("%s/%s/param.sfo", user_patch_base, title_id);
    ADD_PATCH_PATH("%s/%s/param.json", user_patch_base, title_id);
    ADD_PATCH_PATH("/user/patch0/%s/sce_sys/param.sfo", title_id);
    ADD_PATCH_PATH("/user/patch0/%s/sce_sys/param.json", title_id);

    /* System appmeta (PS4 sfo & PS5 json, where updates live on PS5) */
    ADD_PATCH_PATH("%s/%s/param.sfo", appmeta_base, title_id);
    ADD_PATCH_PATH("%s/%s/param.json", appmeta_base, title_id);
    ADD_PATCH_PATH("%s/%s/patch/param.sfo", appmeta_base, title_id);
    ADD_PATCH_PATH("%s/%s/patch/param.json", appmeta_base, title_id);
    ADD_PATCH_PATH("%s/external/%s/param.sfo", appmeta_base, title_id);
    ADD_PATCH_PATH("%s/external/%s/param.json", appmeta_base, title_id);

    /* User appmeta */
    ADD_PATCH_PATH("%s/%s/param.sfo", user_appmeta_base, title_id);
    ADD_PATCH_PATH("%s/%s/param.json", user_appmeta_base, title_id);
    ADD_PATCH_PATH("%s/%s/patch/param.sfo", user_appmeta_base, title_id);
    ADD_PATCH_PATH("%s/%s/patch/param.json", user_appmeta_base, title_id);
#undef ADD_PATCH_PATH

    for (int i = 0; i < num_patch; i++) {
        const char *p = patch_paths[i];
        char cand_ver[64] = {0};
        int parsed = -1;
        if (strstr(p, ".sfo") != NULL) {
            parsed = read_param_sfo_version(p, cand_ver, sizeof(cand_ver));
        } else if (strstr(p, ".json") != NULL) {
            parsed = read_param_json_version(p, cand_ver, sizeof(cand_ver));
        }

        if (parsed == 0 && cand_ver[0] != '\0' && out_version && max_ver_len > 0) {
            if (out_version[0] == '\0') {
                strncpy(out_version, cand_ver, max_ver_len - 1);
                out_version[max_ver_len - 1] = '\0';
            } else if (app_info_compare_versions(cand_ver, out_version) > 0) {
                strncpy(out_version, cand_ver, max_ver_len - 1);
                out_version[max_ver_len - 1] = '\0';
            }
        }
    }

    if (out_version && out_version[0] == '\0') {
        /* Default version if installed but version string unparsed */
        strncpy(out_version, "v1.00", max_ver_len - 1);
        out_version[max_ver_len - 1] = '\0';
    }

    return 1;
}

int app_info_check_dlc_installed(const char *title_id, const char *content_id) {
    if (!title_id || title_id[0] == '\0' || !content_id || content_id[0] == '\0') {
        return 0;
    }

    /* 1. Official PS5 Native API check */
#if defined(__Prospero__) || defined(PS5_BUILD)
    ensure_appinstutil_init();
    int dlc_status = 0;
    if (sceAppInstUtilGetAddcontInstalledStatus(content_id, &dlc_status) == 0 && dlc_status == 1) {
        return 1;
    }
#endif

    /* 2. Check app.db and addcont.db */
    const char *env_db = getenv("PKG_APP_DB_PATH");
    const char *db_path = env_db ? env_db : "/system_data/priv/mms/app.db";
    if (check_app_db_for_dlc(db_path, title_id, content_id)) {
        return 1;
    }
    if (!env_db && check_app_db_for_dlc("/system_data/priv/mms/addcont.db", title_id, content_id)) {
        return 1;
    }

    /* 3. Check filesystem locations (actual DLC payloads only, not leftover metadata) */
    const char *env_addcont = getenv("PKG_ADDCONT_DIR");
    const char *addcont_base = env_addcont ? env_addcont : "/user/addcont";

    char path[512];

    /* 3a. Direct match by content_id directory: /user/addcont/<title_id>/<content_id> */
    snprintf(path, sizeof(path), "%s/%s/%s", addcont_base, title_id, content_id);
    if (path_exists(path)) return 1;

    /* 3b. Check /user/app/<title_id>/addcont/<content_id> */
    const char *env_user_app = getenv("PKG_USER_APP_DIR");
    const char *user_app_base = env_user_app ? env_user_app : "/user/app";
    snprintf(path, sizeof(path), "%s/%s/addcont/%s", user_app_base, title_id, content_id);
    if (path_exists(path)) return 1;

    /* 3c. Scan /user/addcont/<title_id> subdirectories for matching param.sfo / param.json / label */
    snprintf(path, sizeof(path), "%s/%s", addcont_base, title_id);
    DIR *d = opendir(path);
    if (d) {
        const char *dash = strrchr(content_id, '-');
        const char *label = (dash && strlen(dash + 1) >= 4) ? (dash + 1) : NULL;

        struct dirent *entry;
        while ((entry = readdir(d)) != NULL) {
            if (entry->d_name[0] == '.') continue;
            if (strcmp(entry->d_name, content_id) == 0) {
                closedir(d);
                return 1;
            }
            /* Match exact DLC label suffix (e.g. MUSICPACK0000008) */
            if (label && strcmp(entry->d_name, label) == 0) {
                closedir(d);
                return 1;
            }

            char subfile[512];
            /* Check param.sfo in subdir (PS4) */
            snprintf(subfile, sizeof(subfile), "%s/%s/%s/sce_sys/param.sfo", addcont_base, title_id, entry->d_name);
            if (sfo_has_content_id(subfile, content_id)) {
                closedir(d);
                return 1;
            }
            snprintf(subfile, sizeof(subfile), "%s/%s/%s/param.sfo", addcont_base, title_id, entry->d_name);
            if (sfo_has_content_id(subfile, content_id)) {
                closedir(d);
                return 1;
            }
            /* Check param.json in subdir (PS5) */
            snprintf(subfile, sizeof(subfile), "%s/%s/%s/sce_sys/param.json", addcont_base, title_id, entry->d_name);
            if (json_has_content_id(subfile, content_id)) {
                closedir(d);
                return 1;
            }
            snprintf(subfile, sizeof(subfile), "%s/%s/%s/param.json", addcont_base, title_id, entry->d_name);
            if (json_has_content_id(subfile, content_id)) {
                closedir(d);
                return 1;
            }
        }
        closedir(d);
    }

    return 0;
}

int app_info_check_has_leftover(const char *title_id, char *out_info, size_t max_info_len) {
    if (!title_id || title_id[0] == '\0') return 0;
    if (out_info && max_info_len > 0) out_info[0] = '\0';

    /* If the base package is already installed, it's not an orphaned leftover */
    if (app_info_check_installed(title_id, NULL, 0)) {
        return 0;
    }

    const char *env_appmeta = getenv("PKG_APPMETA_DIR");
    const char *appmeta_base = env_appmeta ? env_appmeta : "/system_data/priv/appmeta";

    const char *env_user_patch = getenv("PKG_USER_PATCH_DIR");
    const char *user_patch_base = env_user_patch ? env_user_patch : "/user/patch";

    const char *env_addcont = getenv("PKG_ADDCONT_DIR");
    const char *addcont_base = env_addcont ? env_addcont : "/user/addcont";

    char path[512];
    char ver[32] = {0};

    /* 1. Check /user/patch/<title_id> */
    snprintf(path, sizeof(path), "%s/%s", user_patch_base, title_id);
    if (path_exists(path)) {
        char sfo[512];
        snprintf(sfo, sizeof(sfo), "%s/%s/sce_sys/param.sfo", user_patch_base, title_id);
        if (read_param_sfo_version(sfo, ver, sizeof(ver)) != 0) {
            snprintf(sfo, sizeof(sfo), "%s/%s/param.sfo", user_patch_base, title_id);
            read_param_sfo_version(sfo, ver, sizeof(ver));
        }
        if (out_info && max_info_len > 0) {
            if (ver[0] != '\0') {
                snprintf(out_info, max_info_len, "Leftover update %s on console (missing base)", ver);
            } else {
                snprintf(out_info, max_info_len, "Leftover update on console (missing base)");
            }
        }
        return 1;
    }

    /* 2. Check /user/patch0/<title_id> */
    snprintf(path, sizeof(path), "/user/patch0/%s", title_id);
    if (path_exists(path)) {
        if (out_info && max_info_len > 0) {
            snprintf(out_info, max_info_len, "Leftover update in /user/patch0 (missing base)");
        }
        return 1;
    }

    /* 3. Check /system_data/priv/appmeta/<title_id>/patch/param.sfo or param.json (nested patch metadata) */
    snprintf(path, sizeof(path), "%s/%s/patch/param.sfo", appmeta_base, title_id);
    if (path_exists(path)) {
        read_param_sfo_version(path, ver, sizeof(ver));
        if (out_info && max_info_len > 0) {
            if (ver[0] != '\0') {
                snprintf(out_info, max_info_len, "Leftover update metadata %s (missing base)", ver);
            } else {
                snprintf(out_info, max_info_len, "Leftover update metadata (missing base)");
            }
        }
        return 1;
    }
    snprintf(path, sizeof(path), "%s/%s/patch/param.json", appmeta_base, title_id);
    if (path_exists(path)) {
        read_param_json_version(path, ver, sizeof(ver));
        if (out_info && max_info_len > 0) {
            if (ver[0] != '\0') {
                snprintf(out_info, max_info_len, "Leftover update metadata %s (missing base)", ver);
            } else {
                snprintf(out_info, max_info_len, "Leftover update metadata (missing base)");
            }
        }
        return 1;
    }

    /* 4. Check /user/addcont/<title_id> */
    snprintf(path, sizeof(path), "%s/%s", addcont_base, title_id);
    if (path_exists(path)) {
        if (out_info && max_info_len > 0) {
            snprintf(out_info, max_info_len, "Leftover DLC on console (missing base)");
        }
        return 1;
    }

    return 0;
}

int app_info_check_partially_installed(const char *title_id, char *out_desc, size_t max_desc_len) {
    if (!title_id || title_id[0] == '\0') return 0;
    if (out_desc && max_desc_len > 0) out_desc[0] = '\0';

    const char *env_db = getenv("PKG_APP_DB_PATH");
    const char *db_path = env_db ? env_db : "/system_data/priv/mms/app.db";

    pthread_mutex_lock(&g_appinfo_db_mutex);
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_FULLMUTEX, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        pthread_mutex_unlock(&g_appinfo_db_mutex);
        return 0;
    }
    sqlite3_busy_timeout(db, 100);

    sqlite3_stmt *stmt = NULL;
    int is_partial = 0;

    const char *sql = "SELECT titleId, AppInfoJson, metaDataPath, installStatus, contentStatus, size FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        sql = "SELECT titleId, AppInfoJson, metaDataPath, installStatus FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    }
    if (rc != SQLITE_OK) {
        sql = "SELECT titleId, AppInfoJson, metaDataPath FROM tbl_contentinfo WHERE titleId = ? LIMIT 1;";
        rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    }

    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, title_id, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int content_status = 0;
            if (sqlite3_column_count(stmt) >= 5 && sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
                content_status = sqlite3_column_int(stmt, 4);
            }

            int64_t size_val = -1;
            if (sqlite3_column_count(stmt) >= 6 && sqlite3_column_type(stmt, 5) != SQLITE_NULL) {
                size_val = sqlite3_column_int64(stmt, 5);
            }

            const char *json = (const char *)sqlite3_column_text(stmt, 1);
            int install_sub_status = 0;
            if (json) {
                char sub_buf[32] = {0};
                if (extract_appinfo_json_key(json, strlen(json), "_install_sub_status", sub_buf, sizeof(sub_buf)) == 0) {
                    install_sub_status = atoi(sub_buf);
                }
            }

            if (content_status != 0 || size_val == 0 || install_sub_status == 2) {
                is_partial = 1;
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    pthread_mutex_unlock(&g_appinfo_db_mutex);

    if (is_partial) {
        if (out_desc && max_desc_len > 0) {
            snprintf(out_desc, max_desc_len, "Incomplete or aborted installation on console");
        }
        return 1;
    }

    return 0;
}


