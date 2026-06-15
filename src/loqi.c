/*
 * Loqi — the AI-first language.
 * Reference implementation in C11, dependency-free (builds with nothing but clang).
 *
 * Pipeline: source -> lexer -> Pratt parser -> AST -> single-pass bytecode
 * compiler -> stack-based virtual machine. Locals resolve to stack slots at
 * compile time, globals to an open-addressing hash table, closures capture via
 * upvalues.
 *
 * Memory model: heap objects live on an intrusive arena list and are reclaimed
 * by a precise mark & sweep garbage collector that runs at the VM dispatch
 * safe-point (see "Garbage collector" below). Keeps memory bounded for
 * long-running programs.
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
#include <errno.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>
#include <dirent.h>
#include <pthread.h>

/* ===========================================================================
 * Forward declarations
 * ========================================================================= */
typedef struct Obj Obj;
typedef struct Node Node;
typedef struct Value Value;
typedef struct Interp Interp;
typedef struct Table Table;
typedef struct ObjProto ObjProto;
typedef struct ObjClosure ObjClosure;
typedef struct ObjUpvalue ObjUpvalue;
typedef struct VM VM;

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

typedef enum { OBJ_STRING, OBJ_LIST, OBJ_MAP, OBJ_PROTO, OBJ_CLOSURE, OBJ_UPVALUE, OBJ_NATIVE } ObjType;

struct Obj {
    ObjType type;
    bool marked;     /* GC mark bit (set during marking, cleared on sweep) */
    Obj *arena_next; /* intrusive list of every object — this is the GC sweep list */
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

/* Inline cache for a global name constant: the table its slot was resolved in,
 * that table's version, and the resolved value slot. Valid while the table's
 * version is unchanged (it bumps only on a rehash/grow, which moves entries). */
typedef struct { Table *table; uint32_t version; Value *slot; } GCache;

/* A chunk of compiled bytecode: instruction bytes, source lines, constants. */
typedef struct Chunk {
    uint8_t *code;
    int *lines;
    int count;
    int cap;
    Value *constants;
    int const_count;
    int const_cap;
    GCache *gcache;     /* lazily allocated, one slot per constant index; OP_*_GLOBAL cache */
} Chunk;

/* A compiled function prototype (the immutable code + metadata). */
struct ObjProto {
    Obj obj;
    int arity;
    int upvalue_count;
    Chunk chunk;
    char *name;            /* for diagnostics; NULL for the top-level script */
    Table *module_globals; /* the globals table of the module this proto belongs to */
    const char *source;    /* borrowed: the module's source text (kept alive in I->sources) */
    const char *path;      /* borrowed: the module's file path (kept alive in I->sources) */
};

/* A captured variable that may outlive the stack frame that created it. */
struct ObjUpvalue {
    Obj obj;
    Value *location;     /* points into the VM stack while open */
    Value closed;        /* holds the value once closed */
    ObjUpvalue *next;    /* intrusive list of open upvalues */
};

/* A closure: a prototype plus the upvalues it closed over. */
struct ObjClosure {
    Obj obj;
    ObjProto *proto;
    ObjUpvalue **upvalues;
    int upvalue_count;
};

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
#define IS_CLOSURE(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_CLOSURE)
#define IS_PROTO(v)  (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_PROTO)
#define IS_NATIVE(v) (IS_OBJ(v) && OBJ_TYPE(v) == OBJ_NATIVE)

#define AS_STRING(v) ((ObjString *)AS_OBJ(v))
#define AS_LIST(v)   ((ObjList *)AS_OBJ(v))
#define AS_MAP(v)    ((ObjMap *)AS_OBJ(v))
#define AS_CLOSURE(v) ((ObjClosure *)AS_OBJ(v))
#define AS_PROTO(v)  ((ObjProto *)AS_OBJ(v))
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
    Table *globals;    /* global names -> values (hash table, O(1)) */
    Table *module_defines; /* while running a namespaced module: names it defines (for export) */
    Table *module_cache;   /* canonical path -> namespace map, so each module evaluates once */
    const char *path;  /* current source path for diagnostics */
    VM *vm;            /* the currently running VM (for native -> closure callbacks) */
    jmp_buf err_jmp;   /* runtime error landing pad */
    bool has_error;
    char err_msg[512];
    char err_trace[1024]; /* call-stack backtrace for an uncaught runtime error */
    char err_source[320]; /* the offending source line for an uncaught error */
    const char *current_source; /* source being compiled now (-> proto->source) */
    const char *current_path;   /* path being compiled now (-> proto->path) */
    char **sources;       /* every source text + path, kept alive for diagnostics */
    int source_count, source_cap;
    int import_depth;  /* number of files currently being loaded (import stack height) */
    const char *import_stack[64]; /* paths currently loading, for cycle detection */
    /* mark-sweep garbage collector */
    size_t obj_count;  /* live heap objects (the GC trigger metric) */
    size_t next_gc;    /* collect once obj_count reaches this */
    uint32_t gc_gen;   /* current GC generation (for per-cycle table dedup) */
    Obj **gray;        /* worklist of marked-but-not-yet-scanned objects */
    int gray_count, gray_cap;
    bool gc_stress;    /* LOQI_GC_STRESS=1 -> collect at every safe-point (testing) */
    bool gc_pending;   /* set when a collection is due; the only per-instruction GC check */
    uint64_t rng_state; /* xorshift64 state for random()/randint() */
    int script_argc;   /* CLI arguments passed to the script (for args()) */
    char **script_argv;
    /* every per-module globals table (the `mod` from run_module_ns), owned here so
       interp_free can reclaim them — they are raw Tables, not GC objects, so the
       sweep never touches them and a data-only module would otherwise orphan one. */
    Table **module_tables;
    int module_table_count, module_table_cap;
};

/* An import that re-enters another file recurses through run_source on the C
 * stack; cap the nesting so a deep chain raises a catchable Loqi error instead
 * of overflowing the C stack and crashing with SIGSEGV. A path already on the
 * import stack is a cycle and is reported directly. */
#define MAX_IMPORT_DEPTH 64

/* snprintf returns the would-be length; clamp it so reusing it as an offset
 * into a fixed buffer can never run past the end (long paths/names). */
static int clamp_written(int n, size_t cap) {
    if (n < 0) return 0;
    if ((size_t)n >= cap) return (int)cap - 1;
    return n;
}

/* Routes a built error message to the nearest try/catch handler, or to the
 * top-level landing pad if none is active. Defined after the VM. */
static void raise_error(Interp *I);
static int vm_current_line(Interp *I); /* line of the instruction currently executing */
static const char *vm_current_path(Interp *I); /* file of the frame currently executing */

static void runtime_error(Interp *I, int line, const char *fmt, ...) {
    if (line == 0) line = vm_current_line(I); /* natives pass 0 -> use the call site */
    va_list ap;
    va_start(ap, fmt);
    int n = snprintf(I->err_msg, sizeof(I->err_msg),
                     "runtime error [%s:%d]: ", vm_current_path(I), line);
    n = clamp_written(n, sizeof(I->err_msg));
    vsnprintf(I->err_msg + n, sizeof(I->err_msg) - (size_t)n, fmt, ap);
    va_end(ap);
    raise_error(I);
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
    o->marked = false;
    o->arena_next = I->arena;
    I->arena = o;
    if (++I->obj_count >= I->next_gc) I->gc_pending = true; /* threshold check here, not per-instruction */
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
    T_INT, T_FLOAT, T_STRING, T_RAWSTRING, T_IDENT,
    /* keywords */
    T_LET, T_FN, T_RETURN, T_IF, T_ELSE, T_WHILE, T_FOR, T_IN,
    T_TRUE, T_FALSE, T_NIL, T_AND, T_OR, T_NOT, T_BREAK, T_CONTINUE, T_IMPORT,
    T_MATCH, T_TRY, T_CATCH,
    /* punctuation / operators */
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACKET, T_RBRACKET,
    T_COMMA, T_DOT, T_DOTDOT, T_COLON,
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_SLASHSLASH, T_PERCENT,
    T_EQ, T_EQEQ, T_BANGEQ, T_LT, T_LTEQ, T_GT, T_GTEQ,
    T_QQ, T_QDOT, /* ?? null-coalescing, ?. optional chaining */
    T_PIPE,       /* |> pipeline */
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
        else if (c == '\\' && peek_next(L) == '\n') { /* line continuation (LF) */
            advance_ch(L); advance_ch(L); L->line++;
        }
        else if (c == '\\' && peek_next(L) == '\r') { /* line continuation (CRLF) */
            advance_ch(L); advance_ch(L);
            if (peek_ch(L) == '\n') advance_ch(L);
            L->line++;
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
            if (!memcmp(s, "try", 3)) return T_TRY;
            break;
        case 4:
            if (!memcmp(s, "else", 4)) return T_ELSE;
            if (!memcmp(s, "true", 4)) return T_TRUE;
            break;
        case 5:
            if (!memcmp(s, "while", 5)) return T_WHILE;
            if (!memcmp(s, "false", 5)) return T_FALSE;
            if (!memcmp(s, "break", 5)) return T_BREAK;
            if (!memcmp(s, "match", 5)) return T_MATCH;
            if (!memcmp(s, "catch", 5)) return T_CATCH;
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
    errno = 0;
    if (is_float) {
        tok.float_val = strtod(buf, NULL);
        if (errno == ERANGE) return error_token(L, "floating-point number out of range");
    } else {
        tok.int_val = strtoll(buf, NULL, 10);
        if (errno == ERANGE) return error_token(L, "integer out of 64-bit range");
    }
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
        if ((unsigned char)c == 0x01 || (unsigned char)c == 0x02) {
            /* these bytes are reserved as internal escaped-brace sentinels */
            free(buf);
            return error_token(L, "invalid control byte in string");
        }
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
    if (is_at_end(L)) {
        free(buf);
        return error_token(L, interp > 0
            ? "unclosed interpolation '{' in string (for a literal brace use \\{ )"
            : "unterminated string");
    }
    advance_ch(L); /* closing quote */
    PUSH('\0');
#undef PUSH
    Token tok = make_token(L, T_STRING);
    tok.str_val = buf;
    return tok;
}

/* Raw (verbatim) string: `...` — no escapes, no interpolation. Ideal for JSON,
 * regexes and paths. A literal backtick is written as `` (two backticks). */
static Token lex_raw_string(Lexer *L) {
    char *buf = xmalloc(64); size_t cap = 64, len = 0;
#define PUSHR(c) do { if (len + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); } buf[len++] = (c); } while (0)
    for (;;) {
        if (is_at_end(L)) { free(buf); return error_token(L, "unterminated raw string `...`"); }
        char c = advance_ch(L);
        if (c == '`') {
            if (peek_ch(L) == '`') { advance_ch(L); PUSHR('`'); continue; } /* `` -> literal ` */
            break;
        }
        if (c == '\n') L->line++;
        PUSHR(c);
    }
    PUSHR('\0');
#undef PUSHR
    Token tok = make_token(L, T_RAWSTRING);
    tok.str_val = buf;
    return tok;
}

static Token lex_next(Lexer *L) {
    skip_trivia(L);
    L->start = L->cur;
    if (is_at_end(L)) return make_token(L, T_EOF);

    char c = advance_ch(L);
    if (c == '\n') {
        L->line++;
        /* Line continuation: if the next non-blank content begins with `|>`,
           swallow this newline so a pipeline can break before each `|>`. */
        const char *p = L->cur;
        while (*p == ' ' || *p == '\t' || *p == '\r') p++;
        if (p[0] == '|' && p[1] == '>') { L->cur = p; return lex_next(L); }
        return make_token(L, T_NEWLINE);
    }
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
        case '.': return make_token(L, match_ch(L, '.') ? T_DOTDOT : T_DOT);
        case ':': return make_token(L, T_COLON);
        case ';': return make_token(L, T_NEWLINE); /* ';' separates statements like a newline */
        case '+': return make_token(L, T_PLUS);
        case '-': return make_token(L, T_MINUS);
        case '*': return make_token(L, T_STAR);
        case '/': return make_token(L, match_ch(L, '/') ? T_SLASHSLASH : T_SLASH);
        case '%': return make_token(L, T_PERCENT);
        case '=': return make_token(L, match_ch(L, '=') ? T_EQEQ : T_EQ);
        case '!': return match_ch(L, '=') ? make_token(L, T_BANGEQ)
                                          : error_token(L, "unexpected character '!' (use 'not')");
        case '<': return make_token(L, match_ch(L, '=') ? T_LTEQ : T_LT);
        case '>': return make_token(L, match_ch(L, '=') ? T_GTEQ : T_GT);
        case '?':
            if (match_ch(L, '?')) return make_token(L, T_QQ);
            if (match_ch(L, '.')) return make_token(L, T_QDOT);
            return error_token(L, "unexpected character '?'");
        case '|':
            if (match_ch(L, '>')) return make_token(L, T_PIPE);
            return error_token(L, "unexpected character '|' (did you mean 'or' or '|>'?)");
        case '"': return lex_string(L);
        case '`': return lex_raw_string(L);
    }
    return error_token(L, "unexpected character");
}

/* ===========================================================================
 * AST
 * ========================================================================= */
typedef enum {
    N_NIL, N_BOOL, N_INT, N_FLOAT, N_STR, N_INTERP, N_IDENT,
    N_LIST, N_MAP, N_UNARY, N_BINARY, N_LOGICAL,
    N_CALL, N_INDEX, N_MEMBER, N_ASSIGN,
    N_LET, N_BLOCK, N_IF, N_WHILE, N_FOR, N_FN, N_RETURN,
    N_BREAK, N_CONTINUE, N_EXPRSTMT, N_PROGRAM, N_IMPORT, N_TRY
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
#define MAX_PARSE_DEPTH 500

typedef struct {
    Interp *I;
    Lexer lex;
    Token cur;
    Token prev;
    bool had_error;
    int depth; /* recursion depth guard against pathological nesting */
} Parser;

static void parser_error_at(Parser *P, Token *t, const char *msg) {
    if (P->had_error) return; /* report first only */
    P->had_error = true;
    if (t->type == T_EOF)
        fprintf(stderr, "syntax error [%s:%d] at end of file: %s\n", P->I->path, t->line, msg);
    else if (t->type == T_ERROR)
        fprintf(stderr, "syntax error [%s:%d]: %.*s\n", P->I->path, t->line, t->length, t->start);
    else
        fprintf(stderr, "syntax error [%s:%d] at '%.*s': %s\n", P->I->path, t->line, t->length, t->start, msg);
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
    parser_error_at(P, &P->cur, "expected newline at end of statement");
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
    if (p_match(P, T_RAWSTRING)) {
        Node *n = new_node(N_STR, P->prev.line);
        n->literal = cstring_val(P->I, P->prev.str_val ? P->prev.str_val : "");
        return n;
    }
    if (p_match(P, T_IDENT)) {
        Node *n = new_node(N_IDENT, P->prev.line);
        n->name = copy_lexeme(&P->prev);
        return n;
    }
    if (p_match(P, T_LPAREN)) {
        Node *e = parse_expression(P);
        p_consume(P, T_RPAREN, "expected ')'");
        return e;
    }
    if (p_match(P, T_LBRACKET)) { /* list literal */
        Node *n = new_node(N_LIST, P->prev.line);
        skip_newlines(P);
        if (!p_check(P, T_RBRACKET)) {
            do {
                skip_newlines(P);
                if (p_check(P, T_RBRACKET)) break; /* allow a trailing comma */
                node_add(&n->items, &n->item_count, parse_expression(P));
                skip_newlines(P);
            } while (p_match(P, T_COMMA));
        }
        skip_newlines(P);
        p_consume(P, T_RBRACKET, "expected ']'");
        return n;
    }
    if (p_match(P, T_LBRACE)) { /* map literal: { key: val, "str": val } */
        Node *n = new_node(N_MAP, P->prev.line);
        skip_newlines(P);
        if (!p_check(P, T_RBRACE)) {
            do {
                skip_newlines(P);
                if (p_check(P, T_RBRACE)) break; /* allow a trailing comma */
                Node *key;
                if (p_check(P, T_IDENT)) {
                    p_advance(P);
                    /* bare identifier key: treat as a string literal key */
                    key = new_node(N_STR, P->prev.line);
                    key->literal = string_val(P->I, P->prev.start, P->prev.length);
                } else {
                    key = parse_expression(P);
                }
                p_consume(P, T_COLON, "expected ':' in map");
                skip_newlines(P);
                Node *val = parse_expression(P);
                node_add(&n->items, &n->item_count, key);
                node_add(&n->items2, &n->item2_count, val);
                skip_newlines(P);
            } while (p_match(P, T_COMMA));
        }
        skip_newlines(P);
        p_consume(P, T_RBRACE, "expected '}'");
        return n;
    }
    if (p_match(P, T_FN)) { /* anonymous function expression */
        Node *n = new_node(N_FN, P->prev.line);
        p_consume(P, T_LPAREN, "expected '(' after 'fn'");
        if (!p_check(P, T_RPAREN)) {
            do {
                if (p_check(P, T_RPAREN)) break; /* allow a trailing comma */
                p_consume(P, T_IDENT, "expected parameter name");
                Node *param = new_node(N_IDENT, P->prev.line);
                param->name = copy_lexeme(&P->prev);
                node_add(&n->items, &n->item_count, param);
            } while (p_match(P, T_COMMA));
        }
        p_consume(P, T_RPAREN, "expected ')'");
        skip_newlines(P);
        n->a = parse_block(P);
        return n;
    }
    parser_error_at(P, &P->cur, "expected an expression");
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
                    if (p_check(P, T_RPAREN)) break; /* allow a trailing comma */
                    node_add(&call->items, &call->item_count, parse_expression(P));
                    skip_newlines(P);
                } while (p_match(P, T_COMMA));
            }
            skip_newlines(P);
            p_consume(P, T_RPAREN, "expected ')' after arguments");
            expr = call;
        } else if (p_match(P, T_LBRACKET)) {
            Node *idx = new_node(N_INDEX, P->prev.line);
            idx->a = expr;
            idx->b = parse_expression(P);
            p_consume(P, T_RBRACKET, "expected ']'");
            expr = idx;
        } else if (p_match(P, T_DOT)) {
            p_consume(P, T_IDENT, "expected attribute name after '.'");
            Node *mem = new_node(N_MEMBER, P->prev.line);
            mem->a = expr;
            mem->name = copy_lexeme(&P->prev);
            expr = mem;
        } else if (p_match(P, T_QDOT)) {
            p_consume(P, T_IDENT, "expected attribute name after '?.'");
            Node *mem = new_node(N_MEMBER, P->prev.line);
            mem->a = expr;
            mem->name = copy_lexeme(&P->prev);
            mem->op = T_QDOT; /* optional chaining: nil if the object is nil */
            expr = mem;
        } else break;
    }
    return expr;
}

static Node *parse_unary(Parser *P) {
    if (++P->depth > MAX_PARSE_DEPTH) {
        parser_error_at(P, &P->cur, "expression nesting too deep");
        P->depth--;
        return new_node(N_NIL, P->cur.line);
    }
    Node *result;
    if (p_check(P, T_NOT) || p_check(P, T_MINUS)) {
        p_advance(P);
        Node *n = new_node(N_UNARY, P->prev.line);
        n->op = P->prev.type;
        n->a = parse_unary(P);
        result = n;
    } else {
        result = parse_postfix(P);
    }
    P->depth--;
    return result;
}

/* precedence climbing for binary operators */
static int bin_prec(TokType t) {
    switch (t) {
        case T_STAR: case T_SLASH: case T_SLASHSLASH: case T_PERCENT: return 7;
        case T_PLUS: case T_MINUS: return 6;
        case T_LT: case T_LTEQ: case T_GT: case T_GTEQ: return 5;
        case T_DOTDOT: return 5; /* range: above ==, below arithmetic (0..n-1 == 0..(n-1)) */
        case T_EQEQ: case T_BANGEQ: return 4;
        case T_AND: return 3;
        case T_OR:  return 2;
        case T_QQ:  return 1; /* null-coalescing */
        case T_PIPE: return 1; /* |> pipeline, loosest */
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
        if (op == T_PIPE) {
            /* x |> f(a) == f(x, a); x |> f == f(x). Threads the left value in as
               the FIRST argument of the call on the right. */
            Node *rhs = parse_unary(P); /* the call (or bare callee) */
            Node *call;
            if (rhs->type == N_CALL) {
                call = rhs; /* prepend `left` to the existing args */
                Node **ni = xmalloc(sizeof(Node *) * (size_t)(call->item_count + 1));
                ni[0] = left;
                for (int k = 0; k < call->item_count; k++) ni[k + 1] = call->items[k];
                free(call->items);
                call->items = ni; call->item_count++;
            } else {
                call = new_node(N_CALL, P->prev.line); /* bare callee: f(x) */
                call->a = rhs;
                node_add(&call->items, &call->item_count, left);
            }
            left = call;
            continue;
        }
        Node *right = parse_binary(P, prec + 1); /* left-assoc */
        Node *n = new_node((op == T_AND || op == T_OR || op == T_QQ) ? N_LOGICAL : N_BINARY, P->prev.line);
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
            parser_error_at(P, &P->prev, "invalid assignment target");
        }
        if (expr->type == N_MEMBER && expr->op == T_QDOT) {
            parser_error_at(P, &P->prev, "optional chaining '?.' is not allowed on an assignment target");
        }
        Node *n = new_node(N_ASSIGN, P->prev.line);
        n->a = expr; n->b = value;
        return n;
    }
    return expr;
}

static Node *parse_block(Parser *P) {
    p_consume(P, T_LBRACE, "expected '{'");
    Node *block = new_node(N_BLOCK, P->prev.line);
    skip_newlines(P);
    while (!p_check(P, T_RBRACE) && !p_check(P, T_EOF)) {
        node_add(&block->items, &block->item_count, parse_statement(P));
        skip_newlines(P);
    }
    p_consume(P, T_RBRACE, "expected '}'");
    return block;
}

static Node *parse_let(Parser *P) {
    int line = P->prev.line;
    p_consume(P, T_IDENT, "expected variable name after 'let'");
    Node *n = new_node(N_LET, line);
    n->name = copy_lexeme(&P->prev);
    if (p_match(P, T_EQ)) n->a = parse_expression(P);
    else n->a = NULL; /* declared nil */
    end_statement(P);
    return n;
}

static Node *parse_fn_decl(Parser *P) {
    int line = P->prev.line;
    p_consume(P, T_IDENT, "expected function name after 'fn'");
    Node *n = new_node(N_FN, line);
    n->name = copy_lexeme(&P->prev);
    p_consume(P, T_LPAREN, "expected '(' after the name");
    if (!p_check(P, T_RPAREN)) {
        do {
            if (p_check(P, T_RPAREN)) break; /* allow a trailing comma */
            p_consume(P, T_IDENT, "expected parameter name");
            Node *param = new_node(N_IDENT, P->prev.line);
            param->name = copy_lexeme(&P->prev);
            node_add(&n->items, &n->item_count, param);
        } while (p_match(P, T_COMMA));
    }
    p_consume(P, T_RPAREN, "expected ')'");
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
    skip_newlines(P);                    /* allow newline(s) before else */
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
    p_consume(P, T_IDENT, "expected variable name in 'for'");
    n->name = copy_lexeme(&P->prev);
    p_consume(P, T_IN, "expected 'in' in for loop");
    n->a = parse_expression(P);  /* iterable */
    skip_newlines(P);
    n->b = parse_block(P);
    return n;
}

