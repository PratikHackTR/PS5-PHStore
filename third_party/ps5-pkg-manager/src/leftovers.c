/*
 * PKG Manager - Orphaned Leftover Scanner & Cleanup
 *
 * Detects and safely purges orphaned patches and DLC directories
 * left behind by uninstalled base packages.
 */

#include "leftovers.h"
#include "app_info.h"
#include "pkg_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdarg.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>

/* Endian helper */
static inline uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Helper to check if file or directory exists */
static int path_exists(const char *path) {
    if (!path || path[0] == '\0') return 0;
    struct stat st;
    return (stat(path, &st) == 0) ? 1 : 0;
}

/* Append formatted JSON, growing the buffer on truncation.
 * Returns 0 on success, -1 on OOM/format error (json/pos/buf_size updated). */
static int json_append_grow(char **p_json, size_t *p_pos, size_t *p_buf_size,
                            const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static int json_append_grow(char **p_json, size_t *p_pos, size_t *p_buf_size,
                            const char *fmt, ...) {
    if (!p_json || !*p_json || !p_pos || !p_buf_size || !fmt) return -1;
    for (int tries = 0; tries < 4; tries++) {
        char *json = *p_json;
        size_t pos = *p_pos;
        size_t buf_size = *p_buf_size;
        if (pos >= buf_size) return -1;
        va_list ap;
        va_start(ap, fmt);
        /* vsnprintf needs stdarg.h; include via stdio already. */
        int w = vsnprintf(json + pos, buf_size - pos, fmt, ap);
        va_end(ap);
        if (w < 0) return -1;
        if ((size_t)w < buf_size - pos) {
            *p_pos = pos + (size_t)w;
            return 0;
        }
        /* Truncated: grow to fit w + slack and retry. */
        size_t need = pos + (size_t)w + 2048;
        size_t nsize = buf_size * 2;
        if (nsize < need) nsize = need;
        char *njson = (char *)realloc(json, nsize);
        if (!njson) return -1;
        *p_json = njson;
        *p_buf_size = nsize;
    }
    return -1;
}

/* Helper to escape JSON string */
static void escape_json_str(const char *src, char *dst, size_t max_dst) {
    if (!dst || max_dst == 0) return;
    if (!src) { dst[0] = '\0'; return; }
    size_t d = 0;
    for (size_t s = 0; src[s] != '\0' && d + 2 < max_dst; s++) {
        unsigned char c = (unsigned char)src[s];
        if (c == '"') {
            if (d + 2 >= max_dst) break;
            dst[d++] = '\\'; dst[d++] = '"';
        } else if (c == '\\') {
            if (d + 2 >= max_dst) break;
            dst[d++] = '\\'; dst[d++] = '\\';
        } else if (c == '\n') {
            if (d + 2 >= max_dst) break;
            dst[d++] = '\\'; dst[d++] = 'n';
        } else if (c == '\r') {
            if (d + 2 >= max_dst) break;
            dst[d++] = '\\'; dst[d++] = 'r';
        } else if (c == '\t') {
            if (d + 2 >= max_dst) break;
            dst[d++] = '\\'; dst[d++] = 't';
        } else if (c >= 32 && c <= 126) {
            dst[d++] = (char)c;
        } else {
            dst[d++] = ' ';
        }
    }
    dst[d] = '\0';
}

/* Read TITLE from param.sfo */
static int read_param_sfo_title(const char *filepath, char *out_title, size_t max_title) {
    if (!out_title || max_title == 0) return -1;
    out_title[0] = '\0';

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

        if (strcmp(key, "TITLE") == 0) {
            size_t copy_len = data_len < max_title ? data_len : max_title - 1;
            while (copy_len > 0 && (data[copy_len - 1] == '\0' || data[copy_len - 1] == '\n' || data[copy_len - 1] == '\r')) {
                copy_len--;
            }
            memcpy(out_title, data, copy_len);
            out_title[copy_len] = '\0';
            return 0;
        }
    }
    return -1;
}

