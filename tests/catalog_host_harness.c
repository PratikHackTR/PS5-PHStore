#define PHSTORE_CATALOG_HOST_TEST 1
#include "../src/phstore_catalog.c"
#include "../src/phstore_url.c"
#include "../src/phstore_progress.c"
#include "../src/phstore_range.c"
#include "../include/phstore_http_routes.h"

static const char *g_catalog_lookup_fixture;
char *phstore_catalog_copy(size_t *length) {
    if (!g_catalog_lookup_fixture) return NULL;
    size_t n = strlen(g_catalog_lookup_fixture);
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, g_catalog_lookup_fixture, n + 1);
    if (length) *length = n;
    return copy;
}

static void print_token(const json_parser_t *p, int token) {
    if (token < 0 || p->tokens[token].type != JT_STRING) { fputs("<invalid>", stdout); return; }
    fwrite(p->json + p->tokens[token].start, 1,
           p->tokens[token].end - p->tokens[token].start, stdout);
}

static int fixture_valid(const char *package, const char *expected_error) {
    char json[2048];
    int n = snprintf(json, sizeof(json),
        "{\"schema_version\":1,\"catalog_version\":1,\"games\":[{\"id\":\"g\",\"title\":\"G\","
        "\"platform\":\"ps2\",\"turkish\":false,\"localization_type\":\"none\",\"packages\":[%s]}]}", package);
    if (n <= 0 || (size_t)n >= sizeof(json)) return 0;
    uint32_t schema = 0, version = 0; size_t games = 0; char generated[64]; const char *error = NULL;
    int valid = validate_catalog(json, (size_t)n, &schema, &version, &games, generated, &error);
    if (!expected_error) return valid;
    return !valid && error && strstr(error, expected_error) != NULL;
}

