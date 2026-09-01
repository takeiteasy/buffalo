/*
 * buf_rx.h -- buffalo .bflo spec + regex reader.
 *
 * Pure C, .h/.c pair, fixed arenas, no malloc. Compiled twice: by `cc` for
 * the host unit tests (tests/t_rx.c), and inside cccc's comptime VM --
 * src/buf_comptime.c does `#include @comptime "buf_rx.c"`. It never calls
 * MacroErrorAt (it must also build under a plain `cc`); errors accumulate
 * first-wins in a sticky string, and src/buf_comptime.c -- the boundary --
 * turns that into a comptime diagnostic.
 *
 * buf_rx.c uses `fopen`/`fread`/`fclose` directly; they work in the comptime
 * VM because src/buf_comptime.c routes the whole `.c` through `#include
 * @comptime` (and `#include @comptime <stdio.h>` on its own line).
 *
 * Input: a .bflo spec (see docs/bflo-format.md) --
 *
 *     %tokens NAME...            (required; fixes the enum order)
 *     NAME   <regex>             (a named rule; kind is TOK_<NAME>)
 *     %skip  <regex>             (matched, produces no token)
 *     # comment                  (whole-line; blank lines ignored)
 *     %grammar                   (opens an optional grammar section,
 *                                 running to end of file)
 *     %start NAME                (required inside %grammar)
 *     NAME : SYM... | SYM... ;   (a production; terminal/nonterminal is
 *                                 resolved implicitly against %tokens)
 *
 * Output: for each rule, a regex AST rooted at a node index, with line:col
 * tracked per node and pointed back into the .bflo file for diagnostics. The
 * ordered %tokens list is exposed for buf_tokcheck.h. When a %grammar
 * section is present, its productions, resolved symbols and %start
 * nonterminal are exposed the same way -- read and validated here, but not
 * yet consumed by any table construction.
 *
 * Regex v1 grammar: literals, "..." strings, ., [...] classes with ranges
 * and negation, the \n \t \r \f \v \0 and metacharacter escapes, the
 * \d \D \w \W \s \S shorthands (desugared into the byte set), concatenation,
 * |, * + ?, (...) grouping. No {n,m}, ^ $, \b, lookaround, non-greedy or
 * backreferences. Unquoted whitespace inside a regex is insignificant (it
 * separates atoms); a literal space is written " " or [ ].
 *
 * A rule whose regex can match the empty string is rejected at read time
 * with a line:col error -- a nullable %skip rule would make buf_run spin,
 * and a nullable named rule would emit zero-length tokens.
 *
 * Unlike the lexer section, the grammar section is not line-oriented --
 * productions may span lines, so src/buf_rx.c parses it with its own
 * char-stream sub-parser (BufGrP), mirroring the regex sub-parser (BufRxP).
 */
#ifndef BUF_RX_H
#define BUF_RX_H