/* Read title from param.json */
static int read_param_json_title(const char *filepath, char *out_title, size_t max_title) {
    if (!out_title || max_title == 0) return -1;
    out_title[0] = '\0';

    int fd = open(filepath, O_RDONLY);
    if (fd < 0) return -1;

    char buf[16384];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';

    char tid[PKG_TITLE_ID_LEN] = {0};
    char cat[16] = {0};
    char ver[32] = {0};
    char loc[PKG_LOCALIZED_TITLES_LEN] = {0};
    char def_lang[PKG_DEFAULT_LANG_LEN] = {0};
    pkg_parser_parse_param_json(buf, (size_t)n, tid, sizeof(tid), out_title, max_title,
                                cat, sizeof(cat), ver, sizeof(ver),
                                loc, sizeof(loc), def_lang, sizeof(def_lang));
    return (out_title[0] != '\0') ? 0 : -1;
}

/* Read version from param.sfo */
static int read_param_sfo_version_simple(const char *filepath, char *out_ver, size_t max_ver) {
    if (!out_ver || max_ver == 0) return -1;
    out_ver[0] = '\0';

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

        if (strcmp(key, "APP_VER") == 0 || (strcmp(key, "VERSION") == 0 && out_ver[0] == '\0')) {
            size_t copy_len = data_len < max_ver ? data_len : max_ver - 1;
            while (copy_len > 0 && (data[copy_len - 1] == '\0' || data[copy_len - 1] == '\n' || data[copy_len - 1] == '\r')) {
                copy_len--;
            }
            memcpy(out_ver, data, copy_len);
            out_ver[copy_len] = '\0';
        }
    }
    return (out_ver[0] != '\0') ? 0 : -1;
}

/* Fast non-blocking calculation of directory size with tight item ceiling */
static uint64_t calc_dir_size(const char *dir_path) {
    if (!dir_path || dir_path[0] == '\0') return 0;

    struct stat st;
    if (lstat(dir_path, &st) != 0) return 0;
    if (!S_ISDIR(st.st_mode)) return (uint64_t)st.st_size;

    uint64_t total = 0;
    int items_counted = 0;
    const int MAX_ITEMS = 300; /* Strict bound so scan runs in < 2ms without freezing SSD */

    char queue[64][512];
    int q_head = 0, q_tail = 0;

    strncpy(queue[q_tail++], dir_path, sizeof(queue[0]) - 1);
    queue[0][sizeof(queue[0]) - 1] = '\0';

    while (q_head < q_tail && items_counted < MAX_ITEMS) {
        char curr_dir[512];
        strncpy(curr_dir, queue[q_head++], sizeof(curr_dir) - 1);
        curr_dir[sizeof(curr_dir) - 1] = '\0';

        DIR *d = opendir(curr_dir);
        if (!d) continue;

        struct dirent *de;
        while ((de = readdir(d)) != NULL && items_counted < MAX_ITEMS) {
            if (de->d_name[0] == '.') continue;
            items_counted++;

            char sub[1024];
            snprintf(sub, sizeof(sub), "%s/%s", curr_dir, de->d_name);
            struct stat sub_st;
            if (lstat(sub, &sub_st) == 0) {
                if (S_ISLNK(sub_st.st_mode)) {
                    continue; /* Never follow symlinks */
                } else if (S_ISDIR(sub_st.st_mode)) {
                    if (q_tail < 64) {
                        strncpy(queue[q_tail++], sub, sizeof(queue[0]) - 1);
                        queue[q_tail - 1][sizeof(queue[0]) - 1] = '\0';
                    }
                } else if (S_ISREG(sub_st.st_mode)) {
                    total += (uint64_t)sub_st.st_size;
                }
            }
        }
        closedir(d);
    }

    return total;
}

/* Recursively remove files and directories, returning total freed bytes */
static uint64_t rmdir_recursive_depth(const char *path, int depth) {
    if (!path || depth > 20) return 0;
    uint64_t freed = 0;
    DIR *d = opendir(path);
    if (!d) {
        struct stat st;
        if (lstat(path, &st) == 0) {
            freed = (uint64_t)st.st_size;
            unlink(path);
        }
        return freed;
    }

    struct dirent *de;
    char sub[1024];
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        snprintf(sub, sizeof(sub), "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0) {
            if (S_ISLNK(st.st_mode)) {
                unlink(sub);
            } else if (S_ISDIR(st.st_mode)) {
                freed += rmdir_recursive_depth(sub, depth + 1);
            } else {
                freed += (uint64_t)st.st_size;
                unlink(sub);
            }
        }
    }
    closedir(d);
    rmdir(path);
    return freed;
}

