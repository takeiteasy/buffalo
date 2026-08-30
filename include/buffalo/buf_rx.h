/*
 * buf_rx.h -- buffalo .bflo spec + regex reader.
 *
 * Pure C, header-only, fixed arenas, no malloc. Compiled twice: plain `cc`
 * for the host unit tests (tests/t_rx.c), and inside cccc's comptime VM via
 * `#include @comptime` from src/buf_comptime.c. It never calls MacroErrorAt
 * (it must also build under a plain `cc`); errors accumulate first-wins in a
 * sticky string, and src/buf_comptime.c -- the boundary -- turns that into a
 * comptime diagnostic.
 *
 * `fopen`/`fread`/`fclose` are used directly and are confirmed to work
 * inside the comptime VM once the including file routes <stdio.h> with
 * `#include @comptime`.
 *
 * Input: a .bflo spec (see docs/lex-spec-format.md) --
 *
 *     %tokens NAME...            (required; fixes the enum order)
 *     NAME   <regex>             (a named rule; kind is TOK_<NAME>)
 *     %skip  <regex>             (matched, produces no token)
 *     # comment                  (whole-line; blank lines ignored)
 *
 * Output: for each rule, a regex AST rooted at a node index, with line:col
 * tracked per node and pointed back into the .bflo file for diagnostics. The
 * ordered %tokens list is exposed for buf_tokcheck.h.
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
 */
#ifndef BUF_RX_H
#define BUF_RX_H

#include <stdio.h>
#include <string.h>

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

