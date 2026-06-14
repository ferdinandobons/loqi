/*
 * Lume — the AI-first language.
 * Reference implementation (v0.1): a clean tree-walking interpreter in C11.
 *
 * This single translation unit contains the lexer, parser, AST and evaluator.
 * It is intentionally dependency-free so it builds with nothing but clang.
 * Performance work (bytecode VM, GC) lands in later iterations; correctness and
 * a complete, pleasant language come first.
 *
 * Memory model for v0.1: heap objects are tracked in a global arena and freed at
 * exit. A real garbage collector is a planned iteration — see docs/ROADMAP.md.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdarg.h>
#include <ctype.h>
#include <math.h>
#include <setjmp.h>
#include <time.h>

/* ===========================================================================
 * Forward declarations
 * ========================================================================= */
typedef struct Obj Obj;
typedef struct Env Env;
typedef struct Node Node;
typedef struct Value Value;
typedef struct Interp Interp;

/* ===========================================================================
 * Values
 * ========================================================================= */
typedef enum { VAL_NIL, VAL_BOOL, VAL_INT, VAL_FLOAT, VAL_OBJ } ValueType;

struct Value {
    ValueType type;
    union {
        bool b;
        int64_t i;
        double d;
        Obj *obj;
    } as;
};

typedef enum { OBJ_STRING, OBJ_LIST, OBJ_MAP, OBJ_FUNCTION, OBJ_NATIVE } ObjType;

struct Obj {
    ObjType type;
    Obj *arena_next; /* intrusive list for cleanup at exit */
};

typedef struct {
    Obj obj;
    char *chars;
    int length;
} ObjString;

typedef struct {
    Obj obj;
    Value *items;
    int count;
    int cap;
} ObjList;

typedef struct {
    char *key;
    Value value;
} MapEntry;

typedef struct {
    Obj obj;
    MapEntry *entries;
    int count;
    int cap;
} ObjMap;

typedef struct {
    Obj obj;
    Node *decl;     /* the N_FN node: params + body */
    Env *closure;   /* captured lexical environment */
} ObjFunction;

typedef Value (*NativeFn)(Interp *I, int argc, Value *argv);

typedef struct {
    Obj obj;
    NativeFn fn;
    const char *name;
    int arity; /* -1 = variadic */
} ObjNative;

#define IS_NIL(v)    ((v).type == VAL_NIL)
#define IS_BOOL(v)   ((v).type == VAL_BOOL)
#define IS_INT(v)    ((v).type == VAL_INT)
#define IS_FLOAT(v)  ((v).type == VAL_FLOAT)
#define IS_OBJ(v)    ((v).type == VAL_OBJ)
#define IS_NUM(v)    (IS_INT(v) || IS_FLOAT(v))

#define AS_BOOL(v)   ((v).as.b)
#define AS_INT(v)    ((v).as.i)
#define AS_FLOAT(v)  ((v).as.d)
#define AS_OBJ(v)    ((v).as.obj)

#define OBJ_TYPE(v)  (AS_OBJ(v)->type)
#define IS_STRING(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_STRING)
#define IS_LIST(v)   (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_LIST)
#define IS_MAP(v)    (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_MAP)
#define IS_FUNCTION(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_FUNCTION)
#define IS_NATIVE(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE)

#define AS_STRING(v) ((ObjString *)AS_OBJ(v))
#define AS_LIST(v)   ((ObjList *)AS_OBJ(v))
#define AS_MAP(v)    ((ObjMap *)AS_OBJ(v))
#define AS_FUNCTION(v) ((ObjFunction *)AS_OBJ(v))
#define AS_NATIVE(v) ((ObjNative *)AS_OBJ(v))

static Value NIL_VAL   = { VAL_NIL,   { .i = 0 } };
static inline Value bool_val(bool b)  { Value v; v.type = VAL_BOOL;  v.as.b = b; return v; }
static inline Value int_val(int64_t i){ Value v; v.type = VAL_INT;   v.as.i = i; return v; }
static inline Value float_val(double d){Value v; v.type = VAL_FLOAT; v.as.d = d; return v; }
static inline Value obj_val(Obj *o)   { Value v; v.type = VAL_OBJ;   v.as.obj = o; return v; }

/* ===========================================================================
 * Interpreter context (no globals — keeps the parser re-entrant for
 * string interpolation, which re-parses sub-expressions).
 * ========================================================================= */
struct Interp {
    Obj *arena;        /* all heap objects, for cleanup */
    Env *globals;      /* global scope */
    const char *path;  /* current source path for diagnostics */
    jmp_buf err_jmp;   /* runtime error landing pad */
    bool has_error;
    char err_msg[512];
};

static void runtime_error(Interp *I, int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(I->err_msg, sizeof(I->err_msg),
                     "errore a runtime [%s:%d]: ", I->path ? I->path : "?", line);
    vsnprintf(I->err_msg + n, sizeof(I->err_msg) - n, fmt, ap);
    va_end(ap);
    I->has_error = true;
    longjmp(I->err_jmp, 1);
}

static void *xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) { fprintf(stderr, "out of memory\n"); exit(70); }
    return p;
}
static void *xrealloc(void *p, size_t n) {
    void *q = realloc(p, n);
    if (!q) { fprintf(stderr, "out of memory\n"); exit(70); }
    return q;
}

static Obj *alloc_obj(Interp *I, size_t size, ObjType type) {
    Obj *o = xmalloc(size);
    o->type = type;
    o->arena_next = I->arena;
    I->arena = o;
    return o;
}

static ObjString *new_string_n(Interp *I, const char *chars, int length) {
    ObjString *s = (ObjString *)alloc_obj(I, sizeof(ObjString), OBJ_STRING);
    s->chars = xmalloc((size_t)length + 1);
    if (length) memcpy(s->chars, chars, (size_t)length);
    s->chars[length] = '\0';
    s->length = length;
    return s;
}
static Value string_val(Interp *I, const char *chars, int length) {
    return obj_val((Obj *)new_string_n(I, chars, length));
}
static Value cstring_val(Interp *I, const char *chars) {
    return string_val(I, chars, (int)strlen(chars));
}

static ObjList *new_list(Interp *I) {
    ObjList *l = (ObjList *)alloc_obj(I, sizeof(ObjList), OBJ_LIST);
    l->items = NULL; l->count = 0; l->cap = 0;
    return l;
}
static void list_push(ObjList *l, Value v) {
    if (l->count + 1 > l->cap) {
        l->cap = l->cap < 8 ? 8 : l->cap * 2;
        l->items = xrealloc(l->items, sizeof(Value) * (size_t)l->cap);
    }
    l->items[l->count++] = v;
}

static ObjMap *new_map(Interp *I) {
    ObjMap *m = (ObjMap *)alloc_obj(I, sizeof(ObjMap), OBJ_MAP);
    m->entries = NULL; m->count = 0; m->cap = 0;
    return m;
}
static Value *map_get(ObjMap *m, const char *key) {
    for (int i = 0; i < m->count; i++)
        if (strcmp(m->entries[i].key, key) == 0) return &m->entries[i].value;
    return NULL;
}
static void map_set(ObjMap *m, const char *key, Value v) {
    Value *slot = map_get(m, key);
    if (slot) { *slot = v; return; }
    if (m->count + 1 > m->cap) {
        m->cap = m->cap < 8 ? 8 : m->cap * 2;
        m->entries = xrealloc(m->entries, sizeof(MapEntry) * (size_t)m->cap);
    }
    m->entries[m->count].key = strdup(key);
    m->entries[m->count].value = v;
    m->count++;
}

/* ===========================================================================
 * Lexer
 * ========================================================================= */
typedef enum {
    /* literals */
    T_INT, T_FLOAT, T_STRING, T_IDENT,
    /* keywords */
    T_LET, T_FN, T_RETURN, T_IF, T_ELSE, T_WHILE, T_FOR, T_IN,
    T_TRUE, T_FALSE, T_NIL, T_AND, T_OR, T_NOT, T_BREAK, T_CONTINUE, T_IMPORT,
    /* punctuation / operators */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_DOT, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_SLASHSLASH, T_PERCENT,
    T_EQ, T_EQEQ, T_BANGEQ, T_LT, T_LTEQ, T_GT, T_GTEQ,
    T_NEWLINE, T_EOF, T_ERROR
} TokType;

typedef struct {
    TokType type;
    const char *start;
    int length;
    int line;
    /* pre-decoded payloads */
    int64_t int_val;
    double float_val;
    char *str_val; /* for T_STRING: decoded, but braces preserved for interpolation */
} Token;

typedef struct {
    Interp *I;
    const char *src;
    const char *start;
    const char *cur;
    int line;
} Lexer;

static void lex_init(Lexer *L, Interp *I, const char *src) {
    L->I = I; L->src = src; L->start = src; L->cur = src; L->line = 1;
}

