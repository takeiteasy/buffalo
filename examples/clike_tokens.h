/*
 * clike_tokens.h -- checked in; the comptime pass validates this against
 * examples/clike.l (%tokens list and order) and errors on drift.
 *
 * TOK_EOF = 0 and TOK_ERROR = 1 are reserved (see BUF_TOK_* in buf_rt.h).
 * The remaining constants follow clike.l's %tokens line, in order.
 */
#ifndef CLIKE_TOKENS_H
#define CLIKE_TOKENS_H

enum {
    TOK_EOF = 0, TOK_ERROR = 1,
    TOK_KW_IF, TOK_KW_ELSE, TOK_KW_WHILE, TOK_KW_FOR, TOK_KW_RETURN,
    TOK_KW_INT, TOK_KW_VOID, TOK_KW_STRUCT,
    TOK_IDENT, TOK_FLOAT, TOK_INT, TOK_STRING, TOK_CHARLIT,
    TOK_ARROW, TOK_INCR, TOK_DECR, TOK_LE, TOK_GE, TOK_EQ, TOK_NE,
    TOK_ANDAND, TOK_OROR,
    TOK_PLUS, TOK_MINUS, TOK_STAR, TOK_SLASH, TOK_PERCENT, TOK_ASSIGN,
    TOK_LT, TOK_GT, TOK_NOT, TOK_AMP, TOK_PIPE,
    TOK_LPAREN, TOK_RPAREN, TOK_LBRACE, TOK_RBRACE, TOK_LBRACKET, TOK_RBRACKET,
    TOK_SEMI, TOK_COMMA, TOK_DOT
};

#endif /* CLIKE_TOKENS_H */