static uint64_t rmdir_recursive(const char *path) {
    return rmdir_recursive_depth(path, 0);
}

/* Validate title_id: must be 9-16 alphanumeric characters, CUSA or PPSA */
static int is_valid_title_id(const char *id) {
    if (!id) return 0;
    size_t len = strlen(id);
    if (len < 9 || len > 16) return 0;
    if (strncasecmp(id, "CUSA", 4) != 0 && strncasecmp(id, "PPSA", 4) != 0) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        if (!isalnum((unsigned char)id[i])) return 0;
    }
    return 1;
}

/* Check if title_id is already in a collected list */
static int is_title_in_list(char list[][32], int count, const char *tid) {
    for (int i = 0; i < count; i++) {
        if (strcasecmp(list[i], tid) == 0) return 1;
    }
    return 0;
}


/* Fast check if base package executable/metadata files exist on disk */
static int is_base_package_installed_fs(const char *tid, const char *user_app_base) {
    char check_p[512];
    /* Check internal user/app */
    snprintf(check_p, sizeof(check_p), "%s/%s/sce_sys/param.sfo", user_app_base, tid);
    if (path_exists(check_p)) return 1;
    snprintf(check_p, sizeof(check_p), "%s/%s/sce_sys/param.json", user_app_base, tid);
    if (path_exists(check_p)) return 1;
    snprintf(check_p, sizeof(check_p), "%s/%s/eboot.bin", user_app_base, tid);
    if (path_exists(check_p)) return 1;

    /* Check system_ex */
    snprintf(check_p, sizeof(check_p), "/system_ex/app/%s/sce_sys/param.json", tid);
    if (path_exists(check_p)) return 1;
    snprintf(check_p, sizeof(check_p), "/system_ex/app/%s/sce_sys/param.sfo", tid);
    if (path_exists(check_p)) return 1;

    /* Check external USB / M.2 extended storage */
    snprintf(check_p, sizeof(check_p), "/mnt/ext0/user/app/%s/sce_sys/param.sfo", tid);
    if (path_exists(check_p)) return 1;
    snprintf(check_p, sizeof(check_p), "/mnt/ext0/user/app/%s/sce_sys/param.json", tid);
    if (path_exists(check_p)) return 1;
    snprintf(check_p, sizeof(check_p), "/mnt/ext1/user/app/%s/sce_sys/param.sfo", tid);
    if (path_exists(check_p)) return 1;
    snprintf(check_p, sizeof(check_p), "/mnt/ext1/user/app/%s/sce_sys/param.json", tid);
    if (path_exists(check_p)) return 1;

    return 0;
}