/* match subj { p1, p2: {..}  _: {..} } desugars to a let + if/else == chain. */
static Node *parse_match(Parser *P) {
    int line = P->prev.line;
    Node *subj = parse_expression(P);
    skip_newlines(P);
    p_consume(P, T_LBRACE, "expected '{' after the match expression");

    Node *outer = new_node(N_BLOCK, line);
    Node *letm = new_node(N_LET, line); letm->name = "$m"; letm->a = subj;
    node_add(&outer->items, &outer->item_count, letm);

    Node *first_if = NULL, *last_if = NULL, *default_body = NULL;
    skip_newlines(P);
    while (!p_check(P, T_RBRACE) && !p_check(P, T_EOF)) {
        int arm_line = P->cur.line; /* diagnostics point at the arm, not 'match' */
        Token arm_tok = P->cur;
        /* A default arm must be the last arm: reject any arm after '_'. */
        if (default_body) {
            parser_error_at(P, &P->cur, "the default '_' arm must be last in a match");
            return outer;
        }
        bool is_default = false;
        Node *cond = NULL;
        for (;;) {
            if (p_check(P, T_IDENT) && P->cur.length == 1 && P->cur.start[0] == '_') {
                p_advance(P); is_default = true;
            } else {
                Node *pat = parse_expression(P);
                Node *eq = new_node(N_BINARY, arm_line); eq->op = T_EQEQ;
                Node *mref = new_node(N_IDENT, arm_line); mref->name = "$m";
                eq->a = mref; eq->b = pat;
                if (!cond) cond = eq;
                else { Node *orn = new_node(N_LOGICAL, arm_line); orn->op = T_OR; orn->a = cond; orn->b = eq; cond = orn; }
            }
            if (p_match(P, T_COMMA)) { skip_newlines(P); continue; }
            break;
        }
        /* '_' cannot be combined with other patterns: it would silently swallow them. */
        if (is_default && cond) {
            parser_error_at(P, &arm_tok, "the '_' pattern cannot be combined with other patterns");
            return outer;
        }
        p_consume(P, T_COLON, "expected ':' after the match pattern");
        skip_newlines(P);
        Node *body = parse_block(P);
        if (is_default) {
            default_body = body;
        } else {
            Node *ifn = new_node(N_IF, arm_line); ifn->a = cond; ifn->b = body;
            if (last_if) last_if->c = ifn; else first_if = ifn;
            last_if = ifn;
        }
        skip_newlines(P);
    }
    p_consume(P, T_RBRACE, "expected '}' at end of match");
    if (default_body) { if (last_if) last_if->c = default_body; else first_if = default_body; }
    if (first_if) node_add(&outer->items, &outer->item_count, first_if);
    return outer;
}

/* try { ... } catch e { ... } */
static Node *parse_try(Parser *P) {
    int line = P->prev.line;
    Node *n = new_node(N_TRY, line);
    skip_newlines(P);
    n->a = parse_block(P);                    /* try body */
    skip_newlines(P);
    p_consume(P, T_CATCH, "expected 'catch' after the try block");
    if (p_check(P, T_IDENT)) { p_advance(P); n->name = copy_lexeme(&P->prev); } /* error variable */
    else n->name = NULL;
    skip_newlines(P);
    n->b = parse_block(P);                    /* catch body */
    return n;
}

static Node *parse_statement(Parser *P) {
    skip_newlines(P);
    if (p_match(P, T_LET))    return parse_let(P);
    if (p_match(P, T_FN))     return parse_fn_decl(P);
    if (p_match(P, T_IF))     return parse_if(P);
    if (p_match(P, T_WHILE))  return parse_while(P);
    if (p_match(P, T_FOR))    return parse_for(P);
    if (p_match(P, T_MATCH))  return parse_match(P);
    if (p_match(P, T_TRY))    return parse_try(P);
    if (p_match(P, T_LBRACE)) { /* bare block — '{' already consumed, build it here */
        Node *block = new_node(N_BLOCK, P->prev.line);
        skip_newlines(P);
        while (!p_check(P, T_RBRACE) && !p_check(P, T_EOF)) {
            node_add(&block->items, &block->item_count, parse_statement(P));
            skip_newlines(P);
        }
        p_consume(P, T_RBRACE, "expected '}'");
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
        p_consume(P, T_STRING, "expected module path as a string");
        n->name = P->prev.str_val;
        /* optional: as <name>  (namespaced import) */
        if (p_check(P, T_IDENT) && P->cur.length == 2 && memcmp(P->cur.start, "as", 2) == 0) {
            p_advance(P);
            p_consume(P, T_IDENT, "expected a name after 'as'");
            n->literal = string_val(P->I, P->prev.start, P->prev.length);
        }
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
        /* Stop after the first parse error: the parser has no panic-mode
           synchronization, so continuing can spin if the cursor didn't advance. */
        if (P->had_error) break;
        skip_newlines(P);
    }
    return prog;
}

/* Parse a full expression from a standalone source string (used for
 * string interpolation). Returns NULL on error. */
static Node *parse_expr_from_source(Interp *I, const char *src) {
    Parser P;
    P.I = I; P.had_error = false; P.depth = 0;
    lex_init(&P.lex, I, src);
    p_advance(&P);
    skip_newlines(&P);
    Node *e = parse_expression(&P);
    skip_newlines(&P);
    if (!p_check(&P, T_EOF)) return NULL; /* trailing junk in {interpolation} */
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
            if (!expr) { parser_error_at(P, strtok, "invalid expression in interpolation"); expr = new_node(N_NIL, line); }
            /* concat is always string-typed here (starts as a string literal), so
             * '+' coerces the interpolated value via the VM's string concatenation.
             * No dependency on a (shadowable) global 'str'. */
            Node *b = new_node(N_BINARY, line); b->op = T_PLUS; b->a = concat; b->b = expr;
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
 * Table — open-addressing hash map with string keys, for global variables.
 * O(1) average lookup; this is what replaces the old linear-scan environment.
 * ========================================================================= */
typedef struct {
    char *key;       /* owned copy; NULL = empty slot */
    uint32_t hash;
    Value value;
} TableEntry;

struct Table {
    int count;
    int cap;
    TableEntry *entries;
    uint32_t gc_gen;  /* GC generation this table was last marked in (dedup) */
    uint32_t version; /* bumped on every rehash/grow (entries move) — invalidates GCache */
};

static uint32_t hash_string(const char *s, size_t n) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < n; i++) { h ^= (uint8_t)s[i]; h *= 16777619u; }
    return h;
}

static void table_init(Table *t) { t->count = 0; t->cap = 0; t->entries = NULL; t->gc_gen = 0; t->version = 0; }

static TableEntry *table_find(TableEntry *entries, int cap, const char *key, uint32_t hash) {
    uint32_t idx = hash & (uint32_t)(cap - 1);
    for (;;) {
        TableEntry *e = &entries[idx];
        if (e->key == NULL || (e->hash == hash && strcmp(e->key, key) == 0)) return e;
        idx = (idx + 1) & (uint32_t)(cap - 1);
    }
}

static void table_grow(Table *t) {
    int new_cap = t->cap < 8 ? 8 : t->cap * 2;
    TableEntry *ne = xmalloc(sizeof(TableEntry) * (size_t)new_cap);
    for (int i = 0; i < new_cap; i++) { ne[i].key = NULL; ne[i].hash = 0; ne[i].value = NIL_VAL; }
    for (int i = 0; i < t->cap; i++) {
        TableEntry *old = &t->entries[i];
        if (!old->key) continue;
        TableEntry *dst = table_find(ne, new_cap, old->key, old->hash);
        dst->key = old->key; dst->hash = old->hash; dst->value = old->value;
    }
    free(t->entries);
    t->entries = ne; t->cap = new_cap;
    t->version++; /* entries moved: any cached value-slot pointer is now stale */
}

/* Returns pointer to the stored value, or NULL if absent. */
static Value *table_get(Table *t, const char *key, uint32_t hash) {
    if (t->count == 0) return NULL;
    TableEntry *e = table_find(t->entries, t->cap, key, hash);
    return e->key ? &e->value : NULL;
}

/* Insert or update. Returns true if a new key was added. */
static bool table_set(Table *t, const char *key, uint32_t hash, Value value) {
    if (t->count + 1 > t->cap * 3 / 4) table_grow(t);
    TableEntry *e = table_find(t->entries, t->cap, key, hash);
    bool is_new = e->key == NULL;
    if (is_new) { e->key = strdup(key); e->hash = hash; t->count++; }
    e->value = value;
    return is_new;
}

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
static char *value_to_cstr_depth(Interp *I, Value v, int depth);

#define REPR_BUF 64 /* buffer size for the <fn ...>/<native ...>/<proto ...> reprs */
/* Cap nesting when stringifying so a cyclic structure (a list that contains
   itself) prints "..." instead of recursing until the C stack overflows. */
#define MAX_REPR_DEPTH 64

static void append_str(char **buf, size_t *len, size_t *cap, const char *s, size_t n) {
    if (*len + n + 1 > *cap) {
        while (*len + n + 1 > *cap) *cap = *cap < 16 ? 32 : *cap * 2;
        *buf = xrealloc(*buf, *cap);
    }
    memcpy(*buf + *len, s, n);
    *len += n;
    (*buf)[*len] = '\0';
}

/* Append a value's display form to buf; strings are quoted. Shared by the
 * list and map stringify branches of value_to_cstr. */
static void append_value_repr(Interp *I, char **buf, size_t *len, size_t *cap, Value v, int depth) {
    if (IS_STRING(v)) {
        ObjString *s = AS_STRING(v);
        append_str(buf, len, cap, "\"", 1);
        append_str(buf, len, cap, s->chars, (size_t)s->length);
        append_str(buf, len, cap, "\"", 1);
    } else {
        char *s = value_to_cstr_depth(I, v, depth + 1);
        append_str(buf, len, cap, s, strlen(s));
        free(s);
    }
}