typedef enum {
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

typedef struct {
    char tokens[BUF_RX_MAX_TOKENS][BUF_RX_NAME_MAX];
    int  token_line[BUF_RX_MAX_TOKENS];
    int  token_has_rule[BUF_RX_MAX_TOKENS];
    int  token_count;

    BufRxNode nodes[BUF_RX_MAX_NODES];
    int       node_count;

    BufRule   rules[BUF_RX_MAX_RULES];
    int       rule_count;

    char error[BUF_RX_ERR_MAX];
    int  has_error;

    char        spec[BUF_RX_SPEC_MAX];
    int         spec_len;
    const char *spec_path;      /* label for diagnostics             */
} BufRx;

/* --- small character predicates (no <ctype.h> in comptime) ------------- */

static int buf_rx_is_digit(int c) { return c >= '0' && c <= '9'; }
static int buf_rx_is_alpha(int c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}
static int buf_rx_is_name_start(int c) { return buf_rx_is_alpha(c) || c == '_'; }
static int buf_rx_is_name(int c) {
    return buf_rx_is_alpha(c) || buf_rx_is_digit(c) || c == '_';
}
static int buf_rx_is_space(int c) {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' ||
           c == '\v';
}

/* n-byte prefix compare -- comptime libc is limited to the calls ccccl
 * exercises (strcmp/strncpy/memcpy/snprintf), so strncmp/strlen are
 * open-coded here. */
static int buf_rx_prefix(const char *s, const char *lit, int n) {
    int i;
    for (i = 0; i < n; i++)
        if (s[i] != lit[i]) return 0;
    return 1;
}
static int buf_rx_strlen(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* --- bitset helpers over a 256-bit (32-byte) byte set ----------------- */

static void buf_rx_bits_zero(unsigned char *b) {
    int i;
    for (i = 0; i < 32; i++) b[i] = 0;
}
static void buf_rx_bits_set(unsigned char *b, int c) {
    b[(c >> 3) & 31] = (unsigned char)(b[(c >> 3) & 31] | (1u << (c & 7)));
}
/* Test whether byte c is in a CLASS node's set. Part of the header's
 * surface for consumers that inspect the AST (buf_nfa.h, the tests); kept
 * `static inline` so a table-only TU that never calls it does not trip
 * -Wunused-function. */
static inline int buf_rx_bits_get(const unsigned char *b, int c) {
    return (b[(c >> 3) & 31] >> (c & 7)) & 1;
}
static void buf_rx_bits_or(unsigned char *dst, const unsigned char *src) {
    int i;
    for (i = 0; i < 32; i++) dst[i] = (unsigned char)(dst[i] | src[i]);
}
static void buf_rx_bits_negate(unsigned char *b) {
    int i;
    for (i = 0; i < 32; i++) b[i] = (unsigned char)~b[i];
}

/* Fill `bits` for a \d \w \s shorthand (or its \D \W \S complement). */
static void buf_rx_shorthand(unsigned char *bits, int e) {
    int lo = (e >= 'A' && e <= 'Z') ? e - 'A' + 'a' : e;
    int k;
    buf_rx_bits_zero(bits);
    if (lo == 'd') {
        for (k = '0'; k <= '9'; k++) buf_rx_bits_set(bits, k);
    } else if (lo == 'w') {
        for (k = '0'; k <= '9'; k++) buf_rx_bits_set(bits, k);
        for (k = 'a'; k <= 'z'; k++) buf_rx_bits_set(bits, k);
        for (k = 'A'; k <= 'Z'; k++) buf_rx_bits_set(bits, k);
        buf_rx_bits_set(bits, '_');
    } else if (lo == 's') {
        buf_rx_bits_set(bits, ' ');
        buf_rx_bits_set(bits, '\t');
        buf_rx_bits_set(bits, '\n');
        buf_rx_bits_set(bits, '\r');
        buf_rx_bits_set(bits, '\f');
        buf_rx_bits_set(bits, '\v');
    }
    if (e >= 'A' && e <= 'Z') buf_rx_bits_negate(bits);
}

/* --- sticky first-wins diagnostics ------------------------------------ */

static void buf_rx_err0(BufRx *rx, int line, int col, const char *msg) {
    if (rx->has_error) return;
    snprintf(rx->error, sizeof(rx->error), "%s:%d:%d: %s",
             rx->spec_path, line, col, msg);
    rx->has_error = 1;
}
static void buf_rx_err_s(BufRx *rx, int line, int col, const char *fmt,
                         const char *arg) {
    char tmp[160];
    if (rx->has_error) return;
    snprintf(tmp, sizeof(tmp), fmt, arg);
    buf_rx_err0(rx, line, col, tmp);
}
static void buf_rx_err_c(BufRx *rx, int line, int col, const char *fmt, int ch) {
    char tmp[160];
    char one[2];
    if (rx->has_error) return;
    one[0] = (char)ch;
    one[1] = '\0';
    snprintf(tmp, sizeof(tmp), fmt, one);
    buf_rx_err0(rx, line, col, tmp);
}

/* --- node arena ------------------------------------------------------- */

static int buf_rx_node(BufRx *rx, BufRxKind k, int line, int col) {
    int i;
    if (rx->node_count >= BUF_RX_MAX_NODES) {
        buf_rx_err0(rx, line, col, "regex too complex (node arena exhausted)");
        return 0;
    }
    i = rx->node_count++;
    rx->nodes[i].kind = k;
    rx->nodes[i].line = line;
    rx->nodes[i].col  = col;
    rx->nodes[i].a    = -1;
    rx->nodes[i].b    = -1;
    buf_rx_bits_zero(rx->nodes[i].bits);
    return i;
}

/* --- regex sub-parser ----------------------------------------------- */

typedef struct {
    BufRx      *rx;
    const char *p;      /* cursor into the regex slice                 */
    const char *end;
    int         line;   /* 1-based, into the .bflo file (constant here)   */
    int         col;    /* 1-based, advances as bytes are consumed     */
} BufRxP;

static int buf_rxp_peek(BufRxP *P) {
    return P->p < P->end ? (unsigned char)*P->p : -1;
}
static int buf_rxp_peek2(BufRxP *P) {
    return (P->p + 1) < P->end ? (unsigned char)P->p[1] : -1;
}
static int buf_rxp_next(BufRxP *P) {
    int c;
    if (P->p >= P->end) return -1;
    c = (unsigned char)*P->p++;
    P->col += 1;
    return c;
}
static void buf_rxp_skip_ws(BufRxP *P) {
    while (P->p < P->end && (*P->p == ' ' || *P->p == '\t'))
        buf_rxp_next(P);
}

/* n t r f v 0 and the metacharacter escapes -> the byte; else -1. */
static int buf_rx_simple_escape(int e) {
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case 'f': return '\f';
    case 'v': return '\v';
    case '0': return '\0';
    case '\\': case '.': case '*': case '+': case '?':
    case '(': case ')': case '[': case ']': case '|':
    case '"': case '-':
        return e;
    default: return -1;
    }
}

static int buf_rxp_alt(BufRxP *P);

/* [...] -- consumes through the closing ']'. */
static int buf_rxp_class(BufRxP *P) {
    int line = P->line, col = P->col;
    int neg = 0, first = 1, n;
    unsigned char *bits;

    buf_rxp_next(P); /* '[' */
    if (buf_rxp_peek(P) == '^') { neg = 1; buf_rxp_next(P); }

    n = buf_rx_node(P->rx, BUF_RX_CLASS, line, col);
    if (P->rx->has_error) return 0;
    bits = P->rx->nodes[n].bits;

    for (;;) {
        int c = buf_rxp_peek(P);
        int lo;
        if (c < 0) {
            buf_rx_err0(P->rx, line, col, "unterminated character class");
            return 0;
        }
        if (c == ']' && !first) { buf_rxp_next(P); break; }
        first = 0;

        if (c == '\\') {
            int e;
            buf_rxp_next(P);
            e = buf_rxp_peek(P);
            if (e < 0) {
                buf_rx_err0(P->rx, P->line, P->col,
                            "unterminated escape in character class");
                return 0;
            }
            buf_rxp_next(P);
            if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' ||
                e == 'S') {
                unsigned char sh[32];
                buf_rx_shorthand(sh, e);
                buf_rx_bits_or(bits, sh);
                continue; /* a shorthand is never a range endpoint */
            }
            lo = buf_rx_simple_escape(e);
            if (lo < 0) {
                buf_rx_err_c(P->rx, P->line, P->col - 1,
                             "unknown escape '\\%s' in character class", e);
                return 0;
            }
        } else {
            lo = c;
            buf_rxp_next(P);
        }

        /* a range lo-hi, unless the '-' is the last char before ']' */
        if (buf_rxp_peek(P) == '-' && buf_rxp_peek2(P) != ']' &&
            buf_rxp_peek2(P) >= 0) {
            int hc, hi, k;
            buf_rxp_next(P); /* '-' */
            hc = buf_rxp_peek(P);
            if (hc == '\\') {
                int he;
                buf_rxp_next(P);
                he = buf_rxp_peek(P);
                if (he < 0) {
                    buf_rx_err0(P->rx, P->line, P->col,
                                "unterminated escape in character class");
                    return 0;
                }
                buf_rxp_next(P);
                hi = buf_rx_simple_escape(he);
                if (hi < 0) {
                    buf_rx_err_c(P->rx, P->line, P->col - 1,
                                 "unknown escape '\\%s' in character class", he);
                    return 0;
                }
            } else {
                hi = hc;
                buf_rxp_next(P);
            }
            if (hi < lo) {
                buf_rx_err0(P->rx, P->line, P->col,
                            "reversed range in character class");
                return 0;
            }
            for (k = lo; k <= hi; k++) buf_rx_bits_set(bits, k);
            continue;
        }

        buf_rx_bits_set(bits, lo);
    }

    if (neg) buf_rx_bits_negate(P->rx->nodes[n].bits);
    return n;
}

