#ifndef PHSTORE_GDRIVE_H
#define PHSTORE_GDRIVE_H
#include "phstore_install.h"
typedef void (*phstore_download_progress_fn)(const char *, uint64_t, uint64_t, uint64_t);
int phstore_gdrive_read_prefix(const phstore_package_info_t *, unsigned char *, size_t);
int phstore_gdrive_destination(const phstore_package_info_t *, char *, size_t);
int phstore_gdrive_download(const phstore_package_info_t *, phstore_download_progress_fn);
int phstore_gdrive_id_valid(const char *);
int phstore_gdrive_sha_valid(const char *);
int phstore_public_cover_fetch(const char *,const char *,uint64_t,uint64_t *,int *);
#endif