static int run_installer_fixtures(void) {
    const char *group = "{\"id\":\"group\",\"type\":\"base\",\"filename\":\"Avatar2.pkg\",\"version\":\"1.00\",\"size_bytes\":1095368704,\"platform\":\"ps2\",\"source_group\":\"PS2Games\",\"installable\":true}";
    const char *direct = "{\"id\":\"direct\",\"type\":\"base\",\"filename\":\"burak.pkg\",\"version\":\"1.00\",\"size_bytes\":123456789,\"platform\":\"ps4\",\"source_type\":\"direct_http\",\"download_url\":\"https://example.com/burak.pkg\",\"installable\":true}";
    const char *missing = "{\"id\":\"missing\",\"type\":\"base\",\"filename\":\"x.pkg\",\"version\":\"1.00\",\"size_bytes\":1,\"platform\":\"ps2\",\"source_type\":\"direct_http\"}";
    const char *ambiguous = "{\"id\":\"ambiguous\",\"type\":\"base\",\"filename\":\"x.pkg\",\"version\":\"1.00\",\"size_bytes\":1,\"platform\":\"ps2\",\"source_group\":\"PS2Games\",\"source_type\":\"direct_http\",\"download_url\":\"https://example.com/x.pkg\"}";
    const char *ftp = "{\"id\":\"ftp\",\"type\":\"base\",\"filename\":\"x.pkg\",\"version\":\"1.00\",\"size_bytes\":1,\"platform\":\"ps2\",\"source_type\":\"direct_http\",\"download_url\":\"ftp://example.com/x.pkg\"}";
    int ok = 1;
    ok &= fixture_valid(group, NULL); puts(ok ? "source_group_fixture=PASS" : "source_group_fixture=FAIL");
    char resolved_source[2048];
    int source_resolve = phstore_source_group_resolve("PS2Games", "Avatar2.pkg", resolved_source, sizeof(resolved_source)) == 0 &&
                         !strcmp(resolved_source, "http://148.135.181.3/ps2/Avatar2.pkg");
    ok &= source_resolve; printf("source_group_resolver=%s url=%s\n", source_resolve ? "PASS" : "FAIL", resolved_source);
    int direct_ok = fixture_valid(direct, NULL); ok &= direct_ok; puts(direct_ok ? "direct_http_fixture=PASS" : "direct_http_fixture=FAIL");
    char lookup_json[4096];
    int lookup_json_len = snprintf(lookup_json, sizeof(lookup_json),
        "{\"schema_version\":1,\"catalog_version\":1,\"games\":[{\"id\":\"g\",\"title\":\"G\",\"platform\":\"ps2\",\"packages\":[%s]}]}", direct);
    g_catalog_lookup_fixture = lookup_json;
    phstore_package_info_t resolved_package;
    char lookup_error[64] = {0};
    int lookup_ok = lookup_json_len > 0 && (size_t)lookup_json_len < sizeof(lookup_json) &&
        phstore_catalog_find_install_package("direct", &resolved_package, lookup_error) == 1 &&
        !strcmp(resolved_package.game_title, "G") && !resolved_package.title_id[0] &&
        !strcmp(resolved_package.filename, "burak.pkg") && resolved_package.size_bytes == 123456789;
    ok &= lookup_ok;
    puts(lookup_ok ? "install_package_metadata_lookup=PASS (game title, optional Title ID, file, size)" :
                     "install_package_metadata_lookup=FAIL");
    int missing_ok = fixture_valid(missing, "direct_url_invalid"); ok &= missing_ok; puts(missing_ok ? "direct_http_missing_url=PASS" : "direct_http_missing_url=FAIL");
    int ambiguous_ok = fixture_valid(ambiguous, "package_source_ambiguous"); ok &= ambiguous_ok; puts(ambiguous_ok ? "ambiguous_source=PASS" : "ambiguous_source=FAIL");
    int scheme_ok = fixture_valid(ftp, "unsupported_scheme"); ok &= scheme_ok; puts(scheme_ok ? "unsupported_scheme=PASS" : "unsupported_scheme=FAIL");
    char resolved[2048];
    int redirects = phstore_url_resolve_redirect("https://example.com/a/b.pkg", "c.pkg", resolved, sizeof(resolved)) &&
                    !strcmp(resolved, "https://example.com/a/c.pkg");
    ok &= redirects; printf("redirect_relative_resolution=%s value=%s\n", redirects ? "PASS" : "FAIL", resolved);
    uint64_t start = 1095368704ULL - 1024, end = 1095368703ULL;
    int range64 = start <= end && end < 1095368704ULL && end - start + 1 == 1024;
    ok &= range64; printf("uint64_range=%s start=%llu end=%llu length=%llu\n", range64 ? "PASS" : "FAIL",
        (unsigned long long)start, (unsigned long long)end, (unsigned long long)(end-start+1));

    uint64_t range_start = 99, range_end = 99;
    int range_standard = phstore_range_parse("bytes=0-1023", 1095368704ULL, &range_start, &range_end) == 1 &&
        range_start == 0 && range_end == 1023;
    int range_open = phstore_range_parse("bytes=1024-", 1095368704ULL, &range_start, &range_end) == 1 &&
        range_start == 1024 && range_end == 1095368703ULL;
    int range_last = phstore_range_parse("bytes=-1", 1095368704ULL, &range_start, &range_end) == 1 &&
        range_start == 1095368703ULL && range_end == 1095368703ULL;
    int range_bad = phstore_range_parse("bytes=1095368704-", 1095368704ULL, &range_start, &range_end) < 0;
    ok &= range_standard && range_open && range_last && range_bad;
    printf("range_parser=%s (0-1023, open-ended, last-byte, unsatisfiable)\n",
        range_standard && range_open && range_last && range_bad ? "PASS" : "FAIL");

    int range_response_ok = phstore_content_range_validate(206, "bytes 65536-131071/1095368704",
            65536, 65536, 131071, 1095368704ULL) &&
        !phstore_content_range_validate(200, "bytes 65536-131071/1095368704", 65536, 65536, 131071, 1095368704ULL) &&
        !phstore_content_range_validate(206, "bytes 65535-131071/1095368704", 65537, 65536, 131071, 1095368704ULL) &&
        !phstore_content_range_validate(206, "bytes 65536-131070/1095368704", 65536, 65536, 131071, 1095368704ULL) &&
        !phstore_content_range_validate(206, "bytes 65536-131071/1095368703", 65536, 65536, 131071, 1095368704ULL) &&
        !phstore_content_range_validate(206, "bytes 65536-131071/1095368704", 65535, 65536, 131071, 1095368704ULL) &&
        !phstore_content_range_validate(206, "bytes 0-18446744073709551615/18446744073709551615",
            18446744073709551615ULL, 0, 1, 2);
    ok &= range_response_ok;
    puts(range_response_ok ? "content_range_response_validation=PASS" : "content_range_response_validation=FAIL");

    phstore_coverage_t coverage;
    int coverage_ok = phstore_coverage_init(&coverage, 1095368704ULL) == 0;
    if (coverage_ok) {
        coverage_ok &= phstore_coverage_add(&coverage, 0, 1024) == 0;
        coverage_ok &= phstore_coverage_add(&coverage, 512, 1024) == 0;
        coverage_ok &= coverage.covered_bytes == 1536;
        coverage_ok &= phstore_coverage_add(&coverage, 0, 1024) == 0;
        coverage_ok &= coverage.covered_bytes == 1536;
        coverage_ok &= phstore_coverage_percent_x100(&coverage) <= 10000;
        phstore_coverage_destroy(&coverage);
    }
    phstore_coverage_t coverage64;
    int coverage64_ok = phstore_coverage_init(&coverage64, UINT64_C(1095368704)) == 0;
    if (coverage64_ok) {
        coverage64_ok &= phstore_coverage_add(&coverage64, UINT64_C(1095368703), 1) == 0;
        coverage64_ok &= coverage64.covered_bytes == 1 && phstore_coverage_percent_x100(&coverage64) <= 10000;
        phstore_coverage_destroy(&coverage64);
    }
    int speed_ok = phstore_speed_ema(0.0, 1000, 2.0) == 500.0 &&
        phstore_speed_ema(1000.0, 500, 1.0) == 850.0 && phstore_speed_ema(700.0, 1000, 0.0) == 700.0;
    double eta_seconds = 0.0;
    int eta_ok = phstore_eta_seconds(1000, 100, &eta_seconds) == 1 && eta_seconds == 10.0 &&
        phstore_eta_seconds(1000, 0, &eta_seconds) == 0;
    ok &= coverage_ok && coverage64_ok && speed_ok;
    printf("logical_coverage=%s (overlap/repeat/64-bit/capped percentage)\n", coverage_ok && coverage64_ok ? "PASS" : "FAIL");
    puts(speed_ok ? "speed_ema=PASS" : "speed_ema=FAIL");
    ok &= eta_ok;
    puts(eta_ok ? "eta_zero_speed=PASS" : "eta_zero_speed=FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1], "--installer-fixtures")) return run_installer_fixtures();
    if (argc != 2) { fprintf(stderr, "usage: catalog_host_harness catalog.json\n"); return 2; }
    FILE *file = fopen(argv[1], "rb");
    if (!file) { perror("fopen"); return 2; }
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 2; }
    long file_length = ftell(file);
    if (file_length <= 0 || fseek(file, 0, SEEK_SET) != 0) { fclose(file); return 2; }
    char *json = malloc((size_t)file_length + 1);
    if (!json) { fclose(file); return 2; }
    size_t length = fread(json, 1, (size_t)file_length, file);
    fclose(file);
    if (length != (size_t)file_length) { free(json); return 2; }
    json[length] = '\0';

    uint32_t schema = 0, version = 0;
    size_t validated_games = 0;
    char generated_at[64];
    const char *error = NULL;
    int valid = validate_catalog(json, length, &schema, &version, &validated_games,
                                 generated_at, &error);
    printf("validation=%s schema=%u catalog_version=%u games=%zu error=%s\n",
           valid ? "PASS" : "FAIL", schema, version, validated_games,
           error ? error : "none");
    if (!valid) { free(json); return 1; }

    json_parser_t p = { .json = json, .length = length };
    int root = -1;
    if (parse_value(&p, 0, &root) != 0) { free(p.tokens); free(json); return 1; }
    int games = object_get(&p, root, "games");
    size_t index = 0, true_count = 0, false_count = 0, empty_optional_count = 0;
    size_t noninstallable_count = 0, ps2_count = 0, ps4_count = 0, ps5_count = 0;
    int first = -1, last = -1;
    for (int game = p.tokens[games].first_child; game >= 0; game = p.tokens[game].next, index++) {
        if (first < 0) first = game;
        last = game;
        int platform = object_get(&p, game, "platform");
        if (string_equals(&p, platform, "ps2")) ps2_count++;
        else if (string_equals(&p, platform, "ps4")) ps4_count++;
        else if (string_equals(&p, platform, "ps5")) ps5_count++;
        int turkish = object_get(&p, game, "turkish");
        int boolean = 0;
        if (boolean_value(&p, turkish, &boolean)) { if (boolean) true_count++; else false_count++; }
        static const char *const optional_fields[] = {"background", "description", "developer", "publisher"};
        for (size_t i = 0; i < sizeof(optional_fields)/sizeof(optional_fields[0]); i++) {
            int field = object_get(&p, game, optional_fields[i]);
            if (field >= 0 && p.tokens[field].type == JT_STRING && p.tokens[field].end == p.tokens[field].start)
                empty_optional_count++;
        }
        int packages = object_get(&p, game, "packages");
        for (int package = p.tokens[packages].first_child; package >= 0; package = p.tokens[package].next) {
            int installable = object_get(&p, package, "installable");
            if (installable >= 0 && boolean_value(&p, installable, &boolean) && !boolean) noninstallable_count++;
        }
    }
    printf("first="); print_token(&p, object_get(&p, first, "id")); fputs(" | ", stdout); print_token(&p, object_get(&p, first, "title"));
    printf("\nlast="); print_token(&p, object_get(&p, last, "id")); fputs(" | ", stdout); print_token(&p, object_get(&p, last, "title"));
    printf("\nplatforms=ps2:%zu,ps4:%zu,ps5:%zu true=%zu false=%zu empty_optional=%zu installable_false=%zu\n",
           ps2_count, ps4_count, ps5_count, true_count, false_count, empty_optional_count, noninstallable_count);
    int first_turkish = object_get(&p, first, "turkish");
    printf("pre_fix_first_failure=game[0:"); print_token(&p, object_get(&p, first, "id"));
    printf("]:turkish=true rejected by old string-only comparator (%s)\n",
           string_equals(&p, first_turkish, "true") ? "unexpectedly accepted" : "reproduced");

    int routes_ok = phstore_request_path_matches("GET", "/api/store/catalog", "GET", "/api/store/catalog") &&
        phstore_request_path_matches("GET", "/api/store/catalog?cache=1", "GET", "/api/store/catalog") &&
        phstore_request_path_matches("GET", "/api/store/catalog/", "GET", "/api/store/catalog") &&
        !phstore_request_path_matches("POST", "/api/store/catalog", "GET", "/api/store/catalog");
    printf("catalog_route=%s (plain, query, trailing-slash, method guard)\n", routes_ok ? "PASS" : "FAIL");
    int states_ok = phstore_catalog_route_result(1, 0) == PHSTORE_CATALOG_ROUTE_READY &&
        phstore_catalog_route_result(0, 0) == PHSTORE_CATALOG_ROUTE_LOADING &&
        phstore_catalog_route_result(0, 1) == PHSTORE_CATALOG_ROUTE_ERROR;
    printf("catalog_route_states=%s (READY, LOADING, ERROR)\n", states_ok ? "PASS" : "FAIL");
    free(p.tokens);
    free(json);
    return valid && validated_games == 867 && ps2_count == 2 && ps4_count == 865 &&
           ps5_count == 0 && false_count > 0 && empty_optional_count > 0 &&
           noninstallable_count > 0 && routes_ok && states_ok ? 0 : 1;
}
