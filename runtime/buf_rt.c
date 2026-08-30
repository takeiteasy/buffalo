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
