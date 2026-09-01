/*
 * buf_tokcheck.h -- validate a checked-in <name>_tokens.h against a spec's
 * %tokens list.
 *
 * Pure C, .h/.c pair, fixed arenas, no malloc. Compiled twice: by `cc` for
 * tests/t_tokcheck.c, and inside cccc's comptime VM -- src/buf_comptime.c
 * does `#include @comptime "buf_tokcheck.c"`. Like buf_rx it never calls
 * MacroErrorAt; the sticky first-wins error string is lifted into a comptime
 * diagnostic by src/buf_comptime.c.
 *
 * Token kinds are `enum` constants -- not linkable symbols -- so the header
 * is hand-written and checked in, and this pass guards it against drift from
 * the .bflo spec. The check: the first `enum { ... }` in the header must list,
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

/* --- entry points (defined in src/buf_tokcheck.c) ----------------- */

/* Load a token header from a string / a file into `tc`. 0 ok / -1 with
 * tc->error set. Neither scans; call buf_tc_check next. */
int buf_tc_load_string(BufTc *tc, const char *path, const char *text);
int buf_tc_read_file(BufTc *tc, const char *path);

/* Scan the loaded header and validate it against rx's %tokens list.
 * 0 on agreement, -1 with tc->error on drift or malformed input. */
int buf_tc_check(BufTc *tc, BufRx *rx);

#ifdef __cplusplus
}
#endif

#endif /* BUF_TOKCHECK_H */