static char *value_to_cstr(Interp *I, Value v) { return value_to_cstr_depth(I, v, 0); }
static char *value_to_cstr_depth(Interp *I, Value v, int depth) {
    char tmp[64];
    if (depth > MAX_REPR_DEPTH)
        return strdup(IS_LIST(v) ? "[...]" : IS_MAP(v) ? "{...}" : "...");
    switch (v.type) {
        case VAL_NIL: return strdup("nil");
        case VAL_BOOL: return strdup(v.as.b ? "true" : "false");
        case VAL_INT: snprintf(tmp, sizeof(tmp), "%lld", (long long)v.as.i); return strdup(tmp);
        case VAL_FLOAT: {
            double d = v.as.d;
            /* Range-check before the int64 cast: converting an out-of-range double
               to int64_t is UB, so the magnitude guard must come first. */
            if (isfinite(d) && fabs(d) < 1e15 && d == (double)(int64_t)d)
                snprintf(tmp, sizeof(tmp), "%.1f", d);
            else snprintf(tmp, sizeof(tmp), "%g", d);
            return strdup(tmp);
        }
        case VAL_OBJ:
            switch (OBJ_TYPE(v)) {
                case OBJ_STRING: return strdup(AS_STRING(v)->chars);
                case OBJ_NATIVE: {
                    char *b = xmalloc(REPR_BUF);
                    snprintf(b, REPR_BUF, "<native %s>", AS_NATIVE(v)->name);
                    return b;
                }
                case OBJ_CLOSURE: {
                    ObjProto *p = AS_CLOSURE(v)->proto;
                    char *b = xmalloc(REPR_BUF);
                    snprintf(b, REPR_BUF, "<fn %s>", p->name ? p->name : "anon");
                    return b;
                }
                case OBJ_PROTO: {
                    char *b = xmalloc(REPR_BUF);
                    snprintf(b, REPR_BUF, "<proto %s>", AS_PROTO(v)->name ? AS_PROTO(v)->name : "script");
                    return b;
                }
                case OBJ_UPVALUE: return strdup("<upvalue>");
                case OBJ_LIST: {
                    ObjList *l = AS_LIST(v);
                    char *b = xmalloc(2); b[0] = '['; b[1] = '\0';
                    size_t len = 1, cap = 2;
                    for (int i = 0; i < l->count; i++) {
                        if (i) append_str(&b, &len, &cap, ", ", 2);
                        append_value_repr(I, &b, &len, &cap, l->items[i], depth);
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
                        append_value_repr(I, &b, &len, &cap, m->entries[i].value, depth);
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
                case OBJ_CLOSURE: case OBJ_NATIVE: return "fn";
                case OBJ_PROTO: return "proto";
                case OBJ_UPVALUE: return "upvalue";
            }
    }
    return "?";
}

static double as_double(Value v) { return IS_INT(v) ? (double)AS_INT(v) : AS_FLOAT(v); }

/* ===========================================================================
 * Bytecode
 * ========================================================================= */
typedef enum {
    OP_CONSTANT, OP_NIL, OP_TRUE, OP_FALSE, OP_POP,
    OP_GET_LOCAL, OP_SET_LOCAL,
    OP_GET_GLOBAL, OP_DEFINE_GLOBAL, OP_SET_GLOBAL,
    OP_GET_UPVALUE, OP_SET_UPVALUE,
    OP_EQUAL, OP_NOT_EQUAL, OP_LESS, OP_LESS_EQUAL, OP_GREATER, OP_GREATER_EQUAL,
    OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_FLOORDIV, OP_MOD,
    OP_NEGATE, OP_NOT,
    OP_JUMP, OP_JUMP_IF_FALSE, OP_JUMP_IF_NOT_NIL, OP_JUMP_IF_NIL, OP_LOOP,
    OP_CALL, OP_CLOSURE, OP_CLOSE_UPVALUE, OP_RETURN,
    OP_BUILD_LIST, OP_BUILD_MAP, OP_RANGE,
    OP_GET_INDEX, OP_SET_INDEX, OP_GET_PROPERTY, OP_SET_PROPERTY,
    OP_ITER_SEQ, OP_FOR_NEXT, OP_IMPORT, OP_IMPORT_AS,
    OP_TRY_BEGIN, OP_TRY_END
} OpCode;

static void chunk_init(Chunk *c) { memset(c, 0, sizeof(Chunk)); }
/* Lazily allocate (zeroed) the per-constant global cache on first global access. */
static GCache *chunk_gcache(Chunk *c) {
    if (!c->gcache) {
        size_t n = c->const_count > 0 ? (size_t)c->const_count : 1;
        c->gcache = xmalloc(sizeof(GCache) * n);
        memset(c->gcache, 0, sizeof(GCache) * n);
    }
    return c->gcache;
}
static void chunk_write(Chunk *c, uint8_t byte, int line) {
    if (c->count + 1 > c->cap) {
        c->cap = c->cap < 8 ? 8 : c->cap * 2;
        c->code = xrealloc(c->code, (size_t)c->cap);
        c->lines = xrealloc(c->lines, sizeof(int) * (size_t)c->cap);
    }
    c->code[c->count] = byte; c->lines[c->count] = line; c->count++;
}
static int chunk_add_const(Chunk *c, Value v) {
    if (c->const_count + 1 > c->const_cap) {
        c->const_cap = c->const_cap < 8 ? 8 : c->const_cap * 2;
        c->constants = xrealloc(c->constants, sizeof(Value) * (size_t)c->const_cap);
    }
    c->constants[c->const_count] = v;
    return c->const_count++;
}

static ObjProto *new_proto(Interp *I) {
    ObjProto *p = (ObjProto *)alloc_obj(I, sizeof(ObjProto), OBJ_PROTO);
    p->arity = 0; p->upvalue_count = 0; p->name = NULL;
    p->module_globals = I->globals; /* globals of the module being compiled */
    p->source = I->current_source;  /* for source-line diagnostics */
    p->path = I->current_path;      /* for accurate per-frame file names */
    chunk_init(&p->chunk);
    return p;
}
static ObjClosure *new_closure(Interp *I, ObjProto *proto) {
    ObjUpvalue **ups = NULL;
    if (proto->upvalue_count) {
        ups = xmalloc(sizeof(ObjUpvalue *) * (size_t)proto->upvalue_count);
        for (int i = 0; i < proto->upvalue_count; i++) ups[i] = NULL;
    }
    ObjClosure *c = (ObjClosure *)alloc_obj(I, sizeof(ObjClosure), OBJ_CLOSURE);
    c->proto = proto; c->upvalues = ups; c->upvalue_count = proto->upvalue_count;
    return c;
}
static ObjUpvalue *new_upvalue(Interp *I, Value *slot) {
    ObjUpvalue *u = (ObjUpvalue *)alloc_obj(I, sizeof(ObjUpvalue), OBJ_UPVALUE);
    u->location = slot; u->closed = NIL_VAL; u->next = NULL;
    return u;
}

/* ===========================================================================
 * Compiler — walks the AST and emits bytecode. Locals resolve to stack slots
 * at compile time; globals to a hash table. Closures capture via upvalues.
 * ========================================================================= */
#define MAX_LOCALS 256

typedef struct { const char *name; int name_len; int depth; bool is_captured; } CompLocal;
typedef struct { uint8_t index; bool is_local; } CompUpvalue;

#define MAX_BREAKS_PER_LOOP 256

typedef struct LoopCtx {
    struct LoopCtx *enclosing;
    int continue_target;
    int body_local_base;
    int try_depth_base; /* open try-handler depth when the loop body begins */
    int breaks[MAX_BREAKS_PER_LOOP];
    int break_count;
} LoopCtx;

typedef struct Compiler {
    struct Compiler *enclosing;
    Interp *I;
    ObjProto *proto;
    CompLocal locals[MAX_LOCALS];
    int local_count;
    CompUpvalue upvalues[MAX_LOCALS];
    int scope_depth;
    LoopCtx *loop;
    int try_depth; /* number of try handlers currently open in this function */
    int expr_depth; /* compile_expr recursion depth, to bound deep flat chains */
    int stmt_depth; /* compile_stmt recursion depth, to bound deep nested blocks/if */
    bool *had_error; /* shared flag across nested compilers */
} Compiler;
/* Cap expression-tree depth at compile time: the iterative binary/pipe/index parse
 * loops can build very deep ASTs that the recursive compiler would otherwise walk
 * until the C stack overflows. Kept well below the C-stack ceiling — under
 * AddressSanitizer (large instrumented frames) compile_expr overflows ~1500 deep,
 * so 500 leaves a ~3x margin while staying far above any real expression. Matches
 * the parser's MAX_PARSE_DEPTH. */
#define MAX_EXPR_DEPTH 500
/* Same idea for nested statements (blocks/if/while/for/try): each nesting level
 * enters compile_stmt a few times, and the parser caps statement nesting too, but
 * deeply nested blocks still recurse the compiler. ~1500 nested `if`s overflow the
 * ASan C stack in compile_if/compile_stmt; 250 leaves a comfortable margin and is
 * far beyond any real nesting. */
#define MAX_STMT_DEPTH 250

static void compile_expr(Compiler *C, Node *n);
static void compile_stmt(Compiler *C, Node *n);
static void compile_function(Compiler *enc, Node *fn_node, int line);

static Chunk *cur_chunk(Compiler *C) { return &C->proto->chunk; }
static void emit_byte(Compiler *C, uint8_t b, int line) { chunk_write(cur_chunk(C), b, line); }
static void emit_u16(Compiler *C, int v, int line) {
    emit_byte(C, (uint8_t)((v >> 8) & 0xff), line);
    emit_byte(C, (uint8_t)(v & 0xff), line);
}
static int add_const(Compiler *C, Value v) {
    int idx = chunk_add_const(cur_chunk(C), v);
    if (idx > 0xffff) { *C->had_error = true; fprintf(stderr, "too many constants in a function\n"); return 0; }
    return idx;
}
static void emit_const(Compiler *C, Value v, int line) {
    emit_byte(C, OP_CONSTANT, line); emit_u16(C, add_const(C, v), line);
}
static int name_const(Compiler *C, const char *name) {
    return add_const(C, cstring_val(C->I, name));
}
static int emit_jump(Compiler *C, uint8_t op, int line) {
    emit_byte(C, op, line); emit_byte(C, 0xff, line); emit_byte(C, 0xff, line);
    return cur_chunk(C)->count - 2;
}
static void patch_jump(Compiler *C, int offset) {
    int jump = cur_chunk(C)->count - offset - 2;
    if (jump > 0xffff) { *C->had_error = true; fprintf(stderr, "jump too long\n"); }
    cur_chunk(C)->code[offset] = (uint8_t)((jump >> 8) & 0xff);
    cur_chunk(C)->code[offset + 1] = (uint8_t)(jump & 0xff);
}
static void emit_loop(Compiler *C, int loop_start, int line) {
    emit_byte(C, OP_LOOP, line);
    int offset = cur_chunk(C)->count - loop_start + 2;
    if (offset > 0xffff) { *C->had_error = true; fprintf(stderr, "loop too long\n"); }
    emit_byte(C, (uint8_t)((offset >> 8) & 0xff), line);
    emit_byte(C, (uint8_t)(offset & 0xff), line);
}

static void init_compiler(Compiler *C, Interp *I, Compiler *enclosing, const char *name, bool *had_error) {
    C->enclosing = enclosing; C->I = I; C->local_count = 0; C->scope_depth = 0;
    C->loop = NULL; C->try_depth = 0; C->expr_depth = 0; C->stmt_depth = 0; C->had_error = had_error;
    C->proto = new_proto(I);
    if (name) C->proto->name = strdup(name);
    /* slot 0 holds the executing closure itself */
    CompLocal *l = &C->locals[C->local_count++];
    l->name = ""; l->name_len = 0; l->depth = 0; l->is_captured = false;
}

static void begin_scope(Compiler *C) { C->scope_depth++; }
static void end_scope(Compiler *C, int line) {
    C->scope_depth--;
    while (C->local_count > 0 && C->locals[C->local_count - 1].depth > C->scope_depth) {
        emit_byte(C, C->locals[C->local_count - 1].is_captured ? OP_CLOSE_UPVALUE : OP_POP, line);
        C->local_count--;
    }
}
static int add_local(Compiler *C, const char *name, int len) {
    if (C->local_count >= MAX_LOCALS) { *C->had_error = true; fprintf(stderr, "too many local variables\n"); return -1; }
    CompLocal *l = &C->locals[C->local_count++];
    l->name = name; l->name_len = len; l->depth = -1; l->is_captured = false;
    return C->local_count - 1;
}
static void mark_initialized(Compiler *C) {
    if (C->scope_depth == 0) return;
    C->locals[C->local_count - 1].depth = C->scope_depth;
}
static int resolve_local(Compiler *C, const char *name, int len) {
    for (int i = C->local_count - 1; i >= 0; i--) {
        CompLocal *l = &C->locals[i];
        if (l->name_len == len && memcmp(l->name, name, (size_t)len) == 0) return i;
    }
    return -1;
}
static int add_upvalue(Compiler *C, uint8_t index, bool is_local) {
    int n = C->proto->upvalue_count;
    for (int i = 0; i < n; i++)
        if (C->upvalues[i].index == index && C->upvalues[i].is_local == is_local) return i;
    if (n >= MAX_LOCALS) { *C->had_error = true; fprintf(stderr, "too many upvalues\n"); return 0; }
    C->upvalues[n].index = index; C->upvalues[n].is_local = is_local;
    return C->proto->upvalue_count++;
}
static int resolve_upvalue(Compiler *C, const char *name, int len) {
    if (!C->enclosing) return -1;
    int local = resolve_local(C->enclosing, name, len);
    if (local != -1) {
        C->enclosing->locals[local].is_captured = true;
        return add_upvalue(C, (uint8_t)local, true);
    }
    int up = resolve_upvalue(C->enclosing, name, len);
    if (up != -1) return add_upvalue(C, (uint8_t)up, false);
    return -1;
}

static void named_get(Compiler *C, const char *name, int line) {
    int len = (int)strlen(name);
    int slot = resolve_local(C, name, len);
    if (slot != -1) { emit_byte(C, OP_GET_LOCAL, line); emit_byte(C, (uint8_t)slot, line); return; }
    int up = resolve_upvalue(C, name, len);
    if (up != -1) { emit_byte(C, OP_GET_UPVALUE, line); emit_byte(C, (uint8_t)up, line); return; }
    emit_byte(C, OP_GET_GLOBAL, line); emit_u16(C, name_const(C, name), line);
}
static void named_set(Compiler *C, const char *name, int line) {
    int len = (int)strlen(name);
    int slot = resolve_local(C, name, len);
    if (slot != -1) { emit_byte(C, OP_SET_LOCAL, line); emit_byte(C, (uint8_t)slot, line); return; }
    int up = resolve_upvalue(C, name, len);
    if (up != -1) { emit_byte(C, OP_SET_UPVALUE, line); emit_byte(C, (uint8_t)up, line); return; }
    emit_byte(C, OP_SET_GLOBAL, line); emit_u16(C, name_const(C, name), line);
}

static void compile_assign(Compiler *C, Node *n) {
    Node *t = n->a;
    if (t->type == N_IDENT) {
        compile_expr(C, n->b);
        named_set(C, t->name, n->line);
    } else if (t->type == N_INDEX) {
        compile_expr(C, t->a);
        compile_expr(C, t->b);
        compile_expr(C, n->b);
        emit_byte(C, OP_SET_INDEX, n->line);
    } else if (t->type == N_MEMBER) {
        compile_expr(C, t->a);
        compile_expr(C, n->b);
        emit_byte(C, OP_SET_PROPERTY, n->line);
        emit_u16(C, name_const(C, t->name), n->line);
    } else {
        *C->had_error = true;
        fprintf(stderr, "invalid assignment target\n");
    }
}

static void compile_expr(Compiler *C, Node *n) {
    /* Bound C-stack recursion: the iterative parse loops (binary chains, calls,
       indexing) can build ASTs far deeper than the parser's own MAX_PARSE_DEPTH,
       so guard here instead of overflowing the native stack. */
    if (++C->expr_depth > MAX_EXPR_DEPTH) {
        if (!*C->had_error) /* one diagnostic; suppress the cascade as we unwind */
            fprintf(stderr, "expression nested too deeply (max %d)\n", MAX_EXPR_DEPTH);
        *C->had_error = true;
        C->expr_depth--;
        return;
    }
    switch (n->type) {
        case N_NIL: emit_byte(C, OP_NIL, n->line); break;
        case N_BOOL: emit_byte(C, AS_BOOL(n->literal) ? OP_TRUE : OP_FALSE, n->line); break;
        case N_INT: case N_FLOAT: case N_STR: emit_const(C, n->literal, n->line); break;
        case N_IDENT: named_get(C, n->name, n->line); break;
        case N_LIST:
            if (n->item_count > 0xffff) { *C->had_error = true; fprintf(stderr, "too many elements in list literal (max 65535)\n"); break; }
            for (int i = 0; i < n->item_count; i++) compile_expr(C, n->items[i]);
            emit_byte(C, OP_BUILD_LIST, n->line); emit_u16(C, n->item_count, n->line);
            break;
        case N_MAP:
            if (n->item_count > 0xffff) { *C->had_error = true; fprintf(stderr, "too many pairs in map literal (max 65535)\n"); break; }
            for (int i = 0; i < n->item_count; i++) {
                compile_expr(C, n->items[i]);
                compile_expr(C, n->items2[i]);
            }
            emit_byte(C, OP_BUILD_MAP, n->line); emit_u16(C, n->item_count, n->line);
            break;
        case N_UNARY:
            compile_expr(C, n->a);
            emit_byte(C, n->op == T_MINUS ? OP_NEGATE : OP_NOT, n->line);
            break;
        case N_BINARY: {
            compile_expr(C, n->a);
            compile_expr(C, n->b);
            uint8_t op;
            switch (n->op) {
                case T_PLUS: op = OP_ADD; break;
                case T_MINUS: op = OP_SUB; break;
                case T_STAR: op = OP_MUL; break;
                case T_SLASH: op = OP_DIV; break;
                case T_SLASHSLASH: op = OP_FLOORDIV; break;
                case T_PERCENT: op = OP_MOD; break;
                case T_EQEQ: op = OP_EQUAL; break;
                case T_BANGEQ: op = OP_NOT_EQUAL; break;
                case T_LT: op = OP_LESS; break;
                case T_LTEQ: op = OP_LESS_EQUAL; break;
                case T_GT: op = OP_GREATER; break;
                case T_GTEQ: op = OP_GREATER_EQUAL; break;
                case T_DOTDOT: op = OP_RANGE; break;
                default: op = OP_NIL; *C->had_error = true; break;
            }
            emit_byte(C, op, n->line);
            break;
        }
        case N_LOGICAL: {
            compile_expr(C, n->a);
            if (n->op == T_QQ) { /* a ?? b : a if a is not nil, else b */
                int notNil = emit_jump(C, OP_JUMP_IF_NOT_NIL, n->line);
                emit_byte(C, OP_POP, n->line);  /* discard the nil */
                compile_expr(C, n->b);
                patch_jump(C, notNil);          /* keep a (non-nil) */
            } else if (n->op == T_AND) {
                int end = emit_jump(C, OP_JUMP_IF_FALSE, n->line);
                emit_byte(C, OP_POP, n->line);
                compile_expr(C, n->b);
                patch_jump(C, end);
            } else { /* or */
                int elseJ = emit_jump(C, OP_JUMP_IF_FALSE, n->line);
                int endJ = emit_jump(C, OP_JUMP, n->line);
                patch_jump(C, elseJ);
                emit_byte(C, OP_POP, n->line);
                compile_expr(C, n->b);
                patch_jump(C, endJ);
            }
            break;
        }
        case N_CALL: {
            if (n->item_count > 255) { *C->had_error = true; fprintf(stderr, "too many arguments in a call (max 255)\n"); break; }
            compile_expr(C, n->a);
            for (int i = 0; i < n->item_count; i++) compile_expr(C, n->items[i]);
            emit_byte(C, OP_CALL, n->line); emit_byte(C, (uint8_t)n->item_count, n->line);
            break;
        }
        case N_INDEX:
            compile_expr(C, n->a); compile_expr(C, n->b);
            emit_byte(C, OP_GET_INDEX, n->line);
            break;
        case N_MEMBER:
            compile_expr(C, n->a);
            if (n->op == T_QDOT) { /* a?.b : nil if a is nil, else a.b */
                int end = emit_jump(C, OP_JUMP_IF_NIL, n->line);  /* a nil -> keep nil, skip */
                emit_byte(C, OP_GET_PROPERTY, n->line); emit_u16(C, name_const(C, n->name), n->line);
                patch_jump(C, end);
            } else {
                emit_byte(C, OP_GET_PROPERTY, n->line); emit_u16(C, name_const(C, n->name), n->line);
            }
            break;
        case N_ASSIGN: compile_assign(C, n); break;
        case N_FN: compile_function(C, n, n->line); break;
        default: *C->had_error = true; fprintf(stderr, "expression cannot be compiled\n"); break;
    }
    C->expr_depth--;
}

static void compile_break(Compiler *C, int line) {
    if (!C->loop) { *C->had_error = true; fprintf(stderr, "'break' outside a loop\n"); return; }
    if (C->loop->break_count >= MAX_BREAKS_PER_LOOP) {
        *C->had_error = true; fprintf(stderr, "too many 'break' in a single loop\n"); return;
    }
    /* unwind any try handlers opened inside the loop body so the runtime
       handler stack is restored to the loop's baseline before leaving it */
    for (int d = C->try_depth; d > C->loop->try_depth_base; d--)
        emit_byte(C, OP_TRY_END, line);
    for (int i = C->local_count - 1; i >= C->loop->body_local_base; i--)
        emit_byte(C, C->locals[i].is_captured ? OP_CLOSE_UPVALUE : OP_POP, line);
    int j = emit_jump(C, OP_JUMP, line);
    C->loop->breaks[C->loop->break_count++] = j;
}
static void compile_continue(Compiler *C, int line) {
    if (!C->loop) { *C->had_error = true; fprintf(stderr, "'continue' outside a loop\n"); return; }
    /* unwind any try handlers opened inside the loop body (see compile_break) */
    for (int d = C->try_depth; d > C->loop->try_depth_base; d--)
        emit_byte(C, OP_TRY_END, line);
    for (int i = C->local_count - 1; i >= C->loop->body_local_base; i--)
        emit_byte(C, C->locals[i].is_captured ? OP_CLOSE_UPVALUE : OP_POP, line);
    emit_loop(C, C->loop->continue_target, line);
}

static void compile_if(Compiler *C, Node *n) {
    compile_expr(C, n->a);
    int elseJ = emit_jump(C, OP_JUMP_IF_FALSE, n->line);
    emit_byte(C, OP_POP, n->line);
    compile_stmt(C, n->b);
    int endJ = emit_jump(C, OP_JUMP, n->line);
    patch_jump(C, elseJ);
    emit_byte(C, OP_POP, n->line);
    if (n->c) compile_stmt(C, n->c);
    patch_jump(C, endJ);
}

static void compile_while(Compiler *C, Node *n) {
    LoopCtx loop; loop.enclosing = C->loop; loop.break_count = 0;
    int loop_start = cur_chunk(C)->count;
    loop.continue_target = loop_start;
    compile_expr(C, n->a);
    int exitJ = emit_jump(C, OP_JUMP_IF_FALSE, n->line);
    emit_byte(C, OP_POP, n->line);
    loop.body_local_base = C->local_count;
    loop.try_depth_base = C->try_depth;
    C->loop = &loop;
    compile_stmt(C, n->b);
    C->loop = loop.enclosing;
    emit_loop(C, loop_start, n->line);
    patch_jump(C, exitJ);
    emit_byte(C, OP_POP, n->line);
    for (int i = 0; i < loop.break_count; i++) patch_jump(C, loop.breaks[i]);
}

static void compile_for(Compiler *C, Node *n) {
    int line = n->line;
    begin_scope(C);
    /* hidden: sequence to iterate */
    compile_expr(C, n->a);
    emit_byte(C, OP_ITER_SEQ, line);
    int seq_slot = add_local(C, "$seq", 4); mark_initialized(C);
    /* hidden: index */
    emit_const(C, int_val(0), line);
    int idx_slot = add_local(C, "$idx", 4); mark_initialized(C);
    /* loop variable */
    emit_byte(C, OP_NIL, line);
    int var_slot = add_local(C, n->name, (int)strlen(n->name)); mark_initialized(C);

    LoopCtx loop; loop.enclosing = C->loop; loop.break_count = 0;
    int loop_start = cur_chunk(C)->count;
    loop.continue_target = loop_start;
    loop.body_local_base = C->local_count;
    loop.try_depth_base = C->try_depth;

    emit_byte(C, OP_FOR_NEXT, line);
    emit_byte(C, (uint8_t)seq_slot, line);
    emit_byte(C, (uint8_t)idx_slot, line);
    int exitJ = cur_chunk(C)->count;
    emit_byte(C, 0xff, line); emit_byte(C, 0xff, line);
    emit_byte(C, OP_SET_LOCAL, line); emit_byte(C, (uint8_t)var_slot, line);
    emit_byte(C, OP_POP, line);

    C->loop = &loop;
    compile_stmt(C, n->b);
    C->loop = loop.enclosing;

    emit_loop(C, loop_start, line);
    patch_jump(C, exitJ); /* FOR_NEXT exit target = here */
    for (int i = 0; i < loop.break_count; i++) patch_jump(C, loop.breaks[i]);
    end_scope(C, line); /* pops var, idx, seq */
}

static void compile_function(Compiler *enc, Node *fn_node, int line) {
    if (fn_node->item_count > 255) {
        *enc->had_error = true; fprintf(stderr, "too many parameters in a function (max 255)\n");
    }
    Compiler sub;
    init_compiler(&sub, enc->I, enc, fn_node->name, enc->had_error);
    sub.proto->arity = fn_node->item_count;
    begin_scope(&sub);
    for (int i = 0; i < fn_node->item_count; i++) {
        add_local(&sub, fn_node->items[i]->name, (int)strlen(fn_node->items[i]->name));
        mark_initialized(&sub);
    }
    /* body is an N_BLOCK; compile its statements in the function scope */
    Node *body = fn_node->a;
    for (int i = 0; i < body->item_count; i++) compile_stmt(&sub, body->items[i]);
    emit_byte(&sub, OP_NIL, line);
    emit_byte(&sub, OP_RETURN, line);

    ObjProto *p = sub.proto;
    emit_byte(enc, OP_CLOSURE, line);
    emit_u16(enc, add_const(enc, obj_val((Obj *)p)), line);
    for (int i = 0; i < p->upvalue_count; i++) {
        emit_byte(enc, sub.upvalues[i].is_local ? 1 : 0, line);
        emit_byte(enc, sub.upvalues[i].index, line);
    }
}

static void compile_stmt(Compiler *C, Node *n) {
    /* Bound C-stack recursion through nested blocks/if/while/for/try bodies, the
       statement-level analog of the compile_expr guard. */
    if (++C->stmt_depth > MAX_STMT_DEPTH) {
        if (!*C->had_error)
            fprintf(stderr, "statements nested too deeply (max %d)\n", MAX_STMT_DEPTH);
        *C->had_error = true;
        C->stmt_depth--;
        return;
    }
    switch (n->type) {
        case N_LET:
            if (n->a) compile_expr(C, n->a); else emit_byte(C, OP_NIL, n->line);
            if (C->scope_depth == 0) {
                emit_byte(C, OP_DEFINE_GLOBAL, n->line); emit_u16(C, name_const(C, n->name), n->line);
            } else {
                add_local(C, n->name, (int)strlen(n->name)); mark_initialized(C);
            }
            break;
        case N_FN:
            if (C->scope_depth == 0) {
                compile_function(C, n, n->line);
                emit_byte(C, OP_DEFINE_GLOBAL, n->line); emit_u16(C, name_const(C, n->name), n->line);
            } else {
                add_local(C, n->name, (int)strlen(n->name)); mark_initialized(C);
                compile_function(C, n, n->line);
            }
            break;
        case N_EXPRSTMT: compile_expr(C, n->a); emit_byte(C, OP_POP, n->line); break;
        case N_BLOCK:
            begin_scope(C);
            for (int i = 0; i < n->item_count; i++) compile_stmt(C, n->items[i]);
            end_scope(C, n->line);
            break;
        case N_IF: compile_if(C, n); break;
        case N_WHILE: compile_while(C, n); break;
        case N_FOR: compile_for(C, n); break;
        case N_RETURN:
            if (n->a) compile_expr(C, n->a); else emit_byte(C, OP_NIL, n->line);
            emit_byte(C, OP_RETURN, n->line);
            break;
        case N_BREAK: compile_break(C, n->line); break;
        case N_CONTINUE: compile_continue(C, n->line); break;
        case N_IMPORT:
            if (IS_STRING(n->literal)) { /* import "path" as name */
                emit_byte(C, OP_IMPORT_AS, n->line);
                emit_u16(C, name_const(C, n->name), n->line);
                emit_u16(C, add_const(C, n->literal), n->line);
            } else {
                emit_byte(C, OP_IMPORT, n->line);
                emit_u16(C, name_const(C, n->name), n->line);
            }
            break;
        case N_TRY: {
            int catchJump = emit_jump(C, OP_TRY_BEGIN, n->line); /* operand = offset to catch */
            C->try_depth++;                     /* handler is live for the try body */
            compile_stmt(C, n->a);              /* try body (its own scope) */
            C->try_depth--;                     /* handler consumed by OP_TRY_END / catch */
            emit_byte(C, OP_TRY_END, n->line);  /* pop handler on the success path */
            int endJump = emit_jump(C, OP_JUMP, n->line);
            patch_jump(C, catchJump);           /* catch starts here; error value is on the stack */
            begin_scope(C);
            if (n->name) { add_local(C, n->name, (int)strlen(n->name)); mark_initialized(C); }
            else emit_byte(C, OP_POP, n->line); /* discard the error value if unnamed */
            compile_stmt(C, n->b);              /* catch body */
            end_scope(C, n->line);
            patch_jump(C, endJump);
            break;
        }
        default: compile_expr(C, n); emit_byte(C, OP_POP, n->line); break;
    }
    C->stmt_depth--;
}

static ObjProto *compile_script(Interp *I, Node *program) {
    bool had_error = false;
    Compiler C;
    init_compiler(&C, I, NULL, NULL, &had_error);
    for (int i = 0; i < program->item_count; i++) compile_stmt(&C, program->items[i]);
    emit_byte(&C, OP_NIL, 1);
    emit_byte(&C, OP_RETURN, 1);
    if (had_error) return NULL;
    return C.proto;
}

/* ===========================================================================
 * Virtual machine — a stack-based bytecode interpreter.
 * ========================================================================= */
#define FRAMES_MAX 128
#define STACK_MAX (FRAMES_MAX * 256)

typedef struct { ObjClosure *closure; uint8_t *ip; Value *slots; } CallFrame;

#define MAX_HANDLERS 64
typedef struct { uint8_t *catch_ip; Value *stack_top; int frame_count; } TryHandler;

struct VM {
    Interp *I;
    VM *enclosing;     /* the VM that started this one (during imports) — a GC root chain */
    Value stack[STACK_MAX];
    Value *stack_top;
    CallFrame frames[FRAMES_MAX];
    int frame_count;
    ObjUpvalue *open_upvalues;
    TryHandler handlers[MAX_HANDLERS];
    int handler_count;
    jmp_buf trap;          /* landing pad for try/catch within this VM */
};

static int run_source(Interp *I, const char *src, const char *path);
static Value run_module_ns(Interp *I, char *path /* owned: freed by callee */);
static void vm_error(VM *vm, const char *fmt, ...);

/* Collapse '.', '..' and duplicate '/' segments lexically (no filesystem access).
 * Resolving the same module via different spellings then yields one stable string,
 * which keeps relative-path resolution IDEMPOTENT. Without this a relative cyclic
 * import ('./a.lq') accumulates a './' each level, so I->path keeps changing and the
 * strcmp-based cycle detector never matches — the cycle degrades to the depth-limit
 * backstop instead of an immediate 'cyclic import' error. Returns a fresh string. */
static char *normalize_path(const char *in) {
    size_t n = strlen(in);
    int absolute = (n > 0 && in[0] == '/');
    const char **seg = xmalloc(sizeof(char *) * (n + 1));
    size_t *seglen = xmalloc(sizeof(size_t) * (n + 1));
    int top = 0;
    const char *p = in;
    while (*p) {
        while (*p == '/') p++;                 /* skip separators (collapses '//') */
        if (!*p) break;
        const char *s = p;
        while (*p && *p != '/') p++;
        size_t len = (size_t)(p - s);
        if (len == 1 && s[0] == '.') continue;  /* '.' => current dir, drop */
        if (len == 2 && s[0] == '.' && s[1] == '.') {
            int top_is_dotdot = top > 0 && seglen[top - 1] == 2 &&
                                seg[top - 1][0] == '.' && seg[top - 1][1] == '.';
            if (top > 0 && !top_is_dotdot) { top--; continue; }   /* pop a real segment */
            if (absolute) continue;                                /* '..' at root: drop */
        }
        seg[top] = s; seglen[top] = len; top++;  /* keep (incl. a leading '..' when relative) */
    }
    char *out = xmalloc(n + 2);
    char *w = out;
    if (absolute) *w++ = '/';
    for (int i = 0; i < top; i++) {
        if (i) *w++ = '/';
        memcpy(w, seg[i], seglen[i]); w += seglen[i];
    }
    if (w == out) *w++ = '.';   /* everything cancelled out (relative) => "." */
    *w = '\0';
    free(seg); free(seglen);
    return out;
}

/* Resolve a module path relative to the directory of the importing file, so a
 * module's imports work no matter the current working directory (the pitfall
 * CWD-relative resolution creates). Absolute paths (leading '/') pass through.
 * `importer` is the path of the file doing the import (NULL at the REPL/top).
 * The result is lexically normalized so repeated resolution is stable (cycle
 * detection + the module cache both key on it). Returns a malloc'd string. */
static char *resolve_import_path(const char *importer, const char *module) {
    const char *slash = importer ? strrchr(importer, '/') : NULL;
    char *joined;
    if (module[0] == '/' || !slash) {            /* absolute, or importer in CWD */
        size_t n = strlen(module);
        joined = xmalloc(n + 1);
        memcpy(joined, module, n + 1);
    } else {
        size_t dirlen = (size_t)(slash - importer) + 1; /* keep the trailing '/' */
        size_t mlen = strlen(module);
        joined = xmalloc(dirlen + mlen + 1);
        memcpy(joined, importer, dirlen);
        memcpy(joined + dirlen, module, mlen + 1);
    }
    char *norm = normalize_path(joined);
    free(joined);
    return norm;
}

static inline void vm_push(VM *vm, Value v) {
    if (vm->stack_top >= vm->stack + STACK_MAX)
        vm_error(vm, "value stack overflow (expression or structure too large)");
    *vm->stack_top++ = v;
}
static inline Value vm_pop(VM *vm) { return *(--vm->stack_top); }
static inline Value vm_peek(VM *vm, int distance) { return vm->stack_top[-1 - distance]; }

static void vm_error(VM *vm, const char *fmt, ...) {
    CallFrame *frame = &vm->frames[vm->frame_count - 1];
    Chunk *ch = &frame->closure->proto->chunk;
    int idx = (int)(frame->ip - ch->code) - 1;
    int line = (idx >= 0 && idx < ch->count) ? ch->lines[idx] : 0;
    const char *fpath = frame->closure->proto->path ? frame->closure->proto->path
                      : (vm->I->path ? vm->I->path : "?");
    va_list ap; va_start(ap, fmt);
    int n = snprintf(vm->I->err_msg, sizeof(vm->I->err_msg),
                     "runtime error [%s:%d]: ", fpath, line);
    n = clamp_written(n, sizeof(vm->I->err_msg));
    vsnprintf(vm->I->err_msg + n, sizeof(vm->I->err_msg) - (size_t)n, fmt, ap);
    va_end(ap);
    raise_error(vm->I);
}

/* Routes to the nearest try/catch handler (longjmp to the VM trap, with the
 * error value on the stack) or to the top-level landing pad. */
static void close_upvalues(VM *vm, Value *last);

/* Line of the bytecode instruction the top frame is currently executing. */
static int vm_current_line(Interp *I) {
    VM *vm = I->vm;
    if (!vm || vm->frame_count <= 0) return 0;
    CallFrame *fr = &vm->frames[vm->frame_count - 1];
    Chunk *ch = &fr->closure->proto->chunk;
    int idx = (int)(fr->ip - ch->code) - 1;
    return (idx >= 0 && idx < ch->count) ? ch->lines[idx] : 0;
}
/* File path of the top frame's module (accurate across modules), else I->path. */
static const char *vm_current_path(Interp *I) {
    VM *vm = I->vm;
    if (vm && vm->frame_count > 0) {
        const char *p = vm->frames[vm->frame_count - 1].closure->proto->path;
        if (p) return p;
    }
    return I->path ? I->path : "?";
}

/* Copy source line `line` of `src` into buf as "  N | <text>" (empty if absent). */
static void capture_source_line(char *buf, size_t bufsz, const char *src, int line) {
    buf[0] = '\0';
    if (!src || line <= 0) return;
    const char *p = src;
    for (int l = 1; l < line && *p; ) { if (*p++ == '\n') l++; }
    const char *end = p;
    while (*end && *end != '\n') end++;
    snprintf(buf, bufsz, "  %d | %.*s", line, (int)(end - p), p);
}

/* Build a human-readable backtrace of the live call stack at error time, newest
 * frame first. Captured before the longjmp so the frames are still intact; the
 * top-level reporter prints it under the error message for an uncaught error. */
#define MAX_TRACE_FRAMES 16
static void build_backtrace(Interp *I) {
    I->err_trace[0] = '\0';
    I->err_source[0] = '\0';
    VM *vm = I->vm;
    if (!vm || vm->frame_count <= 0) return;
    size_t off = 0;
    int shown = 0;
    for (int i = vm->frame_count - 1; i >= 0 && shown < MAX_TRACE_FRAMES; i--, shown++) {
        CallFrame *fr = &vm->frames[i];
        Chunk *ch = &fr->closure->proto->chunk;
        int idx = (int)(fr->ip - ch->code) - 1;
        int line = (idx >= 0 && idx < ch->count) ? ch->lines[idx] : 0;
        if (i == vm->frame_count - 1) /* the innermost frame: where the error is */
            capture_source_line(I->err_source, sizeof(I->err_source), fr->closure->proto->source, line);
        const char *name = (i == 0) ? "<script>"
                         : (fr->closure->proto->name ? fr->closure->proto->name : "<anonymous>");
        const char *fpath = fr->closure->proto->path ? fr->closure->proto->path
                          : (I->path ? I->path : "?");
        int w = snprintf(I->err_trace + off, sizeof(I->err_trace) - off,
                         "  at %s (%s:%d)\n", name, fpath, line);
        if (w < 0) break;
        off += (size_t)w;
        if (off >= sizeof(I->err_trace)) { off = sizeof(I->err_trace) - 1; break; }
    }
    if (vm->frame_count > MAX_TRACE_FRAMES && off < sizeof(I->err_trace))
        snprintf(I->err_trace + off, sizeof(I->err_trace) - off,
                 "  ... %d more frame(s)\n", vm->frame_count - MAX_TRACE_FRAMES);
}

static void raise_error(Interp *I) {
    I->has_error = true;
    VM *vm = I->vm;
    if (vm && vm->handler_count > 0) {
        TryHandler h = vm->handlers[--vm->handler_count];
        close_upvalues(vm, h.stack_top);
        vm->frame_count = h.frame_count;
        vm->stack_top = h.stack_top;
        Value errval = cstring_val(I, I->err_msg);
        vm->frames[vm->frame_count - 1].ip = h.catch_ip;
        *vm->stack_top++ = errval;   /* push error (room guaranteed: we just unwound) */
        longjmp(vm->trap, 1);
    }
    build_backtrace(I);              /* uncaught: capture the stack for the reporter */
    longjmp(I->err_jmp, 1);
}

static ObjUpvalue *capture_upvalue(VM *vm, Value *local) {
    ObjUpvalue *prev = NULL, *cur = vm->open_upvalues;
    while (cur && cur->location > local) { prev = cur; cur = cur->next; }
    if (cur && cur->location == local) return cur;
    ObjUpvalue *created = new_upvalue(vm->I, local);
    created->next = cur;
    if (prev) prev->next = created; else vm->open_upvalues = created;
    return created;
}
static void close_upvalues(VM *vm, Value *last) {
    while (vm->open_upvalues && vm->open_upvalues->location >= last) {
        ObjUpvalue *u = vm->open_upvalues;
        u->closed = *u->location;
        u->location = &u->closed;
        vm->open_upvalues = u->next;
    }
}

static void vm_call_closure(VM *vm, ObjClosure *cl, int argc) {
    if (argc != cl->proto->arity)
        vm_error(vm, "%s() expects %d arguments, got %d",
                 cl->proto->name ? cl->proto->name : "fn", cl->proto->arity, argc);
    if (vm->frame_count == FRAMES_MAX) vm_error(vm, "call stack overflow (recursion too deep)");
    CallFrame *frame = &vm->frames[vm->frame_count++];
    frame->closure = cl;
    frame->ip = cl->proto->chunk.code;
    frame->slots = vm->stack_top - argc - 1;
}
static void vm_call_value(VM *vm, Value callee, int argc) {
    if (IS_CLOSURE(callee)) { vm_call_closure(vm, AS_CLOSURE(callee), argc); return; }
    if (IS_NATIVE(callee)) {
        ObjNative *nat = AS_NATIVE(callee);
        if (nat->arity >= 0 && nat->arity != argc)
            vm_error(vm, "%s() expects %d arguments, got %d", nat->name, nat->arity, argc);
        Value result = nat->fn(vm->I, argc, vm->stack_top - argc);
        vm->stack_top -= argc + 1;
        vm_push(vm, result);
        return;
    }
    vm_error(vm, "value of type %s is not callable", type_name(callee));
}

/* numeric / string / list binary operators (equality handled inline) */
static void vm_binary(VM *vm, OpCode op) {
    Value b = vm_pop(vm), a = vm_pop(vm);
    Interp *I = vm->I;
    if (op == OP_ADD && (IS_STRING(a) || IS_STRING(b))) {
        char *sa = value_to_cstr(I, a), *sb = value_to_cstr(I, b);
        size_t la = strlen(sa), lb = strlen(sb);
        char *r = xmalloc(la + lb + 1);
        memcpy(r, sa, la); memcpy(r + la, sb, lb); r[la + lb] = '\0';
        Value out = string_val(I, r, (int)(la + lb));
        free(sa); free(sb); free(r);
        vm_push(vm, out); return;
    }
    if (op == OP_ADD && IS_LIST(a) && IS_LIST(b)) {
        ObjList *r = new_list(I);
        for (int i = 0; i < AS_LIST(a)->count; i++) list_push(r, AS_LIST(a)->items[i]);
        for (int i = 0; i < AS_LIST(b)->count; i++) list_push(r, AS_LIST(b)->items[i]);
        vm_push(vm, obj_val((Obj *)r)); return;
    }
    if (!IS_NUM(a) || !IS_NUM(b))
        vm_error(vm, "unsupported arithmetic operator between %s and %s", type_name(a), type_name(b));
    bool both_int = IS_INT(a) && IS_INT(b);
    switch (op) {
        case OP_ADD:
            if (both_int) { int64_t r; if (__builtin_add_overflow(AS_INT(a), AS_INT(b), &r)) vm_error(vm, "integer overflow in '+'"); vm_push(vm, int_val(r)); }
            else vm_push(vm, float_val(as_double(a) + as_double(b)));
            break;
        case OP_SUB:
            if (both_int) { int64_t r; if (__builtin_sub_overflow(AS_INT(a), AS_INT(b), &r)) vm_error(vm, "integer overflow in '-'"); vm_push(vm, int_val(r)); }
            else vm_push(vm, float_val(as_double(a) - as_double(b)));
            break;
        case OP_MUL:
            if (both_int) { int64_t r; if (__builtin_mul_overflow(AS_INT(a), AS_INT(b), &r)) vm_error(vm, "integer overflow in '*'"); vm_push(vm, int_val(r)); }
            else vm_push(vm, float_val(as_double(a) * as_double(b)));
            break;
        case OP_DIV: {
            double db = as_double(b);
            if (db == 0.0) vm_error(vm, "division by zero");
            vm_push(vm, float_val(as_double(a) / db)); break;
        }
        case OP_FLOORDIV:
            if (both_int) {
                if (AS_INT(b) == 0) vm_error(vm, "division by zero");
                if (AS_INT(a) == INT64_MIN && AS_INT(b) == -1) { vm_push(vm, int_val(INT64_MIN)); break; } /* avoid overflow UB */
                int64_t q = AS_INT(a) / AS_INT(b);
                if ((AS_INT(a) % AS_INT(b) != 0) && ((AS_INT(a) < 0) != (AS_INT(b) < 0))) q--;
                vm_push(vm, int_val(q));
            } else {
                double db = as_double(b);
                if (db == 0.0) vm_error(vm, "division by zero");
                vm_push(vm, float_val(floor(as_double(a) / db)));
            }
            break;
        case OP_MOD:
            if (both_int) {
                if (AS_INT(b) == 0) vm_error(vm, "modulo by zero");
                if (AS_INT(a) == INT64_MIN && AS_INT(b) == -1) { vm_push(vm, int_val(0)); break; } /* avoid overflow UB */
                vm_push(vm, int_val(AS_INT(a) % AS_INT(b)));
            } else vm_push(vm, float_val(fmod(as_double(a), as_double(b))));
            break;
        case OP_LESS:          vm_push(vm, bool_val(as_double(a) <  as_double(b))); break;
        case OP_LESS_EQUAL:    vm_push(vm, bool_val(as_double(a) <= as_double(b))); break;
        case OP_GREATER:       vm_push(vm, bool_val(as_double(a) >  as_double(b))); break;
        case OP_GREATER_EQUAL: vm_push(vm, bool_val(as_double(a) >= as_double(b))); break;
        default: vm_error(vm, "unknown binary operator");
    }
}

/* convert an iterable into a list to walk (for-in) */
static Value iter_seq(VM *vm, Value it) {
    if (IS_LIST(it)) return it;
    Interp *I = vm->I;
    if (IS_MAP(it)) {
        ObjList *l = new_list(I);
        ObjMap *m = AS_MAP(it);
        for (int i = 0; i < m->count; i++) list_push(l, cstring_val(I, m->entries[i].key));
        return obj_val((Obj *)l);
    }
    if (IS_STRING(it)) {
        ObjList *l = new_list(I);
        ObjString *s = AS_STRING(it);
        for (int i = 0; i < s->length; i++) list_push(l, string_val(I, s->chars + i, 1));
        return obj_val((Obj *)l);
    }
    vm_error(vm, "type %s is not iterable", type_name(it));
    return NIL_VAL;
}

/* Run until the current frame count drops back to `stop_at` (0 for the top-level
 * script; a higher baseline for a re-entrant vm_invoke). */
/* ===========================================================================
 * Garbage collector — precise mark & sweep over the object arena.
 *
 * Collection runs only at the VM dispatch safe-point (top of run_vm's loop),
 * where every live value is reachable from a root: the value stacks and call
 * frames of the active VM chain, the current module globals, and the open
 * upvalues. Because GC never fires in the middle of a native, a native's C-local
 * temporaries can't be collected out from under it — the one exception is the
 * higher-order natives (map/filter/reduce) that re-enter the VM via a callback,
 * which root their accumulator on the value stack while the callback runs.
 * ========================================================================= */
static void gc_push_gray(Interp *I, Obj *o) {
    if (I->gray_count + 1 > I->gray_cap) {
        I->gray_cap = I->gray_cap < 64 ? 64 : I->gray_cap * 2;
        I->gray = xrealloc(I->gray, sizeof(Obj *) * (size_t)I->gray_cap);
    }
    I->gray[I->gray_count++] = o;
}
static void gc_mark_obj(Interp *I, Obj *o) {
    if (o == NULL || o->marked) return;
    o->marked = true;
    gc_push_gray(I, o);
}
static void gc_mark_value(Interp *I, Value v) {
    if (IS_OBJ(v)) gc_mark_obj(I, AS_OBJ(v));
}
/* Mark a globals table's values, at most once per collection cycle. */
static void gc_mark_table(Interp *I, Table *t) {
    if (t == NULL || t->gc_gen == I->gc_gen) return;
    t->gc_gen = I->gc_gen;
    for (int i = 0; i < t->cap; i++)
        if (t->entries[i].key) gc_mark_value(I, t->entries[i].value);
}
/* Visit everything an object references (gray -> black). */
static void gc_blacken(Interp *I, Obj *o) {
    switch (o->type) {
        case OBJ_LIST: {
            ObjList *l = (ObjList *)o;
            for (int i = 0; i < l->count; i++) gc_mark_value(I, l->items[i]);
            break;
        }
        case OBJ_MAP: {
            ObjMap *m = (ObjMap *)o;
            for (int i = 0; i < m->count; i++) gc_mark_value(I, m->entries[i].value);
            break;
        }
        case OBJ_PROTO: {
            ObjProto *p = (ObjProto *)o;
            for (int i = 0; i < p->chunk.const_count; i++) gc_mark_value(I, p->chunk.constants[i]);
            gc_mark_table(I, p->module_globals); /* keep module state alive for its closures */
            break;
        }
        case OBJ_CLOSURE: {
            ObjClosure *c = (ObjClosure *)o;
            gc_mark_obj(I, (Obj *)c->proto);
            for (int i = 0; i < c->upvalue_count; i++) gc_mark_obj(I, (Obj *)c->upvalues[i]);
            break;
        }
        case OBJ_UPVALUE:
            gc_mark_value(I, ((ObjUpvalue *)o)->closed); /* NIL while open, so always safe */
            break;
        case OBJ_STRING:
        case OBJ_NATIVE:
            break; /* leaf objects */
    }
}
static void gc_mark_roots(Interp *I) {
    gc_mark_table(I, I->globals);
    gc_mark_table(I, I->module_cache); /* cached module namespaces (and, through them, module state) */
    for (VM *vm = I->vm; vm; vm = vm->enclosing) {
        for (Value *s = vm->stack; s < vm->stack_top; s++) gc_mark_value(I, *s);
        for (int i = 0; i < vm->frame_count; i++) gc_mark_obj(I, (Obj *)vm->frames[i].closure);
        for (ObjUpvalue *u = vm->open_upvalues; u; u = u->next) gc_mark_obj(I, (Obj *)u);
    }
}
static void gc_free_obj(Obj *o) {
    switch (o->type) {
        case OBJ_STRING: free(((ObjString *)o)->chars); break;
        case OBJ_LIST:   free(((ObjList *)o)->items); break;
        case OBJ_MAP: {
            ObjMap *m = (ObjMap *)o;
            for (int i = 0; i < m->count; i++) free(m->entries[i].key);
            free(m->entries);
            break;
        }
        case OBJ_PROTO: {
            ObjProto *p = (ObjProto *)o;
            free(p->chunk.code); free(p->chunk.lines); free(p->chunk.constants);
            free(p->chunk.gcache);
            free(p->name); /* module_globals is borrowed, not owned */
            break;
        }
        case OBJ_CLOSURE: free(((ObjClosure *)o)->upvalues); break; /* proto is a separate obj */
        case OBJ_UPVALUE:
        case OBJ_NATIVE:
            break; /* native name is a static/borrowed string */
    }
    free(o);
}
static void gc_sweep(Interp *I) {
    size_t live = 0;
    Obj *prev = NULL, *o = I->arena;
    while (o) {
        if (o->marked) {
            o->marked = false;          /* reset for the next cycle */
            prev = o; o = o->arena_next; live++;
        } else {
            Obj *dead = o;
            o = o->arena_next;
            if (prev) prev->arena_next = o; else I->arena = o;
            gc_free_obj(dead);
        }
    }
    I->obj_count = live;
}
static void collect_garbage(Interp *I) {
    I->gc_gen++;
    I->gray_count = 0;
    gc_mark_roots(I);
    while (I->gray_count > 0) gc_blacken(I, I->gray[--I->gray_count]);
    gc_sweep(I);
    size_t floor = 1u << 14;
    I->next_gc = (I->obj_count > floor / 2) ? I->obj_count * 2 : floor;
    /* clear the flag (or keep collecting every instruction under stress) */
    I->gc_pending = I->gc_stress;
}

static void run_vm(VM *vm, int stop_at) {
    /* Save the enclosing try landing pad so nested vm_invoke runs nest safely. */
    jmp_buf prev_trap;
    memcpy(prev_trap, vm->trap, sizeof(jmp_buf));
    CallFrame *frame;
    if (setjmp(vm->trap)) {
        /* raise_error landed us here after setting up a catch. If the catch
         * belongs to an outer run_vm invocation, re-propagate to it. */
        if (vm->frame_count <= stop_at) {
            memcpy(vm->trap, prev_trap, sizeof(jmp_buf));
            longjmp(vm->trap, 1);
        }
        /* The error was handled by a catch in this invocation and execution
         * resumes below; clear the sticky flag so it reflects "recovered". */
        vm->I->has_error = false;
    }
    frame = &vm->frames[vm->frame_count - 1];
#define READ_BYTE() (*frame->ip++)
#define READ_U16() (frame->ip += 2, (uint16_t)((frame->ip[-2] << 8) | frame->ip[-1]))
#define READ_CONST() (frame->closure->proto->chunk.constants[READ_U16()])
    for (;;) {
        if (vm->I->gc_pending) collect_garbage(vm->I);
        uint8_t inst = READ_BYTE();
        switch (inst) {
            case OP_CONSTANT: vm_push(vm, READ_CONST()); break;
            case OP_NIL: vm_push(vm, NIL_VAL); break;
            case OP_TRUE: vm_push(vm, bool_val(true)); break;
            case OP_FALSE: vm_push(vm, bool_val(false)); break;
            case OP_POP: vm_pop(vm); break;
            case OP_GET_LOCAL: { uint8_t s = READ_BYTE(); vm_push(vm, frame->slots[s]); break; }
            case OP_SET_LOCAL: { uint8_t s = READ_BYTE(); frame->slots[s] = vm_peek(vm, 0); break; }
            case OP_GET_GLOBAL: {
                uint16_t ci = READ_U16();
                Chunk *ch = &frame->closure->proto->chunk;
                Table *g = frame->closure->proto->module_globals;
                GCache *c = &chunk_gcache(ch)[ci];
                Value *v;
                if (c->table == g && c->version == g->version) {
                    v = c->slot;                 /* fast path: cached value slot */
                } else {
                    ObjString *name = AS_STRING(ch->constants[ci]);
                    v = table_get(g, name->chars, hash_string(name->chars, (size_t)name->length));
                    if (!v) vm_error(vm, "undefined name: '%s'", name->chars);
                    c->table = g; c->version = g->version; c->slot = v;
                }
                vm_push(vm, *v);
                break;
            }
            case OP_DEFINE_GLOBAL: {
                ObjString *name = AS_STRING(READ_CONST());
                uint32_t h = hash_string(name->chars, (size_t)name->length);
                Table *g = frame->closure->proto->module_globals;
                table_set(g, name->chars, h, vm_peek(vm, 0));
                /* record module-level definitions so `import .. as` can export exactly
                   the names the module defines (even ones that shadow a built-in). */
                if (vm->I->module_defines)
                    table_set(vm->I->module_defines, name->chars, h, NIL_VAL);
                vm_pop(vm);
                break;
            }
            case OP_SET_GLOBAL: {
                uint16_t ci = READ_U16();
                Chunk *ch = &frame->closure->proto->chunk;
                Table *g = frame->closure->proto->module_globals;
                GCache *c = &chunk_gcache(ch)[ci];
                Value *v;
                if (c->table == g && c->version == g->version) {
                    v = c->slot;
                } else {
                    ObjString *name = AS_STRING(ch->constants[ci]);
                    v = table_get(g, name->chars, hash_string(name->chars, (size_t)name->length));
                    if (!v) vm_error(vm, "assignment to undeclared variable '%s' (use 'let')", name->chars);
                    c->table = g; c->version = g->version; c->slot = v;
                }
                *v = vm_peek(vm, 0); /* write through the cached slot */
                break;
            }
            case OP_GET_UPVALUE: { uint8_t i = READ_BYTE(); vm_push(vm, *frame->closure->upvalues[i]->location); break; }
            case OP_SET_UPVALUE: { uint8_t i = READ_BYTE(); *frame->closure->upvalues[i]->location = vm_peek(vm, 0); break; }
            case OP_EQUAL: { Value b = vm_pop(vm), a = vm_pop(vm); vm_push(vm, bool_val(values_equal(a, b))); break; }
            case OP_NOT_EQUAL: { Value b = vm_pop(vm), a = vm_pop(vm); vm_push(vm, bool_val(!values_equal(a, b))); break; }
            case OP_LESS: case OP_LESS_EQUAL: case OP_GREATER: case OP_GREATER_EQUAL:
            case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_FLOORDIV: case OP_MOD:
                vm_binary(vm, (OpCode)inst); break;
            case OP_NEGATE: {
                Value v = vm_pop(vm);
                if (IS_INT(v)) {
                    if (AS_INT(v) == INT64_MIN) vm_error(vm, "integer overflow in negation");
                    vm_push(vm, int_val(-AS_INT(v)));
                } else if (IS_FLOAT(v)) vm_push(vm, float_val(-AS_FLOAT(v)));
                else vm_error(vm, "'-' requires a number, got %s", type_name(v));
                break;
            }
            case OP_NOT: vm_push(vm, bool_val(!is_truthy(vm_pop(vm)))); break;
            case OP_JUMP: { uint16_t off = READ_U16(); frame->ip += off; break; }
            case OP_JUMP_IF_FALSE: { uint16_t off = READ_U16(); if (!is_truthy(vm_peek(vm, 0))) frame->ip += off; break; }
            case OP_JUMP_IF_NOT_NIL: { uint16_t off = READ_U16(); if (!IS_NIL(vm_peek(vm, 0))) frame->ip += off; break; }
            case OP_JUMP_IF_NIL: { uint16_t off = READ_U16(); if (IS_NIL(vm_peek(vm, 0))) frame->ip += off; break; }
            case OP_LOOP: { uint16_t off = READ_U16(); frame->ip -= off; break; }
            case OP_CALL: {
                int argc = READ_BYTE();
                vm_call_value(vm, vm_peek(vm, argc), argc);
                frame = &vm->frames[vm->frame_count - 1];
                break;
            }
            case OP_CLOSURE: {
                ObjProto *proto = AS_PROTO(READ_CONST());
                ObjClosure *cl = new_closure(vm->I, proto);
                vm_push(vm, obj_val((Obj *)cl));
                for (int i = 0; i < cl->upvalue_count; i++) {
                    uint8_t is_local = READ_BYTE();
                    uint8_t index = READ_BYTE();
                    if (is_local) cl->upvalues[i] = capture_upvalue(vm, frame->slots + index);
                    else cl->upvalues[i] = frame->closure->upvalues[index];
                }
                break;
            }
            case OP_CLOSE_UPVALUE: close_upvalues(vm, vm->stack_top - 1); vm_pop(vm); break;
            case OP_RETURN: {
                Value result = vm_pop(vm);
                close_upvalues(vm, frame->slots);
                vm->frame_count--;
                /* drop any try handlers belonging to the frame we're leaving
                 * (e.g. `return` from inside a try block) */
                while (vm->handler_count > 0 && vm->handlers[vm->handler_count - 1].frame_count > vm->frame_count)
                    vm->handler_count--;
                vm->stack_top = frame->slots;   /* discard callee + locals */
                vm_push(vm, result);            /* leave result on the stack */
                if (vm->frame_count == stop_at) { memcpy(vm->trap, prev_trap, sizeof(jmp_buf)); return; }
                frame = &vm->frames[vm->frame_count - 1];
                break;
            }
            case OP_TRY_BEGIN: {
                uint16_t off = READ_U16();
                if (vm->handler_count >= MAX_HANDLERS) vm_error(vm, "too many nested 'try' blocks");
                TryHandler *h = &vm->handlers[vm->handler_count++];
                h->catch_ip = frame->ip + off;
                h->stack_top = vm->stack_top;
                h->frame_count = vm->frame_count;
                break;
            }
            case OP_TRY_END:
                if (vm->handler_count > 0) vm->handler_count--;
                break;
            case OP_RANGE: {
                Value bv = vm_pop(vm), av = vm_pop(vm);
                if (!IS_INT(av) || !IS_INT(bv)) vm_error(vm, "range '..' requires two ints");
                int64_t a = AS_INT(av), b = AS_INT(bv);
                if (b >= a && (uint64_t)(b - a) > 100000000ULL)
                    vm_error(vm, "range '..' too large (> 100M elements)");
                ObjList *l = new_list(vm->I);
                for (int64_t i = a; i <= b; i++) list_push(l, int_val(i)); /* inclusive; empty if a > b */
                vm_push(vm, obj_val((Obj *)l));
                break;
            }
            case OP_BUILD_LIST: {
                int count = READ_U16();
                ObjList *l = new_list(vm->I);
                for (int i = 0; i < count; i++) list_push(l, vm->stack_top[-count + i]);
                vm->stack_top -= count;
                vm_push(vm, obj_val((Obj *)l));
                break;
            }
            case OP_BUILD_MAP: {
                int count = READ_U16();
                ObjMap *m = new_map(vm->I);
                Value *base = vm->stack_top - count * 2;
                for (int i = 0; i < count; i++) {
                    char *key = value_to_cstr(vm->I, base[i * 2]);
                    map_set(m, key, base[i * 2 + 1]);
                    free(key);
                }
                vm->stack_top -= count * 2;
                vm_push(vm, obj_val((Obj *)m));
                break;
            }
            case OP_GET_INDEX: {
                Value idx = vm_pop(vm), coll = vm_pop(vm);
                if (IS_LIST(coll)) {
                    if (!IS_INT(idx)) vm_error(vm, "list index must be an int");
                    ObjList *l = AS_LIST(coll);
                    int64_t i = AS_INT(idx); if (i < 0) i += l->count;
                    if (i < 0 || i >= l->count) vm_error(vm, "index %lld out of bounds (len %d)", (long long)AS_INT(idx), l->count);
                    vm_push(vm, l->items[i]);
                } else if (IS_MAP(coll)) {
                    char *key = value_to_cstr(vm->I, idx);
                    Value *slot = map_get(AS_MAP(coll), key); free(key);
                    vm_push(vm, slot ? *slot : NIL_VAL);
                } else if (IS_STRING(coll)) {
                    if (!IS_INT(idx)) vm_error(vm, "string index must be an int");
                    ObjString *s = AS_STRING(coll);
                    int64_t i = AS_INT(idx); if (i < 0) i += s->length;
                    if (i < 0 || i >= s->length) vm_error(vm, "index out of bounds");
                    vm_push(vm, string_val(vm->I, s->chars + i, 1));
                } else vm_error(vm, "type %s is not indexable", type_name(coll));
                break;
            }
            case OP_SET_INDEX: {
                Value value = vm_pop(vm), idx = vm_pop(vm), coll = vm_pop(vm);
                if (IS_LIST(coll)) {
                    if (!IS_INT(idx)) vm_error(vm, "list index must be an int");
                    ObjList *l = AS_LIST(coll);
                    int64_t i = AS_INT(idx); if (i < 0) i += l->count;
                    if (i < 0 || i >= l->count) vm_error(vm, "index out of bounds");
                    l->items[i] = value;
                } else if (IS_MAP(coll)) {
                    char *key = value_to_cstr(vm->I, idx);
                    map_set(AS_MAP(coll), key, value); free(key);
                } else vm_error(vm, "type %s does not support index assignment", type_name(coll));
                vm_push(vm, value);
                break;
            }
            case OP_GET_PROPERTY: {
                ObjString *name = AS_STRING(READ_CONST());
                Value obj = vm_pop(vm);
                if (!IS_MAP(obj)) vm_error(vm, "type %s has no attributes (.%s)", type_name(obj), name->chars);
                Value *slot = map_get(AS_MAP(obj), name->chars);
                vm_push(vm, slot ? *slot : NIL_VAL);
                break;
            }
            case OP_SET_PROPERTY: {
                ObjString *name = AS_STRING(READ_CONST());
                Value value = vm_pop(vm), obj = vm_pop(vm);
                if (!IS_MAP(obj)) vm_error(vm, "attribute assignment on type %s", type_name(obj));
                map_set(AS_MAP(obj), name->chars, value);
                vm_push(vm, value);
                break;
            }
            case OP_ITER_SEQ: { Value it = vm_pop(vm); vm_push(vm, iter_seq(vm, it)); break; }
            case OP_FOR_NEXT: {
                uint8_t seq_slot = READ_BYTE();
                uint8_t idx_slot = READ_BYTE();
                uint16_t off = READ_U16();
                ObjList *l = AS_LIST(frame->slots[seq_slot]);
                int64_t i = AS_INT(frame->slots[idx_slot]);
                if (i >= l->count) { frame->ip += off; }
                else { vm_push(vm, l->items[i]); frame->slots[idx_slot] = int_val(i + 1); }
                break;
            }
            case OP_IMPORT: {
                ObjString *path = AS_STRING(READ_CONST());
                char *resolved = resolve_import_path(vm->I->path, path->chars);
                /* A flat sub-import binds into the current module's scope but its
                   names are NOT part of the importing module's public surface, so
                   suspend define-recording for the duration (a module re-exports
                   only what it defines itself — like Python/JS/Rust). */
                Table *saved_defines = vm->I->module_defines;
                vm->I->module_defines = NULL;
                int rc = run_source(vm->I, NULL, resolved); /* NULL src => read file */
                vm->I->module_defines = saved_defines;
                free(resolved);
                /* On a runtime failure inside the module (rc==70), run_source left
                   the underlying message in I->err_msg without printing it (nested);
                   re-raise it here so an enclosing try/catch can handle it and an
                   uncaught import still reports the real cause exactly once.
                   Copy first: vm_error writes into I->err_msg, so passing it as a
                   %s argument would alias the vsnprintf destination. */
                if (rc == 70) {
                    char cause[512];
                    snprintf(cause, sizeof(cause), "%s", vm->I->err_msg);
                    vm_error(vm, "import of '%s' failed: %s", path->chars, cause);
                } else if (rc != 0) {
                    vm_error(vm, "import of '%s' failed (code %d)", path->chars, rc);
                }
                break;
            }
            case OP_IMPORT_AS: {
                ObjString *path = AS_STRING(READ_CONST());
                ObjString *alias = AS_STRING(READ_CONST());
                char *resolved = resolve_import_path(vm->I->path, path->chars);
                Value ns = run_module_ns(vm->I, resolved); /* takes ownership; raises on failure */
                table_set(vm->I->globals, alias->chars, hash_string(alias->chars, (size_t)alias->length), ns);
                break;
            }
            default: vm_error(vm, "unknown instruction %d", inst);
        }
    }
#undef READ_BYTE
#undef READ_U16
#undef READ_CONST
}

/* Call a Loqi callable (closure or native) from C — enables higher-order
 * built-ins like map/filter/reduce. Re-enters the VM for closures. */
static Value vm_invoke(VM *vm, Value callee, int argc, Value *argv) {
    vm_push(vm, callee);
    for (int i = 0; i < argc; i++) vm_push(vm, argv[i]);
    if (IS_NATIVE(callee)) {
        ObjNative *nat = AS_NATIVE(callee);
        if (nat->arity >= 0 && nat->arity != argc)
            vm_error(vm, "%s() expects %d arguments, got %d", nat->name, nat->arity, argc);
        Value r = nat->fn(vm->I, argc, vm->stack_top - argc);
        vm->stack_top -= argc + 1;
        return r;
    }
    if (IS_CLOSURE(callee)) {
        ObjClosure *cl = AS_CLOSURE(callee);
        /* Check arity here so the message names the callback and makes clear it
           is invoked from a built-in, rather than vm_call_closure raising with
           the (still-current) caller frame's line and a bare "fn". */
        if (argc != cl->proto->arity)
            vm_error(vm, "callback %s expects %d arguments, but the built-in passes %d",
                     cl->proto->name ? cl->proto->name : "(anonymous)", cl->proto->arity, argc);
        int base = vm->frame_count;
        vm_call_closure(vm, cl, argc);
        run_vm(vm, base);          /* runs until the invoked frame returns */
        return vm_pop(vm);         /* result left on top by OP_RETURN */
    }
    vm_error(vm, "value of type %s is not callable", type_name(callee));
    return NIL_VAL;
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
    runtime_error(I, 0, "len() requires str, list or map, got %s", type_name(v));
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
    runtime_error(I, 0, "int() cannot convert %s", type_name(v));
    return NIL_VAL;
}
static Value nat_float(Interp *I, int argc, Value *argv) {
    Value v = argv[0];
    if (IS_FLOAT(v)) return v;
    if (IS_INT(v)) return float_val((double)AS_INT(v));
    if (IS_STRING(v)) return float_val(strtod(AS_STRING(v)->chars, NULL));
    runtime_error(I, 0, "float() cannot convert %s", type_name(v));
    return NIL_VAL;
}
static Value nat_push(Interp *I, int argc, Value *argv) {
    if (argc < 1) runtime_error(I, 0, "push() requires at least a list");
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "push() requires a list, got %s", type_name(argv[0]));
    for (int i = 1; i < argc; i++) list_push(AS_LIST(argv[0]), argv[i]);
    return argv[0];
}
static Value nat_pop(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "pop() requires a list");
    ObjList *l = AS_LIST(argv[0]);
    if (l->count == 0) runtime_error(I, 0, "pop() from empty list");
    return l->items[--l->count];
}
static Value nat_keys(Interp *I, int argc, Value *argv) {
    if (!IS_MAP(argv[0])) runtime_error(I, 0, "keys() requires a map");
    ObjList *l = new_list(I);
    ObjMap *m = AS_MAP(argv[0]);
    for (int i = 0; i < m->count; i++) list_push(l, cstring_val(I, m->entries[i].key));
    return obj_val((Obj *)l);
}
static Value nat_values(Interp *I, int argc, Value *argv) {
    if (!IS_MAP(argv[0])) runtime_error(I, 0, "values() requires a map");
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
    runtime_error(I, 0, "has() requires map or list");
    return NIL_VAL;
}
static Value nat_range(Interp *I, int argc, Value *argv) {
    if (argc < 1 || argc > 3) runtime_error(I, 0, "range() expects 1-3 arguments, got %d", argc);
    for (int i = 0; i < argc; i++)
        if (!IS_INT(argv[i])) runtime_error(I, 0, "range() requires int arguments, got %s", type_name(argv[i]));
    int64_t start = 0, stop = 0, step = 1;
    if (argc == 1) { stop = AS_INT(argv[0]); }
    else if (argc == 2) { start = AS_INT(argv[0]); stop = AS_INT(argv[1]); }
    else { start = AS_INT(argv[0]); stop = AS_INT(argv[1]); step = AS_INT(argv[2]); }
    if (step == 0) runtime_error(I, 0, "range() step cannot be 0");
    ObjList *l = new_list(I);
    int64_t i = start;
    while (step > 0 ? i < stop : i > stop) {
        list_push(l, int_val(i));
        if (__builtin_add_overflow(i, step, &i)) break; /* avoid int64 overflow UB */
    }
    return obj_val((Obj *)l);
}
static Value nat_upper(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "upper() requires str");
    ObjString *s = AS_STRING(argv[0]);
    ObjString *r = new_string_n(I, s->chars, s->length);
    for (int i = 0; i < r->length; i++) r->chars[i] = (char)toupper((unsigned char)r->chars[i]);
    return obj_val((Obj *)r);
}
static Value nat_lower(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "lower() requires str");
    ObjString *s = AS_STRING(argv[0]);
    ObjString *r = new_string_n(I, s->chars, s->length);
    for (int i = 0; i < r->length; i++) r->chars[i] = (char)tolower((unsigned char)r->chars[i]);
    return obj_val((Obj *)r);
}
static Value nat_split(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) runtime_error(I, 0, "split() requires (str, str)");
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
    if (!IS_LIST(argv[0]) || !IS_STRING(argv[1])) runtime_error(I, 0, "join() requires (list, str)");
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
    runtime_error(I, 0, "abs() requires a number");
    return NIL_VAL;
}
static Value nat_sqrt(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "sqrt() requires a number");
    return float_val(sqrt(as_double(argv[0])));
}
/* Safely convert an already-rounded double to int64. Converting a non-finite
   or out-of-[INT64_MIN,INT64_MAX] double to int64_t is undefined behavior. */
