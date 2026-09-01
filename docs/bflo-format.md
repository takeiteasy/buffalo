# The `.bflo` spec format

> Reference for the `.bflo` reader. `examples/calc.bflo` is the small worked
> example (lexer + grammar); `examples/json.bflo` is a mid-sized lexer
> exercising escapes and optional groups; `examples/clike.bflo` is a ~40-rule
> one.

A `.bflo` file has two sections in one file: a **lexer section** (`%tokens`,
named rules, `%skip`) and an optional **grammar section**, opened by
`%grammar` and running to the end of the file. There is no separate grammar
file — the yacc `.l`/`.y` split does not apply here.

```
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

%grammar
%start expr

expr : expr PLUS term
     | term
     ;

term : term STAR factor
     | factor
     ;

factor : LPAREN expr RPAREN
       | INT
       ;
```

## Lexer section

### Directives and rules

- **`%tokens <NAME>...`** — required. Fixes the enum order, lets a name be
  referenced before its rule, and is the list the comptime pass validates the
  checked-in `<name>_tokens.h` against. Every named rule's `NAME` must appear
  here; a `%tokens` entry with no rule is an error.
- **A rule** is `NAME<whitespace>REGEX` to end of line. `NAME` matches
  `[A-Za-z_][A-Za-z0-9_]*`; the token-kind constant is `TOK_<NAME>`.
- **`%skip <regex>`** — matches and produces no token (whitespace, comments). May
  appear multiple times. A `%skip` rule sits in the **same single priority
  ordering** as the named rules — its file position is its priority. Putting
  every `%skip` at the bottom is a convention, not a separate tier.
- **Match strategy:** longest match wins. On a length tie, the earliest rule in
  file order wins (`%skip` included). Standard lex semantics.
- If no rule matches at the current position, `buf_next` returns a `TOK_ERROR`
  token spanning the single offending byte and advances one byte.
- End of input yields `TOK_EOF` forever.
- **A rule whose regex can match the empty string is rejected** at read time
  with a `line:col` error. A nullable `%skip` rule would make the scanner spin
  in place; a nullable named rule would emit zero-length tokens. Write `x+`
  where you would have written `x*` at the top level, or anchor the rule with a
  required piece.

`TOK_EOF = 0` and `TOK_ERROR = 1` are reserved and must not appear in `%tokens`.

### The token header

`buffalo lex SPEC.bflo` validates a checked-in `<name>_tokens.h` against the
`%tokens` list. The path is derived from the spec path — the trailing `.bflo` is
replaced with `_tokens.h` (`calc.bflo` → `calc_tokens.h`) — or given explicitly
with `--tokens PATH`. The header must open with

```c
enum { TOK_EOF = 0, TOK_ERROR = 1, TOK_<N0>, TOK_<N1>, ... };
```

listing the `%tokens` names in order as `TOK_<NAME>`. A missing kind, an extra
kind, a reordering, a wrong reserved value, or an explicit value pinned on a
non-reserved kind is an error. `%tokens` is the only authority on order.

### Regex grammar (v1)

| Form | Meaning |
|---|---|
| `x` | literal byte (any byte that is not a metacharacter) |
| `"..."` | literal string; inside, only `\"` and `\\` are escapes |
| `.` | any byte except `\n` |
| `[abc]` `[a-z]` `[^...]` | character class: set, ranges, negation |
| `\n \t \r \f \v \0 \\ \. \* \+ \? \( \) \[ \] \| \" \-` | escapes (usable bare and inside classes) |
| `\d \D \w \W \s \S` | shorthands (`[0-9]`, `[^0-9]`, `[A-Za-z0-9_]`, …) — desugared to classes |
| `AB` | concatenation |
| `A\|B` | alternation |
| `A*` `A+` `A?` | Kleene star, plus, optional |
| `(A)` | grouping |

Not in v1: `{n,m}` counts, `^` / `$` anchors, `\b`, lookaround, non-greedy,
backreferences. These are not silently accepted — each is reported as a
`line:col` error in the `.bflo` file.

Unquoted whitespace inside a regex is insignificant: it separates atoms, so
`"#" [^\n]*` is the concatenation of `"#"` and `[^\n]*`. A literal space is
written `" "` or `[ ]`.

### Common patterns

- **Keywords vs. identifiers.** A keyword rule and the `IDENT` rule both match
  `if` with length 2. Longest match cannot separate them, so file order does:
  put every keyword rule **before** `IDENT`.

  ```
  KW_IF    "if"
  KW_ELSE  "else"
  IDENT    [a-zA-Z_][a-zA-Z0-9_]*
  ```

