/* Native loopback-store catalog fetcher and validator. */
#include "phstore_catalog.h"
#include "phstore_image_cache.h"
#include "phstore_catalog_config.h"
#include "phstore_install.h"
#include "phstore_source_map.h"
#include "phstore_url.h"
#include "phstore_gdrive_protocol.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CATALOG_MAX_BYTES (4u * 1024u * 1024u)
#define JSON_TOKEN_MAX 300000u
#define JSON_DEPTH_MAX 64u
#ifndef PHSTORE_CATALOG_HOST_TEST
#include "phstore_assets.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define HTTP_HEADER_MAX 16384u
#define CONNECT_TIMEOUT_MS 8000
#define FETCH_TIMEOUT_MS 45000
#define IO_IDLE_TIMEOUT_MS 8000
#endif

typedef enum { JT_OBJECT, JT_ARRAY, JT_STRING, JT_PRIMITIVE } json_type_t;
typedef struct {
    size_t start, end;
    int first_child, last_child, next;
    json_type_t type;
} json_token_t;
typedef struct {
    const char *json;
    size_t length, cursor, count, capacity;
    json_token_t *tokens;
    const char *error;
} json_parser_t;

#ifndef PHSTORE_CATALOG_HOST_TEST
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;
static phstore_catalog_status_t g_status;
static char *g_active_catalog;
static size_t g_active_length;
static int g_worker_active;
static int g_cache_after_refresh;
#endif

static char g_validation_diagnostic[64];

static void skip_ws(json_parser_t *p) {
    while (p->cursor < p->length && (p->json[p->cursor] == ' ' || p->json[p->cursor] == '\t' ||
           p->json[p->cursor] == '\r' || p->json[p->cursor] == '\n')) p->cursor++;
}

static int token_new(json_parser_t *p, json_type_t type, size_t start, int *index) {
    if (p->count >= JSON_TOKEN_MAX) { p->error = "too_many_json_tokens"; return -1; }
    if (p->count == p->capacity) {
        size_t next = p->capacity ? p->capacity * 2 : 4096;
        if (next > JSON_TOKEN_MAX) next = JSON_TOKEN_MAX;
        json_token_t *grown = realloc(p->tokens, next * sizeof(*grown));
        if (!grown) { p->error = "out_of_memory"; return -1; }
        p->tokens = grown;
        p->capacity = next;
    }
    *index = (int)p->count++;
    p->tokens[*index] = (json_token_t){ .start = start, .end = start,
        .first_child = -1, .last_child = -1, .next = -1, .type = type };
    return 0;
}

static void token_add_child(json_parser_t *p, int parent, int child) {
    json_token_t *node = &p->tokens[parent];
    if (node->first_child < 0) node->first_child = child;
    else p->tokens[node->last_child].next = child;
    node->last_child = child;
}

static int parse_value(json_parser_t *p, unsigned depth, int *out);

static int parse_string(json_parser_t *p, int *out) {
    if (p->cursor >= p->length || p->json[p->cursor] != '"') return -1;
    size_t start = ++p->cursor;
    int index;
    if (token_new(p, JT_STRING, start, &index) != 0) return -1;
    while (p->cursor < p->length) {
        unsigned char c = (unsigned char)p->json[p->cursor++];
        if (c == '"') {
            p->tokens[index].end = p->cursor - 1;
            *out = index;
            return 0;
        }
        if (c < 0x20) { p->error = "invalid_json_string"; return -1; }
        if (c == '\\') {
            if (p->cursor >= p->length) { p->error = "invalid_json_escape"; return -1; }
            char escape = p->json[p->cursor++];
            if (strchr("\"\\/bfnrt", escape)) continue;
            if (escape != 'u' || p->length - p->cursor < 4) { p->error = "invalid_json_escape"; return -1; }
            for (int n = 0; n < 4; n++) if (!isxdigit((unsigned char)p->json[p->cursor++])) {
                p->error = "invalid_json_escape"; return -1;
            }
        }
    }
    p->error = "unterminated_json_string";
    return -1;
}

static int parse_value(json_parser_t *p, unsigned depth, int *out) {
    if (depth > JSON_DEPTH_MAX) { p->error = "json_depth_exceeded"; return -1; }
    skip_ws(p);
    if (p->cursor >= p->length) { p->error = "truncated_json"; return -1; }
    char c = p->json[p->cursor];
    if (c == '"') return parse_string(p, out);
    if (c == '{' || c == '[') {
        size_t start = p->cursor++;
        int parent;
        if (token_new(p, c == '{' ? JT_OBJECT : JT_ARRAY, start, &parent) != 0) return -1;
        skip_ws(p);
        char end = c == '{' ? '}' : ']';
        if (p->cursor < p->length && p->json[p->cursor] == end) {
            p->tokens[parent].end = ++p->cursor;
            *out = parent;
            return 0;
        }
        for (;;) {
            if (c == '{') {
                int key;
                skip_ws(p);
                if (parse_string(p, &key) != 0) { p->error = "invalid_object_key"; return -1; }
                size_t key_length = p->tokens[key].end - p->tokens[key].start;
                if (memchr(p->json + p->tokens[key].start, '\\', key_length)) {
                    p->error = "escaped_object_key_not_supported"; return -1;
                }
                for (int previous = p->tokens[parent].first_child; previous >= 0;) {
                    int previous_value = p->tokens[previous].next;
                    if (previous_value < 0) break;
                    size_t previous_length = p->tokens[previous].end - p->tokens[previous].start;
                    if (key_length == previous_length &&
                        memcmp(p->json + p->tokens[key].start, p->json + p->tokens[previous].start, key_length) == 0) {
                        p->error = "duplicate_json_key"; return -1;
                    }
                    previous = p->tokens[previous_value].next;
                }
                token_add_child(p, parent, key);
                skip_ws(p);
                if (p->cursor >= p->length || p->json[p->cursor++] != ':') { p->error = "missing_colon"; return -1; }
            }
            int child;
            if (parse_value(p, depth + 1, &child) != 0) return -1;
            token_add_child(p, parent, child);
            skip_ws(p);
            if (p->cursor >= p->length) { p->error = "truncated_json_container"; return -1; }
            char delimiter = p->json[p->cursor++];
            if (delimiter == end) break;
            if (delimiter != ',') { p->error = "invalid_json_delimiter"; return -1; }
        }
        p->tokens[parent].end = p->cursor;
        *out = parent;
        return 0;
    }
    size_t start = p->cursor;
    while (p->cursor < p->length && p->json[p->cursor] != ',' && p->json[p->cursor] != ']' &&
           p->json[p->cursor] != '}' && p->json[p->cursor] != ' ' && p->json[p->cursor] != '\r' &&
           p->json[p->cursor] != '\n' && p->json[p->cursor] != '\t') p->cursor++;
    if (p->cursor == start) { p->error = "invalid_json_value"; return -1; }
    int token;
    if (token_new(p, JT_PRIMITIVE, start, &token) != 0) return -1;
    p->tokens[token].end = p->cursor;
    size_t primitive_length = p->cursor - start;
    const char *primitive = p->json + start;
    int valid = (primitive_length == 4 && !memcmp(primitive, "true", 4)) ||
                (primitive_length == 5 && !memcmp(primitive, "false", 5)) ||
                (primitive_length == 4 && !memcmp(primitive, "null", 4));
    if (!valid) {
        size_t at = 0;
        if (at < primitive_length && primitive[at] == '-') at++;
        if (at >= primitive_length) { p->error = "invalid_json_primitive"; return -1; }
        if (primitive[at] == '0') at++;
        else if (primitive[at] >= '1' && primitive[at] <= '9') { while (at < primitive_length && isdigit((unsigned char)primitive[at])) at++; }
        else { p->error = "invalid_json_primitive"; return -1; }
        if (at < primitive_length && primitive[at] == '.') {
            at++;
            size_t digits = at;
            while (at < primitive_length && isdigit((unsigned char)primitive[at])) at++;
            if (digits == at) { p->error = "invalid_json_primitive"; return -1; }
        }
        if (at < primitive_length && (primitive[at] == 'e' || primitive[at] == 'E')) {
            at++;
            if (at < primitive_length && (primitive[at] == '+' || primitive[at] == '-')) at++;
            size_t digits = at;
            while (at < primitive_length && isdigit((unsigned char)primitive[at])) at++;
            if (digits == at) { p->error = "invalid_json_primitive"; return -1; }
        }
        if (at != primitive_length) { p->error = "invalid_json_primitive"; return -1; }
    }
    *out = token;
    return 0;
}

