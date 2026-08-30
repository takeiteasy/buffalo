/*
 * clike_main.c -- demo harness for the clike lexer.
 *
 * Reads all of stdin, runs the generated clike lexer, prints one line per
 * token: NAME "lexeme" line:col. Skips (whitespace, line/block comments)
 * produce no line. Ends at TOK_EOF. Same shape as digits_main.c.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "buf_rt.h"
#include "clike_tokens.h"

static const char *tok_name(int kind)
{
    switch (kind) {
    case TOK_EOF:      return "EOF";
    case TOK_ERROR:    return "ERROR";
    case TOK_KW_IF:     return "KW_IF";
    case TOK_KW_ELSE:   return "KW_ELSE";
    case TOK_KW_WHILE:  return "KW_WHILE";
    case TOK_KW_FOR:    return "KW_FOR";
    case TOK_KW_RETURN: return "KW_RETURN";
    case TOK_KW_INT:    return "KW_INT";
    case TOK_KW_VOID:   return "KW_VOID";
    case TOK_KW_STRUCT: return "KW_STRUCT";
    case TOK_IDENT:    return "IDENT";
    case TOK_FLOAT:    return "FLOAT";
    case TOK_INT:      return "INT";
    case TOK_STRING:   return "STRING";
    case TOK_CHARLIT:  return "CHARLIT";
    case TOK_ARROW:    return "ARROW";
    case TOK_INCR:     return "INCR";
    case TOK_DECR:     return "DECR";
    case TOK_LE:       return "LE";
    case TOK_GE:       return "GE";
    case TOK_EQ:       return "EQ";
    case TOK_NE:       return "NE";
    case TOK_ANDAND:   return "ANDAND";
    case TOK_OROR:     return "OROR";
    case TOK_PLUS:     return "PLUS";
    case TOK_MINUS:    return "MINUS";
    case TOK_STAR:     return "STAR";
    case TOK_SLASH:    return "SLASH";
    case TOK_PERCENT:  return "PERCENT";
    case TOK_ASSIGN:   return "ASSIGN";
    case TOK_LT:       return "LT";
    case TOK_GT:       return "GT";
    case TOK_NOT:      return "NOT";
    case TOK_AMP:      return "AMP";
    case TOK_PIPE:     return "PIPE";
    case TOK_LPAREN:   return "LPAREN";
    case TOK_RPAREN:   return "RPAREN";
    case TOK_LBRACE:   return "LBRACE";
    case TOK_RBRACE:   return "RBRACE";
    case TOK_LBRACKET: return "LBRACKET";
    case TOK_RBRACKET: return "RBRACKET";
    case TOK_SEMI:     return "SEMI";
    case TOK_COMMA:    return "COMMA";
    case TOK_DOT:      return "DOT";
    default:           return "?";
    }
}

int main(void)
{
    static char buf[1 << 16];
    size_t len = fread(buf, 1, sizeof(buf), stdin);

    BufLexer lx;
    buf_lexer_init(&lx, buf, (int)len);

    for (;;) {
        BufToken t = buf_next(&lx);
        printf("%-8s \"%.*s\" %d:%d\n",
               tok_name(t.kind), t.length, t.lexeme, t.line, t.col);
        if (t.kind == TOK_EOF)
            break;
    }
    return 0;
}