static Value int_from_double(Interp *I, double r, const char *who) {
    /* 9223372036854775808.0 == 2^63 is the first double > INT64_MAX; the
       smallest representable below it is 9223372036854774784.0. */
    if (!isfinite(r) || r >= 9223372036854775808.0 || r < -9223372036854775808.0)
        runtime_error(I, 0, who);
    return int_val((int64_t)r);
}
static Value nat_floor(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "floor() requires a number");
    return int_from_double(I, floor(as_double(argv[0])), "floor(): value out of range");
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
    if (argc < 1) runtime_error(I, 0, "assert() requires a condition");
    if (!is_truthy(argv[0])) {
        const char *msg = (argc >= 2 && IS_STRING(argv[1])) ? AS_STRING(argv[1])->chars : "assertion failed";
        runtime_error(I, 0, "assert: %s", msg);
    }
    return NIL_VAL;
}

static void define_native(Interp *I, const char *name, NativeFn fn, int arity) {
    ObjNative *nat = (ObjNative *)alloc_obj(I, sizeof(ObjNative), OBJ_NATIVE);
    nat->fn = fn; nat->name = name; nat->arity = arity;
    table_set(I->globals, name, hash_string(name, strlen(name)), obj_val((Obj *)nat));
}

/* ===========================================================================
 * AI-first batteries — the pieces you'd install separately in other languages,
 * here built into the runtime: JSON, HTTP, environment, files, vectors, and a
 * first-class call to an LLM (`ai`). HTTP/LLM shell out to the always-present
 * `curl`; a native client is a later iteration.
 * ========================================================================= */

