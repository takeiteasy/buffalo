# big.l -- a full C-ish tokenizer, deliberately large.
#
# clike.l (~45 rules) collapses to only ~85 DFA states -- below the 150-250
# the M3 spike wanted to probe. This one enumerates the whole C keyword set,
# every operator as its own token, and the realistic integer/float/string
# literal forms: ~100 named rules, and it lands in the projected DFA-state
# range so the go/no-go gate is measured at the size the ticket asked about.
#
# It is a measurement fixture, not a teaching example -- clike.l stays the
# one to read. No _main.c / .expected: like clike it is only driven through
# tests/t_dfa.c and tests/bench.sh.

%tokens KW_AUTO KW_BREAK KW_CASE KW_CHAR KW_CONST KW_CONTINUE KW_DEFAULT KW_DO KW_DOUBLE KW_ELSE KW_ENUM KW_EXTERN KW_FLOAT KW_FOR KW_GOTO KW_IF KW_INLINE KW_INT KW_LONG KW_REGISTER KW_RESTRICT KW_RETURN KW_SHORT KW_SIGNED KW_SIZEOF KW_STATIC KW_STRUCT KW_SWITCH KW_TYPEDEF KW_UNION KW_UNSIGNED KW_VOID KW_VOLATILE KW_WHILE KW_BOOL KW_COMPLEX KW_IMAGINARY KW_ALIGNAS KW_ALIGNOF KW_ATOMIC KW_GENERIC KW_NORETURN KW_STATICASSERT KW_THREADLOCAL IDENT HEXFLOAT FLOAT HEXINT BININT OCTINT DECINT STRING CHARLIT ELLIPSIS SHL_ASSIGN SHR_ASSIGN ARROW INCR DECR SHL SHR LE GE EQ NE ANDAND OROR PLUS_ASSIGN MINUS_ASSIGN STAR_ASSIGN SLASH_ASSIGN PCT_ASSIGN AMP_ASSIGN PIPE_ASSIGN CARET_ASSIGN PLUS MINUS STAR SLASH PERCENT ASSIGN LT GT NOT AMP PIPE CARET TILDE QUESTION COLON SEMI COMMA DOT LPAREN RPAREN LBRACE RBRACE LBRACKET RBRACKET HASH

# keywords -- all before IDENT so a length tie resolves to the keyword
KW_AUTO         "auto"
KW_BREAK        "break"
KW_CASE         "case"
KW_CHAR         "char"
KW_CONST        "const"
KW_CONTINUE     "continue"
KW_DEFAULT      "default"
KW_DO           "do"
KW_DOUBLE       "double"
KW_ELSE         "else"
KW_ENUM         "enum"
KW_EXTERN       "extern"
KW_FLOAT        "float"
KW_FOR          "for"
KW_GOTO         "goto"
KW_IF           "if"
KW_INLINE       "inline"
KW_INT          "int"
KW_LONG         "long"
KW_REGISTER     "register"
KW_RESTRICT     "restrict"
KW_RETURN       "return"
KW_SHORT        "short"
KW_SIGNED       "signed"
KW_SIZEOF       "sizeof"
KW_STATIC       "static"
KW_STRUCT       "struct"
KW_SWITCH       "switch"
KW_TYPEDEF      "typedef"
KW_UNION        "union"
KW_UNSIGNED     "unsigned"
KW_VOID         "void"
KW_VOLATILE     "volatile"
KW_WHILE        "while"
KW_BOOL         "_Bool"
KW_COMPLEX      "_Complex"
KW_IMAGINARY    "_Imaginary"
KW_ALIGNAS      "_Alignas"
KW_ALIGNOF      "_Alignof"
KW_ATOMIC       "_Atomic"
KW_GENERIC      "_Generic"
KW_NORETURN     "_Noreturn"
KW_STATICASSERT "_Static_assert"
KW_THREADLOCAL  "_Thread_local"

IDENT           [a-zA-Z_][a-zA-Z0-9_]*

# numeric literals -- longest-match sorts these out; order breaks ties only
HEXFLOAT        "0" [xX] [0-9a-fA-F]+ ("." [0-9a-fA-F]*)? [pP] [+-]? [0-9]+ [fFlL]?
FLOAT          ([0-9]+ "." [0-9]* | "." [0-9]+) ([eE] [+-]? [0-9]+)? [fFlL]? | [0-9]+ [eE] [+-]? [0-9]+ [fFlL]?
HEXINT          "0" [xX] [0-9a-fA-F]+ [uUlL]*
BININT          "0" [bB] [01]+ [uUlL]*
OCTINT          "0" [0-7]+ [uUlL]*
DECINT          [0-9]+ [uUlL]*

STRING          [LuU]? "\"" ([^"\\\n] | \\ .)* "\""
CHARLIT         [LuU]? "'" ([^'\\\n] | \\ .)+ "'"

# punctuation and operators, longest first within each prefix family
ELLIPSIS        "..."
SHL_ASSIGN      "<<="
SHR_ASSIGN      ">>="
ARROW           "->"
INCR            "++"
DECR            "--"
SHL             "<<"
SHR             ">>"
LE             "<="
GE             ">="
EQ             "=="
NE             "!="
ANDAND          "&&"
OROR            "||"
PLUS_ASSIGN     "+="
MINUS_ASSIGN    "-="
STAR_ASSIGN     "*="
SLASH_ASSIGN    "/="
PCT_ASSIGN      "%="
AMP_ASSIGN      "&="
PIPE_ASSIGN     "|="
CARET_ASSIGN    "^="
PLUS            "+"
MINUS           "-"
STAR            "*"
SLASH           "/"
PERCENT         "%"
ASSIGN          "="
LT             "<"
GT             ">"
NOT             "!"
AMP             "&"
PIPE            "|"
CARET           "^"
TILDE           "~"
QUESTION        "?"
COLON           ":"
SEMI            ";"
COMMA           ","
DOT             "."
LPAREN          "("
RPAREN          ")"
LBRACE          "{"
RBRACE          "}"
LBRACKET        "["
RBRACKET        "]"
HASH            "#"

%skip           [ \t\r\n]+
%skip           "//" [^\n]*
%skip           "/*" ([^*] | "*"+[^*/])* "*"+ "/"