/* "..." -- a CONCAT chain of single-byte CLASS leaves; \" and \\ escape. */
static int buf_rxp_string(BufRxP *P) {
    int line = P->line, col = P->col;
    int root = -1;

    buf_rxp_next(P); /* opening '"' */
    for (;;) {
        int c = buf_rxp_peek(P);
        int leaf;
        if (c < 0) {
            buf_rx_err0(P->rx, line, col, "unterminated string literal");
            return 0;
        }
        if (c == '"') { buf_rxp_next(P); break; }
        if (c == '\\') {
            int e;
            buf_rxp_next(P);
            e = buf_rxp_peek(P);
            if (e < 0) {
                buf_rx_err0(P->rx, line, col, "unterminated string literal");
                return 0;
            }
            if (e == '"' || e == '\\') {
                c = e;
                buf_rxp_next(P);
            } else {
                buf_rx_err_c(P->rx, P->line, P->col - 1,
                             "invalid string escape '\\%s' (only \\\" and \\\\)",
                             e);
                return 0;
            }
        } else {
            buf_rxp_next(P);
        }
        leaf = buf_rx_node(P->rx, BUF_RX_CLASS, line, col);
        if (P->rx->has_error) return 0;
        buf_rx_bits_set(P->rx->nodes[leaf].bits, c);
        if (root < 0) {
            root = leaf;
        } else {
            int cc = buf_rx_node(P->rx, BUF_RX_CONCAT, line, col);
            if (P->rx->has_error) return 0;
            P->rx->nodes[cc].a = root;
            P->rx->nodes[cc].b = leaf;
            root = cc;
        }
    }
    if (root < 0) {
        buf_rx_err0(P->rx, line, col, "empty string literal");
        return 0;
    }
    return root;
}