/* ---- JSON ---- */
#define JSON_MAX_DEPTH 200
static Value json_parse_value(Interp *I, const char **p, int depth);

static void json_skip_ws(const char **p) {
    while (**p == ' ' || **p == '\t' || **p == '\n' || **p == '\r') (*p)++;
}
static void json_encode_utf8(char **buf, size_t *len, size_t *cap, unsigned cp) {
    char tmp[4]; int n = 0;
    if (cp < 0x80) tmp[n++] = (char)cp;
    else if (cp < 0x800) { tmp[n++] = (char)(0xC0 | (cp >> 6)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    else { tmp[n++] = (char)(0xE0 | (cp >> 12)); tmp[n++] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[n++] = (char)(0x80 | (cp & 0x3F)); }
    append_str(buf, len, cap, tmp, (size_t)n);
}
static char *json_parse_string_raw(Interp *I, const char **p) {
    if (**p != '"') runtime_error(I, 0, "json: expected string");
    (*p)++;
    char *buf = xmalloc(16); size_t len = 0, cap = 16; buf[0] = '\0';
    while (**p && **p != '"') {
        char c = **p;
        if (c == '\\') {
            (*p)++;
            char e = **p;
            switch (e) {
                case '"': append_str(&buf, &len, &cap, "\"", 1); break;
                case '\\': append_str(&buf, &len, &cap, "\\", 1); break;
                case '/': append_str(&buf, &len, &cap, "/", 1); break;
                case 'n': append_str(&buf, &len, &cap, "\n", 1); break;
                case 't': append_str(&buf, &len, &cap, "\t", 1); break;
                case 'r': append_str(&buf, &len, &cap, "\r", 1); break;
                case 'b': append_str(&buf, &len, &cap, "\b", 1); break;
                case 'f': append_str(&buf, &len, &cap, "\f", 1); break;
                case 'u': {
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        (*p)++;
                        char h = **p;
                        cp <<= 4;
                        if (h >= '0' && h <= '9') cp |= (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') cp |= (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') cp |= (unsigned)(h - 'A' + 10);
                        else { free(buf); runtime_error(I, 0, "json: invalid \\u escape"); }
                    }
                    json_encode_utf8(&buf, &len, &cap, cp);
                    break;
                }
                default: free(buf); runtime_error(I, 0, "json: invalid escape");
            }
            (*p)++;
        } else {
            append_str(&buf, &len, &cap, p[0], 1);
            (*p)++;
        }
    }
    if (**p != '"') { free(buf); runtime_error(I, 0, "json: unterminated string"); }
    (*p)++;
    return buf;
}
static Value json_parse_value(Interp *I, const char **p, int depth) {
    if (depth > JSON_MAX_DEPTH) runtime_error(I, 0, "json: nesting too deep (max %d)", JSON_MAX_DEPTH);
    json_skip_ws(p);
    char c = **p;
    if (c == '"') { char *s = json_parse_string_raw(I, p); Value v = cstring_val(I, s); free(s); return v; }
    if (c == '{') {
        (*p)++; ObjMap *m = new_map(I); json_skip_ws(p);
        if (**p == '}') { (*p)++; return obj_val((Obj *)m); }
        for (;;) {
            json_skip_ws(p);
            char *key = json_parse_string_raw(I, p);
            json_skip_ws(p);
            if (**p != ':') { free(key); runtime_error(I, 0, "json: expected ':'"); }
            (*p)++;
            Value val = json_parse_value(I, p, depth + 1);
            map_set(m, key, val); free(key);
            json_skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == '}') { (*p)++; break; }
            runtime_error(I, 0, "json: expected ',' or '}'");
        }
        return obj_val((Obj *)m);
    }
    if (c == '[') {
        (*p)++; ObjList *l = new_list(I); json_skip_ws(p);
        if (**p == ']') { (*p)++; return obj_val((Obj *)l); }
        for (;;) {
            list_push(l, json_parse_value(I, p, depth + 1));
            json_skip_ws(p);
            if (**p == ',') { (*p)++; continue; }
            if (**p == ']') { (*p)++; break; }
            runtime_error(I, 0, "json: expected ',' or ']'");
        }
        return obj_val((Obj *)l);
    }
    if (!strncmp(*p, "true", 4)) { *p += 4; return bool_val(true); }
    if (!strncmp(*p, "false", 5)) { *p += 5; return bool_val(false); }
    if (!strncmp(*p, "null", 4)) { *p += 4; return NIL_VAL; }
    if (c == '-' || (c >= '0' && c <= '9')) {
        const char *start = *p;
        bool is_float = false;
        if (**p == '-') (*p)++;
        while ((**p >= '0' && **p <= '9')) (*p)++;
        if (**p == '.') { is_float = true; (*p)++; while (**p >= '0' && **p <= '9') (*p)++; }
        if (**p == 'e' || **p == 'E') { is_float = true; (*p)++; if (**p == '+' || **p == '-') (*p)++; while (**p >= '0' && **p <= '9') (*p)++; }
        char tmp[64]; int n = (int)(*p - start); if (n > 63) n = 63;
        memcpy(tmp, start, (size_t)n); tmp[n] = '\0';
        return is_float ? float_val(strtod(tmp, NULL)) : int_val(strtoll(tmp, NULL, 10));
    }
    runtime_error(I, 0, "json: invalid token");
    return NIL_VAL;
}
static Value nat_json_parse(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "json.parse() requires a str");
    const char *p = AS_STRING(argv[0])->chars;
    Value v = json_parse_value(I, &p, 0);
    json_skip_ws(&p);
    if (*p != '\0') runtime_error(I, 0, "json.parse(): trailing text after value");
    return v;
}
static void json_stringify_value(Interp *I, char **buf, size_t *len, size_t *cap, Value v, int depth) {
    if (depth > JSON_MAX_DEPTH) runtime_error(I, 0, "json.stringify: structure too deep or cyclic (max %d)", JSON_MAX_DEPTH);
    switch (v.type) {
        case VAL_NIL: append_str(buf, len, cap, "null", 4); return;
        case VAL_BOOL: append_str(buf, len, cap, v.as.b ? "true" : "false", v.as.b ? 4 : 5); return;
        case VAL_INT: case VAL_FLOAT: { char *s = value_to_cstr(I, v); append_str(buf, len, cap, s, strlen(s)); free(s); return; }
        case VAL_OBJ: break;
    }
    if (IS_STRING(v)) {
        ObjString *s = AS_STRING(v);
        append_str(buf, len, cap, "\"", 1);
        for (int i = 0; i < s->length; i++) {
            unsigned char c = (unsigned char)s->chars[i];
            switch (c) {
                case '"': append_str(buf, len, cap, "\\\"", 2); break;
                case '\\': append_str(buf, len, cap, "\\\\", 2); break;
                case '\n': append_str(buf, len, cap, "\\n", 2); break;
                case '\t': append_str(buf, len, cap, "\\t", 2); break;
                case '\r': append_str(buf, len, cap, "\\r", 2); break;
                default:
                    if (c < 0x20) { /* JSON requires control chars to be \u-escaped */
                        char esc[8]; snprintf(esc, sizeof(esc), "\\u%04x", c);
                        append_str(buf, len, cap, esc, 6);
                    } else {
                        char ch = (char)c;
                        append_str(buf, len, cap, &ch, 1);
                    }
            }
        }
        append_str(buf, len, cap, "\"", 1);
    } else if (IS_LIST(v)) {
        ObjList *l = AS_LIST(v);
        append_str(buf, len, cap, "[", 1);
        for (int i = 0; i < l->count; i++) { if (i) append_str(buf, len, cap, ",", 1); json_stringify_value(I, buf, len, cap, l->items[i], depth + 1); }
        append_str(buf, len, cap, "]", 1);
    } else if (IS_MAP(v)) {
        ObjMap *m = AS_MAP(v);
        append_str(buf, len, cap, "{", 1);
        for (int i = 0; i < m->count; i++) {
            if (i) append_str(buf, len, cap, ",", 1);
            json_stringify_value(I, buf, len, cap, cstring_val(I, m->entries[i].key), depth + 1);
            append_str(buf, len, cap, ":", 1);
            json_stringify_value(I, buf, len, cap, m->entries[i].value, depth + 1);
        }
        append_str(buf, len, cap, "}", 1);
    } else {
        append_str(buf, len, cap, "null", 4);
    }
}
static Value nat_json_stringify(Interp *I, int argc, Value *argv) {
    char *buf = xmalloc(16); size_t len = 0, cap = 16; buf[0] = '\0';
    json_stringify_value(I, &buf, &len, &cap, argv[0], 0);
    Value out = string_val(I, buf, (int)len);
    free(buf);
    return out;
}

/* ---- shell / curl plumbing ---- */
static char *shell_quote(const char *s) {
    size_t n = strlen(s);
    char *out = xmalloc(n * 4 + 3);
    size_t j = 0;
    out[j++] = '\'';
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\'') { out[j++] = '\''; out[j++] = '\\'; out[j++] = '\''; out[j++] = '\''; }
        else out[j++] = s[i];
    }
    out[j++] = '\'';
    out[j] = '\0';
    return out;
}
/* Run a command, capturing stdout (binary-safe via *out_len) and curl's exit
 * status (*out_status = curl's exit code, or -1 if it couldn't be launched). */
static char *run_capture(const char *cmd, size_t *out_len, int *out_status) {
    *out_len = 0; *out_status = -1;
    FILE *pp = popen(cmd, "r");
    if (!pp) return NULL;
    size_t cap = 4096, len = 0; char *buf = xmalloc(cap);
    for (;;) {
        if (cap - len < 4096) { cap *= 2; buf = xrealloc(buf, cap); }
        size_t got = fread(buf + len, 1, cap - len, pp);
        len += got;
        if (got == 0) break;
    }
    buf[len] = '\0';
    int rc = pclose(pp);
    *out_len = len;
    *out_status = (rc >= 0 && WIFEXITED(rc)) ? WEXITSTATUS(rc) : -1;
    return buf;
}

