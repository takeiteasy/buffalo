# The `.l` lexer spec format

> Reference for the `.l` reader. `examples/calc.l` is the small worked example;
> `examples/json.l` is a mid-sized one exercising escapes and optional groups;
> `examples/clike.l` is a ~40-rule one.

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
```

## Directives and rules

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

## The token header

`buffalo lex SPEC.l` validates a checked-in `<name>_tokens.h` against the
`%tokens` list. The path is derived from the spec path — the trailing `.l` is
replaced with `_tokens.h` (`calc.l` → `calc_tokens.h`) — or given explicitly
with `--tokens PATH`. The header must open with

```c
enum { TOK_EOF = 0, TOK_ERROR = 1, TOK_<N0>, TOK_<N1>, ... };
```

listing the `%tokens` names in order as `TOK_<NAME>`. A missing kind, an extra
kind, a reordering, a wrong reserved value, or an explicit value pinned on a
non-reserved kind is an error. `%tokens` is the only authority on order.

## Regex grammar (v1)

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
`line:col` error in the `.l` file.

Unquoted whitespace inside a regex is insignificant: it separates atoms, so
`"#" [^\n]*` is the concatenation of `"#"` and `[^\n]*`. A literal space is
written `" "` or `[ ]`.

## Common patterns

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

## Token value model

A token is a lexeme slice (`lexeme` pointer + `length`) into the source buffer
plus 1-based `line` / `col`. No value conversion, no interning — the caller
converts from the slice.