/* bare \X -- a shorthand class or a single escaped byte. */
static int buf_rxp_escape_atom(BufRxP *P) {
    int line = P->line, col = P->col;
    int e, n, ch;

    buf_rxp_next(P); /* '\\' */
    e = buf_rxp_peek(P);
    if (e < 0) {
        buf_rx_err0(P->rx, line, col, "trailing backslash in regex");
        return 0;
    }
    buf_rxp_next(P);

    n = buf_rx_node(P->rx, BUF_RX_CLASS, line, col);
    if (P->rx->has_error) return 0;

    if (e == 'd' || e == 'D' || e == 'w' || e == 'W' || e == 's' || e == 'S') {
        buf_rx_shorthand(P->rx->nodes[n].bits, e);
        return n;
    }
    ch = buf_rx_simple_escape(e);
    if (ch < 0) {
        buf_rx_err_c(P->rx, line, col, "unknown escape '\\%s'", e);
        return 0;
    }
    buf_rx_bits_set(P->rx->nodes[n].bits, ch);
    return n;
}

static int buf_rxp_atom(BufRxP *P) {
    int c, line, col, n;

    buf_rxp_skip_ws(P);
    c    = buf_rxp_peek(P);
    line = P->line;
    col  = P->col;

    if (c < 0) {
        buf_rx_err0(P->rx, line, col, "unexpected end of regex");
        return 0;
    }
    if (c == '(') {
        int inner;
        buf_rxp_next(P);
        inner = buf_rxp_alt(P);
        buf_rxp_skip_ws(P);
        if (buf_rxp_peek(P) != ')') {
            buf_rx_err0(P->rx, line, col, "unterminated '(' group");
            return 0;
        }
        buf_rxp_next(P);
        return inner;
    }
    if (c == '[') return buf_rxp_class(P);
    if (c == '"') return buf_rxp_string(P);
    if (c == '\\') return buf_rxp_escape_atom(P);
    if (c == ')' || c == '|') {
        buf_rx_err0(P->rx, line, col, "empty subexpression");
        return 0;
    }
    if (c == '*' || c == '+' || c == '?') {
        buf_rx_err_c(P->rx, line, col, "nothing to repeat before '%s'", c);
        return 0;
    }
    if (c == '{') {
        buf_rx_err0(P->rx, line, col,
                    "{n,m} repetition counts are not supported in v1");
        return 0;
    }
    if (c == '.') {
        unsigned char *db;
        buf_rxp_next(P);
        n = buf_rx_node(P->rx, BUF_RX_CLASS, line, col);
        if (P->rx->has_error) return 0;
        db = P->rx->nodes[n].bits;
        buf_rx_bits_negate(db);                    /* every byte 0x00..0xFF */
        db[('\n' >> 3) & 31] =
            (unsigned char)(db[('\n' >> 3) & 31] & ~(1u << ('\n' & 7))); /* -\n */
        return n;
    }

    /* a plain literal byte */
    buf_rxp_next(P);
    n = buf_rx_node(P->rx, BUF_RX_CLASS, line, col);
    if (P->rx->has_error) return 0;
    buf_rx_bits_set(P->rx->nodes[n].bits, c);
    return n;
}