#ifdef __cplusplus
extern "C" {
#endif

#define BUF_RX_MAX_NODES   4096   /* regex AST nodes; see docs/performance.md
                                  * for the measured arena peaks */
#define BUF_RX_MAX_RULES   256    /* >= BUF_RX_MAX_TOKENS: every %tokens entry
                                  * needs a rule, plus the %skip rules. A full
                                  * C tokenizer is ~100 rules (examples/big.bflo). */
#define BUF_RX_MAX_TOKENS  256
#define BUF_RX_NAME_MAX    64
#define BUF_RX_SPEC_MAX    65536
#define BUF_RX_ERR_MAX     256

#define BUF_RX_MAX_NONTERMS 128   /* grammar nonterminals; see
                                  * docs/performance.md for arena peaks */
#define BUF_RX_MAX_PRODS    512   /* grammar productions (one per alternative) */
#define BUF_RX_MAX_SYMS     2048  /* rhs symbols, pooled across all productions */

/* Tagged, not an anonymous `typedef enum { ... }` -- matches the explicit
 * tags buffalo puts on its other typedefs, and keeps this header safe if the
 * comptime modules ever move from `#include @comptime "buf_rx.c"` to being
 * passed on the cccc command line (cccc mis-dedups an anonymous typedef'd
 * enum reached both `@comptime` and via a forwarded `.c`). */
typedef enum BufRxKind {
    BUF_RX_CLASS,   /* leaf: a set of bytes, held as a 256-bit bitset  */
    BUF_RX_CONCAT,  /* a b   -- children a, b                          */
    BUF_RX_ALT,     /* a|b   -- children a, b                          */
    BUF_RX_STAR,    /* a*    -- child a                                */
    BUF_RX_PLUS,    /* a+    -- child a                                */
    BUF_RX_OPT      /* a?    -- child a                                */
} BufRxKind;

typedef struct {
    BufRxKind     kind;
    int           line, col;   /* 1-based, into the .bflo file           */
    unsigned char bits[32];    /* CLASS only: byte c in set iff bit set */
    int           a, b;        /* child node indices, -1 if unused    */
} BufRxNode;

typedef struct {
    char name[BUF_RX_NAME_MAX]; /* "" for a %skip rule                */
    int  is_skip;
    int  root;                  /* regex AST root node index          */
    int  line, col;             /* of the rule, into the .bflo file      */
    int  tok_index;             /* index into tokens[]; -1 for %skip  */
} BufRule;

/* --- %grammar section: productions over the %tokens vocabulary -------- */

typedef struct {
    int is_terminal; /* 1: `index` is into tokens[]; 0: into nonterms[] */
    int index;
    int line, col;    /* of this symbol's use, into the .bflo file      */
} BufSym;

typedef struct {
    int lhs;              /* nonterms[] index                          */
    int rhs_start, rhs_len; /* slice of syms[]; rhs_len 0 == epsilon   */
    int line, col;         /* of this alternative                     */
} BufProd;

typedef struct {
    char name[BUF_RX_NAME_MAX];
    int  line, col;  /* of the first mention (a use or a definition)  */
    int  defined;    /* has at least one production                  */
} BufNonterm;

typedef struct {
    char tokens[BUF_RX_MAX_TOKENS][BUF_RX_NAME_MAX];
    int  token_line[BUF_RX_MAX_TOKENS];
    int  token_has_rule[BUF_RX_MAX_TOKENS];
    int  token_count;

    BufRxNode nodes[BUF_RX_MAX_NODES];
    int       node_count;

    BufRule   rules[BUF_RX_MAX_RULES];
    int       rule_count;

    int         has_grammar;
    int         grammar_line, grammar_col; /* of the %grammar directive */
    int         has_start;
    int         start_nt;                  /* nonterms[] index          */
    int         start_line, start_col;     /* of the %start symbol name */
    BufNonterm  nonterms[BUF_RX_MAX_NONTERMS];
    int         nonterm_count;
    BufProd     prods[BUF_RX_MAX_PRODS];
    int         prod_count;
    BufSym      syms[BUF_RX_MAX_SYMS];
    int         sym_count;

    char error[BUF_RX_ERR_MAX];
    int  has_error;

    char        spec[BUF_RX_SPEC_MAX];
    int         spec_len;
    const char *spec_path;      /* label for diagnostics             */
} BufRx;

/* --- small character predicates (no <ctype.h> in comptime) -------------
 * Cross-module (buf_tokcheck / buf_grammar use these too); `static inline`
 * so a TU that pulls the header but never calls one does not warn. */

static inline int buf_rx_is_digit(int c) { return c >= '0' && c <= '9'; }
static inline int buf_rx_is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static inline int buf_rx_is_name_start(int c) { return buf_rx_is_alpha(c) || c == '_'; }
static inline int buf_rx_is_name(int c) {
    return buf_rx_is_alpha(c) || buf_rx_is_digit(c) || c == '_';
}
static inline int buf_rx_is_space(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

/* --- bitset helpers over a 256-bit (32-byte) byte set -----------------
 * Cross-module surface (buf_nfa / buf_dfa / buf_grammar inspect CLASS
 * node byte sets); kept `static inline` in the header. */
static inline void buf_rx_bits_zero(unsigned char *b) {
    int i;
    for (i = 0; i < 32; i++) b[i] = 0;
}
static inline void buf_rx_bits_set(unsigned char *b, int c) {
    b[(c >> 3) & 31] = (unsigned char)(b[(c >> 3) & 31] | (1u << (c & 7)));
}
/* Test whether byte c is in a CLASS node's set. Part of the header's
 * surface for consumers that inspect the AST (buf_nfa.h, the tests); kept
 * `static inline` so a table-only TU that never calls it does not trip
 * -Wunused-function. */
static inline int buf_rx_bits_get(const unsigned char *b, int c) {
    return (b[(c >> 3) & 31] >> (c & 7)) & 1;
}
static inline void buf_rx_bits_or(unsigned char *dst, const unsigned char *src) {
    int i;
    for (i = 0; i < 32; i++) dst[i] = (unsigned char)(dst[i] | src[i]);
}

/* --- entry points & lookups (defined in src/buf_rx.c) --------------- */

int buf_rx_parse_string(BufRx *rx, const char *path, const char *text);
int buf_rx_read_file(BufRx *rx, const char *path);

/* %tokens[] / nonterms[] name lookup; -1 if absent. Used by buf_grammar.h
 * and the grammar tests as well as internally. */
int buf_rx_token_index(BufRx *rx, const char *name);
int buf_rx_nt_index(BufRx *rx, const char *name);

#ifdef __cplusplus
}
#endif

#endif /* BUF_RX_H */
