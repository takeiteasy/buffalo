/*
 * buf_rt.h -- buffalo lexer runtime, public surface.
 *
 * Ordinary, dependency-free C compiled by the system `cc`. Zero cccc
 * dependency: this header and buf_rt.c link into the final program as-is,
 * alongside a generated (or, at M0, hand-written) DFA-table file.
 *
 * Included into a generated table file with `#include @shared` so a cccc
 * `Quote()` template can name `buf_run` and the table symbols; a plain
 * `#include`'d extern is rejected by Quote's identifier resolver.
 */
#ifndef BUF_RT_H
#define BUF_RT_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Reserved token kinds. Every checked-in <name>_tokens.h must open with
 * `TOK_EOF = 0, TOK_ERROR = 1`; the comptime pass validates that (M1).
 */
enum {
    BUF_TOK_EOF   = 0,
    BUF_TOK_ERROR = 1
};

typedef struct {
    int         kind;    /* TOK_* constant from the checked-in <name>_tokens.h */
    const char *lexeme;  /* into the source buffer; not NUL-terminated */
    int         length;
    int         line;    /* 1-based */
    int         col;     /* 1-based, byte column */
} BufToken;

typedef struct {         /* concrete: the caller stack-allocates `BufLexer lx;` */
    const char *src;
    int         len;
    int         pos;
    int         line;
    int         col;
} BufLexer;

void buf_lexer_init(BufLexer *lx, const char *src, int len);

/*
 * Provided by the generated table file (examples/digits_tables.c at M0):
 * a one-line wrapper that forwards to buf_run with that lexer's tables.
 * `buf_next` is a fixed global name, so one generated lexer per program.
 */
BufToken buf_next(BufLexer *lx);

/*
 * Generic longest-match DFA driver. Lives in buf_rt.c and is never emitted,
 * which keeps `while`/`break`/`continue` out of the cccc emitter.
 *
 *   cls        byte -> equivalence class, total over 0..255, values 0..nclass-1
 *   next       flat [nstates * nclass]; -1 == dead state
 *   accept     [nstates]; rule index, or -1 if the state is non-accepting
 *   rule_token [nrules]; rule index -> TOK_* kind, or -1 for a %skip rule
 *   start      the DFA start state
 *
 * Table contract, relied on without a runtime guard:
 *   - `cls` is total over all 256 byte values, every entry in 0..nclass-1.
 *   - `accept[start] < 0` -- the start state is non-accepting. Equivalently:
 *     no rule matches the empty string. A zero-length match would make a
 *     %skip restart (below) spin in place and would emit zero-length named
 *     tokens. The generator enforces this by rejecting a nullable rule when
 *     it reads the spec.
 *
 * Longest match wins; on a length tie the lowest rule index wins (baked into
 * `accept` at construction time). A matched %skip rule is consumed and the
 * scan restarts. No rule matching at the current byte yields a TOK_ERROR
 * token spanning that one byte and advances one byte. End of input yields
 * TOK_EOF forever.
 */
BufToken buf_run(BufLexer *lx,
                 const unsigned char *cls,
                 const short *next,
                 const short *accept,
                 const short *rule_token,
                 int nstates,
                 int nclass,
                 int start);

#ifdef __cplusplus
}
#endif

#endif /* BUF_RT_H */