static int utf8_valid(const unsigned char *s, size_t n) {
    for (size_t i = 0; i < n;) {
        unsigned char c = s[i++];
        if (c < 0x80) continue;
        unsigned need; uint32_t value;
        if (c >= 0xC2 && c <= 0xDF) { need = 1; value = c & 0x1f; }
        else if (c >= 0xE0 && c <= 0xEF) { need = 2; value = c & 0x0f; }
        else if (c >= 0xF0 && c <= 0xF4) { need = 3; value = c & 0x07; }
        else return 0;
        if (n - i < need) return 0;
        for (unsigned j = 0; j < need; j++) {
            unsigned char d = s[i++];
            if ((d & 0xc0) != 0x80) return 0;
            value = (value << 6) | (d & 0x3f);
        }
        if ((need == 1 && value < 0x80) || (need == 2 && value < 0x800) ||
            (need == 3 && value < 0x10000) || value > 0x10ffff ||
            (value >= 0xd800 && value <= 0xdfff)) return 0;
    }
    return 1;
}

static int string_equals(const json_parser_t *p, int token, const char *literal) {
    if (token < 0 || p->tokens[token].type != JT_STRING) return 0;
    size_t n = p->tokens[token].end - p->tokens[token].start;
    return strlen(literal) == n && memcmp(p->json + p->tokens[token].start, literal, n) == 0;
}

static int object_get(const json_parser_t *p, int object, const char *name) {
    if (object < 0 || p->tokens[object].type != JT_OBJECT) return -1;
    for (int key = p->tokens[object].first_child; key >= 0;) {
        int value = p->tokens[key].next;
        if (value < 0) return -1;
        if (string_equals(p, key, name)) return value;
        key = p->tokens[value].next;
    }
    return -1;
}

static size_t array_count(const json_parser_t *p, int array) {
    size_t count = 0;
    if (array < 0 || p->tokens[array].type != JT_ARRAY) return 0;
    for (int item = p->tokens[array].first_child; item >= 0; item = p->tokens[item].next) count++;
    return count;
}

static int raw_nonempty_string(const json_parser_t *p, int token) {
    return token >= 0 && p->tokens[token].type == JT_STRING && p->tokens[token].end > p->tokens[token].start;
}

static int uint_value(const json_parser_t *p, int token, uint64_t *value) {
    if (token < 0 || p->tokens[token].type != JT_PRIMITIVE || p->tokens[token].end == p->tokens[token].start) return 0;
    uint64_t n = 0;
    for (size_t i = p->tokens[token].start; i < p->tokens[token].end; i++) {
        char c = p->json[i];
        if (c < '0' || c > '9' || n > (UINT64_MAX - (uint64_t)(c - '0')) / 10) return 0;
        n = n * 10 + (uint64_t)(c - '0');
    }
    *value = n;
    return 1;
}

static int boolean_value(const json_parser_t *p, int token, int *value) {
    if (token < 0 || p->tokens[token].type != JT_PRIMITIVE) return 0;
    size_t n = p->tokens[token].end - p->tokens[token].start;
    const char *raw = p->json + p->tokens[token].start;
    if (n == 4 && memcmp(raw, "true", 4) == 0) { *value = 1; return 1; }
    if (n == 5 && memcmp(raw, "false", 5) == 0) { *value = 0; return 1; }
    return 0;
}

static int enum_value(const json_parser_t *p, int token, const char *const *values, size_t count) {
    for (size_t i = 0; i < count; i++) if (string_equals(p, token, values[i])) return 1;
    return 0;
}