char *leftovers_scan_json(void) {
    const char *env_appmeta = getenv("PKG_APPMETA_DIR");
    const char *appmeta_base = env_appmeta ? env_appmeta : "/system_data/priv/appmeta";

    const char *env_user_appmeta = getenv("PKG_USER_APPMETA_DIR");
    const char *user_appmeta_base = env_user_appmeta ? env_user_appmeta : "/user/appmeta";

    const char *env_user_patch = getenv("PKG_USER_PATCH_DIR");
    const char *user_patch_base = env_user_patch ? env_user_patch : "/user/patch";

    const char *env_addcont = getenv("PKG_ADDCONT_DIR");
    const char *addcont_base = env_addcont ? env_addcont : "/user/addcont";

    const char *env_user_app = getenv("PKG_USER_APP_DIR");
    const char *user_app_base = env_user_app ? env_user_app : "/user/app";

    /* ONLY scan patch and addcont directories for candidates!
     * NEVER scan appmeta as a candidate source, because appmeta
     * contains trophy caches for every title ever played on the console. */
    const char *dirs_to_scan[] = {
        user_patch_base,
        "/user/patch0",
        addcont_base,
        NULL
    };

    typedef char tid_entry_t[32];
    tid_entry_t *candidate_tids = (tid_entry_t *)calloc(256, sizeof(tid_entry_t));
    if (!candidate_tids) return NULL;
    int tid_count = 0;

    for (int d = 0; dirs_to_scan[d] != NULL; d++) {
        DIR *dir = opendir(dirs_to_scan[d]);
        if (!dir) continue;
        struct dirent *de;
        while ((de = readdir(dir)) != NULL) {
            if (de->d_name[0] == '.') continue;
            if (is_valid_title_id(de->d_name) && !is_title_in_list(candidate_tids, tid_count, de->d_name)) {
                if (tid_count < 256) {
                    strncpy(candidate_tids[tid_count++], de->d_name, 31);
                    candidate_tids[tid_count - 1][31] = '\0';
                }
            }
        }
        closedir(dir);
    }

    /* Allocate output JSON buffer */
    size_t buf_size = 65536;
    char *json = (char *)malloc(buf_size);
    if (!json) {
        free(candidate_tids);
        return NULL;
    }
    json[0] = '\0';

    size_t pos = 0;
    if (json_append_grow(&json, &pos, &buf_size, "{\"leftovers\":[") != 0) {
        free(candidate_tids);
        free(json);
        return NULL;
    }

    int emitted_count = 0;

    for (int i = 0; i < tid_count; i++) {
        const char *tid = candidate_tids[i];

        /* Fast FS check first: If base package files exist on disk, it's NOT an orphan! */
        if (is_base_package_installed_fs(tid, user_app_base)) {
            continue;
        }

        /* If not found on disk, check app.db / system APIs */
        if (app_info_check_installed(tid, NULL, 0)) {
            continue;
        }

        /* Collect all existing paths for this orphaned title */
        char paths[8][512];
        int num_paths = 0;

        char pbuf[512];
        snprintf(pbuf, sizeof(pbuf), "%s/%s", user_patch_base, tid);
        if (path_exists(pbuf)) strncpy(paths[num_paths++], pbuf, sizeof(paths[0]) - 1);

        snprintf(pbuf, sizeof(pbuf), "/user/patch0/%s", tid);
        if (path_exists(pbuf)) strncpy(paths[num_paths++], pbuf, sizeof(paths[0]) - 1);

        snprintf(pbuf, sizeof(pbuf), "%s/%s", addcont_base, tid);
        if (path_exists(pbuf)) strncpy(paths[num_paths++], pbuf, sizeof(paths[0]) - 1);

        /* Only include appmeta if this title actually has an orphaned patch/addcont */
        snprintf(pbuf, sizeof(pbuf), "%s/%s", appmeta_base, tid);
        if (path_exists(pbuf)) strncpy(paths[num_paths++], pbuf, sizeof(paths[0]) - 1);

        snprintf(pbuf, sizeof(pbuf), "%s/%s", user_appmeta_base, tid);
        if (path_exists(pbuf)) strncpy(paths[num_paths++], pbuf, sizeof(paths[0]) - 1);

        /* Only include user/app if it's an empty or broken directory without base files */
        snprintf(pbuf, sizeof(pbuf), "%s/%s", user_app_base, tid);
        if (path_exists(pbuf)) {
            char chk_base[1024];
            snprintf(chk_base, sizeof(chk_base), "%s/sce_sys/param.sfo", pbuf);
            int has_base_sfo = path_exists(chk_base);
            snprintf(chk_base, sizeof(chk_base), "%s/sce_sys/param.json", pbuf);
            int has_base_json = path_exists(chk_base);
            snprintf(chk_base, sizeof(chk_base), "%s/eboot.bin", pbuf);
            int has_base_eboot = path_exists(chk_base);
            if (!has_base_sfo && !has_base_json && !has_base_eboot) {
                strncpy(paths[num_paths++], pbuf, sizeof(paths[0]) - 1);
            }
        }

        /* Require at least one patch or addcont directory to be present */
        int has_patch_dir = 0, has_addcont_dir = 0;
        for (int p = 0; p < num_paths; p++) {
            if (strstr(paths[p], "patch") != NULL) has_patch_dir = 1;
            if (strstr(paths[p], "addcont") != NULL) has_addcont_dir = 1;
        }
        if (!has_patch_dir && !has_addcont_dir) {
            continue;
        }

        /* Calculate total size across all paths (bounded fast scan) */
        uint64_t total_size = 0;
        for (int p = 0; p < num_paths; p++) {
            total_size += calc_dir_size(paths[p]);
        }

        /* Read title name */
        char title_name[256] = {0};
        char sfo_test[512];
        snprintf(sfo_test, sizeof(sfo_test), "%s/%s/param.sfo", appmeta_base, tid);
        if (read_param_sfo_title(sfo_test, title_name, sizeof(title_name)) != 0) {
            snprintf(sfo_test, sizeof(sfo_test), "%s/%s/param.json", appmeta_base, tid);
            if (read_param_json_title(sfo_test, title_name, sizeof(title_name)) != 0) {
                snprintf(sfo_test, sizeof(sfo_test), "%s/%s/sce_sys/param.sfo", user_patch_base, tid);
                if (read_param_sfo_title(sfo_test, title_name, sizeof(title_name)) != 0) {
                    snprintf(sfo_test, sizeof(sfo_test), "%s/%s/param.sfo", user_appmeta_base, tid);
                    read_param_sfo_title(sfo_test, title_name, sizeof(title_name));
                }
            }
        }
        if (title_name[0] == '\0') {
            strncpy(title_name, tid, sizeof(title_name) - 1);
        }

        /* Read version */
        char ver[32] = {0};
        snprintf(sfo_test, sizeof(sfo_test), "%s/%s/param.sfo", appmeta_base, tid);
        if (read_param_sfo_version_simple(sfo_test, ver, sizeof(ver)) != 0) {
            snprintf(sfo_test, sizeof(sfo_test), "%s/%s/sce_sys/param.sfo", user_patch_base, tid);
            read_param_sfo_version_simple(sfo_test, ver, sizeof(ver));
        }

        /* Determine leftover type */
        const char *type = "Orphaned Update";
        if (has_patch_dir && has_addcont_dir) {
            type = "Orphaned Update & DLC";
        } else if (has_patch_dir) {
            type = "Orphaned Update";
        } else if (has_addcont_dir) {
            type = "Orphaned DLC";
        }

        char esc_title[512];
        escape_json_str(title_name, esc_title, sizeof(esc_title));

        int item_err = 0;
        if (json_append_grow(&json, &pos, &buf_size,
            "%s{"
            "\"title_id\":\"%s\","
            "\"title_name\":\"%s\","
            "\"version\":\"%s\","
            "\"type\":\"%s\","
            "\"total_size\":%llu,"
            "\"paths\":[",
            (emitted_count > 0 ? "," : ""),
            tid,
            esc_title,
            ver,
            type,
            (unsigned long long)total_size) != 0) {
            item_err = 1;
        } else {
            for (int p = 0; p < num_paths; p++) {
                char esc_p[512];
                escape_json_str(paths[p], esc_p, sizeof(esc_p));
                if (json_append_grow(&json, &pos, &buf_size, "%s\"%s\"",
                                     (p > 0 ? "," : ""), esc_p) != 0) {
                    item_err = 1;
                    break;
                }
            }

            if (!item_err && json_append_grow(&json, &pos, &buf_size, "]}") != 0) {
                item_err = 1;
            }
        }

        if (item_err) {
            free(candidate_tids);
            free(json);
            return NULL;
        }
        emitted_count++;
    }

    if (json_append_grow(&json, &pos, &buf_size, "],\"count\":%d}", emitted_count) != 0) {
        free(candidate_tids);
        free(json);
        return NULL;
    }

    free(candidate_tids);
    return json;
}

