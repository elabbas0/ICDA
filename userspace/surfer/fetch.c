/* fetch - command-line HTTP(S) client on Surfer's network stack.
 *   fetch [-v] [-s] <url>...
 *   -v prints the body, -s also writes a result line to /dev/serial */
#include <stdio.h>
#include <string.h>
#include "http.h"
#include "tls.h"
#include "icda_sys.h"

static int to_serial;

static void report(const char *line) {
    printf("%s", line);
    if (to_serial) icda_write_file("/dev/serial", line, strlen(line));
}

static int fetch_one(const char *url, int verbose) {
    char final_url[URL_CAP];
    char line[URL_CAP + 200];
    uint64_t t0 = icda_ticks();
    http_req_t *r = http_get(url, final_url, sizeof(final_url));
    if (!r) {
        snprintf(line, sizeof(line), "[fetch] %s FAIL too many redirects\n", url);
        report(line);
        return 1;
    }
    if (r->state != HTTP_DONE) {
        snprintf(line, sizeof(line), "[fetch] %s FAIL %s\n", url, r->error);
        report(line);
        http_free(r);
        return 1;
    }
    snprintf(line, sizeof(line), "[fetch] %s -> %d %s, %lu bytes in %lu ms\n", final_url, r->status,
             r->content_type, (unsigned long)r->body_len, (unsigned long)((icda_ticks() - t0) * 10));
    report(line);
    if (verbose && r->body) {
        fwrite(r->body, 1, r->body_len, stdout);
        printf("\n");
    }
    http_free(r);
    return 0;
}

int main(int argc, char **argv) {
    int verbose = 0, failures = 0, urls = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-s") == 0) to_serial = 1;
        else if (strcmp(argv[i], "-d") == 0) tls_debug = 1;
    }
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') continue;
        urls++;
        failures += fetch_one(argv[i], verbose);
    }
    if (!urls) {
        printf("usage: fetch [-v] [-s] <url>...\n");
        return 2;
    }
    return failures ? 1 : 0;
}
