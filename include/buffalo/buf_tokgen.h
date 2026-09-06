/*
 * buf_tokgen.h -- render a spec's %tokens list as a <name>_tokens.h header.
 *
 * Pure C, .h/.c pair, no malloc. Host-only: unlike buf_tokcheck this module
 * never runs inside cccc's comptime VM (it produces text, not tables), so it
 * is not pulled into src/buf_comptime.c -- src/buf_tokgen.c links into the
 * buf_tokgen tool (tools/buf_tokgen_main.c) and the t_tokgen host test under
 * a plain `cc`, next to buf_rx.c.
 *
 * `buffalo tokens SPEC.bflo` uses this to emit the token header the comptime
 * pass validates, for callers who would rather derive the header from the
 * spec than hand-maintain it. Opt-in: the checked-in, buf_tokcheck-validated
 * workflow is unchanged, and buf_tokcheck keeps its full job either way --
 * a generated header is validated exactly like a hand-written one.
 *
 * The output mirrors the hand-written headers' shape: an include guard
 * derived from the spec's base name (examples/calc.bflo -> CALC_TOKENS_H),
 * a provenance comment, then the single enum the checker requires:
 *
 *     enum { TOK_EOF = 0, TOK_ERROR = 1, TOK_<N0>, TOK_<N1>, ... };
 *
 * with TOK_EOF / TOK_ERROR pinned to their reserved values (see BUF_TOK_* in
 * buf_rt.h) and every user kind left uninitialised so the %tokens order
 * stays the only authority.
 */
#ifndef BUF_TOKGEN_H
#define BUF_TOKGEN_H

#include "buf_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUF_TG_OUT_MAX  65536  /* generated text; matches buf_tokcheck.h's
                                * BUF_TC_SRC_MAX, so anything emitted here
                                * also loads there */

typedef struct {
    char out[BUF_TG_OUT_MAX];
    int  out_len;

    char error[BUF_RX_ERR_MAX];
    int  has_error;
} BufTg;

/* Render the header text for rx's %tokens list into tg->out (NUL-terminated;
 * tg->out_len is the text length). 0 ok / -1 with tg->error set. The spec
 * path (rx->spec_path) drives the guard name and the provenance comment;
 * nothing here touches the filesystem. */
int buf_tokgen_emit(BufTg *tg, const BufRx *rx);

#ifdef __cplusplus
}
#endif

#endif /* BUF_TOKGEN_H */
