# clike.l -- a ~30-rule C-like lexer spec.
#
# Lands at M2 so tests/t_dfa.c has a realistic spec to assert the alphabet
# collapses well below 256 classes, that keyword rules beat IDENT on a length
# tie (earlier rule wins), and that both comment forms are skipped. The
# clike_main.c driver and clike.expected golden output arrive at M5.

%tokens KW_IF KW_ELSE KW_WHILE KW_FOR KW_RETURN KW_INT KW_VOID KW_STRUCT IDENT FLOAT INT STRING CHARLIT ARROW INCR DECR LE GE EQ NE ANDAND OROR PLUS MINUS STAR SLASH PERCENT ASSIGN LT GT NOT AMP PIPE LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET SEMI COMMA DOT

# keywords -- must precede IDENT so a length tie resolves to the keyword
KW_IF      "if"
KW_ELSE    "else"
KW_WHILE   "while"
KW_FOR     "for"
KW_RETURN  "return"
KW_INT     "int"
KW_VOID    "void"
KW_STRUCT  "struct"

IDENT      [a-zA-Z_][a-zA-Z0-9_]*
FLOAT      [0-9]+\.[0-9]+
INT        [0-9]+
STRING     "\"" ([^"\\\n] | \\ .)* "\""
CHARLIT    "'" ([^'\\\n] | \\ .) "'"

ARROW      "->"
INCR       "++"
DECR       "--"
LE         "<="
GE         ">="
EQ         "=="
NE         "!="
ANDAND     "&&"
OROR       "||"

PLUS       "+"
MINUS      "-"
STAR       "*"
SLASH      "/"
PERCENT    "%"
ASSIGN     "="
LT         "<"
GT         ">"
NOT        "!"
AMP        "&"
PIPE       "|"
LPAREN     "("
RPAREN     ")"
LBRACE     "{"
RBRACE     "}"
LBRACKET   "["
RBRACKET   "]"
SEMI       ";"
COMMA      ","
DOT        "."

%skip      [ \t\r\n]+
%skip      "//" [^\n]*
%skip      "/*" ([^*] | "*"+[^*/])* "*"+ "/"