static int decoded_string(const json_parser_t *p,int token,char *out,size_t capacity) {
    if(token<0||p->tokens[token].type!=JT_STRING||!capacity)return 0;
    size_t used=0,end=p->tokens[token].end;
    for(size_t i=p->tokens[token].start;i<end;i++) {
        unsigned char bytes[4];size_t n=1;bytes[0]=(unsigned char)p->json[i];
        if(bytes[0]=='\\') {
            if(++i>=end)return 0;unsigned char e=(unsigned char)p->json[i];
            if(e=='u') {
                unsigned v=0;for(int k=0;k<4;k++){if(++i>=end)return 0;unsigned char h=p->json[i];v=v*16+(h<='9'?h-'0':(h|32)-'a'+10);}
                if(v>=0xd800&&v<=0xdbff) {
                    if(i+6>=end||p->json[i+1]!='\\'||p->json[i+2]!='u')return 0;i+=2;
                    unsigned low=0;for(int k=0;k<4;k++){unsigned char h=p->json[++i];low=low*16+(h<='9'?h-'0':(h|32)-'a'+10);}
                    if(low<0xdc00||low>0xdfff)return 0;v=0x10000+((v-0xd800)<<10)+(low-0xdc00);
                } else if(v>=0xdc00&&v<=0xdfff)return 0;
                if(!v)return 0;
                if(v<128)bytes[0]=v;
                else if(v<2048){n=2;bytes[0]=0xc0|(v>>6);bytes[1]=0x80|(v&63);}
                else if(v<65536){n=3;bytes[0]=0xe0|(v>>12);bytes[1]=0x80|((v>>6)&63);bytes[2]=0x80|(v&63);}
                else {n=4;bytes[0]=0xf0|(v>>18);bytes[1]=0x80|((v>>12)&63);bytes[2]=0x80|((v>>6)&63);bytes[3]=0x80|(v&63);}
            } else {
                switch(e){case 'b':bytes[0]=8;break;case 'f':bytes[0]=12;break;case 'n':bytes[0]=10;break;case 'r':bytes[0]=13;break;case 't':bytes[0]=9;break;case '"':case '/':case '\\':bytes[0]=e;break;default:return 0;}
            }
        }
        if(used+n>=capacity)return 0;memcpy(out+used,bytes,n);used+=n;
    }
    out[used]=0;return 1;
}
static int safe_filename(const json_parser_t *p, int token, int noninstallable) {
    char name[256];if(!decoded_string(p,token,name,sizeof(name))||!name[0])return 0;
    size_t n=strlen(name);if(!strcmp(name,".")||!strcmp(name,".."))return 0;
    for(size_t i=0;i<n;i++) {
        unsigned char c=name[i];
        if(c<0x20||c==0x7f||c=='/'||c=='\\'||c=='?'||c=='#'||c=='%'||c==':')return 0;
        if(!noninstallable && c=='.'&&i+1<n&&name[i+1]=='.')return 0;
    }
    return utf8_valid((const unsigned char *)name,n);
}

static int safe_package_id(const json_parser_t *p, int token) {
    if (!raw_nonempty_string(p, token)) return 0;
    size_t n = p->tokens[token].end - p->tokens[token].start;
    if (n >= PHSTORE_PACKAGE_ID_MAX) return 0;
    const unsigned char *s = (const unsigned char *)p->json + p->tokens[token].start;
    for (size_t i = 0; i < n; i++)
        if (!(isalnum(s[i]) || s[i] == '-' || s[i] == '_' || s[i] == '.')) return 0;
    return 1;
}

static int valid_direct_url(const json_parser_t *p, int token) {
    if (!raw_nonempty_string(p, token)) return 0;
    size_t n = p->tokens[token].end - p->tokens[token].start;
    if (n >= PHSTORE_DOWNLOAD_URL_MAX) return 0;
    char url[PHSTORE_DOWNLOAD_URL_MAX];
    memcpy(url, p->json + p->tokens[token].start, n); url[n] = '\0';
    return memchr(url, '\\', n) == NULL && phstore_url_validate(url);
}

static int direct_url_has_supported_scheme(const json_parser_t *p, int token) {
    if (token < 0 || p->tokens[token].type != JT_STRING) return 1;
    size_t n = p->tokens[token].end - p->tokens[token].start;
    const char *url = p->json + p->tokens[token].start;
    size_t colon = 0;
    while (colon < n && url[colon] != ':' && url[colon] != '/' && url[colon] != '?') colon++;
    if (colon >= n || url[colon] != ':') return 1;
    if (colon != 4 && colon != 5) return 0;
    const char *expected = colon == 4 ? "http" : "https";
    for (size_t i = 0; i < colon; i++)
        if (tolower((unsigned char)url[i]) != expected[i]) return 0;
    return 1;
}

static int copy_raw_string(const json_parser_t *,int,char *,size_t);
static int validate_package(json_parser_t *p, int package, const char **error) {
    static const char *const platforms[] = {"ps2", "ps4", "ps5"};
    static const char *const types[] = {"base", "update", "dlc", "homebrew"};
    int id = object_get(p, package, "id"), type = object_get(p, package, "type");
    int filename = object_get(p, package, "filename"), version = object_get(p, package, "version");
    int size = object_get(p, package, "size_bytes"), platform = object_get(p, package, "platform");
    int source = object_get(p, package, "source_group"), installable_token = object_get(p, package, "installable");
    int source_type = object_get(p, package, "source_type"), download_url = object_get(p, package, "download_url");
    int installable = 1;
    if (package < 0 || p->tokens[package].type != JT_OBJECT || !safe_package_id(p, id) ||
        !enum_value(p, type, types, sizeof(types)/sizeof(types[0])) || !raw_nonempty_string(p, version) ||
        !enum_value(p, platform, platforms, sizeof(platforms)/sizeof(platforms[0]))) {
        *error = "invalid_package_fields"; return 0;
    }
    if (source_type >= 0) {
        if (string_equals(p, source_type, "direct_http")) {
            if (source >= 0) { *error = "package_source_ambiguous"; return 0; }
            if (!direct_url_has_supported_scheme(p, download_url)) { *error = "unsupported_scheme"; return 0; }
            if (!valid_direct_url(p, download_url)) { *error = "direct_url_invalid"; return 0; }
        } else if (string_equals(p, source_type, "google_drive_public")) {
            int action=object_get(p,package,"action_type"),fid=object_get(p,package,"file_id"),sha=object_get(p,package,"sha256");
            if(download_url>=0){*error="package_source_ambiguous";return 0;}
            if((!string_equals(p,action,"download_file")&&!string_equals(p,action,"install_package"))||!raw_nonempty_string(p,fid)||(sha>=0&&!raw_nonempty_string(p,sha))){
                *error="invalid_gdrive_fields";return 0;}
            size_t fn=p->tokens[fid].end-p->tokens[fid].start,sn=sha>=0?p->tokens[sha].end-p->tokens[sha].start:0;
            if(fn>128||(sha>=0&&sn!=64)){*error="invalid_gdrive_fields";return 0;}
            for(size_t i=0;i<fn;i++){unsigned char c=(unsigned char)p->json[p->tokens[fid].start+i];
                if(!isalnum(c)&&c!='_'&&c!='-'){*error="invalid_gdrive_fields";return 0;}}
            for(size_t i=0;i<sn;i++)if(!isxdigit((unsigned char)p->json[p->tokens[sha].start+i])){*error="invalid_gdrive_fields";return 0;}
            if(string_equals(p,action,"install_package")) {
                size_t n=p->tokens[filename].end-p->tokens[filename].start;
                if(n<4 || memcmp(p->json+p->tokens[filename].end-4,".pkg",4)){*error="invalid_gdrive_action";return 0;}
            }
        } else if (string_equals(p,source_type,"archive_public")) {
            char archive_url[2048];int action=object_get(p,package,"action_type");
            if(source>=0 || !string_equals(p,action,"download_file") ||
               !copy_raw_string(p,download_url,archive_url,sizeof(archive_url)) ||
               strncmp(archive_url,"https://archive.org/download/",29) || !phstore_archive_url_allowed(archive_url)) {
                *error="invalid_archive_fields";return 0;
            }
        } else { *error = "unsupported_source_type"; return 0; }
    } else {
        if (download_url >= 0) { *error = "package_source_ambiguous"; return 0; }
        if (!raw_nonempty_string(p, source)) { *error = "package_source_missing"; return 0; }
    }
    uint64_t size_value;
    if (!uint_value(p, size, &size_value)) { *error = "invalid_package_size"; return 0; }
    (void)size_value;
    if (installable_token >= 0 && !boolean_value(p, installable_token, &installable)) {
        *error = "invalid_installable_flag"; return 0;
    }
    if ((string_equals(p,source_type,"google_drive_public") || string_equals(p,source_type,"archive_public")) && !size_value) {*error="invalid_package_size";return 0;}
    if (!safe_filename(p, filename, !installable)) { *error = "unsafe_package_filename"; return 0; }
    return 1;
}

