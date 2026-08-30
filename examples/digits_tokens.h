/*
 * digits_tokens.h -- checked-in token vocabulary for the M0 demo.
 *
 * From M1 onward the comptime pass validates a header like this against its
 * .bflo spec (%tokens list, order, no drift). At M0 there is no spec: this
 * pairs with the hand-written examples/digits_tables.c.
 *
 * TOK_EOF = 0 and TOK_ERROR = 1 are reserved (see BUF_TOK_* in buf_rt.h).
 */
#ifndef DIGITS_TOKENS_H
#define DIGITS_TOKENS_H

enum {
    TOK_EOF   = 0,
    TOK_ERROR = 1,
    TOK_INT
};

#endif /* DIGITS_TOKENS_H */
