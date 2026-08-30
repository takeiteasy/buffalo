/*
 * buf_rt.c -- buffalo lexer runtime driver.
 *
 * Ordinary C, system `cc`, zero cccc dependency. `buf_run` is the entire
 * generic longest-match loop; the generated table file contributes only
 * `static const` data plus a one-line `buf_next` wrapper.
 */
#include "buf_rt.h"

void buf_lexer_init(BufLexer *lx, const char *src, int len)
{
    lx->src  = src;
    lx->len  = len;
    lx->pos  = 0;
    lx->line = 1;
    lx->col  = 1;
}

/* Advance one byte, maintaining 1-based line:col. '\n' bumps the line. */
static void buf_advance(BufLexer *lx)
{
    if (lx->src[lx->pos] == '\n') {
        lx->line += 1;
        lx->col   = 1;
    } else {
        lx->col += 1;
    }
    lx->pos += 1;
}

static BufToken buf_make(int kind, const char *lexeme, int length, int line, int col)
{
    BufToken t;
    t.kind   = kind;
    t.lexeme = lexeme;
    t.length = length;
    t.line   = line;
    t.col    = col;
    return t;
}

BufToken buf_run(BufLexer *lx,
                 const unsigned char *cls,
                 const short *next,
                 const short *accept,
                 const short *rule_token,
                 int nstates,
                 int nclass,
                 int start)
{
    (void)nstates; /* bounds are enforced by the table contents, not checked here */

    for (;;) {
        int tok_line, tok_col, tok_pos;
        int state;
        int last_accept_rule; /* winning rule index at the last accepting state */
        int last_accept_pos;  /* input position just past that match */
        int scan;

        if (lx->pos >= lx->len)
            return buf_make(BUF_TOK_EOF, lx->src + lx->pos, 0, lx->line, lx->col);

        tok_line = lx->line;
        tok_col  = lx->col;
        tok_pos  = lx->pos;

        state            = start;
        last_accept_rule = -1;
        last_accept_pos  = tok_pos;
        scan             = tok_pos;

        if (accept[state] >= 0) {
            last_accept_rule = accept[state];
            last_accept_pos  = scan;
        }

        while (scan < lx->len) {
            unsigned char c   = (unsigned char)lx->src[scan];
            short         nxt = next[state * nclass + cls[c]];
            if (nxt < 0)
                break;
            state = nxt;
            scan += 1;
            if (accept[state] >= 0) {
                last_accept_rule = accept[state];
                last_accept_pos  = scan;
            }
        }

        if (last_accept_rule < 0) {
            /* No rule matched: emit TOK_ERROR over the single offending byte. */
            BufToken err = buf_make(BUF_TOK_ERROR, lx->src + tok_pos, 1, tok_line, tok_col);
            buf_advance(lx);
            return err;
        }

        /* Roll forward over the matched lexeme, updating line:col. */
        while (lx->pos < last_accept_pos)
            buf_advance(lx);

        {
            int kind = rule_token[last_accept_rule];
            if (kind < 0) {
                /* %skip rule: consume and rescan. Safe against an infinite
                 * loop because the table contract (buf_rt.h) guarantees
                 * `accept[start] < 0`, so every match spans >= 1 byte and
                 * this restart always advances. */
                continue;
            }
            return buf_make(kind, lx->src + tok_pos, last_accept_pos - tok_pos,
                            tok_line, tok_col);
        }
    }

    /* Unreachable: every exit from the loop above is a return. cccc's
     * -c=native flow analysis does not prove that and rejects the function
     * for reaching the end of a non-void aggregate return; plain `cc`
     * accepts either form. Kept as a harmless tail return so the one-shot
     * `make native` path builds. */
    return buf_make(BUF_TOK_EOF, lx->src + lx->pos, 0, lx->line, lx->col);
}

