#include "phstore_install_request.h"

#include <stdio.h>
#include <string.h>

int main(void) {
    struct { const char *json; int expected; const char *id; } cases[] = {
        {"{\"package_id\":\"avatar\"}", 1, "avatar"},
        {" { \"package_id\" : \"Avatar_2.pkg-v1\" } \n", 1, "Avatar_2.pkg-v1"},
        {"{}", 0, ""},
        {"{\"other\":\"avatar\"}", 0, ""},
        {"{\"package_id\":\"bad/id\"}", 0, ""},
        {"{\"package_id\":\"escaped\\\"id\"}", 0, ""},
        {"{\"package_id\":\"avatar\",\"extra\":true}", 0, ""},
        {"{\"package_id\":\"avatar\"} trailing", 0, ""},
        {"{\"package_id\":\"\"}", 0, ""}
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char id[PHSTORE_PACKAGE_ID_MAX] = {0};
        int actual = phstore_install_request_parse(cases[i].json, strlen(cases[i].json), id);
        if (actual != cases[i].expected || (actual && strcmp(id, cases[i].id) != 0)) {
            fprintf(stderr, "install_request_case_%zu=FAIL parsed=%d id=%s\n", i, actual, id);
            return 1;
        }
    }
    puts("install_request=PASS (browser JSON, whitespace, malformed, escaped, extra-key, empty-id)");
    return 0;
}