static int validate_catalog(const char *json, size_t length, uint32_t *schema_version,
                            uint32_t *catalog_version, size_t *game_count,
                            char generated_at[64], const char **error) {
    if (!length || length > CATALOG_MAX_BYTES || !utf8_valid((const unsigned char *)json, length)) {
        *error = "invalid_catalog_encoding_or_size"; return 0;
    }
    json_parser_t p = { .json = json, .length = length };
    int root;
    if (parse_value(&p, 0, &root) != 0) {
        *error = p.error ? p.error : "malformed_json"; free(p.tokens); return 0;
    }
    skip_ws(&p);
    if (p.cursor != length || p.tokens[root].type != JT_OBJECT) {
        *error = "invalid_catalog_root"; free(p.tokens); return 0;
    }
    uint64_t schema, version;
    int schema_token = object_get(&p, root, "schema_version");
    int version_token = object_get(&p, root, "catalog_version");
    int games = object_get(&p, root, "games");
    if (!uint_value(&p, schema_token, &schema) || schema != 1 || schema > UINT32_MAX ||
        !uint_value(&p, version_token, &version) || version > UINT32_MAX ||
        games < 0 || p.tokens[games].type != JT_ARRAY) {
        *error = "invalid_catalog_schema"; free(p.tokens); return 0;
    }
    size_t count = array_count(&p, games);
    if (count == 0 || count > 10000) { *error = "invalid_game_count"; free(p.tokens); return 0; }
    static const char *const platforms[] = {"ps2", "ps4", "ps5"};
    static const char *const localizations[] = {"none", "text", "interface", "dubbed"};
    size_t array_index = 0;
    for (int game = p.tokens[games].first_child; game >= 0; game = p.tokens[game].next, array_index++) {
        int id = object_get(&p, game, "id"), title = object_get(&p, game, "title");
        int platform = object_get(&p, game, "platform"), turkish_token = object_get(&p, game, "turkish");
        int localization = object_get(&p, game, "localization_type"), packages = object_get(&p, game, "packages");
        int turkish;
        const char *field_error = NULL;
        if (p.tokens[game].type != JT_OBJECT) field_error = "not_object";
        else if (!raw_nonempty_string(&p, id)) field_error = "invalid_id";
        else if (!raw_nonempty_string(&p, title)) field_error = "invalid_title";
        else if (!enum_value(&p, platform, platforms, sizeof(platforms)/sizeof(platforms[0]))) field_error = "invalid_platform";
        else if (!boolean_value(&p, turkish_token, &turkish)) field_error = "invalid_turkish";
        else if (!enum_value(&p, localization, localizations, sizeof(localizations)/sizeof(localizations[0]))) field_error = "invalid_localization";
        else if (packages < 0 || p.tokens[packages].type != JT_ARRAY) field_error = "packages_not_array";
        if (field_error) {
            if (raw_nonempty_string(&p, id)) {
                size_t id_length = p.tokens[id].end - p.tokens[id].start;
                if (id_length > 20) id_length = 20;
                snprintf(g_validation_diagnostic, sizeof(g_validation_diagnostic), "game[%zu:%.20s]:%s", array_index,
                    p.json + p.tokens[id].start, field_error);
            } else snprintf(g_validation_diagnostic, sizeof(g_validation_diagnostic), "game[%zu]:%s", array_index, field_error);
            *error = g_validation_diagnostic; free(p.tokens); return 0;
        }
        (void)turkish; /* Both true and false are valid metadata values. */
        size_t package_count = array_count(&p, packages);
        if (package_count > 32) { *error = "too_many_packages"; free(p.tokens); return 0; }
        size_t package_index = 0;
        for (int item = p.tokens[packages].first_child; item >= 0; item = p.tokens[item].next, package_index++)
            if (!validate_package(&p, item, error)) {
                snprintf(g_validation_diagnostic, sizeof(g_validation_diagnostic), "game[%zu]:package[%zu]:%.24s", array_index, package_index, *error);
                *error = g_validation_diagnostic; free(p.tokens); return 0;
            }
        /* IDs must be unique so lookup and package identity are unambiguous. */
        for (int other = p.tokens[game].next; other >= 0; other = p.tokens[other].next) {
            int other_id = object_get(&p, other, "id");
            if (p.tokens[id].end - p.tokens[id].start == p.tokens[other_id].end - p.tokens[other_id].start &&
                memcmp(p.json + p.tokens[id].start, p.json + p.tokens[other_id].start,
                       p.tokens[id].end - p.tokens[id].start) == 0) {
                *error = "duplicate_game_id"; free(p.tokens); return 0;
            }
        }
    }
    generated_at[0] = '\0';
    int generated = object_get(&p, root, "generated_at");
    if (raw_nonempty_string(&p, generated)) {
        size_t n = p.tokens[generated].end - p.tokens[generated].start;
        int safe = n < 64;
        for (size_t i = 0; safe && i < n; i++) {
            char c = p.json[p.tokens[generated].start + i];
            if (!(isalnum((unsigned char)c) || c == ':' || c == '+' || c == '-' || c == '.' || c == 'T' || c == 'Z')) safe = 0;
        }
        if (safe) { memcpy(generated_at, p.json + p.tokens[generated].start, n); generated_at[n] = '\0'; }
    }
    *schema_version = (uint32_t)schema;
    *catalog_version = (uint32_t)version;
    *game_count = count;
    free(p.tokens);
    return 1;
}