static int buf_rxp_postfix(BufRxP *P) {
    int a = buf_rxp_atom(P);
    if (P->rx->has_error) return 0;
    for (;;) {
        int c = buf_rxp_peek(P); /* tight: no ws skip before a postfix op */
        int line, col, k, n;
        if (c != '*' && c != '+' && c != '?') break;
        line = P->line;
        col  = P->col;
        buf_rxp_next(P);
        k = (c == '*') ? BUF_RX_STAR : (c == '+') ? BUF_RX_PLUS : BUF_RX_OPT;
        n = buf_rx_node(P->rx, k, line, col);
        if (P->rx->has_error) return 0;
        P->rx->nodes[n].a = a;
        a = n;
    }
    return a;
}

static int buf_rxp_concat(BufRxP *P) {
    int a;

    buf_rxp_skip_ws(P);
    {
        int c = buf_rxp_peek(P);
        if (c < 0 || c == '|' || c == ')') {
            buf_rx_err0(P->rx, P->line, P->col, "empty subexpression");
            return 0;
        }
    }
    a = buf_rxp_postfix(P);
    if (P->rx->has_error) return 0;

    for (;;) {
        int c, b, n;
        buf_rxp_skip_ws(P);
        c = buf_rxp_peek(P);
        if (c < 0 || c == '|' || c == ')') break;
        b = buf_rxp_postfix(P);
        if (P->rx->has_error) return 0;
        n = buf_rx_node(P->rx, BUF_RX_CONCAT, P->rx->nodes[a].line,
                        P->rx->nodes[a].col);
        if (P->rx->has_error) return 0;
        P->rx->nodes[n].a = a;
        P->rx->nodes[n].b = b;
        a = n;
    }
    return a;
}

static int buf_rxp_alt(BufRxP *P) {
    int a = buf_rxp_concat(P);
    if (P->rx->has_error) return 0;
    for (;;) {
        int b, n, line, col;
        buf_rxp_skip_ws(P);
        if (buf_rxp_peek(P) != '|') break;
        line = P->line;
        col  = P->col;
        buf_rxp_next(P);
        b = buf_rxp_concat(P);
        if (P->rx->has_error) return 0;
        n = buf_rx_node(P->rx, BUF_RX_ALT, line, col);
        if (P->rx->has_error) return 0;
        P->rx->nodes[n].a = a;
        P->rx->nodes[n].b = b;
        a = n;
    }
    return a;
}

/* Parse one regex slice [text, text+len) whose first byte sits at
 * (file_line, file_col) in the .bflo file. Returns the AST root node index. */
static int buf_rx_parse_regex(BufRx *rx, const char *text, int len,
                              int file_line, int file_col) {
    BufRxP P;
    int    root;

    P.rx   = rx;
    P.p    = text;
    P.end  = text + len;
    P.line = file_line;
    P.col  = file_col;

    root = buf_rxp_alt(&P);
    if (rx->has_error) return 0;

    buf_rxp_skip_ws(&P);
    if (buf_rxp_peek(&P) >= 0) {
        buf_rx_err_c(rx, P.line, P.col, "unexpected '%s' in regex",
                     buf_rxp_peek(&P));
        return 0;
    }
    return root;
}

/* --- nullability (empty-match rejection) ---------------------------- */

