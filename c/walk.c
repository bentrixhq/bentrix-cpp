/*
 * walk.c - BENTRIX's engine from C, through bentrix.h: the CI tape made, a session opened, the
 * workbench armed and advanced thirty hours as Play would, a market order filled, the book read.
 * Exit 0 when every step held; the failing step and BENTRIX's words otherwise.
 *
 *   cc -I crates/bentrix-ffi/include sdk/c/walk.c -L <target>/debug -lbentrix -o walk
 *   LD_LIBRARY_PATH=<target>/debug ./walk <empty directory>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bentrix.h"

static int fail(const char *step) {
    fprintf(stderr, "FAILED %s: %s\n", step, bx_last_error());
    return 1;
}

/* The text after "key":" up to the next quote - enough for a URI in BENTRIX's own JSON. */
static int field(const char *json, const char *key, char *out, size_t n) {
    char needle[64];
    snprintf(needle, sizeof needle, "\"%s\":\"", key);
    const char *at = strstr(json, needle);
    if (!at) return 0;
    at += strlen(needle);
    const char *end = strchr(at, '"');
    if (!end || (size_t)(end - at) >= n) return 0;
    memcpy(out, at, end - at);
    out[end - at] = 0;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: walk <empty directory>\n"); return 2; }
    printf("bentrix %s\n", bx_version());
    char *ws, *store;
    if (bx_make_tape(argv[1], &ws, &store) != 0) return fail("bx_make_tape");
    BxSession *s = bx_open(ws, store);
    if (!s) return fail("bx_open");
    char *state = bx_call(s, "get_state", NULL);
    if (!state) return fail("get_state");
    char bench[256];
    if (!field(state, "uri", bench, sizeof bench) || !strstr(bench, "/bench/")) {
        /* the first uri is the laboratory's: the bench's follows it */
        const char *b = strstr(state, "/bench/");
        if (!b) { fprintf(stderr, "FAILED: no workbench in %s\n", state); return 1; }
        while (b > state && *(b - 1) != '"') b--;
        const char *e = strchr(b, '"');
        memcpy(bench, b, e - b);
        bench[e - b] = 0;
    }
    bx_free(state);
    if (bx_arm(s, bench, 20000) != 0) return fail("bx_arm");
    char args[512];
    snprintf(args, sizeof args, "{\"bench\":\"%s\",\"unit\":\"tick\",\"n\":1}", bench);
    char *step = bx_call(s, "replay_step", args);
    if (!step) return fail("replay_step");
    const char *c = strstr(step, "\"cursor_ms\":");
    long long from = c ? atoll(c + 12) : 0;
    bx_free(step);
    long long reached = 0;
    if (bx_advance(s, bench, from + 30LL * 3600000, (int64_t *)&reached) != 0) return fail("bx_advance");
    if (reached < from + 30LL * 3600000) { fprintf(stderr, "FAILED: the workbench did not advance\n"); return 1; }
    snprintf(args, sizeof args, "{\"bench\":\"%s\",\"symbol\":\"XAUUSD\",\"side\":\"buy\",\"volume\":0.01}", bench);
    char *sent = bx_call(s, "order_market", args);
    if (!sent) return fail("order_market");
    bx_free(sent);
    if (bx_advance(s, bench, reached + 60000, (int64_t *)&reached) != 0) return fail("bx_advance");
    char home[64], owner[64], name[64];
    if (sscanf(bench, "bentrix://%63[^/]/%63[^/]/bench/%63s", home, owner, name) != 3) { fprintf(stderr, "FAILED: %s\n", bench); return 1; }
    snprintf(args, sizeof args, "{\"home\":\"%s\",\"owner\":\"%s\",\"bench\":\"%s\"}", home, owner, name);
    char *book = bx_read(s, "trade_book", args);
    if (!book) return fail("trade_book");
    int filled = strstr(book, "\"side\":\"buy\"") != NULL;
    printf("book: %.160s\n", book);
    bx_free(book);
    if (bx_call(s, "no_such_tool", NULL) != NULL) { fprintf(stderr, "FAILED: a refusal answered as a success\n"); return 1; }
    printf("refused in its words: %s\n", bx_last_error());
    bx_close(s);
    bx_free(ws);
    bx_free(store);
    if (!filled) { fprintf(stderr, "FAILED: the order did not fill\n"); return 1; }
    printf("PASS: bench %s, reached %lld\n", bench, reached);
    return 0;
}