static int snapshot_append_game(const json_parser_t *live,int live_games,const json_parser_t *saved,int game) {
    if(string_equals(saved,object_get(saved,game,"catalog_source"),"sp"))return 1;
    if(!string_equals(saved,object_get(saved,game,"catalog_source"),"ph") ||
       !string_equals(saved,object_get(saved,game,"metadata_origin"),"ph_ps5_extra"))return 0;
    int id=object_get(saved,game,"id");if(id<0)return 0;
    size_t n=saved->tokens[id].end-saved->tokens[id].start;
    for(int i=live->tokens[live_games].first_child;i>=0;i=live->tokens[i].next) {
        int other=object_get(live,i,"id");
        if(other>=0 && live->tokens[other].end-live->tokens[other].start==n &&
           !memcmp(saved->json+saved->tokens[id].start,live->json+live->tokens[other].start,n))return 0;
    }
    return 1;
}
static char *merge_ph_snapshot(const char *ph,size_t ph_length,const char *snapshot,size_t snapshot_length,size_t *length) {
    json_parser_t a={.json=ph,.length=ph_length};
    json_parser_t b={.json=snapshot,.length=snapshot_length};
    int ar,br;char *result=NULL;
    if(parse_value(&a,0,&ar)||parse_value(&b,0,&br))goto done;
    int ag=object_get(&a,ar,"games"),bg=object_get(&b,br,"games");
    if(ag<0||bg<0||a.tokens[ag].type!=JT_ARRAY||b.tokens[bg].type!=JT_ARRAY)goto done;
    size_t cut=a.tokens[ag].end-1,extra=0;
    for(int i=b.tokens[bg].first_child;i>=0;i=b.tokens[i].next)
        if(snapshot_append_game(&a,ag,&b,i))extra+=b.tokens[i].end-b.tokens[i].start+1;
    if(ph_length+extra>CATALOG_MAX_BYTES)goto done;
    result=malloc(ph_length+extra+1);if(!result)goto done;
    memcpy(result,ph,cut);size_t used=cut;int comma=a.tokens[ag].first_child>=0;
    for(int i=b.tokens[bg].first_child;i>=0;i=b.tokens[i].next) {
        if(!snapshot_append_game(&a,ag,&b,i))continue;
        if(comma)result[used++]=',';comma=1;
        size_t n=b.tokens[i].end-b.tokens[i].start;memcpy(result+used,b.json+b.tokens[i].start,n);used+=n;
    }
    memcpy(result+used,ph+cut,ph_length-cut);used+=ph_length-cut;result[used]=0;*length=used;
done:free(a.tokens);free(b.tokens);return result;
}

#ifndef PHSTORE_CATALOG_HOST_TEST
static int64_t monotonicish_ms(void) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
        return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
    struct timeval fallback;
    gettimeofday(&fallback, NULL);
    return (int64_t)fallback.tv_sec * 1000 + fallback.tv_usec / 1000;
}

static int wait_socket(int fd, int write_ready, int64_t deadline_ms) {
    int64_t remaining = deadline_ms - monotonicish_ms();
    if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
    if (remaining > IO_IDLE_TIMEOUT_MS) remaining = IO_IDLE_TIMEOUT_MS;
    fd_set set;
    FD_ZERO(&set); FD_SET(fd, &set);
    struct timeval timeout = { .tv_sec = remaining / 1000, .tv_usec = (remaining % 1000) * 1000 };
    int result = select(fd + 1, write_ready ? NULL : &set, write_ready ? &set : NULL, NULL, &timeout);
    if (result == 0) { errno = ETIMEDOUT; return -1; }
    if (result < 0 && errno == EINTR) return wait_socket(fd, write_ready, deadline_ms);
    return result < 0 ? -1 : 0;
}

static int send_all_timeout(int fd, const char *data, size_t length, int64_t deadline_ms) {
    while (length) {
        if (wait_socket(fd, 1, deadline_ms) != 0) return -1;
        ssize_t sent = send(fd, data, length, 0);
        if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (sent <= 0) return -1;
        data += sent; length -= (size_t)sent;
    }
    return 0;
}

static int connect_catalog(int64_t *deadline_out) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) != 0) { close(fd); return -1; }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_port = htons(PHSTORE_CATALOG_PORT);
    if (inet_pton(AF_INET, PHSTORE_CATALOG_HOST, &address.sin_addr) != 1) { close(fd); return -1; }
    int64_t started = monotonicish_ms();
    int64_t connect_deadline = started + CONNECT_TIMEOUT_MS;
    int result = connect(fd, (struct sockaddr *)&address, sizeof(address));
    if (result != 0 && errno != EINPROGRESS) { close(fd); return -1; }
    if (result != 0) {
        if (wait_socket(fd, 1, connect_deadline) != 0) { close(fd); return -1; }
        int error = 0; socklen_t error_length = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_length) != 0 || error != 0) {
            if (error) errno = error;
            close(fd); return -1;
        }
    }
    if (fcntl(fd, F_SETFL, flags) != 0) { close(fd); return -1; }
    *deadline_out = started + FETCH_TIMEOUT_MS;
    return fd;
}

static int parse_http_headers(char *response, size_t header_length, size_t *body_length) {
    char *end = response + header_length;
    char *line_end = strstr(response, "\r\n");
    if (!line_end || line_end >= end) return -1;
    *line_end = '\0';
    if ((size_t)(line_end - response) < 12 ||
        (strncmp(response, "HTTP/1.1 ", 9) != 0 && strncmp(response, "HTTP/1.0 ", 9) != 0)) return -1;
    if (response[9] != '2' || response[10] != '0' || response[11] != '0' ||
        (response[12] != '\0' && response[12] != ' ')) return -1;
    size_t found_length = 0;
    int have_length = 0;
    char *line = line_end + 2;
    while (line < end - 2) {
        char *next = strstr(line, "\r\n");
        if (!next || next > end) return -1;
        if (next == line) break;
        *next = '\0';
        char *colon = strchr(line, ':');
        if (!colon || colon == line || isspace((unsigned char)line[0])) return -1;
        for (char *c = line; c < colon; c++) if (!(isalnum((unsigned char)*c) || *c == '-')) return -1;
        *colon++ = '\0';
        while (*colon == ' ' || *colon == '\t') colon++;
        char *value_end = colon + strlen(colon);
        while (value_end > colon && (value_end[-1] == ' ' || value_end[-1] == '\t')) *--value_end = '\0';
        if (strcasecmp(line, "Content-Length") == 0) {
            if (!*colon || have_length) return -1;
            size_t n = 0;
            for (char *c = colon; *c; c++) {
                if (*c < '0' || *c > '9' || n > (CATALOG_MAX_BYTES - (size_t)(*c - '0')) / 10) return -1;
                n = n * 10 + (size_t)(*c - '0');
            }
            found_length = n; have_length = 1;
        } else if (strcasecmp(line, "Transfer-Encoding") == 0) {
            if (strcasecmp(colon, "identity") != 0) return -1;
        }
        line = next + 2;
    }
    if (!have_length || found_length == 0 || found_length > CATALOG_MAX_BYTES) return -1;
    *body_length = found_length;
    return 0;
}