static int buf_rx_nullable(BufRx *rx, int n) {
    BufRxNode *nd = &rx->nodes[n];
    switch (nd->kind) {
    case BUF_RX_CLASS:  return 0;
    case BUF_RX_STAR:
    case BUF_RX_OPT:    return 1;
    case BUF_RX_PLUS:   return buf_rx_nullable(rx, nd->a);
    case BUF_RX_CONCAT: return buf_rx_nullable(rx, nd->a) &&
                               buf_rx_nullable(rx, nd->b);
    case BUF_RX_ALT:    return buf_rx_nullable(rx, nd->a) ||
                               buf_rx_nullable(rx, nd->b);
    }
    return 0;
}

/* --- token list ---------------------------------------------------- */

static int buf_rx_token_index(BufRx *rx, const char *name) {
    int i;
    for (i = 0; i < rx->token_count; i++)
        if (strcmp(rx->tokens[i], name) == 0) return i;
    return -1;
}

/* --- spec-file (line) layer -------------------------------------- */

static void buf_rx_init(BufRx *rx) {
    rx->token_count = 0;
    rx->rule_count  = 0;
    rx->error[0]    = '\0';
    rx->has_error   = 0;
    rx->spec_len    = 0;
    rx->spec_path   = "<spec>";
    /* node 0 is a reserved sentinel: buf_rx_node returns 0 on arena
     * exhaustion, and has_error is set alongside, so callers bail before
     * dereferencing it. */
    rx->node_count  = 0;
    buf_rx_node(rx, BUF_RX_CLASS, 0, 0);
}

/* Add one %tokens NAME, with validation. */
static void buf_rx_add_token(BufRx *rx, const char *name, int line, int col) {
    if (rx->has_error) return;
    if (strcmp(name, "EOF") == 0 || strcmp(name, "ERROR") == 0) {
        buf_rx_err_s(rx, line, col,
                     "'%s' is reserved (TOK_EOF/TOK_ERROR) and cannot appear "
                     "in %%tokens", name);
        return;
    }
    if (buf_rx_token_index(rx, name) >= 0) {
        buf_rx_err_s(rx, line, col, "duplicate %%tokens entry '%s'", name);
        return;
    }
    if (rx->token_count >= BUF_RX_MAX_TOKENS) {
        buf_rx_err0(rx, line, col, "too many %tokens entries");
        return;
    }
    strncpy(rx->tokens[rx->token_count], name, BUF_RX_NAME_MAX - 1);
    rx->tokens[rx->token_count][BUF_RX_NAME_MAX - 1] = '\0';
    rx->token_line[rx->token_count]     = line;
    rx->token_has_rule[rx->token_count] = 0;
    rx->token_count++;
}

/* Parse a whitespace-separated NAME list starting at s (col `col0` at s[0]);
 * used for the %tokens directive. */
static void buf_rx_parse_token_list(BufRx *rx, const char *s, int len,
                                    int line, int col0) {
    int i = 0;
    while (i < len) {
        int start, col_start, j;
        char name[BUF_RX_NAME_MAX];
        while (i < len && buf_rx_is_space((unsigned char)s[i])) i++;
        if (i >= len) break;
        start     = i;
        col_start = col0 + i;
        if (!buf_rx_is_name_start((unsigned char)s[i])) {
            buf_rx_err_c(rx, line, col_start,
                         "invalid character '%s' in %%tokens list", s[i]);
            return;
        }
        while (i < len && buf_rx_is_name((unsigned char)s[i])) i++;
        if (i - start >= BUF_RX_NAME_MAX) {
            buf_rx_err0(rx, line, col_start, "%tokens name too long");
            return;
        }
        for (j = 0; j < i - start; j++) name[j] = s[start + j];
        name[i - start] = '\0';
        if (i < len && !buf_rx_is_space((unsigned char)s[i])) {
            buf_rx_err_c(rx, line, col0 + i,
                         "invalid character '%s' in %%tokens list", s[i]);
            return;
        }
        buf_rx_add_token(rx, name, line, col_start);
        if (rx->has_error) return;
    }
}

