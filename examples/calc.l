# calc.l -- the M1 target lexer spec. Checked in now; not built until M4.
# Lines starting with '#' are comments. Blank lines are ignored.

%tokens INT FLOAT IDENT PLUS STAR LPAREN RPAREN

INT       [0-9]+
FLOAT     [0-9]+\.[0-9]+
IDENT     [a-zA-Z_][a-zA-Z0-9_]*
PLUS      "+"
STAR      "*"
LPAREN    "("
RPAREN    ")"

%skip     [ \t\r\n]+
%skip     "#" [^\n]*