static bool is_at_end(Lexer *L) { return *L->cur == '\0'; }
static char advance_ch(Lexer *L) { return *L->cur++; }
static char peek_ch(Lexer *L) { return *L->cur; }
static char peek_next(Lexer *L) { return L->cur[0] ? L->cur[1] : '\0'; }
static bool match_ch(Lexer *L, char c) {
    if (is_at_end(L) || *L->cur != c) return false;
    L->cur++; return true;
}

static Token make_token(Lexer *L, TokType t) {
    Token tok; tok.type = t; tok.start = L->start;
    tok.length = (int)(L->cur - L->start); tok.line = L->line;
    tok.str_val = NULL; tok.int_val = 0; tok.float_val = 0;
    return tok;
}
static Token error_token(Lexer *L, const char *msg) {
    Token tok; tok.type = T_ERROR; tok.start = msg;
    tok.length = (int)strlen(msg); tok.line = L->line;
    tok.str_val = NULL; tok.int_val = 0; tok.float_val = 0;
    return tok;
}

static void skip_trivia(Lexer *L) {
    for (;;) {
        char c = peek_ch(L);
        if (c == ' ' || c == '\t' || c == '\r') { advance_ch(L); }
        else if (c == '#') { while (!is_at_end(L) && peek_ch(L) != '\n') advance_ch(L); }
        else if (c == '\\' && peek_next(L) == '\n') { /* line continuation */
            advance_ch(L); advance_ch(L); L->line++;
        }
        else return;
    }
}

static TokType keyword_type(const char *s, int len) {
    switch (len) {
        case 2:
            if (!memcmp(s, "fn", 2)) return T_FN;
            if (!memcmp(s, "if", 2)) return T_IF;
            if (!memcmp(s, "in", 2)) return T_IN;
            if (!memcmp(s, "or", 2)) return T_OR;
            break;
        case 3:
            if (!memcmp(s, "let", 3)) return T_LET;
            if (!memcmp(s, "for", 3)) return T_FOR;
            if (!memcmp(s, "nil", 3)) return T_NIL;
            if (!memcmp(s, "and", 3)) return T_AND;
            if (!memcmp(s, "not", 3)) return T_NOT;
            break;
        case 4:
            if (!memcmp(s, "else", 4)) return T_ELSE;
            if (!memcmp(s, "true", 4)) return T_TRUE;
            break;
        case 5:
            if (!memcmp(s, "while", 5)) return T_WHILE;
            if (!memcmp(s, "false", 5)) return T_FALSE;
            if (!memcmp(s, "break", 5)) return T_BREAK;
            break;
        case 6:
            if (!memcmp(s, "return", 6)) return T_RETURN;
            if (!memcmp(s, "import", 6)) return T_IMPORT;
            break;
        case 8:
            if (!memcmp(s, "continue", 8)) return T_CONTINUE;
            break;
    }
    return T_IDENT;
}

static Token lex_number(Lexer *L) {
    bool is_float = false;
    while (isdigit((unsigned char)peek_ch(L)) || peek_ch(L) == '_') advance_ch(L);
    if (peek_ch(L) == '.' && isdigit((unsigned char)peek_next(L))) {
        is_float = true;
        advance_ch(L);
        while (isdigit((unsigned char)peek_ch(L)) || peek_ch(L) == '_') advance_ch(L);
    }
    if (peek_ch(L) == 'e' || peek_ch(L) == 'E') {
        is_float = true;
        advance_ch(L);
        if (peek_ch(L) == '+' || peek_ch(L) == '-') advance_ch(L);
        while (isdigit((unsigned char)peek_ch(L))) advance_ch(L);
    }
    /* copy without underscores */
    char buf[64]; int bi = 0;
    for (const char *p = L->start; p < L->cur && bi < 63; p++)
        if (*p != '_') buf[bi++] = *p;
    buf[bi] = '\0';
    Token tok = make_token(L, is_float ? T_FLOAT : T_INT);
    if (is_float) tok.float_val = strtod(buf, NULL);
    else          tok.int_val = strtoll(buf, NULL, 10);
    return tok;
}

/* Decode a string literal, processing escapes but keeping { } so the parser
 * can do interpolation. Escapes: \n \t \r \\ \" \{ \} \0 */
static Token lex_string(Lexer *L) {
    char *buf = xmalloc(64);
    size_t cap = 64, len = 0;
    int interp = 0; /* brace depth inside {interpolation} */
#define PUSH(c) do { if (len + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); } buf[len++] = (c); } while (0)
    while (!is_at_end(L)) {
        if (interp == 0 && peek_ch(L) == '"') break; /* end of string literal */
        char c = advance_ch(L);
        if (c == '\n') L->line++;
        if (interp == 0) {
            if (c == '\\') {
                char e = advance_ch(L);
                switch (e) {
                    case 'n': PUSH('\n'); break;
                    case 't': PUSH('\t'); break;
                    case 'r': PUSH('\r'); break;
                    case '0': PUSH('\0'); break;
                    case '\\': PUSH('\\'); break;
                    case '"': PUSH('"'); break;
                    case '{': PUSH('\x01'); break; /* escaped brace marker (literal {) */
                    case '}': PUSH('\x02'); break; /* escaped brace marker (literal }) */
                    default: PUSH('\\'); PUSH(e); break;
                }
            } else if (c == '{') {
                interp++; PUSH('{');
            } else {
                PUSH(c);
            }
        } else {
            /* inside an interpolation expression: copy verbatim, but track
             * nested braces and skip over nested string literals so their
             * quotes/braces don't confuse termination. */
            if (c == '{') { interp++; PUSH('{'); }
            else if (c == '}') { interp--; PUSH('}'); }
            else if (c == '"') {
                PUSH('"');
                while (!is_at_end(L) && peek_ch(L) != '"') {
                    char d = advance_ch(L);
                    if (d == '\n') L->line++;
                    if (d == '\\') { PUSH('\\'); if (!is_at_end(L)) PUSH(advance_ch(L)); }
                    else PUSH(d);
                }
                if (!is_at_end(L)) { advance_ch(L); PUSH('"'); }
            }
            else PUSH(c);
        }
    }
    if (is_at_end(L)) { free(buf); return error_token(L, "stringa non terminata"); }
    advance_ch(L); /* closing quote */
    PUSH('\0');
#undef PUSH
    Token tok = make_token(L, T_STRING);
    tok.str_val = buf;
    return tok;
}

static Token lex_next(Lexer *L) {
    skip_trivia(L);
    L->start = L->cur;
    if (is_at_end(L)) return make_token(L, T_EOF);

    char c = advance_ch(L);
    if (c == '\n') { Token t = make_token(L, T_NEWLINE); L->line++; return t; }
    if (isdigit((unsigned char)c)) { L->cur--; return lex_number(L); }
    if (isalpha((unsigned char)c) || c == '_') {
        while (isalnum((unsigned char)peek_ch(L)) || peek_ch(L) == '_') advance_ch(L);
        TokType kt = keyword_type(L->start, (int)(L->cur - L->start));
        return make_token(L, kt);
    }
    switch (c) {
        case '(': return make_token(L, T_LPAREN);
        case ')': return make_token(L, T_RPAREN);
        case '{': return make_token(L, T_LBRACE);
        case '}': return make_token(L, T_RBRACE);
        case '[': return make_token(L, T_LBRACKET);
        case ']': return make_token(L, T_RBRACKET);
        case ',': return make_token(L, T_COMMA);
        case '.': return make_token(L, T_DOT);
        case ':': return make_token(L, T_COLON);
        case ';': return make_token(L, T_NEWLINE); /* ';' separa le istruzioni come un a capo */
        case '+': return make_token(L, T_PLUS);
        case '-': return make_token(L, T_MINUS);
        case '*': return make_token(L, T_STAR);
        case '/': return make_token(L, match_ch(L, '/') ? T_SLASHSLASH : T_SLASH);
        case '%': return make_token(L, T_PERCENT);
        case '=': return make_token(L, match_ch(L, '=') ? T_EQEQ : T_EQ);
        case '!': return match_ch(L, '=') ? make_token(L, T_BANGEQ)
                                          : error_token(L, "carattere inatteso '!' (usa 'not')");
        case '<': return make_token(L, match_ch(L, '=') ? T_LTEQ : T_LT);
        case '>': return make_token(L, match_ch(L, '=') ? T_GTEQ : T_GT);
        case '"': return lex_string(L);
    }
    return error_token(L, "carattere inatteso");
}

/* ===========================================================================
 * AST
 * ========================================================================= */
typedef enum {
    N_NIL, N_BOOL, N_INT, N_FLOAT, N_STR, N_INTERP, N_IDENT,
    N_LIST, N_MAP, N_UNARY, N_BINARY, N_LOGICAL,
    N_CALL, N_INDEX, N_MEMBER, N_ASSIGN,
    N_LET, N_BLOCK, N_IF, N_WHILE, N_FOR, N_FN, N_RETURN,
    N_BREAK, N_CONTINUE, N_EXPRSTMT, N_PROGRAM, N_IMPORT
} NodeType;