/* Register a rule (named or %skip) with regex slice [rtext, rtext+rlen). */
static void buf_rx_add_rule(BufRx *rx, const char *name, int is_skip,
                            const char *rtext, int rlen, int line,
                            int rule_col, int rx_col) {
    BufRule *ru;
    int      root;

    if (rx->has_error) return;
    if (rx->rule_count >= BUF_RX_MAX_RULES) {
        buf_rx_err0(rx, line, rule_col, "too many rules");
        return;
    }
    /* trim trailing unquoted whitespace off the slice */
    while (rlen > 0 && (rtext[rlen - 1] == ' ' || rtext[rlen - 1] == '\t'))
        rlen--;
    if (rlen == 0) {
        if (is_skip)
            buf_rx_err0(rx, line, rule_col, "%skip directive has no regex");
        else
            buf_rx_err_s(rx, line, rule_col, "rule '%s' has no regex", name);
        return;
    }

    root = buf_rx_parse_regex(rx, rtext, rlen, line, rx_col);
    if (rx->has_error) return;

    if (buf_rx_nullable(rx, root)) {
        if (is_skip)
            buf_rx_err0(rx, line, rule_col,
                        "%skip rule can match the empty string");
        else
            buf_rx_err_s(rx, line, rule_col,
                         "rule '%s' can match the empty string", name);
        return;
    }

    ru = &rx->rules[rx->rule_count++];
    strncpy(ru->name, is_skip ? "" : name, BUF_RX_NAME_MAX - 1);
    ru->name[BUF_RX_NAME_MAX - 1] = '\0';
    ru->is_skip   = is_skip;
    ru->root      = root;
    ru->line      = line;
    ru->col       = rule_col;
    ru->tok_index = -1;
}