static char *fetch_catalog_path(const char *path, size_t *catalog_length, const char **error) {
    int64_t deadline = 0;
    int fd = connect_catalog(&deadline);
    if (fd < 0) { *error = "connect_failed"; return NULL; }
    char request[512];
    int request_length = snprintf(request, sizeof(request),
        "GET %s HTTP/1.1\r\nHost: %s\r\nAccept: application/json\r\nAccept-Encoding: identity\r\nConnection: close\r\n\r\n",
        path, PHSTORE_CATALOG_HOST);
    if (request_length <= 0 || (size_t)request_length >= sizeof(request) ||
        send_all_timeout(fd, request, (size_t)request_length, deadline) != 0) {
        close(fd); *error = "request_failed"; return NULL;
    }
    size_t capacity = HTTP_HEADER_MAX + CATALOG_MAX_BYTES;
    char *response = malloc(capacity + 1);
    if (!response) { close(fd); *error = "out_of_memory"; return NULL; }
    size_t used = 0, header_length = 0, content_length = 0;
    int parsed_headers = 0;
    while (used < capacity) {
        if (parsed_headers && used >= header_length + content_length) break;
        if (!parsed_headers && used >= HTTP_HEADER_MAX) { *error = "headers_too_large"; goto failed; }
        if (wait_socket(fd, 0, deadline) != 0) { *error = "read_timeout"; goto failed; }
        ssize_t count = recv(fd, response + used, capacity - used, 0);
        if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (count <= 0) { *error = "truncated_http_response"; goto failed; }
        used += (size_t)count;
        if (!parsed_headers) {
            response[used] = '\0';
            char *separator = strstr(response, "\r\n\r\n");
            if (separator) {
                header_length = (size_t)(separator - response) + 4;
                if (header_length > HTTP_HEADER_MAX || parse_http_headers(response, header_length, &content_length) != 0) {
                    *error = "malformed_http_headers"; goto failed;
                }
                parsed_headers = 1;
                if (used > header_length + content_length) { *error = "oversized_http_body"; goto failed; }
            }
        }
    }
    if (!parsed_headers || used < header_length + content_length) { *error = "truncated_http_body"; goto failed; }
    memmove(response, response + header_length, content_length);
    response[content_length] = '\0';
    close(fd);
    *catalog_length = content_length;
    return response;
failed:
    close(fd);
    free(response);
    return NULL;
}

static void *catalog_worker(void *unused) {
    (void)unused;
    size_t length = 0, count = 0;
    uint32_t schema = 0, version = 0;
    char generated_at[64] = {0};
    const char *error = "fetch_failed";
    char *catalog = NULL;
    /* PH metadata refreshes from the user's VDS endpoint. Spectrum stays pinned
     * to its existing embedded snapshot and existing manifests/download flow. */
    char *ph=fetch_catalog_path("/phstore/catalog.json",&length,&error);
    if(ph) {
        if(validate_catalog(ph,length,&schema,&version,&count,generated_at,&error))
            catalog=merge_ph_snapshot(ph,length,(const char *)phstore_catalog_json,phstore_catalog_json_size,&length);
        free(ph);
    }
    fprintf(stderr,"[PHSTORE/CATALOG] PH_LIVE=%s endpoint=/phstore/catalog.json error=%s\n",catalog?"PASS":"FALLBACK",catalog?"none":error);
    if(!catalog) {
        length=phstore_catalog_json_size;catalog=malloc(length+1);
        if(catalog){memcpy(catalog,phstore_catalog_json,length);catalog[length]=0;}
    }
    if (catalog && !validate_catalog(catalog, length, &schema, &version, &count, generated_at, &error)) {
        free(catalog); catalog = NULL;
    }
    pthread_mutex_lock(&g_mutex);
    if (catalog) {
        char *old = g_active_catalog;
        g_active_catalog = catalog;
        g_active_length = length;
        g_status.has_catalog = 1;
        g_status.schema_version = schema;
        g_status.catalog_version = version;
        g_status.game_count = count;
        snprintf(g_status.generated_at, sizeof(g_status.generated_at), "%s", generated_at);
        g_status.state = PHSTORE_CATALOG_READY;
        g_status.last_error[0] = '\0';
        g_worker_active = 0;
        int cache_images = g_cache_after_refresh;
        g_cache_after_refresh = 0;
        pthread_mutex_unlock(&g_mutex);
        free(old);
        if (cache_images) (void)phstore_image_cache_start(1);
        return NULL;
    }
    snprintf(g_status.last_error, sizeof(g_status.last_error), "%s", error ? error : "fetch_failed");
    g_status.state = g_status.has_catalog ? PHSTORE_CATALOG_READY : PHSTORE_CATALOG_ERROR;
    g_cache_after_refresh = 0;
    g_worker_active = 0;
    pthread_mutex_unlock(&g_mutex);
    return NULL;
}

static int start_worker_locked(void) {
    if (g_worker_active) return 1;
    g_worker_active = 1;
    g_status.state = g_status.has_catalog ? PHSTORE_CATALOG_REFRESHING : PHSTORE_CATALOG_LOADING;
    g_status.last_error[0] = '\0';
    pthread_t thread;
    int result = pthread_create(&thread, NULL, catalog_worker, NULL);
    if (result != 0) {
        g_worker_active = 0;
        g_status.state = g_status.has_catalog ? PHSTORE_CATALOG_READY : PHSTORE_CATALOG_ERROR;
        snprintf(g_status.last_error, sizeof(g_status.last_error), "worker_create_failed");
        return -1;
    }
    pthread_detach(thread);
    return 0;
}

void phstore_catalog_init(void) {
    pthread_mutex_lock(&g_mutex);
    memset(&g_status, 0, sizeof(g_status));
    g_status.state = PHSTORE_CATALOG_LOADING;
    pthread_mutex_unlock(&g_mutex);
}

int phstore_catalog_start(void) {
    pthread_mutex_lock(&g_mutex);
    /* Make the bundled, validated snapshot usable before any VDS network wait.
     * The worker still replaces PH metadata with the live endpoint on startup. */
    if (!g_active_catalog) {
        uint32_t schema=0,version=0;size_t count=0;char generated_at[64]={0};
        const char *error=NULL;
        if(validate_catalog((const char *)phstore_catalog_json,phstore_catalog_json_size,
                            &schema,&version,&count,generated_at,&error)) {
            char *snapshot=malloc(phstore_catalog_json_size+1);
            if(snapshot) {
                memcpy(snapshot,phstore_catalog_json,phstore_catalog_json_size);
                snapshot[phstore_catalog_json_size]=0;g_active_catalog=snapshot;
                g_active_length=phstore_catalog_json_size;g_status.has_catalog=1;
                g_status.schema_version=schema;g_status.catalog_version=version;g_status.game_count=count;
                snprintf(g_status.generated_at,sizeof(g_status.generated_at),"%s",generated_at);
                fprintf(stderr,"[PHSTORE/CATALOG] EMBEDDED_READY games=%zu live_refresh=BACKGROUND\n",count);
            }
        }
    }
    int result = start_worker_locked();
    pthread_mutex_unlock(&g_mutex);
    return result;
}