struct Node {
    NodeType type;
    int line;
    Value literal;        /* N_INT/N_FLOAT/N_BOOL/N_STR */
    char *name;           /* identifier, member name, var name, fn name */
    TokType op;           /* operator for unary/binary/logical */
    Node *a, *b, *c, *d;  /* generic child slots */
    Node **items; int item_count;   /* block stmts / call args / list / params / map keys */
    Node **items2; int item2_count; /* map values */
};

static Node *new_node(NodeType t, int line) {
    Node *n = xmalloc(sizeof(Node));
    memset(n, 0, sizeof(Node));
    n->type = t; n->line = line;
    n->literal = NIL_VAL;
    return n;
}
static void node_add(Node ***arr, int *count, Node *child) {
    *arr = xrealloc(*arr, sizeof(Node *) * (size_t)(*count + 1));
    (*arr)[(*count)++] = child;
}

/* ===========================================================================
 * Parser (recursive descent + Pratt for binary precedence)
 * ========================================================================= */
typedef struct {
    Interp *I;
    Lexer lex;
    Token cur;
    Token prev;
    bool had_error;
} Parser;

static void parser_error_at(Parser *P, Token *t, const char *msg) {
    if (P->had_error) return; /* report first only */
    P->had_error = true;
    if (t->type == T_EOF)
        fprintf(stderr, "errore di sintassi [%s:%d] a fine file: %s\n", P->I->path, t->line, msg);
    else if (t->type == T_ERROR)
        fprintf(stderr, "errore di sintassi [%s:%d]: %.*s\n", P->I->path, t->line, t->length, t->start);
    else
        fprintf(stderr, "errore di sintassi [%s:%d] a '%.*s': %s\n", P->I->path, t->line, t->length, t->start, msg);
}

static void p_advance(Parser *P) {
    P->prev = P->cur;
    for (;;) {
        P->cur = lex_next(&P->lex);
        if (P->cur.type != T_ERROR) break;
        parser_error_at(P, &P->cur, "");
    }
}
static bool p_check(Parser *P, TokType t) { return P->cur.type == t; }
static bool p_match(Parser *P, TokType t) {
    if (!p_check(P, t)) return false;
    p_advance(P); return true;
}
static void p_consume(Parser *P, TokType t, const char *msg) {
    if (P->cur.type == t) { p_advance(P); return; }
    parser_error_at(P, &P->cur, msg);
}
/* skip insignificant newlines */
static void skip_newlines(Parser *P) { while (p_check(P, T_NEWLINE)) p_advance(P); }
/* a statement terminator is a newline, '}' lookahead, or EOF */
static void end_statement(Parser *P) {
    if (p_check(P, T_NEWLINE)) { p_advance(P); return; }
    if (p_check(P, T_RBRACE) || p_check(P, T_EOF)) return;
    parser_error_at(P, &P->cur, "atteso a capo a fine istruzione");
}

static Node *parse_expression(Parser *P);
static Node *parse_statement(Parser *P);
static Node *parse_block(Parser *P);

static char *copy_lexeme(Token *t) {
    char *s = xmalloc((size_t)t->length + 1);
    memcpy(s, t->start, (size_t)t->length);
    s[t->length] = '\0';
    return s;
}

/* --- interpolation: turn "a {expr} b" into a concatenation node --- */
static Node *parse_string_literal(Parser *P, Token *strtok);

static Node *parse_primary(Parser *P) {
    if (p_match(P, T_NIL))   { Node *n = new_node(N_NIL, P->prev.line); return n; }
    if (p_match(P, T_TRUE))  { Node *n = new_node(N_BOOL, P->prev.line); n->literal = bool_val(true); return n; }
    if (p_match(P, T_FALSE)) { Node *n = new_node(N_BOOL, P->prev.line); n->literal = bool_val(false); return n; }
    if (p_match(P, T_INT))   { Node *n = new_node(N_INT, P->prev.line); n->literal = int_val(P->prev.int_val); return n; }
    if (p_match(P, T_FLOAT)) { Node *n = new_node(N_FLOAT, P->prev.line); n->literal = float_val(P->prev.float_val); return n; }
    if (p_match(P, T_STRING)) { return parse_string_literal(P, &P->prev); }
    if (p_match(P, T_IDENT)) {
        Node *n = new_node(N_IDENT, P->prev.line);
        n->name = copy_lexeme(&P->prev);
        return n;
    }
    if (p_match(P, T_LPAREN)) {
        Node *e = parse_expression(P);
        p_consume(P, T_RPAREN, "attesa ')'");
        return e;
    }
    if (p_match(P, T_LBRACKET)) { /* list literal */
        Node *n = new_node(N_LIST, P->prev.line);
        skip_newlines(P);
        if (!p_check(P, T_RBRACKET)) {
            do {
                skip_newlines(P);
                node_add(&n->items, &n->item_count, parse_expression(P));
                skip_newlines(P);
            } while (p_match(P, T_COMMA));
        }
        skip_newlines(P);
        p_consume(P, T_RBRACKET, "attesa ']'");
        return n;
    }
    if (p_match(P, T_LBRACE)) { /* map literal: { key: val, "str": val } */
        Node *n = new_node(N_MAP, P->prev.line);
        skip_newlines(P);
        if (!p_check(P, T_RBRACE)) {
            do {
                skip_newlines(P);
                Node *key;
                if (p_check(P, T_IDENT)) {
                    p_advance(P);
                    key = new_node(N_STR, P->prev.line);
                    key->literal = cstring_val(P->I, "");
                    key->name = copy_lexeme(&P->prev); /* keep raw key */
                    /* store the key string in literal too */
                    key->literal = string_val(P->I, P->prev.start, P->prev.length);
                } else {
                    key = parse_expression(P);
                }
                p_consume(P, T_COLON, "atteso ':' nella mappa");
                skip_newlines(P);
                Node *val = parse_expression(P);
                node_add(&n->items, &n->item_count, key);
                node_add(&n->items2, &n->item2_count, val);
                skip_newlines(P);
            } while (p_match(P, T_COMMA));
        }
        skip_newlines(P);
        p_consume(P, T_RBRACE, "attesa '}'");
        return n;
    }
    if (p_match(P, T_FN)) { /* anonymous function expression */
        Node *n = new_node(N_FN, P->prev.line);
        p_consume(P, T_LPAREN, "attesa '(' dopo 'fn'");
        if (!p_check(P, T_RPAREN)) {
            do {
                p_consume(P, T_IDENT, "atteso nome parametro");
                Node *param = new_node(N_IDENT, P->prev.line);
                param->name = copy_lexeme(&P->prev);
                node_add(&n->items, &n->item_count, param);
            } while (p_match(P, T_COMMA));
        }
        p_consume(P, T_RPAREN, "attesa ')'");
        skip_newlines(P);
        n->a = parse_block(P);
        return n;
    }
    parser_error_at(P, &P->cur, "attesa un'espressione");
    return new_node(N_NIL, P->cur.line);
}

/* postfix: call(), index[], member . */
static Node *parse_postfix(Parser *P) {
    Node *expr = parse_primary(P);
    for (;;) {
        if (p_match(P, T_LPAREN)) {
            Node *call = new_node(N_CALL, P->prev.line);
            call->a = expr;
            skip_newlines(P);
            if (!p_check(P, T_RPAREN)) {
                do {
                    skip_newlines(P);
                    node_add(&call->items, &call->item_count, parse_expression(P));
                    skip_newlines(P);
                } while (p_match(P, T_COMMA));
            }
            skip_newlines(P);
            p_consume(P, T_RPAREN, "attesa ')' dopo gli argomenti");
            expr = call;
        } else if (p_match(P, T_LBRACKET)) {
            Node *idx = new_node(N_INDEX, P->prev.line);
            idx->a = expr;
            idx->b = parse_expression(P);
            p_consume(P, T_RBRACKET, "attesa ']'");
            expr = idx;
        } else if (p_match(P, T_DOT)) {
            p_consume(P, T_IDENT, "atteso nome attributo dopo '.'");
            Node *mem = new_node(N_MEMBER, P->prev.line);
            mem->a = expr;
            mem->name = copy_lexeme(&P->prev);
            expr = mem;
        } else break;
    }
    return expr;
}

static Node *parse_unary(Parser *P) {
    if (p_check(P, T_NOT) || p_check(P, T_MINUS)) {
        p_advance(P);
        Node *n = new_node(N_UNARY, P->prev.line);
        n->op = P->prev.type;
        n->a = parse_unary(P);
        return n;
    }
    return parse_postfix(P);
}

/* precedence climbing for binary operators */
static int bin_prec(TokType t) {
    switch (t) {
        case T_STAR: case T_SLASH: case T_SLASHSLASH: case T_PERCENT: return 7;
        case T_PLUS: case T_MINUS: return 6;
        case T_LT: case T_LTEQ: case T_GT: case T_GTEQ: return 5;
        case T_EQEQ: case T_BANGEQ: return 4;
        case T_AND: return 3;
        case T_OR:  return 2;
        default: return -1;
    }
}
static Node *parse_binary(Parser *P, int min_prec) {
    Node *left = parse_unary(P);
    for (;;) {
        TokType op = P->cur.type;
        int prec = bin_prec(op);
        if (prec < min_prec) break;
        p_advance(P);
        Node *right = parse_binary(P, prec + 1); /* left-assoc */
        Node *n = new_node((op == T_AND || op == T_OR) ? N_LOGICAL : N_BINARY, P->prev.line);
        n->op = op; n->a = left; n->b = right;
        left = n;
    }
    return left;
}

