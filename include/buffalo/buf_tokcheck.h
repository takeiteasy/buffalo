/*
 * buf_tokcheck.h -- validate a checked-in <name>_tokens.h against a spec's
 * %tokens list.
 *
 * Pure C, header-only, fixed arenas, no malloc. Compiled twice: plain `cc`
 * for tests/t_tokcheck.c, and inside cccc's comptime VM via `#include
 * @comptime` from src/buf_comptime.c. Like buf_rx.h it never calls
 * MacroErrorAt; the sticky first-wins error string is lifted into a comptime
 * diagnostic by src/buf_comptime.c.
 *
 * Token kinds are `enum` constants -- not linkable symbols -- so the header
 * is hand-written and checked in, and this pass guards it against drift from
 * the .l spec. The check: the first `enum { ... }` in the header must list,
 * in order,
 *
 *     TOK_EOF = 0, TOK_ERROR = 1, TOK_<N0>, TOK_<N1>, ...
 *
 * where N0, N1, ... is the spec's %tokens list. Any missing kind, extra
 * kind, misordering, or wrong reserved value is reported with the header
 * path and the offending position.
 *
 * The scanner is deliberately small: it skips `/x ... x/` and `// ...`
 * comments and `#` preprocessor lines, finds the first `enum`, then collects
 * identifiers up to the matching `}`, tolerating `= <int>` initialisers and
 * trailing commas. It does not evaluate arbitrary constant expressions -- a
 * `= <int>` is read only for the two reserved slots.
 */
#ifndef BUF_TOKCHECK_H
#define BUF_TOKCHECK_H

#include <stdio.h>
#include <string.h>

#include "buf_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUF_TC_MAX_ENUM   512
#define BUF_TC_SRC_MAX    65536

typedef struct {
    char name[BUF_RX_NAME_MAX];
    int  has_value;
    long value;
} BufTcEntry;

typedef struct {
    BufTcEntry  entries[BUF_TC_MAX_ENUM];
    int         count;

    char        src[BUF_TC_SRC_MAX];
    int         src_len;
    const char *path;

    char error[BUF_RX_ERR_MAX];
    int  has_error;
} BufTc;

static void buf_tc_err0(BufTc *tc, const char *msg) {
    if (tc->has_error) return;
    snprintf(tc->error, sizeof(tc->error), "%s: %s", tc->path, msg);
    tc->has_error = 1;
}
static void buf_tc_err_s(BufTc *tc, const char *fmt, const char *arg) {
    char tmp[192];
    if (tc->has_error) return;
    snprintf(tmp, sizeof(tmp), fmt, arg);
    buf_tc_err0(tc, tmp);
}
static void buf_tc_err_ss(BufTc *tc, const char *fmt, const char *a,
                          const char *b) {
    char tmp[192];
    if (tc->has_error) return;
    snprintf(tmp, sizeof(tmp), fmt, a, b);
    buf_tc_err0(tc, tmp);
}

/* --- tokenising scan over the header text -------------------------- */

typedef struct {
    const char *p, *end;
} BufTcLx;

/* Advance past whitespace, /x x/ and // comments, and #... lines. */
static void buf_tc_skip(BufTcLx *lx) {
    for (;;) {
        if (lx->p >= lx->end) return;
        if (*lx->p == ' ' || *lx->p == '\t' || *lx->p == '\r' ||
            *lx->p == '\n' || *lx->p == '\f' || *lx->p == '\v') {
            lx->p++;
            continue;
        }
        if (lx->p + 1 < lx->end && lx->p[0] == '/' && lx->p[1] == '*') {
            lx->p += 2;
            while (lx->p + 1 < lx->end &&
                   !(lx->p[0] == '*' && lx->p[1] == '/'))
                lx->p++;
            if (lx->p + 1 < lx->end) lx->p += 2;
            else lx->p = lx->end;
            continue;
        }
        if (lx->p + 1 < lx->end && lx->p[0] == '/' && lx->p[1] == '/') {
            while (lx->p < lx->end && *lx->p != '\n') lx->p++;
            continue;
        }
        if (*lx->p == '#') { /* preprocessor line -- include guard etc. */
            while (lx->p < lx->end && *lx->p != '\n') lx->p++;
            continue;
        }
        return;
    }
}

/* Read an identifier into `out` (cap BUF_RX_NAME_MAX). Returns length, or 0
 * if the cursor is not on an identifier start. */