char *leftovers_delete_json(const char *title_id) {
    if (!title_id || !is_valid_title_id(title_id)) {
        char *res = (char *)malloc(128);
        if (res) snprintf(res, 128, "{\"success\":false,\"error\":\"Invalid Title ID\"}");
        return res;
    }

    const char *env_user_app = getenv("PKG_USER_APP_DIR");
    const char *user_app_base = env_user_app ? env_user_app : "/user/app";

    /* Guard: Refuse to delete if base package is installed! */
    if (is_base_package_installed_fs(title_id, user_app_base) || app_info_check_installed(title_id, NULL, 0)) {
        char *res = (char *)malloc(128);
        if (res) snprintf(res, 128, "{\"success\":false,\"error\":\"Base package is currently installed. Cannot delete!\"}");
        return res;
    }

    const char *env_appmeta = getenv("PKG_APPMETA_DIR");
    const char *appmeta_base = env_appmeta ? env_appmeta : "/system_data/priv/appmeta";

    const char *env_user_appmeta = getenv("PKG_USER_APPMETA_DIR");
    const char *user_appmeta_base = env_user_appmeta ? env_user_appmeta : "/user/appmeta";

    const char *env_user_patch = getenv("PKG_USER_PATCH_DIR");
    const char *user_patch_base = env_user_patch ? env_user_patch : "/user/patch";

    const char *env_addcont = getenv("PKG_ADDCONT_DIR");
    const char *addcont_base = env_addcont ? env_addcont : "/user/addcont";

    char paths_to_delete[8][512];
    int count = 0;

    char pbuf[512];

    snprintf(pbuf, sizeof(pbuf), "%s/%s", user_patch_base, title_id);
    if (path_exists(pbuf)) strncpy(paths_to_delete[count++], pbuf, sizeof(paths_to_delete[0]) - 1);

    snprintf(pbuf, sizeof(pbuf), "/user/patch0/%s", title_id);
    if (path_exists(pbuf)) strncpy(paths_to_delete[count++], pbuf, sizeof(paths_to_delete[0]) - 1);

    snprintf(pbuf, sizeof(pbuf), "%s/%s", addcont_base, title_id);
    if (path_exists(pbuf)) strncpy(paths_to_delete[count++], pbuf, sizeof(paths_to_delete[0]) - 1);

    snprintf(pbuf, sizeof(pbuf), "%s/%s", appmeta_base, title_id);
    if (path_exists(pbuf)) strncpy(paths_to_delete[count++], pbuf, sizeof(paths_to_delete[0]) - 1);

    snprintf(pbuf, sizeof(pbuf), "%s/%s", user_appmeta_base, title_id);
    if (path_exists(pbuf)) strncpy(paths_to_delete[count++], pbuf, sizeof(paths_to_delete[0]) - 1);

    /* user/app: verify again that no base package eboot or param exists */
    snprintf(pbuf, sizeof(pbuf), "%s/%s", user_app_base, title_id);
    if (path_exists(pbuf)) {
        char chk[1024];
        snprintf(chk, sizeof(chk), "%s/sce_sys/param.sfo", pbuf);
        int has_sfo = path_exists(chk);
        snprintf(chk, sizeof(chk), "%s/sce_sys/param.json", pbuf);
        int has_json = path_exists(chk);
        snprintf(chk, sizeof(chk), "%s/eboot.bin", pbuf);
        int has_eboot = path_exists(chk);
        if (!has_sfo && !has_json && !has_eboot) {
            strncpy(paths_to_delete[count++], pbuf, sizeof(paths_to_delete[0]) - 1);
        }
    }

    uint64_t freed_bytes = 0;
    for (int p = 0; p < count; p++) {
        freed_bytes += rmdir_recursive(paths_to_delete[p]);
    }

#if defined(__Prospero__) || defined(PS5_BUILD)
    /* Unregister leftover patches/addcont only. Never call AppUnInstall
     * here: TOCTOU between the installed-guard above and this point could
     * delete the user's base package. Base deletion is explicitly refused.
     * NOTE: uses global sceAppInstUtil init from installer_init(); never
     * Terminate here or concurrent installs will fail. */
    extern int sceAppInstUtilAppUnInstallPat(const char *title_id);
    extern int sceAppInstUtilAppUnInstallAddcont(const char *title_id);

    sceAppInstUtilAppUnInstallPat(title_id);
    sceAppInstUtilAppUnInstallAddcont(title_id);
#endif

    char *res = NULL;
    size_t res_size = 4096;
    size_t rpos = 0;
    res = (char *)malloc(res_size);
    if (!res) return NULL;
    if (json_append_grow(&res, &rpos, &res_size,
        "{\"success\":true,\"title_id\":\"%s\",\"freed_bytes\":%llu,\"deleted_paths\":[",
        title_id, (unsigned long long)freed_bytes) != 0) {
        free(res);
        return NULL;
    }

    for (int p = 0; p < count; p++) {
        char esc[512];
        escape_json_str(paths_to_delete[p], esc, sizeof(esc));
        if (json_append_grow(&res, &rpos, &res_size, "%s\"%s\"",
                             (p > 0 ? "," : ""), esc) != 0) {
            free(res);
            return NULL;
        }
    }

    if (json_append_grow(&res, &rpos, &res_size, "]}") != 0) {
        free(res);
        return NULL;
    }

    return res;
}