static Node *parse_expression(Parser *P) {
    Node *expr = parse_binary(P, 1);
    if (p_match(P, T_EQ)) {
        /* assignment: target = value (right associative) */
        Node *value = parse_expression(P);
        if (expr->type != N_IDENT && expr->type != N_INDEX && expr->type != N_MEMBER) {
            parser_error_at(P, &P->prev, "destinazione di assegnamento non valida");
        }
        Node *n = new_node(N_ASSIGN, P->prev.line);
        n->a = expr; n->b = value;
        return n;
    }
    return expr;
}

static Node *parse_block(Parser *P) {
    p_consume(P, T_LBRACE, "attesa '{'");
    Node *block = new_node(N_BLOCK, P->prev.line);
    skip_newlines(P);
    while (!p_check(P, T_RBRACE) && !p_check(P, T_EOF)) {
        node_add(&block->items, &block->item_count, parse_statement(P));
        skip_newlines(P);
    }
    p_consume(P, T_RBRACE, "attesa '}'");
    return block;
}

static Node *parse_let(Parser *P) {
    int line = P->prev.line;
    p_consume(P, T_IDENT, "atteso nome di variabile dopo 'let'");
    Node *n = new_node(N_LET, line);
    n->name = copy_lexeme(&P->prev);
    if (p_match(P, T_EQ)) n->a = parse_expression(P);
    else n->a = NULL; /* declared nil */
    end_statement(P);
    return n;
}

static Node *parse_fn_decl(Parser *P) {
    int line = P->prev.line;
    p_consume(P, T_IDENT, "atteso nome funzione dopo 'fn'");
    Node *n = new_node(N_FN, line);
    n->name = copy_lexeme(&P->prev);
    p_consume(P, T_LPAREN, "attesa '(' dopo il nome");
    if (!p_check(P, T_RPAREN)) {
        do {
            p_consume(P, T_IDENT, "atteso nome parametro");
            Node *param = new_node(N_IDENT, P->prev.line);
            param->name = copy_lexeme(&P->prev);
            node_add(&n->items, &n->item_count, param);
        } while (p_match(P, T_COMMA));
    }
    p_consume(P, T_RPAREN, "attesa ')'");
    skip_newlines(P);
    n->a = parse_block(P);
    return n; /* declaration: has a name */
}

static Node *parse_if(Parser *P) {
    int line = P->prev.line;
    Node *n = new_node(N_IF, line);
    n->a = parse_expression(P);          /* condition */
    skip_newlines(P);
    n->b = parse_block(P);               /* then */
    /* allow newline(s) before else */
    Parser save = *P; (void)save;
    skip_newlines(P);
    if (p_match(P, T_ELSE)) {
        skip_newlines(P);
        if (p_check(P, T_IF)) { p_advance(P); n->c = parse_if(P); }
        else n->c = parse_block(P);      /* else */
    }
    return n;
}

static Node *parse_while(Parser *P) {
    int line = P->prev.line;
    Node *n = new_node(N_WHILE, line);
    n->a = parse_expression(P);
    skip_newlines(P);
    n->b = parse_block(P);
    return n;
}

static Node *parse_for(Parser *P) {
    int line = P->prev.line;
    Node *n = new_node(N_FOR, line);
    p_consume(P, T_IDENT, "atteso nome variabile in 'for'");
    n->name = copy_lexeme(&P->prev);
    p_consume(P, T_IN, "atteso 'in' nel ciclo for");
    n->a = parse_expression(P);  /* iterable */
    skip_newlines(P);
    n->b = parse_block(P);
    return n;
}

static Node *parse_statement(Parser *P) {
    skip_newlines(P);
    if (p_match(P, T_LET))    return parse_let(P);
    if (p_match(P, T_FN))     return parse_fn_decl(P);
    if (p_match(P, T_IF))     return parse_if(P);
    if (p_match(P, T_WHILE))  return parse_while(P);
    if (p_match(P, T_FOR))    return parse_for(P);
    if (p_match(P, T_LBRACE)) { /* bare block — '{' already consumed, build it here */
        Node *block = new_node(N_BLOCK, P->prev.line);
        skip_newlines(P);
        while (!p_check(P, T_RBRACE) && !p_check(P, T_EOF)) {
            node_add(&block->items, &block->item_count, parse_statement(P));
            skip_newlines(P);
        }
        p_consume(P, T_RBRACE, "attesa '}'");
        return block;
    }
    if (p_match(P, T_RETURN)) {
        Node *n = new_node(N_RETURN, P->prev.line);
        if (!p_check(P, T_NEWLINE) && !p_check(P, T_RBRACE) && !p_check(P, T_EOF))
            n->a = parse_expression(P);
        end_statement(P);
        return n;
    }
    if (p_match(P, T_BREAK))    { Node *n = new_node(N_BREAK, P->prev.line); end_statement(P); return n; }
    if (p_match(P, T_CONTINUE)) { Node *n = new_node(N_CONTINUE, P->prev.line); end_statement(P); return n; }
    if (p_match(P, T_IMPORT)) {
        Node *n = new_node(N_IMPORT, P->prev.line);
        p_consume(P, T_STRING, "atteso percorso del modulo come stringa");
        n->name = P->prev.str_val;
        end_statement(P);
        return n;
    }
    /* expression statement */
    Node *n = new_node(N_EXPRSTMT, P->cur.line);
    n->a = parse_expression(P);
    end_statement(P);
    return n;
}

static Node *parse_program(Parser *P) {
    Node *prog = new_node(N_PROGRAM, 1);
    skip_newlines(P);
    while (!p_check(P, T_EOF)) {
        node_add(&prog->items, &prog->item_count, parse_statement(P));
        skip_newlines(P);
    }
    return prog;
}

/* Parse a full expression from a standalone source string (used for
 * string interpolation). Returns NULL on error. */
static Node *parse_expr_from_source(Interp *I, const char *src) {
    Parser P;
    P.I = I; P.had_error = false;
    lex_init(&P.lex, I, src);
    p_advance(&P);
    skip_newlines(&P);
    Node *e = parse_expression(&P);
    if (P.had_error) return NULL;
    return e;
}

/* interpolation: split on top-level { } (respecting nesting); pieces become
 * either string literals or expressions. Markers \x01/\x02 are literal braces. */
static Node *parse_string_literal(Parser *P, Token *strtok) {
    const char *s = strtok->str_val ? strtok->str_val : "";
    int line = strtok->line;
    Node *concat = NULL; /* build a chain of N_BINARY '+' */

    char *buf = xmalloc(64); size_t cap = 64, len = 0;
#define PUSHC(c) do { if (len + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); } buf[len++] = (c); } while (0)
    size_t i = 0;
    bool any_interp = false;

    while (s[i]) {
        if (s[i] == '{') {
            /* flush current literal piece */
            buf[len] = '\0';
            Node *lit = new_node(N_STR, line);
            lit->literal = string_val(P->I, buf, (int)len);
            len = 0;
            if (concat) {
                Node *cb = new_node(N_BINARY, line);
                cb->op = T_PLUS; cb->a = concat; cb->b = lit;
                concat = cb;
            } else {
                concat = lit;
            }
            /* read expression until matching } */
            i++;
            int depth = 1;
            char *ebuf = xmalloc(64); size_t ecap = 64, elen = 0;
#define EPUSH(ch) do { if (elen + 1 >= ecap) { ecap *= 2; ebuf = xrealloc(ebuf, ecap); } ebuf[elen++] = (ch); } while (0)
            while (s[i] && depth > 0) {
                if (s[i] == '"') { /* copy a nested string verbatim */
                    EPUSH(s[i++]);
                    while (s[i] && s[i] != '"') {
                        if (s[i] == '\\' && s[i + 1]) { EPUSH(s[i++]); EPUSH(s[i++]); }
                        else EPUSH(s[i++]);
                    }
                    if (s[i] == '"') EPUSH(s[i++]);
                    continue;
                }
                if (s[i] == '{') depth++;
                else if (s[i] == '}') { depth--; if (depth == 0) break; }
                EPUSH(s[i++]);
            }
#undef EPUSH
            ebuf[elen] = '\0';
            if (s[i] == '}') i++;
            Node *expr = parse_expr_from_source(P->I, ebuf);
            free(ebuf);
            if (!expr) { parser_error_at(P, strtok, "espressione non valida in interpolazione"); expr = new_node(N_NIL, line); }
            /* wrap in str() coercion via a special marker: use N_CALL of builtin 'str' */
            Node *call = new_node(N_CALL, line);
            Node *fnref = new_node(N_IDENT, line);
            fnref->name = strdup("str");
            call->a = fnref;
            node_add(&call->items, &call->item_count, expr);
            Node *b = new_node(N_BINARY, line); b->op = T_PLUS; b->a = concat; b->b = call;
            concat = b;
            any_interp = true;
        } else if ((unsigned char)s[i] == 0x01) {
            PUSHC('{'); i++;
        } else if ((unsigned char)s[i] == 0x02) {
            PUSHC('}'); i++;
        } else {
            PUSHC(s[i]); i++;
        }
    }
    /* trailing literal */
    buf[len] = '\0';
    if (!any_interp) {
        Node *lit = new_node(N_STR, line);
        lit->literal = string_val(P->I, buf, (int)len);
        free(buf);
        return lit;
    }
    if (len > 0 || concat == NULL) {
        Node *lit = new_node(N_STR, line);
        lit->literal = string_val(P->I, buf, (int)len);
        if (concat) { Node *b = new_node(N_BINARY, line); b->op = T_PLUS; b->a = concat; b->b = lit; concat = b; }
        else concat = lit;
    }
    free(buf);
#undef PUSHC
    return concat;
}