/* ---- HTTP ---- */
static Value nat_http_get(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "http.get() requires a url (str)");
    char *qurl = shell_quote(AS_STRING(argv[0])->chars);
    size_t clen = strlen(qurl) + 64;
    char *cmd = xmalloc(clen);
    snprintf(cmd, clen, "curl -fsSL --max-time 60 %s", qurl);
    size_t len; int status;
    char *resp = run_capture(cmd, &len, &status);
    free(qurl); free(cmd);
    if (!resp) runtime_error(I, 0, "http.get(): could not run curl");
    if (status != 0) { free(resp); runtime_error(I, 0, "http.get(): curl returned an error (code %d)", status); }
    Value v = string_val(I, resp, (int)len); /* binary-safe: no NUL truncation */
    free(resp);
    return v;
}
static Value nat_http_post(Interp *I, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0]) || !IS_STRING(argv[1]))
        runtime_error(I, 0, "http.post() requires (url: str, body: str)");
    const char *ctype = (argc >= 3 && IS_STRING(argv[2])) ? AS_STRING(argv[2])->chars : "application/json";
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "Content-Type: %.200s", ctype);
    char *qurl = shell_quote(AS_STRING(argv[0])->chars);
    char *qbody = shell_quote(AS_STRING(argv[1])->chars);
    char *qhdr = shell_quote(hdr);
    size_t clen = strlen(qurl) + strlen(qbody) + strlen(qhdr) + 64;
    char *cmd = xmalloc(clen);
    snprintf(cmd, clen, "curl -fsSL --max-time 60 -X POST -H %s --data %s %s", qhdr, qbody, qurl);
    size_t len; int status;
    char *resp = run_capture(cmd, &len, &status);
    free(qurl); free(qbody); free(qhdr); free(cmd);
    if (!resp) runtime_error(I, 0, "http.post(): could not run curl");
    if (status != 0) { free(resp); runtime_error(I, 0, "http.post(): curl returned an error (code %d)", status); }
    Value v = string_val(I, resp, (int)len);
    free(resp);
    return v;
}

/* ---- ai: a first-class LLM call (Anthropic Messages API via curl) ---- */
/* Parse model text into a native value for ai(prompt, { json: true }), tolerating
   the ```/```json markdown fences models often wrap JSON in. json_parse_value
   reads one value and stops, so any trailing fence is simply ignored. */
static Value ai_parse_json_text(Interp *I, const char *text) {
    const char *s = text;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    if (s[0] == '`' && s[1] == '`' && s[2] == '`') {
        const char *nl = strchr(s, '\n');
        s = nl ? nl + 1 : s + 3; /* skip the opening fence line (``` or ```json) */
    }
    const char *p = s;
    return json_parse_value(I, &p, 0);
}

/* Format a runtime-error message into I->err_msg WITHOUT raising, returning NULL.
   Lets a caller (e.g. ai_all) release its own resources first, then raise_error()
   — so a mid-loop failure can't strand key-bearing temp files on disk. */
static char *fail_msg(Interp *I, const char *msg) {
    int n = snprintf(I->err_msg, sizeof(I->err_msg), "runtime error [%s:%d]: ",
                     I->path ? I->path : "?", vm_current_line(I));
    n = clamp_written(n, sizeof(I->err_msg));
    snprintf(I->err_msg + n, sizeof(I->err_msg) - (size_t)n, "%s", msg);
    return NULL;
}

/* Parsed form of ai()'s optional 2nd argument. */
typedef struct {
    const char *model;
    const char *system;
    int64_t max_tokens;
    bool has_temp; double temperature;
    bool want_json;
} AiOpts;

/* Parse argv[optidx] (a model str, an options map, or absent) into AiOpts. */
static AiOpts ai_parse_opts(Interp *I, int argc, Value *argv, int optidx) {
    AiOpts o;
    o.model = getenv("LOQI_AI_MODEL") ? getenv("LOQI_AI_MODEL") : "claude-sonnet-4-6";
    o.system = NULL; o.max_tokens = 1024; o.has_temp = false; o.temperature = 0; o.want_json = false;
    if (argc > optidx && IS_STRING(argv[optidx])) {
        o.model = AS_STRING(argv[optidx])->chars;
    } else if (argc > optidx && IS_MAP(argv[optidx])) {
        ObjMap *m = AS_MAP(argv[optidx]); Value *v;
        if ((v = map_get(m, "model")) && IS_STRING(*v)) o.model = AS_STRING(*v)->chars;
        if ((v = map_get(m, "system")) && IS_STRING(*v)) o.system = AS_STRING(*v)->chars;
        if ((v = map_get(m, "max_tokens")) && IS_INT(*v)) o.max_tokens = AS_INT(*v);
        if ((v = map_get(m, "temperature")) && IS_NUM(*v)) { o.has_temp = true; o.temperature = as_double(*v); }
        if ((v = map_get(m, "json")) != NULL) o.want_json = is_truthy(*v);
    } else if (argc > optidx && !IS_NIL(argv[optidx])) {
        runtime_error(I, 0, "ai(): options must be a model name (str) or an options map");
    }
    if (o.max_tokens <= 0) runtime_error(I, 0, "ai(): max_tokens must be positive");
    if (o.want_json && !o.system)
        o.system = "Respond with a single valid JSON value and nothing else. Do not wrap it in markdown code fences.";
    return o;
}

/* Build the request body + 0600 curl config temp files for one prompt and return
   the curl command (malloc'd, caller frees). Fills body_tmpl/cfg_tmpl (>= 32
   bytes; caller unlinks them). Runs on the MAIN thread only — it allocates Loqi
   objects (json_stringify) and so must not run on a worker. The API key goes
   only into the 0600 config file, never argv. */
static char *ai_build_request(Interp *I, Value prompt, AiOpts o, const char *key,
                              char *body_tmpl, char *cfg_tmpl) {
    ObjMap *body = new_map(I);
    map_set(body, "model", cstring_val(I, o.model));
    map_set(body, "max_tokens", int_val(o.max_tokens));
    if (o.has_temp) map_set(body, "temperature", float_val(o.temperature));
    if (o.system) map_set(body, "system", cstring_val(I, o.system));
    ObjList *msgs = new_list(I);
    if (IS_LIST(prompt)) {
        /* a conversation: pass the caller's [{role, content}, ...] through as-is */
        ObjList *conv = AS_LIST(prompt);
        for (int i = 0; i < conv->count; i++) list_push(msgs, conv->items[i]);
    } else {
        /* a single string prompt -> one user message */
        ObjMap *msg = new_map(I);
        map_set(msg, "role", cstring_val(I, "user"));
        map_set(msg, "content", prompt);
        list_push(msgs, obj_val((Obj *)msg));
    }
    map_set(body, "messages", obj_val((Obj *)msgs));

    char *jbuf = xmalloc(16); size_t jl = 0, jc = 16; jbuf[0] = '\0';
    json_stringify_value(I, &jbuf, &jl, &jc, obj_val((Obj *)body), 0);

    strcpy(body_tmpl, "/tmp/loqi_ai_XXXXXX");
    int fd = mkstemp(body_tmpl); /* creates the file 0600 */
    if (fd < 0) { free(jbuf); return fail_msg(I, "ai(): could not create temp file"); }
    if (write(fd, jbuf, jl) < 0) { close(fd); unlink(body_tmpl); free(jbuf); return fail_msg(I, "ai(): temp write failed"); }
    close(fd);
    free(jbuf);

    strcpy(cfg_tmpl, "/tmp/loqi_aicfg_XXXXXX");
    int cfd = mkstemp(cfg_tmpl);
    if (cfd < 0) { unlink(body_tmpl); return fail_msg(I, "ai(): could not create config file"); }
    FILE *cf = fdopen(cfd, "w");
    if (!cf) { close(cfd); unlink(cfg_tmpl); unlink(body_tmpl); return fail_msg(I, "ai(): config not writable"); }
    fputs("url = \"https://api.anthropic.com/v1/messages\"\n", cf);
    fputs("header = \"content-type: application/json\"\n", cf);
    fputs("header = \"anthropic-version: 2023-06-01\"\n", cf);
    fputs("header = \"x-api-key: ", cf);
    for (const char *k = key; *k; k++) { if (*k == '"' || *k == '\\') fputc('\\', cf); fputc(*k, cf); }
    fputs("\"\n", cf);
    fprintf(cf, "data = \"@%s\"\n", body_tmpl);
    fclose(cf);

    char *qcfg = shell_quote(cfg_tmpl);
    size_t clen = strlen(qcfg) + 48;
    char *cmd = xmalloc(clen);
    snprintf(cmd, clen, "curl -fsS --max-time 120 -K %s", qcfg);
    free(qcfg);
    return cmd;
}

/* Parse one Anthropic Messages response into a Loqi value (the first text block,
   or its parsed JSON if want_json). Takes ownership of `resp` (frees it). Raises
   on a curl/API/format error. */
static Value ai_parse_response(Interp *I, char *resp, int status, bool want_json) {
    if (!resp) runtime_error(I, 0, "ai(): could not run curl");
    if (status != 0 && !*resp) { free(resp); runtime_error(I, 0, "ai(): API request failed (curl %d) — check network and key", status); }
    if (!*resp) { free(resp); runtime_error(I, 0, "ai(): no response from the API"); }
    const char *p = resp;
    Value parsed = json_parse_value(I, &p, 0);
    free(resp);
    if (IS_MAP(parsed)) {
        Value *content = map_get(AS_MAP(parsed), "content");
        if (content && IS_LIST(*content)) {
            ObjList *blocks = AS_LIST(*content);
            for (int i = 0; i < blocks->count; i++) { /* first text block */
                if (IS_MAP(blocks->items[i])) {
                    Value *text = map_get(AS_MAP(blocks->items[i]), "text");
                    if (text && IS_STRING(*text))
                        return want_json ? ai_parse_json_text(I, AS_STRING(*text)->chars) : *text;
                }
            }
        }
        Value *err = map_get(AS_MAP(parsed), "error");
        if (err && IS_MAP(*err)) {
            Value *m = map_get(AS_MAP(*err), "message");
            if (m && IS_STRING(*m)) runtime_error(I, 0, "ai(): %s", AS_STRING(*m)->chars);
        }
    }
    runtime_error(I, 0, "ai(): API response in unexpected format");
    return NIL_VAL;
}

static Value nat_ai(Interp *I, int argc, Value *argv) {
    if (argc < 1 || (!IS_STRING(argv[0]) && !IS_LIST(argv[0])))
        runtime_error(I, 0, "ai() requires a prompt (str) or a list of messages");
    const char *key = getenv("ANTHROPIC_API_KEY");
    if (!key || !*key)
        runtime_error(I, 0, "ai(): set the ANTHROPIC_API_KEY environment variable");
    AiOpts o = ai_parse_opts(I, argc, argv, 1);
    char body_tmpl[32], cfg_tmpl[32];
    char *cmd = ai_build_request(I, argv[0], o, key, body_tmpl, cfg_tmpl);
    if (!cmd) raise_error(I); /* build cleaned up its own temp files; raise the message */
    size_t rlen; int status;
    char *resp = run_capture(cmd, &rlen, &status);
    free(cmd); unlink(body_tmpl); unlink(cfg_tmpl);
    return ai_parse_response(I, resp, status, o.want_json);
}

/* ---- environment, files, vectors ---- */
static Value nat_env(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "env() requires a name (str)");
    const char *v = getenv(AS_STRING(argv[0])->chars);
    return v ? cstring_val(I, v) : NIL_VAL;
}
static char *read_file(const char *path); /* defined in the driver */
static Value nat_read(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "read() requires a path (str)");
    char *src = read_file(AS_STRING(argv[0])->chars);
    if (!src) runtime_error(I, 0, "read(): could not read '%s'", AS_STRING(argv[0])->chars);
    Value v = cstring_val(I, src);
    free(src);
    return v;
}
static Value nat_write(Interp *I, int argc, Value *argv) {
    if (argc < 2 || !IS_STRING(argv[0]) || !IS_STRING(argv[1]))
        runtime_error(I, 0, "write() requires (path: str, content: str)");
    FILE *f = fopen(AS_STRING(argv[0])->chars, "wb");
    if (!f) runtime_error(I, 0, "write(): could not open '%s'", AS_STRING(argv[0])->chars);
    ObjString *s = AS_STRING(argv[1]);
    fwrite(s->chars, 1, (size_t)s->length, f);
    fclose(f);
    return NIL_VAL;
}
/* ---- filesystem (these are pure C / OS calls: no VM re-entry, no GC rooting) ---- */
static Value nat_ls(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "ls() requires a path (str)");
    const char *path = AS_STRING(argv[0])->chars;
    DIR *d = opendir(path);
    if (!d) runtime_error(I, 0, "ls(): could not open directory '%s'", path);
    ObjList *out = new_list(I);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        list_push(out, cstring_val(I, e->d_name));
    }
    closedir(d);
    return obj_val((Obj *)out);
}
static Value nat_exists(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "exists() requires a path (str)");
    struct stat st;
    return bool_val(stat(AS_STRING(argv[0])->chars, &st) == 0);
}
static Value nat_is_dir(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "is_dir() requires a path (str)");
    struct stat st;
    return bool_val(stat(AS_STRING(argv[0])->chars, &st) == 0 && S_ISDIR(st.st_mode));
}
static Value nat_mkdir(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "mkdir() requires a path (str)");
    ObjString *s = AS_STRING(argv[0]);
    char *tmp = xmalloc((size_t)s->length + 1);
    memcpy(tmp, s->chars, (size_t)s->length); tmp[s->length] = '\0';
    /* create intermediate directories, like `mkdir -p` */
    for (int i = 1; i < s->length; i++) {
        if (tmp[i] == '/') {
            tmp[i] = '\0';
            if (mkdir(tmp, 0777) != 0 && errno != EEXIST) { free(tmp); runtime_error(I, 0, "mkdir(): could not create '%s'", AS_STRING(argv[0])->chars); }
            tmp[i] = '/';
        }
    }
    if (mkdir(tmp, 0777) != 0 && errno != EEXIST) { free(tmp); runtime_error(I, 0, "mkdir(): could not create '%s'", AS_STRING(argv[0])->chars); }
    free(tmp);
    return NIL_VAL;
}
static Value nat_rm(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "rm() requires a path (str)");
    const char *path = AS_STRING(argv[0])->chars;
    if (remove(path) != 0) runtime_error(I, 0, "rm(): could not remove '%s'", path);
    return NIL_VAL;
}
/* ---- process / OS ---- */
static Value nat_run(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "run() requires a command (str)");
    size_t len; int status;
    /* runs via the shell and captures stdout; the caller is responsible for the
       command string (quote untrusted input). Returns { out, code }. */
    char *out = run_capture(AS_STRING(argv[0])->chars, &len, &status);
    if (!out) runtime_error(I, 0, "run(): could not start the command");
    ObjMap *m = new_map(I);
    map_set(m, "out", string_val(I, out, (int)len));
    free(out);
    map_set(m, "code", int_val(status));
    return obj_val((Obj *)m);
}
static Value nat_args(Interp *I, int argc, Value *argv) {
    (void)argc; (void)argv;
    ObjList *l = new_list(I);
    for (int i = 0; i < I->script_argc; i++) list_push(l, cstring_val(I, I->script_argv[i]));
    return obj_val((Obj *)l);
}

/* run_all(cmds) — run shell commands concurrently and collect { out, code } for
 * each, in input order. This is Loqi's concurrency primitive: the worker threads
 * only run a subprocess (popen) and malloc — they never touch the Loqi heap or
 * the VM — so the single-threaded GC stays safe. The blocking I/O (curl, tools)
 * is what overlaps, which is exactly the win for fanning out AI/HTTP calls. */
typedef struct { const char *cmd; char *out; size_t len; int status; } RunJob;
static void *run_one_job(void *arg) {
    RunJob *j = (RunJob *)arg;
    j->out = run_capture(j->cmd, &j->len, &j->status);
    return NULL;
}
#define MAX_PARALLEL 32
/* Run n jobs (each a shell command) on a thread pool, in waves of MAX_PARALLEL.
   Workers only popen/malloc — they never touch the Loqi heap — so this is safe
   to call while the single-threaded GC is idle (no VM re-entry here). After it
   returns, every jobs[i].{out,len,status} is filled. Shared by run_all/ai_all. */
static void run_jobs_parallel(RunJob *jobs, int n) {
    pthread_t threads[MAX_PARALLEL];
    bool started[MAX_PARALLEL];
    for (int base = 0; base < n; base += MAX_PARALLEL) {
        int cnt = n - base < MAX_PARALLEL ? n - base : MAX_PARALLEL;
        for (int k = 0; k < cnt; k++)
            started[k] = (pthread_create(&threads[k], NULL, run_one_job, &jobs[base + k]) == 0);
        for (int k = 0; k < cnt; k++) {
            if (started[k]) pthread_join(threads[k], NULL);
            else run_one_job(&jobs[base + k]); /* thread spawn failed: run it synchronously */
        }
    }
}
static Value nat_run_all(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "run_all() requires a list of command strings");
    ObjList *cmds = AS_LIST(argv[0]);
    int n = cmds->count;
    for (int i = 0; i < n; i++)
        if (!IS_STRING(cmds->items[i])) runtime_error(I, 0, "run_all(): each command must be a str");
    RunJob *jobs = (n > 0) ? xmalloc(sizeof(RunJob) * (size_t)n) : NULL;
    for (int i = 0; i < n; i++) {
        jobs[i].cmd = AS_STRING(cmds->items[i])->chars;
        jobs[i].out = NULL; jobs[i].len = 0; jobs[i].status = -1;
    }
    run_jobs_parallel(jobs, n);
    /* back on the single Loqi thread — allocating Loqi objects is safe again */
    ObjList *out = new_list(I);
    for (int i = 0; i < n; i++) {
        ObjMap *m = new_map(I);
        map_set(m, "out", jobs[i].out ? string_val(I, jobs[i].out, (int)jobs[i].len) : cstring_val(I, ""));
        map_set(m, "code", int_val(jobs[i].status));
        list_push(out, obj_val((Obj *)m));
        free(jobs[i].out);
    }
    free(jobs);
    return obj_val((Obj *)out);
}

/* ai_all(prompts [, options]) — run many model calls concurrently (the AI payoff
   of run_all). Requests are built on the main thread (Loqi-safe), the curl calls
   overlap on the thread pool, responses are parsed on the main thread. Returns a
   list of answers (text, or parsed data with { json: true }) in input order;
   raises on the first failed call. */
static Value nat_ai_all(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "ai_all() requires a list of prompts");
    const char *key = getenv("ANTHROPIC_API_KEY");
    if (!key || !*key) runtime_error(I, 0, "ai_all(): set the ANTHROPIC_API_KEY environment variable");
    ObjList *prompts = AS_LIST(argv[0]);
    int n = prompts->count;
    for (int i = 0; i < n; i++)
        if (!IS_STRING(prompts->items[i])) runtime_error(I, 0, "ai_all(): each prompt must be a str");
    AiOpts o = ai_parse_opts(I, argc, argv, 1);
    if (n == 0) return obj_val((Obj *)new_list(I));

    RunJob *jobs = xmalloc(sizeof(RunJob) * (size_t)n);
    char (*body_tmpls)[32] = xmalloc(sizeof(char[32]) * (size_t)n);
    char (*cfg_tmpls)[32]  = xmalloc(sizeof(char[32]) * (size_t)n);
    for (int i = 0; i < n; i++) {
        jobs[i].cmd = ai_build_request(I, prompts->items[i], o, key, body_tmpls[i], cfg_tmpls[i]);
        if (!jobs[i].cmd) {
            /* a build failed (its own partial files are already cleaned): remove the
               temp files of the prompts built so far — no API key left on disk — then raise */
            for (int j = 0; j < i; j++) { unlink(body_tmpls[j]); unlink(cfg_tmpls[j]); free((char *)jobs[j].cmd); }
            free(jobs); free(body_tmpls); free(cfg_tmpls);
            raise_error(I);
        }
        jobs[i].out = NULL; jobs[i].len = 0; jobs[i].status = -1;
    }
    run_jobs_parallel(jobs, n);
    for (int i = 0; i < n; i++) { unlink(body_tmpls[i]); unlink(cfg_tmpls[i]); free((char *)jobs[i].cmd); }
    free(body_tmpls); free(cfg_tmpls);

    ObjList *out = new_list(I);
    for (int i = 0; i < n; i++)
        list_push(out, ai_parse_response(I, jobs[i].out, jobs[i].status, o.want_json));
    free(jobs);
    return obj_val((Obj *)out);
}
static Value nat_exit(Interp *I, int argc, Value *argv) {
    (void)I;
    int code = (argc >= 1 && IS_INT(argv[0])) ? (int)AS_INT(argv[0]) : 0;
    exit(code);
}
/* ---- path manipulation (string-only, length-aware) ---- */
static Value nat_path_join(Interp *I, int argc, Value *argv) {
    char *buf = xmalloc(16); size_t len = 0, cap = 16; buf[0] = '\0';
    for (int i = 0; i < argc; i++) {
        if (!IS_STRING(argv[i])) { free(buf); runtime_error(I, 0, "path.join() requires string components"); }
        ObjString *s = AS_STRING(argv[i]);
        if (s->length == 0) continue;
        if (s->chars[0] == '/') { len = 0; buf[0] = '\0'; } /* an absolute part resets */
        else if (len > 0 && buf[len - 1] != '/') append_str(&buf, &len, &cap, "/", 1);
        append_str(&buf, &len, &cap, s->chars, (size_t)s->length);
    }
    Value v = string_val(I, buf, (int)len);
    free(buf);
    return v;
}
static int last_slash(ObjString *s) { /* index of the last '/', or -1 */
    int slash = -1;
    for (int i = 0; i < s->length; i++) if (s->chars[i] == '/') slash = i;
    return slash;
}
static Value nat_path_dirname(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "path.dirname() requires a path (str)");
    ObjString *s = AS_STRING(argv[0]);
    int slash = last_slash(s);
    if (slash < 0) return cstring_val(I, ".");
    if (slash == 0) return cstring_val(I, "/");
    return string_val(I, s->chars, slash);
}
static Value nat_path_basename(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "path.basename() requires a path (str)");
    ObjString *s = AS_STRING(argv[0]);
    int start = last_slash(s) + 1;
    return string_val(I, s->chars + start, s->length - start);
}
static Value nat_path_ext(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "path.ext() requires a path (str)");
    ObjString *s = AS_STRING(argv[0]);
    int start = last_slash(s) + 1; /* start of the basename */
    int dot = -1;
    for (int i = start; i < s->length; i++) if (s->chars[i] == '.') dot = i;
    if (dot <= start) return cstring_val(I, ""); /* no dot, or a leading-dot hidden file */
    return string_val(I, s->chars + dot, s->length - dot); /* includes the dot */
}