static int buf_tc_ident(BufTcLx *lx, char *out) {
    int n = 0;
    if (lx->p >= lx->end || !buf_rx_is_name_start((unsigned char)*lx->p))
        return 0;
    while (lx->p < lx->end && buf_rx_is_name((unsigned char)*lx->p)) {
        if (n < BUF_RX_NAME_MAX - 1) out[n] = *lx->p;
        n++;
        lx->p++;
    }
    if (n > BUF_RX_NAME_MAX - 1) n = BUF_RX_NAME_MAX - 1;
    out[n] = '\0';
    return n;
}

/* Parse an integer literal (optional sign, decimal or 0x..). Returns 1 on
 * success. Cursor must be on a digit or sign. */
static int buf_tc_int(BufTcLx *lx, long *out) {
    long v = 0;
    int  neg = 0, any = 0, base = 10;
    if (lx->p < lx->end && (*lx->p == '+' || *lx->p == '-')) {
        neg = (*lx->p == '-');
        lx->p++;
    }
    if (lx->p + 1 < lx->end && lx->p[0] == '0' &&
        (lx->p[1] == 'x' || lx->p[1] == 'X')) {
        base = 16;
        lx->p += 2;
        while (lx->p < lx->end) {
            int c = (unsigned char)*lx->p, d;
            if (c >= '0' && c <= '9') d = c - '0';
            else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
            else break;
            v = v * base + d;
            any = 1;
            lx->p++;
        }
    } else {
        while (lx->p < lx->end && *lx->p >= '0' && *lx->p <= '9') {
            v = v * 10 + (*lx->p - '0');
            any = 1;
            lx->p++;
        }
    }
    /* tolerate an integer suffix (U/L) */
    while (lx->p < lx->end && (*lx->p == 'u' || *lx->p == 'U' ||
                              *lx->p == 'l' || *lx->p == 'L'))
        lx->p++;
    if (!any) return 0;
    *out = neg ? -v : v;
    return 1;
}

/* Locate and read the first enum body into tc->entries. 0 ok / -1 error. */
static int buf_tc_scan(BufTc *tc) {
    BufTcLx lx;
    char    id[BUF_RX_NAME_MAX];
    int     found = 0;

    lx.p   = tc->src;
    lx.end = tc->src + tc->src_len;
    tc->count = 0;

    /* find `enum` */
    while (lx.p < lx.end) {
        buf_tc_skip(&lx);
        if (lx.p >= lx.end) break;
        if (buf_rx_is_name_start((unsigned char)*lx.p)) {
            int n = buf_tc_ident(&lx, id);
            if (n == 4 && strcmp(id, "enum") == 0) { found = 1; break; }
            continue; /* skip any other identifier (a tag name, etc.) */
        }
        lx.p++; /* punctuation we do not care about before the enum */
    }
    if (!found) {
        buf_tc_err0(tc, "no `enum { ... }` found in the token header");
        return -1;
    }

    buf_tc_skip(&lx);
    /* optional enum tag */
    if (lx.p < lx.end && buf_rx_is_name_start((unsigned char)*lx.p))
        buf_tc_ident(&lx, id);
    buf_tc_skip(&lx);
    if (lx.p >= lx.end || *lx.p != '{') {
        buf_tc_err0(tc, "malformed enum in the token header (expected '{')");
        return -1;
    }
    lx.p++; /* '{' */

    for (;;) {
        int n;
        buf_tc_skip(&lx);
        if (lx.p >= lx.end) {
            buf_tc_err0(tc, "unterminated enum in the token header");
            return -1;
        }
        if (*lx.p == '}') { lx.p++; break; }
        if (*lx.p == ',') { lx.p++; continue; }

        n = buf_tc_ident(&lx, id);
        if (n == 0) {
            buf_tc_err0(tc, "unexpected token in enum body of the token header");
            return -1;
        }
        if (tc->count >= BUF_TC_MAX_ENUM) {
            buf_tc_err0(tc, "token header enum has too many entries");
            return -1;
        }
        strncpy(tc->entries[tc->count].name, id, BUF_RX_NAME_MAX - 1);
        tc->entries[tc->count].name[BUF_RX_NAME_MAX - 1] = '\0';
        tc->entries[tc->count].has_value = 0;
        tc->entries[tc->count].value     = 0;

        buf_tc_skip(&lx);
        if (lx.p < lx.end && *lx.p == '=') {
            long v;
            lx.p++;
            buf_tc_skip(&lx);
            if (!buf_tc_int(&lx, &v)) {
                buf_tc_err_s(tc,
                             "enum entry '%s' has a non-integer initialiser "
                             "(only plain integers are understood)",
                             id);
                return -1;
            }
            tc->entries[tc->count].has_value = 1;
            tc->entries[tc->count].value     = v;
        }
        tc->count++;

        buf_tc_skip(&lx);
        if (lx.p < lx.end && *lx.p == ',') { lx.p++; continue; }
        if (lx.p < lx.end && *lx.p == '}') { lx.p++; break; }
    }
    return tc->has_error ? -1 : 0;
}

