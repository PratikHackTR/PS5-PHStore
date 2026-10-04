#ifndef DEBUG_LOG_RETENTION_H
#define DEBUG_LOG_RETENTION_H

/* Keep the newest `limit` regular files matching prefix + *.txt in directory.
 * current_path is always retained when it names a matching file. */
void debug_log_retain_latest(const char *directory, const char *prefix,
                            const char *current_path, unsigned limit);

#endif