/* ---- base64 / hex encoding (byte-accurate, length-aware) ---- */
static const char B64_ENC[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int b64_dec(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}
static Value nat_base64_encode(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "base64.encode() requires a str");
    ObjString *s = AS_STRING(argv[0]);
    const unsigned char *in = (const unsigned char *)s->chars;
    int n = s->length, o = 0;
    char *out = xmalloc((size_t)((n + 2) / 3) * 4 + 1);
    for (int i = 0; i < n; i += 3) {
        unsigned v = (unsigned)in[i] << 16;
        if (i + 1 < n) v |= (unsigned)in[i + 1] << 8;
        if (i + 2 < n) v |= (unsigned)in[i + 2];
        out[o++] = B64_ENC[(v >> 18) & 0x3f];
        out[o++] = B64_ENC[(v >> 12) & 0x3f];
        out[o++] = (i + 1 < n) ? B64_ENC[(v >> 6) & 0x3f] : '=';
        out[o++] = (i + 2 < n) ? B64_ENC[v & 0x3f] : '=';
    }
    Value r = string_val(I, out, o);
    free(out);
    return r;
}
static Value nat_base64_decode(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "base64.decode() requires a str");
    ObjString *s = AS_STRING(argv[0]);
    int n = s->length, o = 0, qc = 0, quad[4];
    char *out = xmalloc((size_t)(n / 4 + 1) * 3 + 1);
    for (int i = 0; i < n; i++) {
        char c = s->chars[i];
        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue; /* ignore whitespace */
        if (c == '=') break;                                            /* padding ends input */
        int v = b64_dec(c);
        if (v < 0) { free(out); runtime_error(I, 0, "base64.decode(): invalid character"); }
        quad[qc++] = v;
        if (qc == 4) {
            out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
            out[o++] = (char)((quad[1] << 4) | (quad[2] >> 2));
            out[o++] = (char)((quad[2] << 6) | quad[3]);
            qc = 0;
        }
    }
    if (qc == 1) { free(out); runtime_error(I, 0, "base64.decode(): truncated input"); }
    if (qc >= 2) out[o++] = (char)((quad[0] << 2) | (quad[1] >> 4));
    if (qc == 3) out[o++] = (char)((quad[1] << 4) | (quad[2] >> 2));
    Value r = string_val(I, out, o);
    free(out);
    return r;
}
static Value nat_hex_encode(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "hex.encode() requires a str");
    ObjString *s = AS_STRING(argv[0]);
    static const char H[] = "0123456789abcdef";
    const unsigned char *in = (const unsigned char *)s->chars;
    char *out = xmalloc((size_t)s->length * 2 + 1);
    for (int i = 0; i < s->length; i++) {
        out[i * 2]     = H[in[i] >> 4];
        out[i * 2 + 1] = H[in[i] & 0xf];
    }
    Value r = string_val(I, out, s->length * 2);
    free(out);
    return r;
}
static int hex_dec(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static Value nat_hex_decode(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "hex.decode() requires a str");
    ObjString *s = AS_STRING(argv[0]);
    if (s->length % 2 != 0) runtime_error(I, 0, "hex.decode(): odd-length input");
    char *out = xmalloc((size_t)(s->length / 2) + 1);
    for (int i = 0; i < s->length; i += 2) {
        int hi = hex_dec(s->chars[i]), lo = hex_dec(s->chars[i + 1]);
        if (hi < 0 || lo < 0) { free(out); runtime_error(I, 0, "hex.decode(): invalid character"); }
        out[i / 2] = (char)((hi << 4) | lo);
    }
    Value r = string_val(I, out, s->length / 2);
    free(out);
    return r;
}

/* ---- date/time (UTC, from a Unix timestamp; default = now) ---- */
static time_t time_arg(int argc, Value *argv) {
    return (argc >= 1 && IS_NUM(argv[0])) ? (time_t)as_double(argv[0]) : time(NULL);
}
static Value nat_time_iso(Interp *I, int argc, Value *argv) {
    time_t t = time_arg(argc, argv);
    struct tm tmv; gmtime_r(&t, &tmv);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);  /* ISO-8601 UTC */
    return cstring_val(I, buf);
}
static Value nat_time_parts(Interp *I, int argc, Value *argv) {
    time_t t = time_arg(argc, argv);
    struct tm tmv; gmtime_r(&t, &tmv);
    ObjMap *m = new_map(I);
    map_set(m, "year", int_val(tmv.tm_year + 1900));
    map_set(m, "month", int_val(tmv.tm_mon + 1));
    map_set(m, "day", int_val(tmv.tm_mday));
    map_set(m, "hour", int_val(tmv.tm_hour));
    map_set(m, "minute", int_val(tmv.tm_min));
    map_set(m, "second", int_val(tmv.tm_sec));
    map_set(m, "weekday", int_val(tmv.tm_wday)); /* 0 = Sunday */
    map_set(m, "yearday", int_val(tmv.tm_yday + 1));
    return obj_val((Obj *)m);
}
static Value nat_time_format(Interp *I, int argc, Value *argv) {
    if (argc < 2 || !IS_NUM(argv[0]) || !IS_STRING(argv[1]))
        runtime_error(I, 0, "time.format() requires (seconds: number, fmt: str)");
    time_t t = (time_t)as_double(argv[0]);
    struct tm tmv; gmtime_r(&t, &tmv);
    char buf[256];
    size_t n = strftime(buf, sizeof(buf), AS_STRING(argv[1])->chars, &tmv);
    return string_val(I, buf, (int)n); /* n == 0 if the result didn't fit */
}
static Value nat_similarity(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0]) || !IS_LIST(argv[1]))
        runtime_error(I, 0, "similarity() requires two vectors (list of numbers)");
    ObjList *a = AS_LIST(argv[0]), *b = AS_LIST(argv[1]);
    if (a->count != b->count) runtime_error(I, 0, "similarity(): vectors of different length");
    double dot = 0, na = 0, nb = 0;
    for (int i = 0; i < a->count; i++) {
        if (!IS_NUM(a->items[i]) || !IS_NUM(b->items[i])) runtime_error(I, 0, "similarity(): vectors must contain numbers");
        double x = as_double(a->items[i]), y = as_double(b->items[i]);
        dot += x * y; na += x * x; nb += y * y;
    }
    if (na == 0 || nb == 0) return float_val(0.0);
    return float_val(dot / (sqrt(na) * sqrt(nb)));
}

/* ---- collections, strings, math: completeness for everyday tasks ---- */
static int value_compare(Interp *I, Value a, Value b) {
    if (IS_NUM(a) && IS_NUM(b)) { double x = as_double(a), y = as_double(b); return x < y ? -1 : (x > y ? 1 : 0); }
    if (IS_STRING(a) && IS_STRING(b)) { int c = strcmp(AS_STRING(a)->chars, AS_STRING(b)->chars); return c < 0 ? -1 : (c > 0 ? 1 : 0); }
    runtime_error(I, 0, "invalid comparison between %s and %s", type_name(a), type_name(b));
    return 0;
}
/* qsort_r's argument order and comparator signature differ between the
   BSD/macOS and glibc/Linux flavors; provide both so the source is portable. */
#if defined(__GLIBC__)
static int sort_cmp(const void *pa, const void *pb, void *thunk) {
    return value_compare((Interp *)thunk, *(const Value *)pa, *(const Value *)pb);
}
#else
static int sort_cmp(void *thunk, const void *pa, const void *pb) {
    return value_compare((Interp *)thunk, *(const Value *)pa, *(const Value *)pb);
}
#endif
static Value nat_sort(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "sort() requires a list");
    ObjList *src = AS_LIST(argv[0]);
    ObjList *out = new_list(I);
    for (int i = 0; i < src->count; i++) list_push(out, src->items[i]);
    if (out->count > 1) {
#if defined(__GLIBC__)
        qsort_r(out->items, (size_t)out->count, sizeof(Value), sort_cmp, I);
#else
        qsort_r(out->items, (size_t)out->count, sizeof(Value), I, sort_cmp);
#endif
    }
    return obj_val((Obj *)out);
}
static Value nat_reverse(Interp *I, int argc, Value *argv) {
    if (IS_LIST(argv[0])) {
        ObjList *src = AS_LIST(argv[0]); ObjList *out = new_list(I);
        for (int i = src->count - 1; i >= 0; i--) list_push(out, src->items[i]);
        return obj_val((Obj *)out);
    }
    if (IS_STRING(argv[0])) {
        ObjString *s = AS_STRING(argv[0]); ObjString *r = new_string_n(I, s->chars, s->length);
        for (int i = 0; i < s->length; i++) r->chars[i] = s->chars[s->length - 1 - i];
        return obj_val((Obj *)r);
    }
    runtime_error(I, 0, "reverse() requires list or str");
    return NIL_VAL;
}
static Value nat_sum(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "sum() requires a list");
    ObjList *l = AS_LIST(argv[0]);
    int64_t si = 0; double sf = 0; bool is_float = false; bool int_overflow = false;
    for (int i = 0; i < l->count; i++) {
        if (!IS_NUM(l->items[i])) runtime_error(I, 0, "sum(): non-numeric element (%s)", type_name(l->items[i]));
        if (IS_FLOAT(l->items[i])) is_float = true;
        sf += as_double(l->items[i]);
        /* Accumulate the int sum overflow-safely (matching the VM '+' operator).
           __builtin_add_overflow never invokes UB and still wraps si on overflow.
           Defer reporting: the int sum is only the result when no float appears,
           so a mixed int+float sum must not fail on an int-only overflow. */
        if (IS_INT(l->items[i]) && __builtin_add_overflow(si, AS_INT(l->items[i]), &si))
            int_overflow = true;
    }
    if (!is_float && int_overflow) runtime_error(I, 0, "sum(): integer overflow");
    return is_float ? float_val(sf) : int_val(si);
}
static Value nat_min(Interp *I, int argc, Value *argv) {
    if (argc == 0) runtime_error(I, 0, "min() requires at least one argument");
    if (argc == 1 && IS_LIST(argv[0])) {
        ObjList *l = AS_LIST(argv[0]);
        if (l->count == 0) runtime_error(I, 0, "min() of empty list");
        Value best = l->items[0];
        for (int i = 1; i < l->count; i++) if (value_compare(I, l->items[i], best) < 0) best = l->items[i];
        return best;
    }
    Value best = argv[0];
    for (int i = 1; i < argc; i++) if (value_compare(I, argv[i], best) < 0) best = argv[i];
    return best;
}
static Value nat_max(Interp *I, int argc, Value *argv) {
    if (argc == 0) runtime_error(I, 0, "max() requires at least one argument");
    if (argc == 1 && IS_LIST(argv[0])) {
        ObjList *l = AS_LIST(argv[0]);
        if (l->count == 0) runtime_error(I, 0, "max() of empty list");
        Value best = l->items[0];
        for (int i = 1; i < l->count; i++) if (value_compare(I, l->items[i], best) > 0) best = l->items[i];
        return best;
    }
    Value best = argv[0];
    for (int i = 1; i < argc; i++) if (value_compare(I, argv[i], best) > 0) best = argv[i];
    return best;
}
static void norm_range(int64_t *start, int64_t *end, int len) {
    if (*start < 0) *start += len; if (*end < 0) *end += len;
    if (*start < 0) *start = 0; if (*end > len) *end = len;
    if (*end < *start) *end = *start;
}
static Value nat_slice(Interp *I, int argc, Value *argv) {
    if (argc < 2 || !IS_INT(argv[1]) || (argc >= 3 && !IS_INT(argv[2])))
        runtime_error(I, 0, "slice() requires (coll, start:int [, end:int])");
    if (IS_LIST(argv[0])) {
        ObjList *l = AS_LIST(argv[0]);
        int64_t s = AS_INT(argv[1]), e = (argc >= 3) ? AS_INT(argv[2]) : l->count;
        norm_range(&s, &e, l->count);
        ObjList *out = new_list(I);
        for (int64_t i = s; i < e; i++) list_push(out, l->items[i]);
        return obj_val((Obj *)out);
    }
    if (IS_STRING(argv[0])) {
        ObjString *str = AS_STRING(argv[0]);
        int64_t s = AS_INT(argv[1]), e = (argc >= 3) ? AS_INT(argv[2]) : str->length;
        norm_range(&s, &e, str->length);
        return string_val(I, str->chars + s, (int)(e - s));
    }
    runtime_error(I, 0, "slice() requires list or str");
    return NIL_VAL;
}
/* Length-aware substring search: respects ObjString->length instead of stopping
   at the first NUL (strings are length-prefixed and can hold embedded NULs).
   Returns the byte offset of the first match, or -1. An empty needle matches at 0. */
static int64_t str_find(const char *hay, int hlen, const char *needle, int nlen) {
    if (nlen == 0) return 0;
    if (nlen > hlen) return -1;
    for (int i = 0; i <= hlen - nlen; i++)
        if (memcmp(hay + i, needle, (size_t)nlen) == 0) return i;
    return -1;
}
static Value nat_index_of(Interp *I, int argc, Value *argv) {
    if (IS_LIST(argv[0])) {
        ObjList *l = AS_LIST(argv[0]);
        for (int i = 0; i < l->count; i++) if (values_equal(l->items[i], argv[1])) return int_val(i);
        return int_val(-1);
    }
    if (IS_STRING(argv[0]) && IS_STRING(argv[1])) {
        ObjString *h = AS_STRING(argv[0]), *n = AS_STRING(argv[1]);
        return int_val(str_find(h->chars, h->length, n->chars, n->length));
    }
    runtime_error(I, 0, "index_of() requires (list, x) or (str, str)");
    return NIL_VAL;
}
static Value nat_contains(Interp *I, int argc, Value *argv) {
    if (IS_STRING(argv[0]) && IS_STRING(argv[1])) {
        ObjString *h = AS_STRING(argv[0]), *n = AS_STRING(argv[1]);
        return bool_val(str_find(h->chars, h->length, n->chars, n->length) >= 0);
    }
    if (IS_LIST(argv[0])) {
        ObjList *l = AS_LIST(argv[0]);
        for (int i = 0; i < l->count; i++) if (values_equal(l->items[i], argv[1])) return bool_val(true);
        return bool_val(false);
    }
    if (IS_MAP(argv[0])) {
        char *k = value_to_cstr(I, argv[1]); bool f = map_get(AS_MAP(argv[0]), k) != NULL; free(k);
        return bool_val(f);
    }
    runtime_error(I, 0, "contains() requires str, list or map");
    return NIL_VAL;
}
static Value nat_trim(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "trim() requires str");
    ObjString *s = AS_STRING(argv[0]);
    int a = 0, b = s->length;
    while (a < b && isspace((unsigned char)s->chars[a])) a++;
    while (b > a && isspace((unsigned char)s->chars[b - 1])) b--;
    return string_val(I, s->chars + a, b - a);
}
static Value nat_replace(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1]) || !IS_STRING(argv[2]))
        runtime_error(I, 0, "replace() requires (str, str, str)");
    ObjString *s = AS_STRING(argv[0]), *o = AS_STRING(argv[1]), *n = AS_STRING(argv[2]);
    if (o->length == 0) return argv[0];
    char *buf = xmalloc(16); size_t len = 0, cap = 16; buf[0] = '\0';
    const char *p = s->chars, *end = s->chars + s->length;
    while (p < end) {
        int64_t off = str_find(p, (int)(end - p), o->chars, o->length); /* length-aware */
        if (off < 0) { append_str(&buf, &len, &cap, p, (size_t)(end - p)); break; }
        const char *hit = p + off;
        append_str(&buf, &len, &cap, p, (size_t)(hit - p));
        append_str(&buf, &len, &cap, n->chars, (size_t)n->length);
        p = hit + o->length;
    }
    Value out = string_val(I, buf, (int)len); free(buf); return out;
}
static Value nat_starts_with(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) runtime_error(I, 0, "starts_with() requires (str, str)");
    ObjString *s = AS_STRING(argv[0]), *p = AS_STRING(argv[1]);
    return bool_val(s->length >= p->length && memcmp(s->chars, p->chars, (size_t)p->length) == 0);
}
static Value nat_ends_with(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0]) || !IS_STRING(argv[1])) runtime_error(I, 0, "ends_with() requires (str, str)");
    ObjString *s = AS_STRING(argv[0]), *p = AS_STRING(argv[1]);
    return bool_val(s->length >= p->length && memcmp(s->chars + s->length - p->length, p->chars, (size_t)p->length) == 0);
}
static Value nat_repeat(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0]) || !IS_INT(argv[1])) runtime_error(I, 0, "repeat() requires (str, int)");
    ObjString *s = AS_STRING(argv[0]); int64_t n = AS_INT(argv[1]);
    if (n < 0) n = 0;
    /* All size math in 64-bit: a truncated (int)n could bypass the guard
       (giant alloc / silently wrong length) or hit a divide-by-zero. */
    int64_t limit = 100 * 1024 * 1024;
    if (n > 0 && (uint64_t)s->length > (uint64_t)limit / (uint64_t)n) runtime_error(I, 0, "repeat(): result too large");
    int64_t total = (int64_t)s->length * n; /* <= limit, fits in int */
    char *buf = xmalloc((size_t)total + 1);
    for (int64_t i = 0; i < n; i++) memcpy(buf + i * s->length, s->chars, (size_t)s->length);
    Value out = string_val(I, buf, (int)total); free(buf); return out;
}
static Value nat_round(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "round() requires a number");
    return int_from_double(I, round(as_double(argv[0])), "round(): value out of range");
}
static Value nat_ceil(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "ceil() requires a number");
    return int_from_double(I, ceil(as_double(argv[0])), "ceil(): value out of range");
}
static Value nat_pow(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0]) || !IS_NUM(argv[1])) runtime_error(I, 0, "pow() requires two numbers");
    return float_val(pow(as_double(argv[0]), as_double(argv[1])));
}
static Value nat_sin(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "sin() requires a number");
    return float_val(sin(as_double(argv[0])));
}
static Value nat_cos(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "cos() requires a number");
    return float_val(cos(as_double(argv[0])));
}
static Value nat_tan(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "tan() requires a number");
    return float_val(tan(as_double(argv[0])));
}
static Value nat_log(Interp *I, int argc, Value *argv) {
    /* log(x) = natural log; log(x, base) = log base b */
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "log() requires a number");
    double x = as_double(argv[0]);
    if (argc >= 2) {
        if (!IS_NUM(argv[1])) runtime_error(I, 0, "log(): base must be a number");
        return float_val(log(x) / log(as_double(argv[1])));
    }
    return float_val(log(x));
}
static Value nat_exp(Interp *I, int argc, Value *argv) {
    if (!IS_NUM(argv[0])) runtime_error(I, 0, "exp() requires a number");
    return float_val(exp(as_double(argv[0])));
}

/* xorshift64 — small, fast, decent-quality PRNG (state is never zero). */
static uint64_t rng_next(Interp *I) {
    uint64_t x = I->rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    I->rng_state = x;
    return x;
}
static Value nat_random(Interp *I, int argc, Value *argv) {
    (void)argc; (void)argv;
    /* 53 random mantissa bits -> a double uniformly in [0, 1) */
    uint64_t bits = rng_next(I) >> 11;
    return float_val((double)bits * (1.0 / 9007199254740992.0));
}
static Value nat_randint(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_INT(argv[0]) || !IS_INT(argv[1])) runtime_error(I, 0, "randint() requires (int, int)");
    int64_t a = AS_INT(argv[0]), b = AS_INT(argv[1]);
    if (a > b) runtime_error(I, 0, "randint(): low must be <= high");
    uint64_t span = (uint64_t)(b - a) + 1;        /* inclusive range */
    return int_val(a + (int64_t)(rng_next(I) % span));
}
static Value nat_seed(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_INT(argv[0])) runtime_error(I, 0, "seed() requires an int");
    uint64_t s = (uint64_t)AS_INT(argv[0]);
    I->rng_state = s ? s : 0x9e3779b97f4a7c15ULL; /* avoid the zero fixed point */
    return NIL_VAL;
}
static Value nat_del(Interp *I, int argc, Value *argv) {
    if (!IS_MAP(argv[0])) runtime_error(I, 0, "del() requires a map");
    char *key = value_to_cstr(I, argv[1]);
    ObjMap *m = AS_MAP(argv[0]);
    for (int i = 0; i < m->count; i++) {
        if (strcmp(m->entries[i].key, key) == 0) {
            free(m->entries[i].key);
            for (int j = i; j < m->count - 1; j++) m->entries[j] = m->entries[j + 1];
            m->count--;
            break;
        }
    }
    free(key);
    return argv[0];
}
static Value nat_chars(Interp *I, int argc, Value *argv) {
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "chars() requires str");
    ObjString *s = AS_STRING(argv[0]); ObjList *l = new_list(I);
    for (int i = 0; i < s->length; i++) list_push(l, string_val(I, s->chars + i, 1));
    return obj_val((Obj *)l);
}
/* pad a string to `width` with `fill` (default " ", cycled) on the left/right */
static Value nat_pad(Interp *I, int argc, Value *argv, bool at_start) {
    const char *who = at_start ? "pad_start" : "pad_end";
    if (!IS_STRING(argv[0]) || !IS_INT(argv[1]))
        runtime_error(I, 0, "%s() requires (str, width:int [, fill:str])", who);
    ObjString *s = AS_STRING(argv[0]);
    int64_t width = AS_INT(argv[1]);
    bool has_fill = argc >= 3 && IS_STRING(argv[2]);
    const char *fill = has_fill ? AS_STRING(argv[2])->chars : " ";
    int fill_len = has_fill ? AS_STRING(argv[2])->length : 1;
    if (fill_len <= 0 || width <= s->length) return argv[0]; /* nothing to add */
    if (width > 100 * 1024 * 1024) runtime_error(I, 0, "%s(): width too large", who);
    int pad = (int)width - s->length;
    char *buf = xmalloc((size_t)width + 1);
    char *fillp = at_start ? buf : buf + s->length;
    char *strp  = at_start ? buf + pad : buf;
    for (int i = 0; i < pad; i++) fillp[i] = fill[i % fill_len];
    memcpy(strp, s->chars, (size_t)s->length);
    Value out = string_val(I, buf, (int)width);
    free(buf);
    return out;
}
static Value nat_pad_start(Interp *I, int argc, Value *argv) { return nat_pad(I, argc, argv, true); }
static Value nat_pad_end(Interp *I, int argc, Value *argv) { return nat_pad(I, argc, argv, false); }
static Value nat_lines(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "lines() requires str");
    ObjString *s = AS_STRING(argv[0]);
    ObjList *out = new_list(I);
    if (s->length == 0) return obj_val((Obj *)out);
    int start = 0;
    for (int i = 0; i <= s->length; i++) {
        if (i == s->length || s->chars[i] == '\n') {
            int end = i;
            if (end > start && s->chars[end - 1] == '\r') end--; /* handle CRLF */
            /* drop the final empty segment produced by a trailing newline */
            if (i == s->length && start == i && s->chars[s->length - 1] == '\n') break;
            list_push(out, string_val(I, s->chars + start, end - start));
            start = i + 1;
        }
    }
    return obj_val((Obj *)out);
}
static Value nat_words(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_STRING(argv[0])) runtime_error(I, 0, "words() requires str");
    ObjString *s = AS_STRING(argv[0]);
    ObjList *out = new_list(I);
    int i = 0;
    while (i < s->length) {
        while (i < s->length && isspace((unsigned char)s->chars[i])) i++;
        int start = i;
        while (i < s->length && !isspace((unsigned char)s->chars[i])) i++;
        if (i > start) list_push(out, string_val(I, s->chars + start, i - start));
    }
    return obj_val((Obj *)out);
}
static Value nat_now(Interp *I, int argc, Value *argv) {
    (void)I; (void)argc; (void)argv;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return float_val((double)tv.tv_sec + (double)tv.tv_usec / 1e6); /* sub-second wall clock */
}

