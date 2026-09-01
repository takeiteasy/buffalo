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
    BUF_TOK_EOF        = 0,
    BUF_TOK_ERROR      = 1,
    BUF_TOK_FIRST_USER = 2   /* first %tokens entry lands here; buf_dfa.h
                              * maps rule index -> tok_index + this */
};

/*
 * Both structs carry an explicit tag (`struct BufToken`, `struct BufLexer`)
 * on top of the typedef -- plain good C, and no reason to drop it now that
 * cccc's own anonymous-typedef lowering has been fixed.
 */
typedef struct BufToken {
    int         kind;    /* TOK_* constant from the checked-in <name>_tokens.h */
    const char *lexeme;  /* into the source buffer; not NUL-terminated */
    int         length;
    int         line;    /* 1-based */
    int         col;     /* 1-based, byte column */
} BufToken;

typedef struct BufLexer { /* concrete: the caller stack-allocates `BufLexer lx;` */
    const char *src;
    int         len;
    int         pos;
    int         line;
    int         col;
} BufLexer;

/*
 * Byte -> DFA equivalence class. The emitter names this type for the
 * `buf_dfa_class` table it generates: `buf_rt.h` reaches the comptime pass
 * via `#include @shared`, so `GetType("BufClass")` resolves there. A bare
 * `GetType("unsigned char")` does not (the comptime type resolver has no
 * spelling for a multi-word base type), hence the typedef.
 */
typedef unsigned char BufClass;

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

/*
 * LALR(1) action-table encoding. Must track buf_grammar.h's BUF_LALR_ACT_*
 * macros exactly (cross-checked by a static assert in tests/t_parse.c) --
 * kept as an independent copy here rather than #include-ing the comptime
 * header, the same way BUF_TOK_FIRST_USER / BUF_LALR_FIRST_USER_TOK /
 * BUF_DFA_FIRST_USER_TOK are three independent copies of one invariant.
 */
#define BUF_PARSE_ACT_ERROR      0
#define BUF_PARSE_ACT_ACCEPT     (-1000000)
#define BUF_PARSE_IS_SHIFT(v)    ((v) > 0)
#define BUF_PARSE_IS_REDUCE(v)   ((v) < 0 && (v) != BUF_PARSE_ACT_ACCEPT)
#define BUF_PARSE_SHIFT_STATE(v) ((v) - 1)
#define BUF_PARSE_REDUCE_PROD(v) (-(v) - 1)

/*
 * Concrete syntax tree node, built by buf_parse into a caller-owned flat
 * pool -- children are pool indices (BufParser.child[]), not pointers, so
 * the whole tree lives in arrays the caller sized and owns, no allocation
 * inside buf_rt.c.
 */
typedef struct BufCstNode {
    int      is_terminal; /* 1: `index` is a TOK_* kind; 0: a nonterminal index */
    int      index;       /* leaf: TOK_* kind; interior: prod_lhs[prod] */
    int      prod;        /* interior: production index; -1 on a leaf */
    BufToken token;        /* leaf only; unused on an interior node */
    int      line, col;    /* 1-based. Interior: inherited from the leftmost
                             * child, or (on an epsilon reduce, 0 children)
                             * the position of the current lookahead. */
    int      child_off;    /* interior only: offset into BufParser.child[] */
    int      nchild;       /* interior only; 0 on a leaf or an epsilon reduce */
} BufCstNode;

/* buf_parse outcome. BUF_PARSE_OK on success; anything else means the
 * driver stopped early and BufParser.status/error_tok/error_state say why. */
enum {
    BUF_PARSE_OK             = 0,
    BUF_PARSE_ERR_SYNTAX     = 1, /* action[state][lookahead] had no entry */
    BUF_PARSE_ERR_NODE_POOL  = 2, /* nodes[] exhausted */
    BUF_PARSE_ERR_CHILD_POOL = 3, /* child[] exhausted */
    BUF_PARSE_ERR_STACK      = 4  /* state_stack[]/node_stack[] exhausted */
};

typedef struct BufParser { /* concrete: caller stack-allocates storage and
                             * `BufParser ps;`, then calls buf_parser_init */
    BufCstNode *nodes;        int node_cap,  node_used;
    int        *child;        int child_cap, child_used;
    int        *state_stack;
    int        *node_stack;   int stack_cap;

    int      status;      /* BUF_PARSE_OK on success */
    BufToken error_tok;    /* offending lookahead, when status == ERR_SYNTAX */
    int      error_state;  /* LR state with no action, when status == ERR_SYNTAX */
} BufParser;

void buf_parser_init(BufParser *ps,
                     BufCstNode *nodes, int node_cap,
                     int *child, int child_cap,
                     int *state_stack, int *node_stack, int stack_cap);

/*
 * Generic LALR(1) shift/reduce driver, building a concrete syntax tree.
 * Lives in buf_rt.c and is never emitted, same as buf_run. Drives buf_run
 * itself for each lookahead token, over the DFA tables (cls..start_dfa,
 * same contract as buf_run) and the LALR(1) tables buf_grammar.h builds:
 *
 *   action      [nstates * ntok], BUF_LALR_ACT_*-encoded (buf_grammar.h).
 *               `int`, not `short` -- BUF_LALR_ACT_ACCEPT doesn't fit short.
 *   goto_tab    [nstates * nnonterm]; next state on a nonterminal, or -1.
 *   prod_lhs    [nprods] -> nonterminal index, i.e. buf_lalr_plhs(rx, p)
 *               baked flat by the caller (buf_parse cannot take a BufRx*:
 *               it's a comptime-only fixed arena, not a runtime type).
 *   prod_len    [nprods] -> RHS length, i.e. buf_lalr_plen(rx, p), likewise
 *               baked flat by the caller.
 *   ntok, nnonterm, start_state as in BufGrammar.
 *
 * Returns the root node's index into ps->nodes on success, or -1 on failure
 * (ps->status says why). A BUF_TOK_ERROR lookahead from buf_run has no
 * action[] entry and is reported like any other unexpected token --
 * ps->error_tok.kind == BUF_TOK_ERROR distinguishes it from a rejected but
 * otherwise well-formed token.
 */
int buf_parse(BufParser *ps, BufLexer *lx,
             const unsigned char *cls,
             const short *next,
             const short *accept,
             const short *rule_token,
             int nstates_dfa,
             int nclass,
             int start_dfa,
             const int *action,
             const int *goto_tab,
             const int *prod_lhs,
             const int *prod_len,
             int ntok,
             int nnonterm,
             int start_state);

/*
 * Parser-side counterpart to buf_next: a one-line wrapper that buf_emit.h
 * emits into a `buffalo parse` .gen.c, forwarding to buf_parse with this
 * spec's baked DFA + LALR(1) tables and scalars. Named by a Quote() template,
 * so the generated file must reach this header via `#include @shared` for the
 * same reason buf_next does. Returns the CST root's index into ps->nodes, or
 * -1 (ps->status says why). The caller still owns and sizes the node/child/
 * stack arrays and calls buf_parser_init first, exactly as for buf_parse.
 */
int buf_parse_tree(BufParser *ps, BufLexer *lx);

#ifdef __cplusplus
}
#endif

#endif /* BUF_RT_H */
