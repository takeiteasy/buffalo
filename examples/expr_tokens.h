/*
 * expr_tokens.h -- checked in; the comptime pass validates this against
 * examples/expr.bflo (%tokens list and order) and errors on drift.
 *
 * TOK_EOF = 0 and TOK_ERROR = 1 are reserved (see BUF_TOK_* in buf_rt.h).
 * The remaining constants follow expr.bflo's %tokens line, in order.
 */
#ifndef EXPR_TOKENS_H
#define EXPR_TOKENS_H

enum {
    TOK_EOF = 0, TOK_ERROR = 1,
    TOK_INT, TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_LPAREN, TOK_RPAREN
};

#endif /* EXPR_TOKENS_H */