void buf_parser_init(BufParser *ps,
                     BufCstNode *nodes, int node_cap,
                     int *child, int child_cap,
                     int *state_stack, int *node_stack, int stack_cap)
{
    ps->nodes       = nodes;
    ps->node_cap    = node_cap;
    ps->node_used   = 0;
    ps->child       = child;
    ps->child_cap   = child_cap;
    ps->child_used  = 0;
    ps->state_stack = state_stack;
    ps->node_stack  = node_stack;
    ps->stack_cap   = stack_cap;
    ps->status      = BUF_PARSE_OK;
}

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
             int start_state)
{
    int sp;
    BufToken tk;

    if (ps->stack_cap < 1) {
        ps->status = BUF_PARSE_ERR_STACK;
        return -1;
    }
    ps->state_stack[0] = start_state;
    ps->node_stack[0]  = -1;
    sp = 1;

    tk = buf_run(lx, cls, next, accept, rule_token, nstates_dfa, nclass, start_dfa);

    for (;;) {
        int state = ps->state_stack[sp - 1];
        int v     = action[state * ntok + tk.kind];

        if (v == BUF_PARSE_ACT_ERROR) {
            ps->status      = BUF_PARSE_ERR_SYNTAX;
            ps->error_tok   = tk;
            ps->error_state = state;
            return -1;
        }

        if (v == BUF_PARSE_ACT_ACCEPT) {
            ps->status = BUF_PARSE_OK;
            return ps->node_stack[sp - 1];
        }

        if (BUF_PARSE_IS_SHIFT(v)) {
            int leaf;

            if (ps->node_used >= ps->node_cap) {
                ps->status = BUF_PARSE_ERR_NODE_POOL;
                return -1;
            }
            leaf = ps->node_used++;
            ps->nodes[leaf].is_terminal = 1;
            ps->nodes[leaf].index       = tk.kind;
            ps->nodes[leaf].prod        = -1;
            ps->nodes[leaf].token       = tk;
            ps->nodes[leaf].line        = tk.line;
            ps->nodes[leaf].col         = tk.col;
            ps->nodes[leaf].child_off   = 0;
            ps->nodes[leaf].nchild      = 0;

            if (sp >= ps->stack_cap) {
                ps->status = BUF_PARSE_ERR_STACK;
                return -1;
            }
            ps->state_stack[sp] = BUF_PARSE_SHIFT_STATE(v);
            ps->node_stack[sp]  = leaf;
            sp++;

            tk = buf_run(lx, cls, next, accept, rule_token, nstates_dfa, nclass, start_dfa);
        } else {
            int p    = BUF_PARSE_REDUCE_PROD(v);
            int L    = prod_len[p];
            int lhs  = prod_lhs[p];
            int node, gs, i;

            if (sp - L < 1) {
                ps->status = BUF_PARSE_ERR_STACK;
                return -1;
            }

            if (ps->child_used + L > ps->child_cap) {
                ps->status = BUF_PARSE_ERR_CHILD_POOL;
                return -1;
            }
            for (i = 0; i < L; i++)
                ps->child[ps->child_used + i] = ps->node_stack[sp - L + i];

            if (ps->node_used >= ps->node_cap) {
                ps->status = BUF_PARSE_ERR_NODE_POOL;
                return -1;
            }
            node = ps->node_used++;
            ps->nodes[node].is_terminal = 0;
            ps->nodes[node].index       = lhs;
            ps->nodes[node].prod        = p;
            ps->nodes[node].child_off   = ps->child_used;
            ps->nodes[node].nchild      = L;
            if (L > 0) {
                int first = ps->child[ps->child_used];
                ps->nodes[node].line = ps->nodes[first].line;
                ps->nodes[node].col  = ps->nodes[first].col;
            } else {
                ps->nodes[node].line = tk.line;
                ps->nodes[node].col  = tk.col;
            }
            ps->child_used += L;

            sp -= L;
            gs = goto_tab[ps->state_stack[sp - 1] * nnonterm + lhs];
            if (gs < 0) {
                /* Internal table inconsistency (buf_grammar.h's goto_tab
                 * should always have an entry here after a successful
                 * reduce) -- report defensively rather than index OOB. */
                ps->status      = BUF_PARSE_ERR_SYNTAX;
                ps->error_tok   = tk;
                ps->error_state = ps->state_stack[sp - 1];
                return -1;
            }

            if (sp >= ps->stack_cap) {
                ps->status = BUF_PARSE_ERR_STACK;
                return -1;
            }
            ps->state_stack[sp] = gs;
            ps->node_stack[sp]  = node;
            sp++;
        }
    }

    /* Unreachable: every exit from the loop above is a return. Same cccc
     * -c=native flow-analysis quirk as buf_run's own tail return. */
    ps->status = BUF_PARSE_ERR_SYNTAX;
    return -1;
}