/* Parse rx->spec (already loaded). Fills tokens[], rules[], nodes[]. */
static void buf_rx_parse(BufRx *rx) {
    const char *s   = rx->spec;
    int         len = rx->spec_len;
    int         i   = 0;
    int         line = 0;
    int         k;

    while (i < len && !rx->has_error) {
        int ls = i, le, c0, col0;
        line++;
        while (i < len && s[i] != '\n') i++;
        le = i;
        if (i < len) i++;                 /* step past '\n' */
        if (le > ls && s[le - 1] == '\r') le--;

        /* first non-blank column on the line */
        c0 = ls;
        while (c0 < le && (s[c0] == ' ' || s[c0] == '\t')) c0++;
        col0 = (c0 - ls) + 1;

        if (c0 >= le) continue;           /* blank */
        if (s[c0] == '#') continue;       /* whole-line comment */

        if (s[c0] == '%') {
            /* directive: %tokens or %skip */
            int de = c0 + 1; /* past the '%' */
            while (de < le && buf_rx_is_name((unsigned char)s[de])) de++;
            {
                int dlen = de - c0;
                if (dlen == 7 && buf_rx_prefix(&s[c0], "%tokens", 7)) {
                    int a = de;
                    while (a < le &&
                           buf_rx_is_space((unsigned char)s[a])) a++;
                    buf_rx_parse_token_list(rx, &s[a], le - a, line,
                                            (a - ls) + 1);
                } else if (dlen == 5 && buf_rx_prefix(&s[c0], "%skip", 5)) {
                    int a = de;
                    while (a < le &&
                           buf_rx_is_space((unsigned char)s[a])) a++;
                    buf_rx_add_rule(rx, "", 1, &s[a], le - a, line, col0,
                                    (a - ls) + 1);
                } else {
                    char dir[BUF_RX_NAME_MAX];
                    int  n = dlen < BUF_RX_NAME_MAX - 1 ? dlen
                                                        : BUF_RX_NAME_MAX - 1;
                    memcpy(dir, &s[c0], n);
                    dir[n] = '\0';
                    buf_rx_err_s(rx, line, col0, "unknown directive '%s'", dir);
                }
            }
            continue;
        }

        /* a named rule: NAME <ws> REGEX */
        if (!buf_rx_is_name_start((unsigned char)s[c0])) {
            buf_rx_err_c(rx, line, col0, "unexpected character '%s'", s[c0]);
            continue;
        }
        {
            int  ne = c0;
            char name[BUF_RX_NAME_MAX];
            int  n, a;
            while (ne < le && buf_rx_is_name((unsigned char)s[ne])) ne++;
            if (ne - c0 >= BUF_RX_NAME_MAX) {
                buf_rx_err0(rx, line, col0, "rule name too long");
                continue;
            }
            n = ne - c0;
            memcpy(name, &s[c0], n);
            name[n] = '\0';
            if (ne >= le || !buf_rx_is_space((unsigned char)s[ne])) {
                buf_rx_err_s(rx, line, col0, "rule '%s' has no regex", name);
                continue;
            }
            a = ne;
            while (a < le && buf_rx_is_space((unsigned char)s[a])) a++;
            buf_rx_add_rule(rx, name, 0, &s[a], le - a, line, col0,
                            (a - ls) + 1);
        }
    }
    if (rx->has_error) return;

    /* every named rule must be in %tokens; record coverage the other way */
    for (k = 0; k < rx->rule_count; k++) {
        int ti;
        if (rx->rules[k].is_skip) continue;
        ti = buf_rx_token_index(rx, rx->rules[k].name);
        if (ti < 0) {
            buf_rx_err_s(rx, rx->rules[k].line, rx->rules[k].col,
                         "rule '%s' is not listed in %%tokens",
                         rx->rules[k].name);
            return;
        }
        rx->rules[k].tok_index    = ti;
        rx->token_has_rule[ti]    = 1;
    }

    if (rx->token_count == 0) {
        buf_rx_err0(rx, 1, 1, "the %tokens directive is required");
        return;
    }
    for (k = 0; k < rx->token_count; k++) {
        if (!rx->token_has_rule[k]) {
            buf_rx_err_s(rx, rx->token_line[k], 1,
                         "%%tokens entry '%s' has no matching rule",
                         rx->tokens[k]);
            return;
        }
    }
}

/* --- public entry points ---------------------------------------- */

/* Parse a spec held in memory (NUL-terminated). Returns 0 / -1. */
static int buf_rx_parse_string(BufRx *rx, const char *path, const char *text) {
    int n = buf_rx_strlen(text);
    buf_rx_init(rx);
    rx->spec_path = path ? path : "<memory>";
    if (n >= BUF_RX_SPEC_MAX) {
        buf_rx_err0(rx, 1, 1, "spec text too large");
        return -1;
    }
    memcpy(rx->spec, text, n + 1);
    rx->spec_len = n;
    buf_rx_parse(rx);
    return rx->has_error ? -1 : 0;
}

/* Read and parse a spec file. Returns 0 / -1 (rx->error set on failure). */
static int buf_rx_read_file(BufRx *rx, const char *path) {
    FILE *f;
    long  n;

    buf_rx_init(rx);
    rx->spec_path = path;
    f = fopen(path, "rb");
    if (!f) {
        snprintf(rx->error, sizeof(rx->error), "cannot open spec file '%s'",
                 path);
        rx->has_error = 1;
        return -1;
    }
    n = (long)fread(rx->spec, 1, BUF_RX_SPEC_MAX - 1, f);
    fclose(f);
    if (n >= BUF_RX_SPEC_MAX - 1) {
        snprintf(rx->error, sizeof(rx->error),
                 "spec file '%s' is too large (limit %d bytes)", path,
                 BUF_RX_SPEC_MAX - 1);
        rx->has_error = 1;
        return -1;
    }
    rx->spec[n]  = '\0';
    rx->spec_len = (int)n;
    buf_rx_parse(rx);
    return rx->has_error ? -1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* BUF_RX_H */