/* ===========================================================================
 * Environments
 * ========================================================================= */
typedef struct {
    char *name;
    Value value;
} Binding;

struct Env {
    Env *parent;
    Binding *vars;
    int count;
    int cap;
};

static Env *env_new(Env *parent) {
    Env *e = xmalloc(sizeof(Env));
    e->parent = parent; e->vars = NULL; e->count = 0; e->cap = 0;
    return e;
}
static void env_define(Env *e, const char *name, Value v) {
    for (int i = 0; i < e->count; i++)
        if (strcmp(e->vars[i].name, name) == 0) { e->vars[i].value = v; return; }
    if (e->count + 1 > e->cap) {
        e->cap = e->cap < 8 ? 8 : e->cap * 2;
        e->vars = xrealloc(e->vars, sizeof(Binding) * (size_t)e->cap);
    }
    e->vars[e->count].name = strdup(name);
    e->vars[e->count].value = v;
    e->count++;
}
static Value *env_find(Env *e, const char *name) {
    for (; e; e = e->parent)
        for (int i = 0; i < e->count; i++)
            if (strcmp(e->vars[i].name, name) == 0) return &e->vars[i].value;
    return NULL;
}

/* ===========================================================================
 * Evaluator
 * ========================================================================= */
typedef enum { EX_NORMAL, EX_RETURN, EX_BREAK, EX_CONTINUE } ExecKind;
typedef struct { ExecKind kind; Value value; } Exec;

static Value eval(Interp *I, Node *n, Env *env);
static Exec exec_stmt(Interp *I, Node *n, Env *env);
static Exec exec_block(Interp *I, Node *block, Env *env);

static bool is_truthy(Value v) {
    switch (v.type) {
        case VAL_NIL: return false;
        case VAL_BOOL: return v.as.b;
        case VAL_INT: return v.as.i != 0;
        case VAL_FLOAT: return v.as.d != 0.0;
        default: return true;
    }
}

static bool values_equal(Value a, Value b) {
    if (IS_NUM(a) && IS_NUM(b)) {
        if (IS_INT(a) && IS_INT(b)) return AS_INT(a) == AS_INT(b);
        double x = IS_INT(a) ? (double)AS_INT(a) : AS_FLOAT(a);
        double y = IS_INT(b) ? (double)AS_INT(b) : AS_FLOAT(b);
        return x == y;
    }
    if (a.type != b.type) return false;
    switch (a.type) {
        case VAL_NIL: return true;
        case VAL_BOOL: return AS_BOOL(a) == AS_BOOL(b);
        case VAL_OBJ:
            if (IS_STRING(a) && IS_STRING(b))
                return AS_STRING(a)->length == AS_STRING(b)->length &&
                       memcmp(AS_STRING(a)->chars, AS_STRING(b)->chars, (size_t)AS_STRING(a)->length) == 0;
            if (IS_LIST(a) && IS_LIST(b)) {
                ObjList *la = AS_LIST(a), *lb = AS_LIST(b);
                if (la->count != lb->count) return false;
                for (int i = 0; i < la->count; i++)
                    if (!values_equal(la->items[i], lb->items[i])) return false;
                return true;
            }
            if (IS_MAP(a) && IS_MAP(b)) {
                ObjMap *ma = AS_MAP(a), *mb = AS_MAP(b);
                if (ma->count != mb->count) return false;
                for (int i = 0; i < ma->count; i++) {
                    Value *other = map_get(mb, ma->entries[i].key);
                    if (!other || !values_equal(ma->entries[i].value, *other)) return false;
                }
                return true;
            }
            return AS_OBJ(a) == AS_OBJ(b);
        default: return false;
    }
}

/* stringify a value into a freshly heap string buffer (caller frees) */
static char *value_to_cstr(Interp *I, Value v);

static void append_str(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap < 16 ? 32 : *cap * 2;
        *buf = xrealloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

static char *value_to_cstr(Interp *I, Value v) {
    char tmp[64];
    switch (v.type) {
        case VAL_NIL: return strdup("nil");
        case VAL_BOOL: return strdup(v.as.b ? "true" : "false");
        case VAL_INT: snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.i); return strdup(tmp);
        case VAL_FLOAT: {
            double d = v.as.d;
            if (d == (int64_t)d && fabs(d) < 1e15) snprintf(tmp, sizeof(tmp), "%.1f", d);
            else snprintf(tmp, sizeof(tmp), "%g", d);
            return strdup(tmp);
        }
        case VAL_OBJ:
            switch (OBJ_TYPE(v)) {
                case OBJ_STRING: return strdup(AS_STRING(v)->chars);
                case OBJ_NATIVE: {
                    char *b = xmalloc(64);
                    snprintf(b, 64, "<native %s>", AS_NATIVE(v)->name);
                    return b;
                }
                case OBJ_FUNCTION: {
                    ObjFunction *f = AS_FUNCTION(v);
                    char *b = xmalloc(64);
                    snprintf(b, 64, "<fn %s>", f->decl->name ? f->decl->name : "anon");
                    return b;
                }
                case OBJ_LIST: {
                    ObjList *l = AS_LIST(v);
                    char *b = xmalloc(2); b[0] = '['; b[1] = '\0';
                    size_t len = 1, cap = 2;
                    for (int i = 0; i < l->count; i++) {
                        if (i) append_str(&b, &len, &cap, ", ", 2);
                        char *s;
                        if (IS_STRING(l->items[i])) {
                            ObjString *str = AS_STRING(l->items[i]);
                            append_str(&b, &len, &cap, "\"", 1);
                            append_str(&b, &len, &cap, str->chars, (size_t)str->length);
                            append_str(&b, &len, &cap, "\"", 1);
                        } else {
                            s = value_to_cstr(I, l->items[i]);
                            append_str(&b, &len, &cap, s, strlen(s));
                            free(s);
                        }
                    }
                    append_str(&b, &len, &cap, "]", 1);
                    return b;
                }
                case OBJ_MAP: {
                    ObjMap *m = AS_MAP(v);
                    char *b = xmalloc(2); b[0] = '{'; b[1] = '\0';
                    size_t len = 1, cap = 2;
                    for (int i = 0; i < m->count; i++) {
                        if (i) append_str(&b, &len, &cap, ", ", 2);
                        append_str(&b, &len, &cap, m->entries[i].key, strlen(m->entries[i].key));
                        append_str(&b, &len, &cap, ": ", 2);
                        if (IS_STRING(m->entries[i].value)) {
                            ObjString *str = AS_STRING(m->entries[i].value);
                            append_str(&b, &len, &cap, "\"", 1);
                            append_str(&b, &len, &cap, str->chars, (size_t)str->length);
                            append_str(&b, &len, &cap, "\"", 1);
                        } else {
                            char *s = value_to_cstr(I, m->entries[i].value);
                            append_str(&b, &len, &cap, s, strlen(s));
                            free(s);
                        }
                    }
                    append_str(&b, &len, &cap, "}", 1);
                    return b;
                }
            }
    }
    return strdup("?");
}

static const char *type_name(Value v) {
    switch (v.type) {
        case VAL_NIL: return "nil";
        case VAL_BOOL: return "bool";
        case VAL_INT: return "int";
        case VAL_FLOAT: return "float";
        case VAL_OBJ:
            switch (OBJ_TYPE(v)) {
                case OBJ_STRING: return "str";
                case OBJ_LIST: return "list";
                case OBJ_MAP: return "map";
                case OBJ_FUNCTION: case OBJ_NATIVE: return "fn";
            }
    }
    return "?";
}

static double as_double(Value v) { return IS_INT(v) ? (double)AS_INT(v) : AS_FLOAT(v); }

