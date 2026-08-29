/*
 * calc_tokens.h -- checked in; from M1 the comptime pass validates this
 * against examples/calc.l (%tokens list and order) and errors on drift.
 *
 * TOK_EOF = 0 and TOK_ERROR = 1 are reserved (see BUF_TOK_* in buf_rt.h).
 * The remaining constants follow calc.l's %tokens line, in order.
 */
#ifndef CALC_TOKENS_H
#define CALC_TOKENS_H

enum {
    TOK_EOF = 0, TOK_ERROR = 1,
    TOK_INT, TOK_FLOAT, TOK_IDENT, TOK_PLUS, TOK_STAR, TOK_LPAREN, TOK_RPAREN
};

#endif /* CALC_TOKENS_H */