int phstore_catalog_refresh(void) {
    pthread_mutex_lock(&g_mutex);
    g_cache_after_refresh = 1;
    int result = start_worker_locked();
    if (result < 0) g_cache_after_refresh = 0;
    pthread_mutex_unlock(&g_mutex);
    return result;
}

void phstore_catalog_get_status(phstore_catalog_status_t *status) {
    if (!status) return;
    pthread_mutex_lock(&g_mutex);
    *status = g_status;
    pthread_mutex_unlock(&g_mutex);
}

char *phstore_catalog_copy(size_t *length) {
    pthread_mutex_lock(&g_mutex);
    if (!g_active_catalog) { pthread_mutex_unlock(&g_mutex); return NULL; }
    char *copy = malloc(g_active_length + 1);
    if (copy) {
        memcpy(copy, g_active_catalog, g_active_length);
        copy[g_active_length] = '\0';
        if (length) *length = g_active_length;
    }
    pthread_mutex_unlock(&g_mutex);
    return copy;
}
#endif

static int copy_raw_string(const json_parser_t *p, int token, char *out, size_t capacity) {
    if (token < 0 || p->tokens[token].type != JT_STRING || !capacity) return 0;
    size_t length = p->tokens[token].end - p->tokens[token].start;
    if (length >= capacity || memchr(p->json + p->tokens[token].start, '\\', length)) return 0;
    memcpy(out, p->json + p->tokens[token].start, length);
    out[length] = '\0';
    return 1;
}

static int package_source_url(phstore_package_info_t *package, char *error, size_t error_size) {
    if (!strcmp(package->source_type,"google_drive_public")) {
        int n=snprintf(package->download_url,sizeof(package->download_url),
            "https://drive.usercontent.google.com/download?id=%s&export=download&confirm=t",package->file_id);
        if(n<0||(size_t)n>=sizeof(package->download_url)){snprintf(error,error_size,"source_url_too_long");return -1;}
        return 0;
    }
    if (strcmp(package->source_type, "direct_http") == 0) {
        if (!package->download_url[0]) { snprintf(error, error_size, "direct_url_invalid"); return -1; }
        /* Resolve by catalog ID on the VDS; never send arbitrary upstream URLs. */
        if (strncmp(package->download_url, "https://", 8) == 0) {
            if (!package->package_id[0]) { snprintf(error, error_size, "invalid_package_id"); return -1; }
            for (const unsigned char *c = (const unsigned char *)package->package_id; *c; c++)
                if (!(isalnum(*c) || *c == '-' || *c == '_' || *c == '.')) {
                    snprintf(error, error_size, "invalid_package_id"); return -1;
                }
            int n = snprintf(package->download_url, sizeof(package->download_url),
                             "http://" PHSTORE_CATALOG_HOST "%s%s",
                             !strncmp(package->package_id, "sp-", 3) ? "/phstore2/pkg/" : "/phstore/pkg/", package->package_id);
            if (n < 0 || (size_t)n >= sizeof(package->download_url)) {
                snprintf(error, error_size, "source_url_too_long"); return -1;
            }
        }
        return 0;
    }
    int result = phstore_source_group_resolve(package->source_group, package->filename,
                                               package->download_url, sizeof(package->download_url));
    if (result == -2) { snprintf(error, error_size, "source_group_unknown"); return -1; }
    if (result != 0) { snprintf(error, error_size, "source_url_too_long"); return -1; }
    return 0;
}

int phstore_catalog_find_install_package(const char *package_id, phstore_package_info_t *package,
                                         char error[64]) {
    if (error) error[0] = '\0';
    if (!package_id || !*package_id || !package) {
        if (error) snprintf(error, 64, "package_not_found");
        return 0;
    }
    for (const unsigned char *c = (const unsigned char *)package_id; *c; c++)
        if (!(isalnum(*c) || *c == '-' || *c == '_' || *c == '.')) {
            if (error) snprintf(error, 64, "package_not_found");
            return 0;
        }
    size_t catalog_length = 0;
    char *catalog = phstore_catalog_copy(&catalog_length);
    if (!catalog) {
        if (error) snprintf(error, 64, "catalog_unavailable");
        return -1;
    }
    json_parser_t p = {.json = catalog, .length = catalog_length};
    int root = -1;
    if (parse_value(&p, 0, &root) != 0) {
        free(p.tokens); free(catalog);
        if (error) snprintf(error, 64, "catalog_parse_failed");
        return -1;
    }
    int games = object_get(&p, root, "games");
    int found = 0;
    memset(package, 0, sizeof(*package));
    for (int game = games >= 0 ? p.tokens[games].first_child : -1; game >= 0 && !found; game = p.tokens[game].next) {
        int game_id = object_get(&p, game, "id");
        int game_title = object_get(&p, game, "title");
        int game_title_id = object_get(&p, game, "title_id");
        int packages = object_get(&p, game, "packages");
        for (int item = packages >= 0 ? p.tokens[packages].first_child : -1; item >= 0; item = p.tokens[item].next) {
            int id = object_get(&p, item, "id");
            if (id < 0 || !string_equals(&p, id, package_id)) continue;
            int type = object_get(&p, item, "type"), filename = object_get(&p, item, "filename");
            int version = object_get(&p, item, "version"), platform = object_get(&p, item, "platform");
            int group = object_get(&p, item, "source_group"), source_type = object_get(&p, item, "source_type");
            int download_url = object_get(&p, item, "download_url"), size = object_get(&p, item, "size_bytes");
            int installable = object_get(&p, item, "installable"), installable_value = 1;
            if (!copy_raw_string(&p, id, package->package_id, sizeof(package->package_id)) ||
                !copy_raw_string(&p, game_id, package->game_id, sizeof(package->game_id)) ||
                !decoded_string(&p, game_title, package->game_title, sizeof(package->game_title)) ||
                !copy_raw_string(&p, type, package->package_type, sizeof(package->package_type)) ||
                !decoded_string(&p, filename, package->filename, sizeof(package->filename)) ||
                !copy_raw_string(&p, version, package->version, sizeof(package->version)) ||
                !copy_raw_string(&p, platform, package->platform, sizeof(package->platform)) ||
                !uint_value(&p, size, &package->size_bytes) ||
                (group >= 0 && !copy_raw_string(&p, group, package->source_group, sizeof(package->source_group))) ||
                (installable >= 0 && !boolean_value(&p, installable, &installable_value)) ||
                (game_title_id >= 0 && !copy_raw_string(&p, game_title_id, package->title_id, sizeof(package->title_id)))) {
                if (error) snprintf(error, 64, "package_metadata_invalid");
                found = -1; break;
            }
            if (source_type >= 0) {
                if (!copy_raw_string(&p, source_type, package->source_type, sizeof(package->source_type))) {
                    if (error) snprintf(error, 64, "direct_url_invalid");
                    found = -1; break;
                }
                if(!strcmp(package->source_type,"google_drive_public")) {
                    if(!copy_raw_string(&p,object_get(&p,item,"action_type"),package->action_type,sizeof(package->action_type))||
                       !copy_raw_string(&p,object_get(&p,item,"file_id"),package->file_id,sizeof(package->file_id))||
                       (object_get(&p,item,"sha256")>=0 && !copy_raw_string(&p,object_get(&p,item,"sha256"),package->sha256,sizeof(package->sha256)))) {
                        if(error)snprintf(error,64,"invalid_gdrive_fields");found=-1;break;
                    }
                }else if(!copy_raw_string(&p,download_url,package->download_url,sizeof(package->download_url))) {
                    if(error)snprintf(error,64,"direct_url_invalid");found=-1;break;
                }
            } else {
                snprintf(package->source_type, sizeof(package->source_type), "source_group");
            }
            if(!strcmp(package->source_type,"archive_public")) {
                if(!copy_raw_string(&p,object_get(&p,item,"action_type"),package->action_type,sizeof(package->action_type)) ||
                   (object_get(&p,item,"sha256")>=0 && !copy_raw_string(&p,object_get(&p,item,"sha256"),package->sha256,sizeof(package->sha256)))) {
                    if(error)snprintf(error,64,"invalid_archive_fields");found=-1;break;
                }
            }
            package->installable = installable_value;
            found = 1;
            break;
        }
    }
    free(p.tokens);
    free(catalog);
    if (!found && error) snprintf(error, 64, "package_not_found");
    return found;
}

