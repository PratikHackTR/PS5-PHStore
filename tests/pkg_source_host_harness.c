#define _CRT_SECURE_NO_WARNINGS
#include "../src/phstore_pkg_source.c"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIXTURE_SIZE UINT64_C(1095368704)
static int g_native;
static volatile LONG g_cancel;
static volatile LONG g_http_error;
static volatile LONG g_short_body;

int phstore_catalog_find_install_package(const char *package_id,
        phstore_package_info_t *package, char error[64]) {
    if (!package_id || strcmp(package_id, "avatar") != 0 || !package) {
        if (error) strcpy(error, "package_not_found");
        return 0;
    }
    memset(package, 0, sizeof(*package));
    strcpy(package->package_id, "avatar");
    strcpy(package->filename, "Avatar2.pkg");
    strcpy(package->download_url, "http://fixture.invalid/Avatar2.pkg");
    package->size_bytes = FIXTURE_SIZE;
    package->installable = 1;
    if(g_native){strcpy(package->source_type,"google_drive_public");strcpy(package->action_type,"install_package");package->size_bytes=32768;}

    if (error) error[0] = '\0';
    return 1;
}

int phstore_gdrive_destination(const phstore_package_info_t *p,char *out,size_t capacity) {
    (void)p;snprintf(out,capacity,"native-staged-fixture.pkg");return 0;
}
int phstore_catalog_resolve_package_source(phstore_package_info_t *package, char error[64]) {
    if (!package || strcmp(package->package_id, "avatar") != 0) {
        if (error) strcpy(error, "package_source_missing");
        return -1;
    }
    return 0;
}

void phstore_raw_http_source_begin(const phstore_package_info_t *package) { (void)package; }
void phstore_install_detailed_log(const char *event, const char *detail) { (void)event; (void)detail; }
int phstore_install_cancel_requested(void) { return InterlockedCompareExchange(&g_cancel, 0, 0) != 0; }
int phstore_raw_http_open_range(const phstore_package_info_t *package, uint64_t start,
        uint64_t end, phstore_raw_http_response_t *response, char error[64]) {
    if (InterlockedCompareExchange(&g_http_error, 0, 0)) {
        strcpy(error, "fixture_http_error");
        return -1;
    }
    if (!package || !response || start > end || end >= package->size_bytes) {
        strcpy(error, "upstream_range_invalid");
        return -1;
    }
    memset(response, 0, sizeof(*response));
    response->fd = -1;
    response->remaining = end - start + 1;
    if (InterlockedCompareExchange(&g_short_body, 0, 0) && response->remaining) response->remaining--;
    _snprintf(response->endpoint_host, sizeof(response->endpoint_host), "%llu", (unsigned long long)start);
    return 0;
}
int phstore_raw_http_read(phstore_raw_http_response_t *response, void *buffer, size_t capacity) {
    if (phstore_install_cancel_requested()) return -1;
    if (!response || !buffer || !response->remaining) return 0;
    uint64_t start = _strtoui64(response->endpoint_host, NULL, 10) + response->bytes_received;
    size_t count = response->remaining < capacity ? (size_t)response->remaining : capacity;
    unsigned char *bytes = (unsigned char *)buffer;
    for (size_t i = 0; i < count; i++) bytes[i] = (unsigned char)((start + i) % 251);
    response->remaining -= count;
    response->bytes_received += count;
    return (int)count;
}
void phstore_raw_http_close(phstore_raw_http_response_t *response) { if (response) response->fd = -1; }

static int check_bytes(const unsigned char *bytes, uint64_t offset, size_t length) {
    for (size_t i = 0; i < length; i++) if (bytes[i] != (unsigned char)((offset + i) % 251)) return 0;
    return 1;
}

typedef struct { uint64_t offset; int ok; } parallel_read_t;
static DWORD WINAPI parallel_read(void *opaque) {
    parallel_read_t *test = opaque;
    unsigned char bytes[8192];
    void *handle = NULL; uint64_t size = 0; char filename[32];
    if (phstore_pkg_source_open_uri("phstore://avatar", &handle, &size, filename, sizeof(filename)) != 0) return 1;
    int64_t result = phstore_pkg_source_read_at(handle, test->offset, bytes, sizeof(bytes));
    test->ok = result == (int64_t)sizeof(bytes) && check_bytes(bytes, test->offset, sizeof(bytes));
    phstore_pkg_source_close(handle);
    return test->ok ? 0 : 1;
}