static Value eval_binary(Interp *I, Node *n, Env *env) {
    Value a = eval(I, n->a, env);
    Value b = eval(I, n->b, env);
    TokType op = n->op;

    if (op == T_EQEQ) return bool_val(values_equal(a, b));
    if (op == T_BANGEQ) return bool_val(!values_equal(a, b));

    /* string concatenation with + */
    if (op == T_PLUS && (IS_STRING(a) || IS_STRING(b))) {
        char *sa = value_to_cstr(I, a);
        char *sb = value_to_cstr(I, b);
        size_t la = strlen(sa), lb = strlen(sb);
        char *r = xmalloc(la + lb + 1);
        memcpy(r, sa, la); memcpy(r + la, sb, lb); r[la + lb] = '\0';
        Value out = string_val(I, r, (int)(la + lb));
        free(sa); free(sb); free(r);
        return out;
    }
    /* list concatenation with + */
    if (op == T_PLUS && IS_LIST(a) && IS_LIST(b)) {
        ObjList *r = new_list(I);
        for (int i = 0; i < AS_LIST(a)->count; i++) list_push(r, AS_LIST(a)->items[i]);
        for (int i = 0; i < AS_LIST(b)->count; i++) list_push(r, AS_LIST(b)->items[i]);
        return obj_val((Obj *)r);
    }

    if (!IS_NUM(a) || !IS_NUM(b))
        runtime_error(I, n->line, "operatore '%s' non supportato tra %s e %s",
                      op == T_PLUS ? "+" : op == T_MINUS ? "-" : op == T_STAR ? "*" :
                      op == T_SLASH ? "/" : op == T_SLASHSLASH ? "//" : op == T_PERCENT ? "%" :
                      op == T_LT ? "<" : op == T_LTEQ ? "<=" : op == T_GT ? ">" : ">=",
                      type_name(a), type_name(b));

    bool both_int = IS_INT(a) && IS_INT(b);
    switch (op) {
        case T_PLUS:  return both_int ? int_val(AS_INT(a) + AS_INT(b)) : float_val(as_double(a) + as_double(b));
        case T_MINUS: return both_int ? int_val(AS_INT(a) - AS_INT(b)) : float_val(as_double(a) - as_double(b));
        case T_STAR:  return both_int ? int_val(AS_INT(a) * AS_INT(b)) : float_val(as_double(a) * as_double(b));
        case T_SLASH: {
            double db = as_double(b);
            if (db == 0.0) runtime_error(I, n->line, "divisione per zero");
            return float_val(as_double(a) / db); /* '/' is float division */
        }
        case T_SLASHSLASH: {
            if (both_int) {
                if (AS_INT(b) == 0) runtime_error(I, n->line, "divisione per zero");
                int64_t q = AS_INT(a) / AS_INT(b);
                /* floor division */
                if ((AS_INT(a) % AS_INT(b) != 0) && ((AS_INT(a) < 0) != (AS_INT(b) < 0))) q--;
                return int_val(q);
            }
            double db = as_double(b);
            if (db == 0.0) runtime_error(I, n->line, "divisione per zero");
            return float_val(floor(as_double(a) / db));
        }
        case T_PERCENT: {
            if (both_int) {
                if (AS_INT(b) == 0) runtime_error(I, n->line, "modulo per zero");
                return int_val(AS_INT(a) % AS_INT(b));
            }
            return float_val(fmod(as_double(a), as_double(b)));
        }
        case T_LT:   return bool_val(as_double(a) <  as_double(b));
        case T_LTEQ: return bool_val(as_double(a) <= as_double(b));
        case T_GT:   return bool_val(as_double(a) >  as_double(b));
        case T_GTEQ: return bool_val(as_double(a) >= as_double(b));
        default: runtime_error(I, n->line, "operatore binario sconosciuto");
    }
    return NIL_VAL;
}

static Value call_value(Interp *I, Value callee, int argc, Value *argv, int line);

static Value eval_call(Interp *I, Node *n, Env *env) {
    Value callee = eval(I, n->a, env);
    int argc = n->item_count;
    Value *argv = argc ? xmalloc(sizeof(Value) * (size_t)argc) : NULL;
    for (int i = 0; i < argc; i++) argv[i] = eval(I, n->items[i], env);
    Value r = call_value(I, callee, argc, argv, n->line);
    free(argv);
    return r;
}

static Value call_value(Interp *I, Value callee, int argc, Value *argv, int line) {
    if (IS_NATIVE(callee)) {
        ObjNative *nat = AS_NATIVE(callee);
        if (nat->arity >= 0 && nat->arity != argc)
            runtime_error(I, line, "%s() attende %d argomenti, ricevuti %d", nat->name, nat->arity, argc);
        return nat->fn(I, argc, argv);
    }
    if (IS_FUNCTION(callee)) {
        ObjFunction *fn = AS_FUNCTION(callee);
        Node *decl = fn->decl;
        if (decl->item_count != argc)
            runtime_error(I, line, "%s() attende %d argomenti, ricevuti %d",
                          decl->name ? decl->name : "fn", decl->item_count, argc);
        Env *local = env_new(fn->closure);
        for (int i = 0; i < argc; i++)
            env_define(local, decl->items[i]->name, argv[i]);
        Exec ex = exec_block(I, decl->a, local);
        if (ex.kind == EX_RETURN) return ex.value;
        return NIL_VAL;
    }
    runtime_error(I, line, "valore di tipo %s non è chiamabile", type_name(callee));
    return NIL_VAL;
}

static Value eval(Interp *I, Node *n, Env *env) {
    switch (n->type) {
        case N_NIL: return NIL_VAL;
        case N_BOOL: case N_INT: case N_FLOAT: case N_STR: return n->literal;
        case N_IDENT: {
            Value *slot = env_find(env, n->name);
            if (!slot) runtime_error(I, n->line, "nome non definito: '%s'", n->name);
            return *slot;
        }
        case N_LIST: {
            ObjList *l = new_list(I);
            for (int i = 0; i < n->item_count; i++) list_push(l, eval(I, n->items[i], env));
            return obj_val((Obj *)l);
        }
        case N_MAP: {
            ObjMap *m = new_map(I);
            for (int i = 0; i < n->item_count; i++) {
                Value k = eval(I, n->items[i], env);
                char *key = value_to_cstr(I, k);
                map_set(m, key, eval(I, n->items2[i], env));
                free(key);
            }
            return obj_val((Obj *)m);
        }
        case N_UNARY: {
            Value v = eval(I, n->a, env);
            if (n->op == T_MINUS) {
                if (IS_INT(v)) return int_val(-AS_INT(v));
                if (IS_FLOAT(v)) return float_val(-AS_FLOAT(v));
                runtime_error(I, n->line, "'-' richiede un numero, trovato %s", type_name(v));
            }
            if (n->op == T_NOT) return bool_val(!is_truthy(v));
            return NIL_VAL;
        }
        case N_BINARY: return eval_binary(I, n, env);
        case N_LOGICAL: {
            Value a = eval(I, n->a, env);
            if (n->op == T_AND) return is_truthy(a) ? eval(I, n->b, env) : a;
            else /* OR */       return is_truthy(a) ? a : eval(I, n->b, env);
        }
        case N_CALL: return eval_call(I, n, env);
        case N_INDEX: {
            Value coll = eval(I, n->a, env);
            Value idx = eval(I, n->b, env);
            if (IS_LIST(coll)) {
                if (!IS_INT(idx)) runtime_error(I, n->line, "indice di list deve essere int");
                int64_t i = AS_INT(idx);
                ObjList *l = AS_LIST(coll);
                if (i < 0) i += l->count;
                if (i < 0 || i >= l->count) runtime_error(I, n->line, "indice %lld fuori dai limiti (len %d)", (long long)AS_INT(idx), l->count);
                return l->items[i];
            }
            if (IS_MAP(coll)) {
                char *key = value_to_cstr(I, idx);
                Value *slot = map_get(AS_MAP(coll), key);
                free(key);
                return slot ? *slot : NIL_VAL;
            }
            if (IS_STRING(coll)) {
                if (!IS_INT(idx)) runtime_error(I, n->line, "indice di str deve essere int");
                ObjString *s = AS_STRING(coll);
                int64_t i = AS_INT(idx);
                if (i < 0) i += s->length;
                if (i < 0 || i >= s->length) runtime_error(I, n->line, "indice %lld fuori dai limiti", (long long)AS_INT(idx));
                return string_val(I, s->chars + i, 1);
            }
            runtime_error(I, n->line, "il tipo %s non è indicizzabile", type_name(coll));
        }
        case N_MEMBER: {
            Value obj = eval(I, n->a, env);
            if (IS_MAP(obj)) {
                Value *slot = map_get(AS_MAP(obj), n->name);
                return slot ? *slot : NIL_VAL;
            }
            runtime_error(I, n->line, "il tipo %s non ha attributi (.%s)", type_name(obj), n->name);
        }
        case N_ASSIGN: {
            Value v = eval(I, n->b, env);
            Node *target = n->a;
            if (target->type == N_IDENT) {
                Value *slot = env_find(env, target->name);
                if (!slot) runtime_error(I, n->line, "assegnamento a variabile non dichiarata '%s' (usa 'let')", target->name);
                *slot = v;
            } else if (target->type == N_INDEX) {
                Value coll = eval(I, target->a, env);
                Value idx = eval(I, target->b, env);
                if (IS_LIST(coll)) {
                    if (!IS_INT(idx)) runtime_error(I, n->line, "indice di list deve essere int");
                    int64_t i = AS_INT(idx);
                    ObjList *l = AS_LIST(coll);
                    if (i < 0) i += l->count;
                    if (i < 0 || i >= l->count) runtime_error(I, n->line, "indice fuori dai limiti");
                    l->items[i] = v;
                } else if (IS_MAP(coll)) {
                    char *key = value_to_cstr(I, idx);
                    map_set(AS_MAP(coll), key, v);
                    free(key);
                } else runtime_error(I, n->line, "il tipo %s non supporta assegnamento per indice", type_name(coll));
            } else if (target->type == N_MEMBER) {
                Value obj = eval(I, target->a, env);
                if (!IS_MAP(obj)) runtime_error(I, n->line, "assegnamento attributo su tipo %s", type_name(obj));
                map_set(AS_MAP(obj), target->name, v);
            }
            return v;
        }
        case N_FN: { /* anonymous fn expression */
            ObjFunction *fn = (ObjFunction *)alloc_obj(I, sizeof(ObjFunction), OBJ_FUNCTION);
            fn->decl = n; fn->closure = env;
            return obj_val((Obj *)fn);
        }
        default:
            runtime_error(I, n->line, "espressione non valutabile");
    }
    return NIL_VAL;
}