int phstore_catalog_resolve_package_source(phstore_package_info_t *package, char error[64]) {
    if (!package) { if (error) snprintf(error, 64, "package_source_missing"); return -1; }
    return package_source_url(package, error ? error : g_validation_diagnostic, 64);
}

char *phstore_catalog_game_copy(const char *id, size_t *length) {
    if (!id || !*id) return NULL;
    for (const unsigned char *c = (const unsigned char *)id; *c; c++)
        if (!(isalnum(*c) || *c == '-' || *c == '_' || *c == '.')) return NULL;
    size_t catalog_length;
    char *catalog = phstore_catalog_copy(&catalog_length);
    if (!catalog) return NULL;
    char needle[128];
    int n = snprintf(needle, sizeof(needle), "\"id\":\"%s\"", id);
    if (n <= 0 || (size_t)n >= sizeof(needle)) { free(catalog); return NULL; }
    char *match = strstr(catalog, needle);
    if (!match) { free(catalog); return NULL; }
    char *start = match;
    while (start > catalog && *start != '{') start--;
    if (*start != '{') { free(catalog); return NULL; }
    unsigned depth = 0; int quoted = 0, escaped = 0;
    char *end = NULL;
    for (char *c = start; c < catalog + catalog_length; c++) {
        if (quoted) {
            if (escaped) escaped = 0;
            else if (*c == '\\') escaped = 1;
            else if (*c == '"') quoted = 0;
        } else if (*c == '"') quoted = 1;
        else if (*c == '{') depth++;
        else if (*c == '}' && --depth == 0) { end = c + 1; break; }
    }
    if (!end) { free(catalog); return NULL; }
    size_t object_length = (size_t)(end - start);
    char *result = malloc(object_length + 1);
    if (result) { memcpy(result, start, object_length); result[object_length] = '\0'; if (length) *length = object_length; }
    free(catalog);
    return result;
}

#ifndef PHSTORE_CATALOG_HOST_TEST
/* Decode URL JSON escapes without interpreting any non-cover catalog fields. */
static int cover_url_decode(const json_parser_t *p, int token, char out[2048]) {
    if (token < 0 || p->tokens[token].type != JT_STRING) return 0;
    size_t used = 0;
    for (size_t i = p->tokens[token].start; i < p->tokens[token].end; i++) {
        unsigned char c = (unsigned char)p->json[i];
        if (c == '\\') {
            c = (unsigned char)p->json[++i];
            if (c == 'u') {
                unsigned value = 0;
                for (int k = 0; k < 4; k++) {
                    unsigned char h = (unsigned char)p->json[++i];
                    value = value * 16 + (h <= '9' ? h - '0' : (h | 32) - 'a' + 10);
                }
                if (value > 127) return 0; /* URLs must use percent-encoded non-ASCII. */
                c = (unsigned char)value;
            } else if (c != '/' && c != '\\' && c != '"') return 0;
        }
        if (used + 1 >= 2048) return 0;
        out[used++] = (char)c;
    }
    out[used] = '\0';
    return phstore_url_validate(out);
}
char **phstore_catalog_cover_urls(size_t *count) {
    *count = 0;
    size_t length = 0;
    char *catalog = phstore_catalog_copy(&length);
    if (!catalog) return NULL;
    json_parser_t p = {.json = catalog, .length = length};
    int root;
    if (parse_value(&p, 0, &root) != 0) { free(p.tokens); free(catalog); return NULL; }
    int games = object_get(&p, root, "games");
    size_t capacity = array_count(&p, games);
    char **urls = calloc(capacity ? capacity : 1, sizeof(*urls));
    if (!urls) { free(p.tokens); free(catalog); return NULL; }
    for (int game = games >= 0 ? p.tokens[games].first_child : -1; game >= 0; game = p.tokens[game].next) {
        char url[2048];
        if (!cover_url_decode(&p, object_get(&p, game, "cover"), url)) continue;
        int duplicate = 0;
        for (size_t i = 0; i < *count; i++) if (!strcmp(urls[i], url)) { duplicate = 1; break; }
        if (duplicate) continue;
        urls[*count] = strdup(url);
        if (!urls[*count]) {
            for (size_t i = 0; i < *count; i++) free(urls[i]);
            free(urls); urls = NULL; *count = 0; break;
        }
        (*count)++;
    }
    free(p.tokens); free(catalog);
    return urls;
}
#endif
