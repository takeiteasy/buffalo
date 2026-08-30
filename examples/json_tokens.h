/*
 * json_tokens.h -- checked in; the comptime pass validates this against
 * examples/json.bflo (%tokens list and order) and errors on drift.
 *
 * TOK_EOF = 0 and TOK_ERROR = 1 are reserved (see BUF_TOK_* in buf_rt.h).
 * The remaining constants follow json.bflo's %tokens line, in order.
 */
#ifndef JSON_TOKENS_H
#define JSON_TOKENS_H

enum {
    TOK_EOF = 0, TOK_ERROR = 1,
    TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET, TOK_COLON, TOK_COMMA,
    TOK_STRING, TOK_NUMBER, TOK_TRUE, TOK_FALSE, TOK_NULL
};

#endif /* JSON_TOKENS_H */