/* ---- higher-order: map / filter / reduce / each / find (need a VM callback) ---- */
static Value nat_map(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "map() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    ObjList *out = new_list(I);
    vm_push(I->vm, obj_val((Obj *)out)); /* root the result while callbacks run the GC */
    for (int i = 0; i < src->count; i++) {
        Value a = src->items[i];
        list_push(out, vm_invoke(I->vm, argv[1], 1, &a));
    }
    vm_pop(I->vm);
    return obj_val((Obj *)out);
}
static Value nat_filter(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "filter() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    ObjList *out = new_list(I);
    vm_push(I->vm, obj_val((Obj *)out)); /* root the result while callbacks run the GC */
    for (int i = 0; i < src->count; i++) {
        Value a = src->items[i];
        if (is_truthy(vm_invoke(I->vm, argv[1], 1, &a))) list_push(out, a);
    }
    vm_pop(I->vm);
    return obj_val((Obj *)out);
}
static Value nat_reduce(Interp *I, int argc, Value *argv) {
    if (argc < 3 || !IS_LIST(argv[0])) runtime_error(I, 0, "reduce() requires (list, fn, initial)");
    ObjList *src = AS_LIST(argv[0]);
    vm_push(I->vm, argv[2]);              /* accumulator, kept rooted on the value stack */
    Value *acc = I->vm->stack_top - 1;
    for (int i = 0; i < src->count; i++) {
        Value pair[2] = { *acc, src->items[i] };
        *acc = vm_invoke(I->vm, argv[1], 2, pair);
    }
    Value result = *acc;
    vm_pop(I->vm);
    return result;
}
static Value nat_each(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "each() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    for (int i = 0; i < src->count; i++) {
        Value a = src->items[i];
        vm_invoke(I->vm, argv[1], 1, &a);
    }
    return NIL_VAL;
}
static Value nat_find(Interp *I, int argc, Value *argv) {
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "find() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    for (int i = 0; i < src->count; i++) {
        Value a = src->items[i];
        if (is_truthy(vm_invoke(I->vm, argv[1], 1, &a))) return a;
    }
    return NIL_VAL;
}
/* The pure-C collection helpers below never re-enter the VM, so the GC (which
   only runs at the VM safe-point) cannot fire mid-call — their intermediate
   allocations need no rooting. any/all run a callback but hold no unrooted
   accumulator; group_by does, so it roots its result map on the value stack. */
static Value nat_zip(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0]) || !IS_LIST(argv[1])) runtime_error(I, 0, "zip() requires (list, list)");
    ObjList *a = AS_LIST(argv[0]), *b = AS_LIST(argv[1]);
    int n = a->count < b->count ? a->count : b->count;
    ObjList *out = new_list(I);
    for (int i = 0; i < n; i++) {
        ObjList *pair = new_list(I);
        list_push(pair, a->items[i]);
        list_push(pair, b->items[i]);
        list_push(out, obj_val((Obj *)pair));
    }
    return obj_val((Obj *)out);
}
static Value nat_enumerate(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "enumerate() requires a list");
    ObjList *src = AS_LIST(argv[0]);
    ObjList *out = new_list(I);
    for (int i = 0; i < src->count; i++) {
        ObjList *pair = new_list(I);
        list_push(pair, int_val(i));
        list_push(pair, src->items[i]);
        list_push(out, obj_val((Obj *)pair));
    }
    return obj_val((Obj *)out);
}
static Value nat_unique(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "unique() requires a list");
    ObjList *src = AS_LIST(argv[0]);
    ObjList *out = new_list(I);
    for (int i = 0; i < src->count; i++) {
        bool seen = false;
        for (int j = 0; j < out->count; j++)
            if (values_equal(out->items[j], src->items[i])) { seen = true; break; }
        if (!seen) list_push(out, src->items[i]);
    }
    return obj_val((Obj *)out);
}
static Value nat_flatten(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "flatten() requires a list");
    ObjList *src = AS_LIST(argv[0]);
    ObjList *out = new_list(I);
    for (int i = 0; i < src->count; i++) {
        if (IS_LIST(src->items[i])) {
            ObjList *inner = AS_LIST(src->items[i]);
            for (int j = 0; j < inner->count; j++) list_push(out, inner->items[j]);
        } else {
            list_push(out, src->items[i]);
        }
    }
    return obj_val((Obj *)out);
}
static Value nat_count(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "count() requires (list, value)");
    ObjList *src = AS_LIST(argv[0]);
    int64_t n = 0;
    for (int i = 0; i < src->count; i++)
        if (values_equal(src->items[i], argv[1])) n++;
    return int_val(n);
}
static Value nat_any(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "any() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    for (int i = 0; i < src->count; i++) {
        Value a = src->items[i];
        if (is_truthy(vm_invoke(I->vm, argv[1], 1, &a))) return bool_val(true);
    }
    return bool_val(false);
}
static Value nat_all(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "all() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    for (int i = 0; i < src->count; i++) {
        Value a = src->items[i];
        if (!is_truthy(vm_invoke(I->vm, argv[1], 1, &a))) return bool_val(false);
    }
    return bool_val(true);
}
static Value nat_group_by(Interp *I, int argc, Value *argv) {
    (void)argc;
    if (!IS_LIST(argv[0])) runtime_error(I, 0, "group_by() requires (list, fn)");
    ObjList *src = AS_LIST(argv[0]);
    ObjMap *groups = new_map(I);
    vm_push(I->vm, obj_val((Obj *)groups)); /* root accumulator across callbacks */
    for (int i = 0; i < src->count; i++) {
        Value x = src->items[i];
        Value k = vm_invoke(I->vm, argv[1], 1, &x);
        char *key = value_to_cstr(I, k);     /* group key as a string */
        Value *slot = map_get(groups, key);
        ObjList *bucket;
        if (slot && IS_LIST(*slot)) {
            bucket = AS_LIST(*slot);
        } else {
            bucket = new_list(I);
            map_set(groups, key, obj_val((Obj *)bucket));
        }
        list_push(bucket, x);
        free(key);
    }
    vm_pop(I->vm);
    return obj_val((Obj *)groups);
}

/* register a native inside a namespace map (e.g. json.parse) */
static void define_native_in(Interp *I, ObjMap *ns, const char *name, NativeFn fn, int arity) {
    ObjNative *nat = (ObjNative *)alloc_obj(I, sizeof(ObjNative), OBJ_NATIVE);
    nat->fn = fn; nat->name = name; nat->arity = arity;
    map_set(ns, name, obj_val((Obj *)nat));
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
    define_native(I, "now", nat_now, 0);
    define_native(I, "input", nat_input, -1);
    define_native(I, "assert", nat_assert, -1);

    /* --- collections --- */
    define_native(I, "sort", nat_sort, 1);
    define_native(I, "reverse", nat_reverse, 1);
    define_native(I, "sum", nat_sum, 1);
    define_native(I, "min", nat_min, -1);
    define_native(I, "max", nat_max, -1);
    define_native(I, "slice", nat_slice, -1);
    define_native(I, "index_of", nat_index_of, 2);
    define_native(I, "contains", nat_contains, 2);
    define_native(I, "del", nat_del, 2);
    define_native(I, "map", nat_map, 2);
    define_native(I, "filter", nat_filter, 2);
    define_native(I, "reduce", nat_reduce, 3);
    define_native(I, "each", nat_each, 2);
    define_native(I, "find", nat_find, 2);
    define_native(I, "any", nat_any, 2);
    define_native(I, "all", nat_all, 2);
    define_native(I, "zip", nat_zip, 2);
    define_native(I, "enumerate", nat_enumerate, 1);
    define_native(I, "unique", nat_unique, 1);
    define_native(I, "flatten", nat_flatten, 1);
    define_native(I, "count", nat_count, 2);
    define_native(I, "group_by", nat_group_by, 2);

    /* --- strings --- */
    define_native(I, "trim", nat_trim, 1);
    define_native(I, "replace", nat_replace, 3);
    define_native(I, "starts_with", nat_starts_with, 2);
    define_native(I, "ends_with", nat_ends_with, 2);
    define_native(I, "repeat", nat_repeat, 2);
    define_native(I, "chars", nat_chars, 1);
    define_native(I, "pad_start", nat_pad_start, -1);
    define_native(I, "pad_end", nat_pad_end, -1);
    define_native(I, "lines", nat_lines, 1);
    define_native(I, "words", nat_words, 1);

    /* --- math --- */
    define_native(I, "round", nat_round, 1);
    define_native(I, "ceil", nat_ceil, 1);
    define_native(I, "pow", nat_pow, 2);
    define_native(I, "sin", nat_sin, 1);
    define_native(I, "cos", nat_cos, 1);
    define_native(I, "tan", nat_tan, 1);
    define_native(I, "log", nat_log, -1);   /* log(x) | log(x, base) */
    define_native(I, "exp", nat_exp, 1);
    define_native(I, "random", nat_random, 0);
    define_native(I, "randint", nat_randint, 2);
    define_native(I, "seed", nat_seed, 1);
    table_set(I->globals, "PI", hash_string("PI", 2), float_val(3.14159265358979323846));
    table_set(I->globals, "E", hash_string("E", 1), float_val(2.71828182845904523536));

    /* --- AI-first batteries --- */
    define_native(I, "ai", nat_ai, -1);              /* ai("prompt") | ai("prompt", model|opts) */
    define_native(I, "ai_all", nat_ai_all, -1);      /* ai_all([prompts] [, opts]) — concurrent */
    define_native(I, "env", nat_env, 1);
    define_native(I, "read", nat_read, 1);
    define_native(I, "write", nat_write, 2);
    define_native(I, "similarity", nat_similarity, 2);
    /* filesystem */
    define_native(I, "ls", nat_ls, 1);
    define_native(I, "exists", nat_exists, 1);
    define_native(I, "is_dir", nat_is_dir, 1);
    define_native(I, "mkdir", nat_mkdir, 1);
    define_native(I, "rm", nat_rm, 1);
    /* process / OS */
    define_native(I, "run", nat_run, 1);
    define_native(I, "run_all", nat_run_all, 1);
    define_native(I, "args", nat_args, 0);
    define_native(I, "exit", nat_exit, -1);

    ObjMap *json = new_map(I);
    define_native_in(I, json, "parse", nat_json_parse, 1);
    define_native_in(I, json, "stringify", nat_json_stringify, 1);
    table_set(I->globals, "json", hash_string("json", 4), obj_val((Obj *)json));

    ObjMap *http = new_map(I);
    define_native_in(I, http, "get", nat_http_get, 1);
    define_native_in(I, http, "post", nat_http_post, -1);
    table_set(I->globals, "http", hash_string("http", 4), obj_val((Obj *)http));

    ObjMap *path = new_map(I);
    define_native_in(I, path, "join", nat_path_join, -1);
    define_native_in(I, path, "dirname", nat_path_dirname, 1);
    define_native_in(I, path, "basename", nat_path_basename, 1);
    define_native_in(I, path, "ext", nat_path_ext, 1);
    table_set(I->globals, "path", hash_string("path", 4), obj_val((Obj *)path));

    ObjMap *b64 = new_map(I);
    define_native_in(I, b64, "encode", nat_base64_encode, 1);
    define_native_in(I, b64, "decode", nat_base64_decode, 1);
    table_set(I->globals, "base64", hash_string("base64", 6), obj_val((Obj *)b64));

    ObjMap *hex = new_map(I);
    define_native_in(I, hex, "encode", nat_hex_encode, 1);
    define_native_in(I, hex, "decode", nat_hex_decode, 1);
    table_set(I->globals, "hex", hash_string("hex", 3), obj_val((Obj *)hex));

    ObjMap *tm = new_map(I);
    define_native_in(I, tm, "iso", nat_time_iso, -1);       /* time.iso([secs]) */
    define_native_in(I, tm, "parts", nat_time_parts, -1);   /* time.parts([secs]) */
    define_native_in(I, tm, "format", nat_time_format, 2);  /* time.format(secs, fmt) */
    table_set(I->globals, "time", hash_string("time", 4), obj_val((Obj *)tm));
}

/* ===========================================================================
 * Driver
 * ========================================================================= */
static char *read_file(const char *path) {
    struct stat st;
    if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
        fprintf(stderr, "'%s' is a directory, not a file\n", path);
        return NULL;
    }
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "could not open '%s'\n", path); return NULL; }
    /* stream-read: works for regular files and unseekable pipes/stdin alike */
    size_t cap = 4096, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        if (cap - len < 4096) { cap *= 2; buf = xrealloc(buf, cap); }
        size_t got = fread(buf + len, 1, cap - len, f);
        len += got;
        if (got == 0) break;
    }
    if (ferror(f)) { fprintf(stderr, "read error on '%s'\n", path); free(buf); fclose(f); return NULL; }
    buf[len] = '\0';
    fclose(f);
    return buf;
}

/* Compile and run a source string (or a file when src == NULL). The error
 * landing pad (I->err_jmp) is saved/restored so nested imports nest safely. */
/* Keep a source text alive for the program's lifetime so proto->source (used for
   source-line diagnostics) never dangles after a module's run_source returns. */
static void keep_source(Interp *I, char *s) {
    if (I->source_count + 1 > I->source_cap) {
        I->source_cap = I->source_cap < 8 ? 8 : I->source_cap * 2;
        I->sources = xrealloc(I->sources, sizeof(char *) * (size_t)I->source_cap);
    }
    I->sources[I->source_count++] = s;
}
static int run_source(Interp *I, const char *src, const char *path) {
    /* Bound import recursion: a nested import re-enters run_source on the C
       stack, so an unguarded cycle would overflow it. Both guards return error
       code 70 with a message OP_IMPORT/run_module_ns re-raise as a catchable
       Loqi error. They return BEFORE pushing onto the import stack, so the
       matching decrement below stays balanced even when the run unwinds via
       setjmp. */
    if (path) {
        for (int i = 0; i < I->import_depth; i++) {
            if (I->import_stack[i] && strcmp(I->import_stack[i], path) == 0) {
                snprintf(I->err_msg, sizeof(I->err_msg), "cyclic import of '%s'", path);
                return 70;
            }
        }
    }
    if (I->import_depth >= MAX_IMPORT_DEPTH) {
        snprintf(I->err_msg, sizeof(I->err_msg),
                 "import nesting too deep (possible import cycle) while loading '%s'",
                 path ? path : "?");
        return 70;
    }
    I->import_stack[I->import_depth++] = path; /* pop = import_depth-- on every exit */
    char *owned = NULL;
    if (src == NULL) {
        owned = read_file(path);
        if (!owned) { I->import_depth--; return 74; }
        src = owned;
    }
    /* keep copies of the source + path alive for diagnostics (proto->source/path) */
    char *kept_source = strdup(src);
    keep_source(I, kept_source);
    char *kept_path = strdup(path ? path : "?");
    keep_source(I, kept_path);
    const char *prev_source = I->current_source;
    const char *prev_cur_path = I->current_path;
    I->current_source = kept_source;
    I->current_path = kept_path;
    const char *prev_path = I->path;
    I->path = path;

    Parser P;
    P.I = I; P.had_error = false; P.depth = 0;
    lex_init(&P.lex, I, src);
    p_advance(&P);
    Node *prog = parse_program(&P);
    if (P.had_error) { free(owned); I->path = prev_path; I->current_source = prev_source; I->current_path = prev_cur_path; I->import_depth--; return 65; }

    ObjProto *script = compile_script(I, prog);
    if (!script) { free(owned); I->path = prev_path; I->current_source = prev_source; I->current_path = prev_cur_path; I->import_depth--; return 65; }

    /* save the enclosing error landing pad + VM for safe nesting (imports) */
    jmp_buf saved; memcpy(saved, I->err_jmp, sizeof(jmp_buf));
    VM *prev_vm = I->vm;
    VM *vm = xmalloc(sizeof(VM));
    vm->I = I; vm->enclosing = prev_vm;
    vm->stack_top = vm->stack; vm->frame_count = 0; vm->open_upvalues = NULL;
    vm->handler_count = 0;
    memset(vm->trap, 0, sizeof(jmp_buf));
    I->vm = vm;

    int rc = 0;
    if (setjmp(I->err_jmp)) {
        /* Only report at true top-level. For a nested import (prev_vm != NULL)
           OP_IMPORT re-raises via vm_error in the outer VM, where an enclosing
           try/catch may handle it; printing here would spam stderr for a fully
           recovered import failure. The outer handler/top-level is the sole
           reporter in that case. */
        if (prev_vm == NULL) {
            fprintf(stderr, "%s\n", I->err_msg);
            if (I->err_source[0]) fprintf(stderr, "%s\n", I->err_source);
            if (I->err_trace[0]) fputs(I->err_trace, stderr);
        }
        rc = 70;
    } else {
        ObjClosure *cl = new_closure(I, script);
        vm_push(vm, obj_val((Obj *)cl));
        vm_call_closure(vm, cl, 0);
        run_vm(vm, 0);
    }
    free(vm);
    free(owned);
    I->vm = prev_vm;
    memcpy(I->err_jmp, saved, sizeof(jmp_buf)); /* restore enclosing landing pad */
    I->path = prev_path;
    I->current_source = prev_source;
    I->current_path = prev_cur_path;
    I->import_depth--;
    return rc;
}

static int interpret(Interp *I, const char *src, const char *path) {
    return run_source(I, src, path);
}

/* Run a module in a fresh global namespace (seeded with the stdlib) and return a
 * map of its own top-level definitions, for `import "path" as name`. */
static void table_free(Table *t) {
    for (int i = 0; i < t->cap; i++) free(t->entries[i].key);
    free(t->entries);
    table_init(t);
}
static Value run_module_ns(Interp *I, char *path /* owned */) {
    /* A module evaluates exactly once: a second `import "x" as ...` — including a
       diamond where two modules both import the same file — returns the cached
       namespace instead of re-running the file's top-level code. Keyed by the
       canonical absolute path so "./x.lq", "x.lq" and "../d/x.lq" dedupe. */
    char canon[PATH_MAX];
    const char *key = realpath(path, canon) ? canon : path;
    uint32_t kh = hash_string(key, strlen(key));
    Value *hit = table_get(I->module_cache, key, kh);
    if (hit) { free(path); return *hit; }

    Table *saved = I->globals;
    Table *prev_defines = I->module_defines;
    Table defines; table_init(&defines);   /* names the module defines, for export */
    Table *mod = xmalloc(sizeof(Table));
    table_init(mod);
    /* own `mod` so interp_free reclaims it (it is not a GC object). Register before
       evaluation: even if the module errors out, the table is tracked and freed. */
    if (I->module_table_count == I->module_table_cap) {
        I->module_table_cap = I->module_table_cap ? I->module_table_cap * 2 : 8;
        I->module_tables = xrealloc(I->module_tables, sizeof(Table *) * (size_t)I->module_table_cap);
    }
    I->module_tables[I->module_table_count++] = mod;
    I->globals = mod;
    I->module_defines = &defines;
    install_stdlib(I);                 /* seeds built-ins (not via OP_DEFINE_GLOBAL, so unrecorded) */
    int rc = run_source(I, NULL, path);
    I->globals = saved;                /* restore before raising / building the map */
    I->module_defines = prev_defines;
    if (rc != 0) {
        /* copy what the message needs, then free `path` before longjmp-ing out */
        char name[512]; snprintf(name, sizeof(name), "%s", path);
        char cause[512]; cause[0] = '\0';
        if (rc == 70) snprintf(cause, sizeof(cause), "%s", I->err_msg);
        free(path); table_free(&defines);
        if (rc == 70) runtime_error(I, 0, "import of '%s' failed: %s", name, cause);
        runtime_error(I, 0, "import of '%s' failed (code %d)", name, rc);
    }
    /* namespace = exactly the names the module defined at top level (so a module
       can export a name even when it shadows a built-in, e.g. PI or log). */
    ObjMap *ns = new_map(I);
    for (int i = 0; i < mod->cap; i++) {
        char *k = mod->entries[i].key;
        if (k && table_get(&defines, k, mod->entries[i].hash))
            map_set(ns, k, mod->entries[i].value);
    }
    table_free(&defines);
    /* cache for single-evaluation; `key` (canon or path) is still valid here. The
       cache is a GC root, so ns and the module's state stay alive. */
    table_set(I->module_cache, key, kh, obj_val((Obj *)ns));
    free(path);
    return obj_val((Obj *)ns);
}

static void interp_init(Interp *I) {
    I->arena = NULL;
    I->path = NULL;
    I->vm = NULL;
    I->has_error = false;
    I->err_msg[0] = '\0';
    I->err_trace[0] = '\0';
    I->err_source[0] = '\0';
    I->current_source = NULL;
    I->current_path = NULL;
    I->sources = NULL; I->source_count = 0; I->source_cap = 0;
    I->import_depth = 0;
    I->obj_count = 0;
    I->next_gc = 1u << 14;   /* ~16k objects before the first collection */
    I->gc_gen = 0;
    I->gray = NULL; I->gray_count = 0; I->gray_cap = 0;
    I->gc_stress = getenv("LOQI_GC_STRESS") != NULL;
    I->gc_pending = I->gc_stress; /* stress mode: collect from the first instruction */
    { uint64_t s = (uint64_t)time(NULL) * 0x2545F4914F6CDD1DULL + 0x9e3779b97f4a7c15ULL;
      I->rng_state = s ? s : 0x9e3779b97f4a7c15ULL; } /* non-zero xorshift seed */
    I->module_defines = NULL;
    I->module_cache = xmalloc(sizeof(Table));
    table_init(I->module_cache);
    I->module_tables = NULL; I->module_table_count = 0; I->module_table_cap = 0;
    I->script_argc = 0; I->script_argv = NULL;
    I->globals = xmalloc(sizeof(Table));
    table_init(I->globals);
    install_stdlib(I);
}

/* Tear the interpreter all the way down so a run leaves no reachable-at-exit
 * allocations (clean under leak detectors / valgrind). Frees every heap object in
 * the arena, the global + module-cache tables, every per-module globals table, the
 * kept source/path strings, and the GC gray worklist. Order: objects first (they
 * own only their own buffers), then the tables (which never own their values), so
 * nothing is touched after it is freed. */
static void interp_free(Interp *I) {
    Obj *o = I->arena;
    while (o) { Obj *next = o->arena_next; gc_free_obj(o); o = next; }
    I->arena = NULL;
    if (I->globals) { table_free(I->globals); free(I->globals); I->globals = NULL; }
    if (I->module_cache) { table_free(I->module_cache); free(I->module_cache); I->module_cache = NULL; }
    for (int i = 0; i < I->module_table_count; i++) { table_free(I->module_tables[i]); free(I->module_tables[i]); }
    free(I->module_tables); I->module_tables = NULL; I->module_table_count = I->module_table_cap = 0;
    for (int i = 0; i < I->source_count; i++) free(I->sources[i]);
    free(I->sources); I->sources = NULL; I->source_count = I->source_cap = 0;
    free(I->gray); I->gray = NULL; I->gray_count = I->gray_cap = 0;
}

#define LOQI_VERSION "0.2.0"

static void repl(Interp *I) {
    printf("Loqi %s — REPL. Ctrl-D to exit.\n", LOQI_VERSION);
    char *line = NULL; size_t cap = 0;
    for (;;) {
        fputs("loqi> ", stdout); fflush(stdout);
        ssize_t got = getline(&line, &cap, stdin);
        if (got < 0) { printf("\n"); break; }
        interpret(I, line, "<repl>");
    }
    free(line);
}

int main(int argc, char **argv) {
    Interp I;
    interp_init(&I);

    int rc = 0;
    if (argc == 1) {
        repl(&I);
    } else if (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-v") == 0) {
        printf("Loqi %s\n", LOQI_VERSION);
    } else {
        char *src = read_file(argv[1]);
        if (!src) { interp_free(&I); return 74; }
        /* expose any arguments after the script path to args() */
        I.script_argc = argc - 2;
        I.script_argv = (argc > 2) ? &argv[2] : NULL;
        rc = interpret(&I, src, argv[1]);
        free(src);
    }
    interp_free(&I);
    return rc;
}