- **Block comments** without `{n,m}` or non-greedy — the classic form:

  ```
  %skip  "/*" ([^*] | "*"+[^*/])* "*"+ "/"
  ```

- **String literals** with escapes:

  ```
  STRING  "\"" ([^"\\\n] | \\ .)* "\""
  ```

- **`%tokens` is one line.** There is no line continuation; list every name on
  the single `%tokens` line.

### Token value model

A token is a lexeme slice (`lexeme` pointer + `length`) into the source buffer
plus 1-based `line` / `col`. No value conversion, no interning — the caller
converts from the slice.

## Grammar section

The reader accepts and validates a `%grammar` section: it resolves every
symbol, checks that the grammar is well-formed, and reports `line:col`
diagnostics in the same style as the lexer section. `buf_grammar.h` then
builds LALR(1) parser tables (action/goto) from it — see
[design.md](design.md#lalr1-table-construction) — and `buffalo parse` lowers
those tables plus a `buf_parse_tree` wrapper into the generated C, where
[`buf_parse`](design.md#the-parser-driver-buf_parse) drives them into a
concrete syntax tree. `buffalo lex` ignores the `%grammar` section: a spec
with one lexes exactly as it would without one.

### `%grammar`

Opens the grammar section. It must be the last thing in the file — once seen,
everything to end of file is grammar syntax, not lexer directives or rules.
Unlike the lexer section, the grammar section is **not** line-oriented:
whitespace (including newlines) and `#`-to-end-of-line comments are
insignificant between symbols, so a production may freely span lines.

### `%start NAME`

Required exactly once inside a `%grammar` section, naming the start
nonterminal. `NAME` must not be a `%tokens` entry, and must have at least one
production.

### Productions

```
NAME : SYM SYM ... | SYM ... | ... ;
```

- `NAME` is the left-hand side nonterminal.
- Each `|`-separated alternative is zero or more whitespace-separated symbol
  names, terminated by `;`. An alternative with zero symbols is legal — it is
  the empty production (epsilon).
- **Terminal vs. nonterminal is implicit**, resolved against `%tokens`: a
  symbol name (on either side of `:`) that appears in `%tokens` is a
  terminal; any other name is a nonterminal. There is no separate
  nonterminal-declaration directive — a nonterminal exists the moment it is
  mentioned, either as a use (an rhs symbol, or the `%start` target) or a
  definition (a production's left-hand side).
- A nonterminal may be **used before it is defined** (left-recursion and
  mutual recursion are both fine); it just needs a production somewhere in
  the section. A nonterminal that is used but never given a production is an
  error, pointing at its first use.
- A `%tokens` entry cannot be a production's left-hand side or the `%start`
  symbol — a token is always a terminal.
- Symbol classification is **exact-match, not case-folded** — `expr` and
  `EXPR` are different symbols even if only one of them is a `%tokens` entry.

### Diagnostics

```
<spec>:12:6: expected ':' after nonterminal 'expr'
<spec>:14:1: expected ';' at end of production 'expr'
<spec>:15:8: unexpected character '@' in grammar
<spec>:13:12: 'INT' is a %tokens terminal and cannot be a production left-hand side
<spec>:16:14: nonterminal 'facter' is used but never defined
<spec>:10:1: the %start directive is required in a %grammar section
<spec>:11:8: %start symbol 'expr' has no production
<spec>:3:1: %start is only valid inside a %grammar section
```

`%start` written before `%grammar` is rejected with a dedicated message
(rather than falling through to a generic "unknown directive") since it is
an easy mistake to make.

`buf_grammar.h`'s own validation and LALR(1) table construction add these
diagnostics, in the same style:

```
<spec>:6:1: nonterminal 'dead' is unreachable from %start 'expr'
<spec>:7:1: nonterminal 'loop' is non-productive (derives no finite string)
<spec>:24:8: shift/reduce conflict on 'PLUS' in state 12: reduce 'expr' vs. shift
<spec>:9:1: reduce/reduce conflict in state 4 between 'a' (9:1) and 'b' (11:1) on lookahead 'INT'
```

A shift/reduce or reduce/reduce conflict is always a hard error — buffalo's
grammar format has no `%left`/`%right` precedence declarations to resolve
one silently (see [design.md](design.md#lalr1-table-construction)), so a
real conflict always means the grammar is ambiguous at that point.
