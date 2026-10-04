#ifndef PHSTORE_GDRIVE_PROTOCOL_H
#define PHSTORE_GDRIVE_PROTOCOL_H
#include <stddef.h>
#include <stdint.h>
typedef struct { long status; uint64_t start,end,total,length; int cr,cl,encoding,bad,allow_archive; } ph_gdrive_headers;
int phstore_gdrive_id_valid(const char *);
int phstore_gdrive_sha_valid(const char *);
int phstore_gdrive_url_allowed(const char *);
void ph_gdrive_header_line(ph_gdrive_headers *, const char *, size_t);
int ph_gdrive_headers_exact(const ph_gdrive_headers *,uint64_t,uint64_t,uint64_t);
int phstore_archive_url_allowed(const char *);
#endif