static Exec normal(void) { Exec e; e.kind = EX_NORMAL; e.value = NIL_VAL; return e; }

static Exec exec_block(Interp *I, Node *block, Env *env) {
    for (int i = 0; i < block->item_count; i++) {
        Exec ex = exec_stmt(I, block->items[i], env);
        if (ex.kind != EX_NORMAL) return ex;
    }
    return normal();
}

static void run_module(Interp *I, const char *path); /* for import */

static Exec exec_stmt(Interp *I, Node *n, Env *env) {
    switch (n->type) {
        case N_LET: {
            Value v = n->a ? eval(I, n->a, env) : NIL_VAL;
            env_define(env, n->name, v);
            return normal();
        }
        case N_FN: {
            ObjFunction *fn = (ObjFunction *)alloc_obj(I, sizeof(ObjFunction), OBJ_FUNCTION);
            fn->decl = n; fn->closure = env;
            env_define(env, n->name, obj_val((Obj *)fn));
            return normal();
        }
        case N_EXPRSTMT: eval(I, n->a, env); return normal();
        case N_BLOCK: {
            Env *inner = env_new(env);
            return exec_block(I, n, inner);
        }
        case N_IF: {
            if (is_truthy(eval(I, n->a, env))) {
                Env *inner = env_new(env);
                return exec_block(I, n->b, inner);
            } else if (n->c) {
                if (n->c->type == N_IF) return exec_stmt(I, n->c, env);
                Env *inner = env_new(env);
                return exec_block(I, n->c, inner);
            }
            return normal();
        }
        case N_WHILE: {
            while (is_truthy(eval(I, n->a, env))) {
                Env *inner = env_new(env);
                Exec ex = exec_block(I, n->b, inner);
                if (ex.kind == EX_BREAK) break;
                if (ex.kind == EX_RETURN) return ex;
                /* CONTINUE / NORMAL: keep looping */
            }
            return normal();
        }
        case N_FOR: {
            Value iter = eval(I, n->a, env);
            if (IS_LIST(iter)) {
                ObjList *l = AS_LIST(iter);
                for (int i = 0; i < l->count; i++) {
                    Env *inner = env_new(env);
                    env_define(inner, n->name, l->items[i]);
                    Exec ex = exec_block(I, n->b, inner);
                    if (ex.kind == EX_BREAK) break;
                    if (ex.kind == EX_RETURN) return ex;
                }
            } else if (IS_MAP(iter)) {
                ObjMap *m = AS_MAP(iter);
                for (int i = 0; i < m->count; i++) {
                    Env *inner = env_new(env);
                    env_define(inner, n->name, cstring_val(I, m->entries[i].key));
                    Exec ex = exec_block(I, n->b, inner);
                    if (ex.kind == EX_BREAK) break;
                    if (ex.kind == EX_RETURN) return ex;
                }
            } else if (IS_STRING(iter)) {
                ObjString *s = AS_STRING(iter);
                for (int i = 0; i < s->length; i++) {
                    Env *inner = env_new(env);
                    env_define(inner, n->name, string_val(I, s->chars + i, 1));
                    Exec ex = exec_block(I, n->b, inner);
                    if (ex.kind == EX_BREAK) break;
                    if (ex.kind == EX_RETURN) return ex;
                }
            } else {
                runtime_error(I, n->line, "il tipo %s non è iterabile", type_name(iter));
            }
            return normal();
        }
        case N_RETURN: {
            Exec e; e.kind = EX_RETURN;
            e.value = n->a ? eval(I, n->a, env) : NIL_VAL;
            return e;
        }
        case N_BREAK: { Exec e; e.kind = EX_BREAK; e.value = NIL_VAL; return e; }
        case N_CONTINUE: { Exec e; e.kind = EX_CONTINUE; e.value = NIL_VAL; return e; }
        case N_IMPORT: {
            run_module(I, n->name);
            return normal();
        }
        default:
            eval(I, n, env);
            return normal();
    }
}

/* ===========================================================================
 * Native functions (the "batteries included" standard library — core set)
 * ========================================================================= */