/* --- comparison against the spec's %tokens list ------------------- */

/* Build the expected name for %tokens slot i (i<0 => reserved slots). */
static void buf_tc_expected(char *out, BufRx *rx, int idx) {
    /* idx 0 -> TOK_EOF, 1 -> TOK_ERROR, 2.. -> TOK_<tokens[idx-2]> */
    if (idx == 0) {
        strncpy(out, "TOK_EOF", BUF_RX_NAME_MAX - 1);
    } else if (idx == 1) {
        strncpy(out, "TOK_ERROR", BUF_RX_NAME_MAX - 1);
    } else {
        int k = 0;
        const char *nm = rx->tokens[idx - 2];
        out[k++] = 'T'; out[k++] = 'O'; out[k++] = 'K'; out[k++] = '_';
        while (nm[0] && k < BUF_RX_NAME_MAX - 1) out[k++] = *nm++;
        out[k] = '\0';
        return;
    }
    out[BUF_RX_NAME_MAX - 1] = '\0';
}

/* Validate tc (already scanned) against rx's %tokens list. 0 ok / -1. */
static int buf_tc_compare(BufTc *tc, BufRx *rx) {
    int want = rx->token_count + 2; /* + TOK_EOF, TOK_ERROR */
    int i;
    char exp[BUF_RX_NAME_MAX];

    if (tc->has_error) return -1;

    for (i = 0; i < want; i++) {
        buf_tc_expected(exp, rx, i);
        if (i >= tc->count) {
            buf_tc_err_s(tc, "token header is missing '%s'", exp);
            return -1;
        }
        if (strcmp(tc->entries[i].name, exp) != 0) {
            buf_tc_err_ss(tc, "token header has '%s' where '%s' is expected",
                          tc->entries[i].name, exp);
            return -1;
        }
        if (i == 0 && (!tc->entries[i].has_value || tc->entries[i].value != 0)) {
            buf_tc_err0(tc, "token header must define TOK_EOF = 0");
            return -1;
        }
        if (i == 1 && (!tc->entries[i].has_value || tc->entries[i].value != 1)) {
            buf_tc_err0(tc, "token header must define TOK_ERROR = 1");
            return -1;
        }
        if (i >= 2 && tc->entries[i].has_value) {
            buf_tc_err_s(tc,
                         "token header pins '%s' to an explicit value; "
                         "%%tokens order must be the only authority",
                         exp);
            return -1;
        }
    }
    if (tc->count > want) {
        buf_tc_err_s(tc, "token header has an extra entry '%s' not in %%tokens",
                     tc->entries[want].name);
        return -1;
    }
    return 0;
}

/* --- public entry points ---------------------------------------- */

static void buf_tc_init(BufTc *tc) {
    tc->count     = 0;
    tc->src_len   = 0;
    tc->path      = "<tokens.h>";
    tc->error[0]  = '\0';
    tc->has_error = 0;
}

static int buf_tc_load_string(BufTc *tc, const char *path, const char *text) {
    int n = 0;
    while (text[n]) n++;
    buf_tc_init(tc);
    tc->path = path ? path : "<tokens.h>";
    if (n >= BUF_TC_SRC_MAX) {
        buf_tc_err0(tc, "token header text too large");
        return -1;
    }
    memcpy(tc->src, text, n + 1);
    tc->src_len = n;
    return 0;
}

static int buf_tc_read_file(BufTc *tc, const char *path) {
    FILE *f;
    long  n;
    buf_tc_init(tc);
    tc->path = path;
    f = fopen(path, "rb");
    if (!f) {
        buf_tc_err_s(tc, "cannot open token header '%s'", path);
        return -1;
    }
    n = (long)fread(tc->src, 1, BUF_TC_SRC_MAX - 1, f);
    fclose(f);
    if (n >= BUF_TC_SRC_MAX - 1) {
        buf_tc_err0(tc, "token header is too large");
        return -1;
    }
    tc->src[n]  = '\0';
    tc->src_len = (int)n;
    return 0;
}

/* Scan + compare in one call. Returns 0 on agreement, -1 with tc->error
 * set on any drift or malformed input. */
static int buf_tc_check(BufTc *tc, BufRx *rx) {
    if (tc->has_error) return -1;
    if (buf_tc_scan(tc) != 0) return -1;
    return buf_tc_compare(tc, rx);
}

#ifdef __cplusplus
}
#endif

#endif /* BUF_TOKCHECK_H */