int main(void) {
    int ok = 1;
    void *handle = NULL; uint64_t size = 0; char filename[64];
    ok &= phstore_pkg_source_open_uri("phstore://avatar", &handle, &size, filename, sizeof(filename)) == 0;
    ok &= size == FIXTURE_SIZE && strcmp(filename, "Avatar2.pkg") == 0;
    unsigned char first[65536];
    ok &= phstore_pkg_source_read_at(handle, 0, first, sizeof(first)) == (int64_t)sizeof(first);
    ok &= check_bytes(first, 0, sizeof(first));
    unsigned char offset[4096];
    ok &= phstore_pkg_source_read_at(handle, 12345, offset, sizeof(offset)) == (int64_t)sizeof(offset);
    ok &= check_bytes(offset, 12345, sizeof(offset));
    ok &= phstore_pkg_source_read_at(handle, FIXTURE_SIZE - 10, offset, 11) < 0;
    phstore_pkg_source_close(handle);

    parallel_read_t cases[4] = {{65536,0},{1048576,0},{UINT64_C(99999999),0},{FIXTURE_SIZE-8192,0}};
    HANDLE threads[4];
    for (int i = 0; i < 4; i++) threads[i] = CreateThread(NULL, 0, parallel_read, &cases[i], 0, NULL);
    WaitForMultipleObjects(4, threads, TRUE, INFINITE);
    for (int i = 0; i < 4; i++) { DWORD code = 1; GetExitCodeThread(threads[i], &code); ok &= code == 0; CloseHandle(threads[i]); }

    handle = NULL;
    ok &= phstore_pkg_source_open_uri("phstore://missing", &handle, &size, filename, sizeof(filename)) != 0;
    ok &= phstore_pkg_source_open_uri("phstore://avatar/other", &handle, &size, filename, sizeof(filename)) != 0;
    ok &= phstore_pkg_source_open_uri("file://avatar", &handle, &size, filename, sizeof(filename)) != 0;
    ok &= phstore_pkg_source_open_uri("phstore://avatar", &handle, &size, filename, sizeof(filename)) == 0;
    InterlockedExchange(&g_short_body, 1);
    ok &= phstore_pkg_source_read_at(handle, 4096, offset, sizeof(offset)) < 0;
    InterlockedExchange(&g_short_body, 0);
    InterlockedExchange(&g_http_error, 1);
    ok &= phstore_pkg_source_read_at(handle, 8192, offset, sizeof(offset)) < 0;
    InterlockedExchange(&g_http_error, 0);
    InterlockedExchange(&g_cancel, 1);
    ok &= phstore_pkg_source_read_at(handle, 12288, offset, sizeof(offset)) < 0;
    InterlockedExchange(&g_cancel, 0);
    phstore_pkg_source_close(handle);

    g_native=1;FILE *local=fopen("native-staged-fixture.pkg","wb");
    if(!local)return 1;for(unsigned i=0;i<32768;i++)fputc(i%251,local);fclose(local);
    InterlockedExchange(&g_cancel,0);InterlockedExchange(&g_http_error,1);
    ok &= phstore_pkg_source_open_uri("phstore://avatar",&handle,&size,filename,sizeof(filename))==0;
    if(!handle)return 1;
    ok &= size==32768;
    ok &= phstore_pkg_source_read_at(handle,12345,offset,sizeof(offset))==sizeof(offset);
    ok &= check_bytes(offset,12345,sizeof(offset));
    ok &= phstore_pkg_source_read_at(handle,32760,offset,9)<0;
    InterlockedExchange(&g_cancel,1);ok &= phstore_pkg_source_read_at(handle,0,offset,16)<0;
    phstore_pkg_source_close(handle);remove("native-staged-fixture.pkg");
    printf("phstore_pkg_source=%s (open,size,0-65535,offset,parallel,short,http_error,cancel,bounds,URI)\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