static Value nat_print(Interp *I, int argc, Value *argv) {
    for (int i = 0; i < argc; i++) {
        if (i) fputc(' ', stdout);
        char *s = value_to_cstr(I, argv[i]);
        fputs(s, stdout);
        free(s);
    }
    fputc('\n', stdout);
    return NIL_VAL;
}
static Value nat_str(Interp *I, int argc, Value *argv) {
    char *s = value_to_cstr(I, argv[0]);
    Value v = cstring_val(I, s);
    free(s);
    return v;
}
static Value nat_len(Interp *I, int argc, Value *argv) {
    Value v = argv[0];
    if (IS_STRING(v)) return int_val(AS_STRING(v)->length);
    if (IS_LIST(v)) return int_val(AS_LIST(v)->count);
    if (IS_MAP(v)) return int_val(AS_MAP(v)->count);
    runtime_error(I, 0, "len() richiede str, list o map, trovato %s", type_name(v));
    return NIL_VAL;
}
static Value nat_type(Interp *I, int argc, Value *argv) {
    return cstring_val(I, type_name(argv[0]));
}
static Value nat_int(Interp *I, int argc, Value *argv) {
    Value v = argv[0];
    if (IS_INT(v)) return v;
    if (IS_FLOAT(v)) return int_val((int64_t)AS_FLOAT(v));
    if (IS_BOOL(v)) return int_val(AS_BOOL(v) ? 1 : 0);
    if (IS_STRING(v)) return int_val(strtoll(AS_STRING(v)->chars, NULL, 10));
    runtime_error(I, 0, "int() non può convertire %s", type_name(v));
    return NIL_VAL;
}
static Value nat_float(Interp *I, int argc, Value *argv) {
    Value v = argv[0];
    if (IS_FLOAT(v)) return v;
    if (IS_INT(v)) return float_val((double)AS_INT(v));
    if (IS_STRING(v)) return float_val(strtod(AS_STRING(v)->chars, NULL));
    runtime_error(I, 0, "float() non può convertire %s", type_name(v));
    return NIL_VAL;
}
static Value nat_push(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "push() richiede una list");
    for (int i = 1; i < argc; i++) list_push(AS_LIST(argv[0]), argv[i]);
    return argv[0];
}
static Value nat_pop(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "pop() richiede una list");
    ObjList *l = AS_LIST(argv[0]);
    if (l->count == 0) runtime_error(I, 0, "pop() da list vuota");
    return l->items[--l->count];
}
static Value nat_keys(Interp *I, int argc, Value *argv) {
    if (!IS_MAP(argv[0])) runtime_error(I, 0, "keys() richiede una map");
    ObjList *l = new_list(I);
    ObjMap *m = AS_MAP(argv[0]);
    for (int i = 0; i < m->count; i++) list_push(l, cstring_val(I, m->entries[i].key));
    return obj_val((Obj *)l);
}
static Value nat_values(Interp *I, int argc, Value *argv) {
    if (!IS_MAP(argv[0])) runtime_error(I, 0, "values() richiede una map");
    ObjList *l = new_list(I);
    ObjMap *m = AS_MAP(argv[0]);
    for (int i = 0; i < m->count; i++) list_push(l, m->entries[i].value);
    return obj_val((Obj *)l);
}
static Value nat_has(Interp *I, int argc, Value *argv) {
    if (IS_MAP(argv[0])) {
        char *key = value_to_cstr(I, argv[1]);
        bool found = map_get(AS_MAP(argv[0]), key) != NULL;
        free(key);
        return bool_val(found);
    }
    if (IS_LIST(argv[0])) {
        ObjList *l = AS_LIST(argv[0]);
        for (int i = 0; i < l->count; i++) if (values_equal(l->items[i], argv[1])) return bool_val(true);
        return bool_val(false);
    }
    runtime_error(I, 0, "has() richiede map o list");
    return NIL_VAL;
}
static Value nat_range(Interp *I, int argc, Value *argv) {
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) { stop = AS_INT(argv[0]); }
    else if (argc == 2) { start = AS_INT(argv[0]); stop = AS_INT(argv[1]); }
    else if (argc == 3) { start = AS_INT(argv[0]); stop = AS_INT(argv[1]); step = AS_INT(argv[2]); }
    else runtime_error(I, 0, "range() attende 1-3 argomenti");
    if (step == 0) runtime_error(I, 0, "range() step non può essere 0");
    ObjList *l = new_list(I);
    if (step > 0) for (int64_t i = start; i < stop; i += step) list_push(l, int_val(i));
    else          for (int64_t i = start; i > stop; i += step) list_push(l, int_val(i));
    return obj_val((Obj *)l);
}
static Value nat_upper(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "upper() richiede str");
    ObjString *s = AS_STRING(argv[0]);
    ObjString *r = new_string_n(I, s->chars, s->length);
    for (int i = 0; i < r->length; i++) r->chars[i] = (char)toupper((unsigned char)r->chars[i]);
    return obj_val((Obj *)r);
}
static Value nat_lower(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "lower() richiede str");
    ObjString *s = AS_STRING(argv[0]);
    ObjString *r = new_string_n(I, s->chars, s->length);
    for (int i = 0; i < r->length; i++) r->chars[i] = (char)tolower((unsigned char)r->chars[i]);
    return obj_val((Obj *)r);
}
static Value nat_split(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) runtime_error(I, 0, "split() richiede (str, str)");
    ObjString *s = AS_STRING(argv[0]);
    ObjString *sep = AS_STRING(argv[1]);
    ObjList *l = new_list(I);
    if (sep->length == 0) {
        for (int i = 0; i < s->length; i++) list_push(l, string_val(I, s->chars + i, 1));
        return obj_val((Obj *)l);
    }
    const char *p = s->chars;
    const char *end = s->chars + s->length;
    while (p <= end) {
        const char *hit = strstr(p, sep->chars);
        if (!hit) { list_push(l, string_val(I, p, (int)(end - p))); break; }
        list_push(l, string_val(I, p, (int)(hit - p)));
        p = hit + sep->length;
    }
    return obj_val((Obj *)l);
}
static Value nat_join(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0]) || !IS_STRING(argv[1])) runtime_error(I, 0, "join() richiede (list, str)");
    ObjList *l = AS_LIST(argv[0]);
    ObjString *sep = AS_STRING(argv[1]);
    char *buf = xmalloc(16); size_t len = 0, cap = 16; buf[0] = '\0';
    for (int i = 0; i < l->count; i++) {
        if (i) append_str(&buf, &len, &cap, sep->chars, (size_t)sep->length);
        char *s = value_to_cstr(I, l->items[i]);
        append_str(&buf, &len, &cap, s, strlen(s));
        free(s);
    }
    Value out = string_val(I, buf, (int)len);
    free(buf);
    return out;
}
static Value nat_abs(Interp *I, int argc, Value *argv) {
    if (IS_INT(argv[0])) return int_val(llabs(AS_INT(argv[0])));
    if (IS_FLOAT(argv[0])) return float_val(fabs(AS_FLOAT(argv[0])));
    runtime_error(I, 0, "abs() richiede un numero");
    return NIL_VAL;
}
static Value nat_sqrt(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "sqrt() richiede un numero");
    return float_val(sqrt(as_double(argv[0])));
}
static Value nat_floor(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "floor() richiede un numero");
    return int_val((int64_t)floor(as_double(argv[0])));
}
static Value nat_clock(Interp *I, int argc, Value *argv) {
    return float_val((double)clock() / CLOCKS_PER_SEC);
}
static Value nat_input(Interp *I, int argc, Value *argv) {
    if (argc >= 1) { char *p = value_to_cstr(I, argv[0]); fputs(p, stdout); free(p); fflush(stdout); }
    char *line = NULL; size_t cap = 0;
    ssize_t got = getline(&line, &cap, stdin);
    if (got < 0) { free(line); return NIL_VAL; }
    if (got > 0 && line[got - 1] == '\n') line[got - 1] = '\0', got--;
    Value v = string_val(I, line, (int)got);
    free(line);
    return v;
}
static Value nat_assert(Interp *I, int argc, Value *argv) {
    if (!is_truthy(argv[0])) {
        const char *msg = (argc >= 2 && IS_STRING(argv[1])) ? AS_STRING(argv[1])->chars : "assert fallita";
        runtime_error(I, 0, "assert: %s", msg);
    }
    return NIL_VAL;
}

static void define_native(Interp *I, const char *name, NativeFn fn, int arity) {
    ObjNative *nat = (ObjNative *)alloc_obj(I, sizeof(ObjNative), OBJ_NATIVE);
    nat->fn = fn; nat->name = name; nat->arity = arity;
    env_define(I->globals, name, obj_val((Obj *)nat));
}

static void install_stdlib(Interp *I) {
    define_native(I, "print", nat_print, -1);
    define_native(I, "str", nat_str, 1);
    define_native(I, "len", nat_len, 1);
    define_native(I, "type", nat_type, 1);
    define_native(I, "int", nat_int, 1);
    define_native(I, "float", nat_float, 1);
    define_native(I, "push", nat_push, -1);
    define_native(I, "pop", nat_pop, 1);
    define_native(I, "keys", nat_keys, 1);
    define_native(I, "values", nat_values, 1);
    define_native(I, "has", nat_has, 2);
    define_native(I, "range", nat_range, -1);
    define_native(I, "upper", nat_upper, 1);
    define_native(I, "lower", nat_lower, 1);
    define_native(I, "split", nat_split, 2);
    define_native(I, "join", nat_join, 2);
    define_native(I, "abs", nat_abs, 1);
    define_native(I, "sqrt", nat_sqrt, 1);
    define_native(I, "floor", nat_floor, 1);
    define_native(I, "clock", nat_clock, 0);
    define_native(I, "input", nat_input, -1);
    define_native(I, "assert", nat_assert, -1);
}

/* ===========================================================================
 * Driver
 * ========================================================================= */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "impossibile aprire '%s'\n", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = xmalloc((size_t)size + 1);
    size_t read = fread(buf, 1, (size_t)size, f);
    buf[read] = '\0';
    fclose(f);
    return buf;
}

static int interpret(Interp *I, const char *src, const char *path) {
    const char *prev_path = I->path;
    I->path = path;
    Parser P;
    P.I = I; P.had_error = false;
    lex_init(&P.lex, I, src);
    p_advance(&P);
    Node *prog = parse_program(&P);
    if (P.had_error) { I->path = prev_path; return 65; }

    if (setjmp(I->err_jmp)) {
        fprintf(stderr, "%s\n", I->err_msg);
        I->path = prev_path;
        return 70;
    }
    for (int i = 0; i < prog->item_count; i++) {
        Exec ex = exec_stmt(I, prog->items[i], I->globals);
        (void)ex;
    }
    I->path = prev_path;
    return 0;
}

static void run_module(Interp *I, const char *path) {
    char *src = read_file(path);
    if (!src) runtime_error(I, 0, "import: modulo '%s' non trovato", path);
    /* run in global scope (simple module model for v0.1) */
    Parser P;
    P.I = I; P.had_error = false;
    const char *prev = I->path;
    I->path = path;
    lex_init(&P.lex, I, src);
    p_advance(&P);
    Node *prog = parse_program(&P);
    if (!P.had_error)
        for (int i = 0; i < prog->item_count; i++) exec_stmt(I, prog->items[i], I->globals);
    I->path = prev;
}

static void interp_init(Interp *I) {
    I->arena = NULL;
    I->path = NULL;
    I->has_error = false;
    I->err_msg[0] = '\0';
    I->globals = env_new(NULL);
    install_stdlib(I);
}

#define LUME_VERSION "0.1.0"

static void repl(Interp *I) {
    printf("Lume %s — REPL. Ctrl-D per uscire.\n", LUME_VERSION);
    char *line = NULL; size_t cap = 0;
    for (;;) {
        fputs("lume> ", stdout); fflush(stdout);
        ssize_t got = getline(&line, &cap, stdin);
        if (got < 0) { printf("\n"); break; }
        interpret(I, line, "<repl>");
    }
    free(line);
}

int main(int argc, char **argv) {
    Interp I;
    interp_init(&I);

    if (argc == 1) {
        repl(&I);
        return 0;
    }
    if (argc >= 2) {
        if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
            printf("Lume %s\n", LUME_VERSION);
            return 0;
        }
        char *src = read_file(argv[1]);
        if (!src) return 74;
        int rc = interpret(&I, src, argv[1]);
        free(src);
        return rc;
    }
    return 0;
}
