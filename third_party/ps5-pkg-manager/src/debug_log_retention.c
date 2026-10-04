#include "debug_log_retention.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    char *name;
    int is_current;
} log_file_t;

static int log_file_cmp(const void *a, const void *b) {
    const log_file_t *fa = (const log_file_t *)a;
    const log_file_t *fb = (const log_file_t *)b;
    if (fa->is_current != fb->is_current) return fa->is_current - fb->is_current;
    /* Both log types end in the sortable local timestamp _YYYYMMDD_HHMMSS.txt. */
    size_t la = strlen(fa->name), lb = strlen(fb->name);
    if (la >= 20 && lb >= 20) {
        int c = strncmp(fa->name + la - 20, fb->name + lb - 20, 20);
        if (c) return c;
    }
    return strcmp(fa->name, fb->name);
}

void debug_log_retain_latest(const char *directory, const char *prefix,
                            const char *current_path, unsigned limit) {
    if (!directory || !prefix || !current_path || limit == 0) return;
    DIR *dp = opendir(directory);
    if (!dp) return;
    const char *current_name = strrchr(current_path, '/');
    current_name = current_name ? current_name + 1 : current_path;
    const size_t prefix_len = strlen(prefix);
    log_file_t *files = NULL;
    size_t count = 0, capacity = 0;
    struct dirent *entry;
    int scan_failed = 0;
    while ((entry = readdir(dp)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len <= prefix_len + 4 || strncmp(entry->d_name, prefix, prefix_len) != 0 ||
            strcmp(entry->d_name + len - 4, ".txt") != 0) continue;
        char path[1024];
        int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) continue;
        struct stat st;
        if (lstat(path, &st) != 0 || !S_ISREG(st.st_mode)) continue;
        if (count == capacity) {
            size_t next = capacity ? capacity * 2 : 32;
            log_file_t *grown = (log_file_t *)realloc(files, next * sizeof(*files));
            if (!grown) { scan_failed = 1; break; }
            files = grown;
            capacity = next;
        }
        files[count].name = strdup(entry->d_name);
        if (!files[count].name) { scan_failed = 1; break; }
        files[count].is_current = strcmp(entry->d_name, current_name) == 0;
        count++;
    }
    closedir(dp);
    if (!scan_failed && count > limit) {
        qsort(files, count, sizeof(*files), log_file_cmp);
        size_t remove_count = count - limit;
        for (size_t i = 0; i < remove_count; i++) {
            char path[1024];
            int n = snprintf(path, sizeof(path), "%s/%s", directory, files[i].name);
            if (n >= 0 && (size_t)n < sizeof(path)) unlink(path);
        }
    }
    for (size_t i = 0; i < count; i++) free(files[i].name);
    free(files);
}
