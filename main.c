/*
 * MiniSQLite - a tiny educational SQL-like database that runs in the terminal.
 *
 * Pipeline for every line the user types:
 *   1. read line            (main loop, read_line)
 *   2. tokenize             (tokenize: words, numbers, 'strings', symbols)
 *   3. parse + validate     (exec_create / exec_insert / ...)  - nothing is
 *                           changed until the whole command is known to be valid
 *   4. apply to memory      (Database -> Table -> rows of Value)
 *   5. save to disk         (save_db writes database.json atomically)
 *
 * Storage format (database.json):
 *   { "tables": [ { "name": "students",
 *                   "columns": [ {"name": "id", "type": "INTEGER"}, ... ],
 *                   "rows": [ [1, "Ani", 20], ... ] } ] }
 *
 * No external libraries: the JSON reader/writer is implemented below.
 */
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define MAX_NAME_LEN 64

/* ======================================================================
 *  Data model
 * ====================================================================== */

typedef enum { TYPE_INTEGER, TYPE_TEXT } ColType;

typedef struct {
    ColType type;
    long long i; /* used when type == TYPE_INTEGER */
    char *s;     /* used when type == TYPE_TEXT */
} Value;

typedef struct {
    char *name;
    ColType type;
} Column;

typedef struct {
    char *name;
    int ncols;
    Column *cols;
    int nrows, caprows;
    Value **rows; /* rows[r] is an array of ncols values */
} Table;

typedef struct {
    int ntables, cap;
    Table **tables;
} Database;

static Database db;
static const char *db_path = "database.json";

/* ---------- memory helpers ---------- */

static void *xmalloc(size_t n) {
    void *p = malloc(n ? n : 1);
    if (!p) { fprintf(stderr, "Fatal: out of memory\n"); exit(1); }
    return p;
}

static void *xrealloc(void *p, size_t n) {
    p = realloc(p, n ? n : 1);
    if (!p) { fprintf(stderr, "Fatal: out of memory\n"); exit(1); }
    return p;
}

static char *xstrdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *d = xmalloc(n);
    memcpy(d, s, n);
    return d;
}

static const char *type_name(ColType t) { return t == TYPE_INTEGER ? "INTEGER" : "TEXT"; }

static void free_row(Value *row, int ncols) {
    for (int c = 0; c < ncols; c++)
        if (row[c].type == TYPE_TEXT) free(row[c].s);
    free(row);
}

static void free_table(Table *t) {
    for (int r = 0; r < t->nrows; r++) free_row(t->rows[r], t->ncols);
    free(t->rows);
    for (int c = 0; c < t->ncols; c++) free(t->cols[c].name);
    free(t->cols);
    free(t->name);
    free(t);
}

static Table *find_table(const char *name) {
    for (int i = 0; i < db.ntables; i++)
        if (strcasecmp(db.tables[i]->name, name) == 0) return db.tables[i];
    return NULL;
}

static int find_col(const Table *t, const char *name) {
    for (int c = 0; c < t->ncols; c++)
        if (strcasecmp(t->cols[c].name, name) == 0) return c;
    return -1;
}

static void db_add_table(Table *t) {
    if (db.ntables == db.cap) {
        db.cap = db.cap ? db.cap * 2 : 8;
        db.tables = xrealloc(db.tables, sizeof(Table *) * db.cap);
    }
    db.tables[db.ntables++] = t;
}

static void table_add_row(Table *t, Value *row) {
    if (t->nrows == t->caprows) {
        t->caprows = t->caprows ? t->caprows * 2 : 8;
        t->rows = xrealloc(t->rows, sizeof(Value *) * t->caprows);
    }
    t->rows[t->nrows++] = row;
}

static Value value_copy(const Value *v) {
    Value out = *v;
    if (v->type == TYPE_TEXT) out.s = xstrdup(v->s);
    return out;
}

static int value_equals(const Value *a, const Value *b) {
    if (a->type != b->type) return 0;
    if (a->type == TYPE_INTEGER) return a->i == b->i;
    return strcmp(a->s, b->s) == 0;
}

/* ======================================================================
 *  JSON writer
 * ====================================================================== */

static void json_write_string(FILE *f, const char *s) {
    fputc('"', f);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        switch (*p) {
        case '"':  fputs("\\\"", f); break;
        case '\\': fputs("\\\\", f); break;
        case '\n': fputs("\\n", f); break;
        case '\r': fputs("\\r", f); break;
        case '\t': fputs("\\t", f); break;
        default:
            if (*p < 0x20) fprintf(f, "\\u%04x", *p);
            else fputc(*p, f);
        }
    }
    fputc('"', f);
}

/* Writes to a temporary file first and then renames it over the real file,
 * so a crash in the middle of saving never leaves a half-written database. */
static int save_db(void) {
    size_t len = strlen(db_path) + 5;
    char *tmp = xmalloc(len);
    snprintf(tmp, len, "%s.tmp", db_path);

    FILE *f = fopen(tmp, "w");
    if (!f) {
        fprintf(stderr, "Error: cannot write '%s': %s\n", tmp, strerror(errno));
        free(tmp);
        return 0;
    }
    fprintf(f, "{\n  \"tables\": [");
    for (int i = 0; i < db.ntables; i++) {
        Table *t = db.tables[i];
        fprintf(f, "%s\n    {\n      \"name\": ", i ? "," : "");
        json_write_string(f, t->name);
        fprintf(f, ",\n      \"columns\": [");
        for (int c = 0; c < t->ncols; c++) {
            fprintf(f, "%s\n        {\"name\": ", c ? "," : "");
            json_write_string(f, t->cols[c].name);
            fprintf(f, ", \"type\": \"%s\"}", type_name(t->cols[c].type));
        }
        fprintf(f, "\n      ],\n      \"rows\": [");
        for (int r = 0; r < t->nrows; r++) {
            fprintf(f, "%s\n        [", r ? "," : "");
            for (int c = 0; c < t->ncols; c++) {
                Value *v = &t->rows[r][c];
                if (c) fputs(", ", f);
                if (v->type == TYPE_INTEGER) fprintf(f, "%lld", v->i);
                else json_write_string(f, v->s);
            }
            fputc(']', f);
        }
        fprintf(f, "%s]\n    }", t->nrows ? "\n      " : "");
    }
    fprintf(f, "%s]\n}\n", db.ntables ? "\n  " : "");

    int ok = !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (ok && rename(tmp, db_path) != 0) ok = 0;
    if (!ok) {
        fprintf(stderr, "Error: failed to save database to '%s': %s\n", db_path, strerror(errno));
        remove(tmp);
    }
    free(tmp);
    return ok;
}

/* ======================================================================
 *  JSON reader (small, generic: builds a tree, then the loader walks it)
 * ====================================================================== */

typedef enum { J_NULL, J_BOOL, J_NUM, J_STR, J_ARR, J_OBJ } JType;

typedef struct JNode {
    JType type;
    long long num;         /* J_NUM, J_BOOL */
    char *str;             /* J_STR */
    int n;                 /* J_ARR, J_OBJ: number of children */
    struct JNode **items;  /* J_ARR, J_OBJ: children */
    char **keys;           /* J_OBJ: keys */
} JNode;

typedef struct {
    const char *s;
    size_t pos;
    char err[256];
} JParser;

static void jnode_free(JNode *n) {
    if (!n) return;
    for (int i = 0; i < n->n; i++) {
        jnode_free(n->items[i]);
        if (n->keys) free(n->keys[i]);
    }
    free(n->items);
    free(n->keys);
    free(n->str);
    free(n);
}

static int jline(JParser *p) {
    int line = 1;
    for (size_t i = 0; i < p->pos && p->s[i]; i++)
        if (p->s[i] == '\n') line++;
    return line;
}

static void jerr(JParser *p, const char *msg) {
    if (!p->err[0]) snprintf(p->err, sizeof p->err, "%s (line %d)", msg, jline(p));
}

static void jskip_ws(JParser *p) {
    while (p->s[p->pos] == ' ' || p->s[p->pos] == '\t' || p->s[p->pos] == '\n' || p->s[p->pos] == '\r')
        p->pos++;
}

static void utf8_append(char **buf, size_t *len, size_t *cap, unsigned cp) {
    char tmp[4];
    int n;
    if (cp < 0x80) { tmp[0] = (char)cp; n = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000) {
        tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        tmp[2] = (char)(0x80 | (cp & 0x3F)); n = 3;
    } else {
        tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); n = 4;
    }
    while (*len + n + 1 > *cap) { *cap *= 2; *buf = xrealloc(*buf, *cap); }
    memcpy(*buf + *len, tmp, n);
    *len += n;
}

static int jhex4(JParser *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        char c = p->s[p->pos++];
        v <<= 4;
        if (c >= '0' && c <= '9') v |= c - '0';
        else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
        else { jerr(p, "invalid \\u escape"); return 0; }
    }
    *out = v;
    return 1;
}

static char *jparse_string(JParser *p) {
    if (p->s[p->pos] != '"') { jerr(p, "expected string"); return NULL; }
    p->pos++;
    size_t cap = 32, len = 0;
    char *buf = xmalloc(cap);
    for (;;) {
        char c = p->s[p->pos++];
        if (c == '\0') { jerr(p, "unterminated string"); free(buf); return NULL; }
        if (c == '"') break;
        if (c == '\\') {
            char e = p->s[p->pos++];
            unsigned cp;
            switch (e) {
            case '"': c = '"'; break;
            case '\\': c = '\\'; break;
            case '/': c = '/'; break;
            case 'b': c = '\b'; break;
            case 'f': c = '\f'; break;
            case 'n': c = '\n'; break;
            case 'r': c = '\r'; break;
            case 't': c = '\t'; break;
            case 'u':
                if (!jhex4(p, &cp)) { free(buf); return NULL; }
                if (cp >= 0xD800 && cp <= 0xDBFF && p->s[p->pos] == '\\' && p->s[p->pos + 1] == 'u') {
                    unsigned lo;
                    p->pos += 2;
                    if (!jhex4(p, &lo)) { free(buf); return NULL; }
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                }
                utf8_append(&buf, &len, &cap, cp);
                continue;
            default:
                jerr(p, "invalid escape in string"); free(buf); return NULL;
            }
        }
        if (len + 2 > cap) { cap *= 2; buf = xrealloc(buf, cap); }
        buf[len++] = c;
    }
    buf[len] = '\0';
    return buf;
}

static JNode *jnew(JType t) {
    JNode *n = xmalloc(sizeof *n);
    memset(n, 0, sizeof *n);
    n->type = t;
    return n;
}

static void jappend(JNode *parent, char *key, JNode *child) {
    parent->items = xrealloc(parent->items, sizeof(JNode *) * (parent->n + 1));
    if (parent->type == J_OBJ) {
        parent->keys = xrealloc(parent->keys, sizeof(char *) * (parent->n + 1));
        parent->keys[parent->n] = key;
    }
    parent->items[parent->n++] = child;
}

static JNode *jparse_value(JParser *p, int depth) {
    if (depth > 64) { jerr(p, "nesting too deep"); return NULL; }
    jskip_ws(p);
    char c = p->s[p->pos];
    if (c == '{' || c == '[') {
        int obj = (c == '{');
        char close = obj ? '}' : ']';
        JNode *n = jnew(obj ? J_OBJ : J_ARR);
        p->pos++;
        jskip_ws(p);
        if (p->s[p->pos] == close) { p->pos++; return n; }
        for (;;) {
            char *key = NULL;
            if (obj) {
                jskip_ws(p);
                if (!(key = jparse_string(p))) { jnode_free(n); return NULL; }
                jskip_ws(p);
                if (p->s[p->pos] != ':') { jerr(p, "expected ':'"); free(key); jnode_free(n); return NULL; }
                p->pos++;
            }
            JNode *child = jparse_value(p, depth + 1);
            if (!child) { free(key); jnode_free(n); return NULL; }
            jappend(n, key, child);
            jskip_ws(p);
            if (p->s[p->pos] == ',') { p->pos++; continue; }
            if (p->s[p->pos] == close) { p->pos++; return n; }
            jerr(p, obj ? "expected ',' or '}'" : "expected ',' or ']'");
            jnode_free(n);
            return NULL;
        }
    }
    if (c == '"') {
        char *s = jparse_string(p);
        if (!s) return NULL;
        JNode *n = jnew(J_STR);
        n->str = s;
        return n;
    }
    if (c == '-' || isdigit((unsigned char)c)) {
        char *end;
        errno = 0;
        long long v = strtoll(p->s + p->pos, &end, 10);
        if (errno == ERANGE) { jerr(p, "integer out of range"); return NULL; }
        if (*end == '.' || *end == 'e' || *end == 'E') { jerr(p, "only integer numbers are supported"); return NULL; }
        p->pos = end - p->s;
        JNode *n = jnew(J_NUM);
        n->num = v;
        return n;
    }
    if (strncmp(p->s + p->pos, "true", 4) == 0) { p->pos += 4; JNode *n = jnew(J_BOOL); n->num = 1; return n; }
    if (strncmp(p->s + p->pos, "false", 5) == 0) { p->pos += 5; return jnew(J_BOOL); }
    if (strncmp(p->s + p->pos, "null", 4) == 0) { p->pos += 4; return jnew(J_NULL); }
    jerr(p, "unexpected character");
    return NULL;
}

static JNode *jget(const JNode *obj, const char *key) {
    if (!obj || obj->type != J_OBJ) return NULL;
    for (int i = 0; i < obj->n; i++)
        if (strcmp(obj->keys[i], key) == 0) return obj->items[i];
    return NULL;
}

/* Turns the JSON tree into Tables. Returns NULL on success, or an error text. */
static const char *load_tables(const JNode *root, char *err, size_t errsz) {
    const JNode *tables = jget(root, "tables");
    if (!tables || tables->type != J_ARR) return "missing \"tables\" array";
    for (int i = 0; i < tables->n; i++) {
        const JNode *jt = tables->items[i];
        const JNode *jname = jget(jt, "name"), *jcols = jget(jt, "columns"), *jrows = jget(jt, "rows");
        if (!jname || jname->type != J_STR || !jcols || jcols->type != J_ARR || !jrows || jrows->type != J_ARR) {
            snprintf(err, errsz, "table #%d must have \"name\", \"columns\" and \"rows\"", i + 1);
            return err;
        }
        if (jcols->n == 0) {
            snprintf(err, errsz, "table '%s' has no columns", jname->str);
            return err;
        }
        if (find_table(jname->str)) {
            snprintf(err, errsz, "duplicate table '%s'", jname->str);
            return err;
        }
        Table *t = xmalloc(sizeof *t);
        memset(t, 0, sizeof *t);
        t->name = xstrdup(jname->str);
        t->cols = xmalloc(sizeof(Column) * (jcols->n ? jcols->n : 1));
        for (int c = 0; c < jcols->n; c++) {
            const JNode *cn = jget(jcols->items[c], "name"), *ct = jget(jcols->items[c], "type");
            if (!cn || cn->type != J_STR || !ct || ct->type != J_STR ||
                (strcmp(ct->str, "INTEGER") != 0 && strcmp(ct->str, "TEXT") != 0)) {
                snprintf(err, errsz, "bad column #%d in table '%s'", c + 1, t->name);
                free_table(t);
                return err;
            }
            t->cols[c].name = xstrdup(cn->str);
            t->cols[c].type = strcmp(ct->str, "INTEGER") == 0 ? TYPE_INTEGER : TYPE_TEXT;
            t->ncols++;
        }
        for (int r = 0; r < jrows->n; r++) {
            const JNode *jr = jrows->items[r];
            if (jr->type != J_ARR || jr->n != t->ncols) {
                snprintf(err, errsz, "row #%d of table '%s' has wrong number of values", r + 1, t->name);
                free_table(t);
                return err;
            }
            Value *row = xmalloc(sizeof(Value) * t->ncols);
            for (int c = 0; c < t->ncols; c++) {
                const JNode *jv = jr->items[c];
                int ok = t->cols[c].type == TYPE_INTEGER ? jv->type == J_NUM : jv->type == J_STR;
                if (!ok) {
                    snprintf(err, errsz, "row #%d of table '%s': value for column '%s' must be %s",
                             r + 1, t->name, t->cols[c].name, type_name(t->cols[c].type));
                    free_row(row, c);
                    free_table(t);
                    return err;
                }
                row[c].type = t->cols[c].type;
                if (jv->type == J_NUM) row[c].i = jv->num;
                else row[c].s = xstrdup(jv->str);
            }
            table_add_row(t, row);
        }
        db_add_table(t);
    }
    return NULL;
}

static void load_db(void) {
    FILE *f = fopen(db_path, "rb");
    if (!f) {
        if (errno == ENOENT) {
            /* First launch: start with an empty database and create the file. */
            if (save_db()) printf("Created new empty database '%s'.\n", db_path);
            return;
        }
        fprintf(stderr, "Fatal: cannot open '%s': %s\n", db_path, strerror(errno));
        exit(1);
    }
    size_t cap = 4096, len = 0, n;
    char *buf = xmalloc(cap);
    while ((n = fread(buf + len, 1, cap - len - 1, f)) > 0) {
        len += n;
        if (len + 1 >= cap) { cap *= 2; buf = xrealloc(buf, cap); }
    }
    fclose(f);
    buf[len] = '\0';

    JParser p = { buf, 0, "" };
    JNode *root = jparse_value(&p, 0);
    if (root) {
        jskip_ws(&p);
        if (p.s[p.pos] != '\0') { jerr(&p, "unexpected data after JSON"); jnode_free(root); root = NULL; }
    }
    if (!root) {
        fprintf(stderr, "Fatal: '%s' is not valid JSON: %s\n", db_path, p.err);
        fprintf(stderr, "Fix or remove the file and start again (your data was not touched).\n");
        free(buf);
        exit(1);
    }
    char err[256];
    const char *msg = load_tables(root, err, sizeof err);
    jnode_free(root);
    free(buf);
    if (msg) {
        fprintf(stderr, "Fatal: '%s' has an invalid structure: %s\n", db_path, msg);
        fprintf(stderr, "Fix or remove the file and start again (your data was not touched).\n");
        exit(1);
    }
}

/* ======================================================================
 *  Tokenizer
 * ====================================================================== */

typedef enum { TK_IDENT, TK_INT, TK_STR, TK_SYM, TK_END } TokKind;

typedef struct {
    TokKind kind;
    char *text;   /* identifier / string contents / symbol / original number text */
    long long iv; /* TK_INT */
} Token;

typedef struct {
    Token *v;
    int n, cap;
} TokList;

static void tok_push(TokList *tl, TokKind kind, const char *start, size_t len, long long iv) {
    if (tl->n == tl->cap) {
        tl->cap = tl->cap ? tl->cap * 2 : 16;
        tl->v = xrealloc(tl->v, sizeof(Token) * tl->cap);
    }
    Token *t = &tl->v[tl->n++];
    t->kind = kind;
    t->text = xmalloc(len + 1);
    memcpy(t->text, start, len);
    t->text[len] = '\0';
    t->iv = iv;
}

static void toklist_free(TokList *tl) {
    for (int i = 0; i < tl->n; i++) free(tl->v[i].text);
    free(tl->v);
    tl->v = NULL;
    tl->n = tl->cap = 0;
}

/* Splits a command into tokens. Returns 1 on success, 0 on error (message in err). */
static int tokenize(const char *s, TokList *tl, char *err, size_t errsz) {
    size_t i = 0;
    while (s[i]) {
        unsigned char c = (unsigned char)s[i];
        if (isspace(c)) { i++; continue; }

        if (isalpha(c) || c == '_') {
            size_t st = i;
            while (isalnum((unsigned char)s[i]) || s[i] == '_') i++;
            tok_push(tl, TK_IDENT, s + st, i - st, 0);
            continue;
        }

        if (isdigit(c) || ((c == '-' || c == '+') && isdigit((unsigned char)s[i + 1]))) {
            size_t st = i;
            i++;
            while (isdigit((unsigned char)s[i])) i++;
            if (isalpha((unsigned char)s[i]) || s[i] == '_' || s[i] == '.') {
                while (s[i] && !isspace((unsigned char)s[i]) && !strchr(",;()=", s[i])) i++;
                snprintf(err, errsz, "invalid number '%.*s' (only whole numbers are supported)", (int)(i - st), s + st);
                return 0;
            }
            char *num = xmalloc(i - st + 1);
            memcpy(num, s + st, i - st);
            num[i - st] = '\0';
            errno = 0;
            long long v = strtoll(num, NULL, 10);
            if (errno == ERANGE) {
                snprintf(err, errsz, "number %s is too large", num);
                free(num);
                return 0;
            }
            free(num);
            tok_push(tl, TK_INT, s + st, i - st, v);
            continue;
        }

        if (c == '\'') {
            size_t st = ++i;
            while (s[i] && s[i] != '\'') i++;
            if (!s[i]) {
                snprintf(err, errsz, "unterminated text value: missing closing quote (')");
                return 0;
            }
            tok_push(tl, TK_STR, s + st, i - st, 0);
            i++;
            continue;
        }

        if (strchr("(),;=*", c)) {
            tok_push(tl, TK_SYM, s + i, 1, 0);
            i++;
            continue;
        }

        if (c == '"') {
            snprintf(err, errsz, "text values must use single quotes, e.g. 'Ani'");
            return 0;
        }
        snprintf(err, errsz, "unexpected character '%c'", c);
        return 0;
    }
    tok_push(tl, TK_END, "", 0, 0);
    return 1;
}

/* ======================================================================
 *  Parser helpers
 * ====================================================================== */

typedef struct {
    TokList *tl;
    int pos;
    char err[512];
} Parser;

static void perr(Parser *p, const char *fmt, ...) {
    if (p->err[0]) return; /* keep the first error */
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(p->err, sizeof p->err, fmt, ap);
    va_end(ap);
}

static Token *peek(Parser *p) { return &p->tl->v[p->pos]; }

/* Human readable description of a token for error messages. */
static const char *describe(Token *t, char *buf, size_t sz) {
    switch (t->kind) {
    case TK_END: return "end of command";
    case TK_STR: snprintf(buf, sz, "'%s'", t->text); return buf;
    default: snprintf(buf, sz, "\"%s\"", t->text); return buf;
    }
}

static void perr_expected(Parser *p, const char *what) {
    char buf[128];
    perr(p, "syntax error: expected %s but found %s", what, describe(peek(p), buf, sizeof buf));
}

static int is_kw(Token *t, const char *kw) { return t->kind == TK_IDENT && strcasecmp(t->text, kw) == 0; }

static int accept_kw(Parser *p, const char *kw) {
    if (is_kw(peek(p), kw)) { p->pos++; return 1; }
    return 0;
}

static int expect_kw(Parser *p, const char *kw) {
    if (accept_kw(p, kw)) return 1;
    perr_expected(p, kw);
    return 0;
}

static int accept_sym(Parser *p, char c) {
    Token *t = peek(p);
    if (t->kind == TK_SYM && t->text[0] == c) { p->pos++; return 1; }
    return 0;
}

static int expect_sym(Parser *p, char c) {
    if (accept_sym(p, c)) return 1;
    char what[8];
    snprintf(what, sizeof what, "'%c'", c);
    perr_expected(p, what);
    return 0;
}

/* Reads a table/column name. Returns a pointer into the token (not a copy). */
static const char *expect_ident(Parser *p, const char *what) {
    Token *t = peek(p);
    if (t->kind != TK_IDENT) { perr_expected(p, what); return NULL; }
    if (strlen(t->text) > MAX_NAME_LEN) {
        perr(p, "name '%s' is too long (max %d characters)", t->text, MAX_NAME_LEN);
        return NULL;
    }
    p->pos++;
    return t->text;
}

/* Reads a literal: number or 'text'. The Value's text points into the token. */
static int parse_literal(Parser *p, Value *out) {
    Token *t = peek(p);
    if (t->kind == TK_INT) { out->type = TYPE_INTEGER; out->i = t->iv; out->s = NULL; p->pos++; return 1; }
    if (t->kind == TK_STR) { out->type = TYPE_TEXT; out->s = t->text; p->pos++; return 1; }
    perr_expected(p, "a value (number or 'text')");
    return 0;
}

static int check_type(Parser *p, const Column *col, const Value *v) {
    if (col->type == v->type) return 1;
    if (v->type == TYPE_TEXT)
        perr(p, "type mismatch: column '%s' is INTEGER, but got text '%s'", col->name, v->s);
    else
        perr(p, "type mismatch: column '%s' is TEXT, but got number %lld (text must be in single quotes)",
             col->name, v->i);
    return 0;
}

/* Every command must end with ';' and nothing may follow it. */
static int expect_end(Parser *p) {
    if (!expect_sym(p, ';')) return 0;
    if (peek(p)->kind != TK_END) {
        char buf[128];
        perr(p, "syntax error: unexpected %s after ';' (one command per line)", describe(peek(p), buf, sizeof buf));
        return 0;
    }
    return 1;
}

static Table *expect_table(Parser *p) {
    const char *name = expect_ident(p, "table name");
    if (!name) return NULL;
    Table *t = find_table(name);
    if (!t) perr(p, "no such table: '%s' (use .tables to list tables)", name);
    return t;
}

static int expect_column(Parser *p, Table *t) {
    const char *name = expect_ident(p, "column name");
    if (!name) return -1;
    int c = find_col(t, name);
    if (c < 0) perr(p, "no such column: '%s' in table '%s'", name, t->name);
    return c;
}

/* WHERE <column> = <value>   (optional). */
typedef struct {
    int has;
    int col;
    Value val;
} Where;

static int parse_where(Parser *p, Table *t, Where *w) {
    w->has = 0;
    if (!accept_kw(p, "WHERE")) return 1;
    if ((w->col = expect_column(p, t)) < 0) return 0;
    if (!expect_sym(p, '=')) return 0;
    if (!parse_literal(p, &w->val)) return 0;
    if (!check_type(p, &t->cols[w->col], &w->val)) return 0;
    w->has = 1;
    return 1;
}

/* This is how rows to change are found: a linear scan comparing one column. */
static int row_matches(Value *row, const Where *w) {
    return !w->has || value_equals(&row[w->col], &w->val);
}

/* ======================================================================
 *  Output: pretty table
 * ====================================================================== */

/* Number of characters on screen (UTF-8 aware, so Armenian/Russian text lines up). */
static int display_width(const char *s) {
    int w = 0;
    for (; *s; s++)
        if (((unsigned char)*s & 0xC0) != 0x80) w++;
    return w;
}

static void print_border(const int *widths, int n) {
    for (int c = 0; c < n; c++) {
        putchar('+');
        for (int k = 0; k < widths[c] + 2; k++) putchar('-');
    }
    puts("+");
}

static void print_cell(const char *s, int width, int right) {
    int pad = width - display_width(s);
    printf("| ");
    if (right) for (int k = 0; k < pad; k++) putchar(' ');
    fputs(s, stdout);
    if (!right) for (int k = 0; k < pad; k++) putchar(' ');
    putchar(' ');
}

static void print_rows(Table *t, const int *cols, int ncols, Value **rows, int nrows) {
    int *widths = xmalloc(sizeof(int) * (ncols + 1));
    memset(widths, 0, sizeof(int) * (ncols + 1));
    char num[32];
    for (int k = 0; k < ncols; k++) {
        widths[k] = display_width(t->cols[cols[k]].name);
        for (int r = 0; r < nrows; r++) {
            Value *v = &rows[r][cols[k]];
            int w;
            if (v->type == TYPE_INTEGER) { snprintf(num, sizeof num, "%lld", v->i); w = (int)strlen(num); }
            else w = display_width(v->s);
            if (w > widths[k]) widths[k] = w;
        }
    }
    print_border(widths, ncols);
    for (int k = 0; k < ncols; k++) print_cell(t->cols[cols[k]].name, widths[k], 0);
    puts("|");
    print_border(widths, ncols);
    for (int r = 0; r < nrows; r++) {
        for (int k = 0; k < ncols; k++) {
            Value *v = &rows[r][cols[k]];
            if (v->type == TYPE_INTEGER) {
                snprintf(num, sizeof num, "%lld", v->i);
                print_cell(num, widths[k], 1);
            } else {
                print_cell(v->s, widths[k], 0);
            }
        }
        puts("|");
    }
    if (nrows) print_border(widths, ncols);
    printf("(%d row%s)\n", nrows, nrows == 1 ? "" : "s");
    free(widths);
}

/* ======================================================================
 *  Commands
 * ====================================================================== */

/* CREATE TABLE name (col TYPE, col TYPE, ...); */
static int exec_create(Parser *p) {
    if (!expect_kw(p, "TABLE")) return 0;
    const char *name = expect_ident(p, "table name");
    if (!name) return 0;
    if (!expect_sym(p, '(')) return 0;

    int n = 0, cap = 8;
    Column *cols = xmalloc(sizeof(Column) * cap);
    int ok = 1;
    do {
        const char *cname = expect_ident(p, "column name");
        if (!cname) { ok = 0; break; }
        for (int k = 0; k < n; k++)
            if (strcasecmp(cols[k].name, cname) == 0) {
                perr(p, "duplicate column name '%s'", cname);
                ok = 0;
            }
        if (!ok) break;
        Token *tt = peek(p);
        ColType type;
        if (is_kw(tt, "INTEGER") || is_kw(tt, "INT")) type = TYPE_INTEGER;
        else if (is_kw(tt, "TEXT")) type = TYPE_TEXT;
        else {
            if (tt->kind == TK_IDENT)
                perr(p, "unknown type '%s' for column '%s' (supported: INTEGER, TEXT)", tt->text, cname);
            else
                perr_expected(p, "column type (INTEGER or TEXT)");
            ok = 0;
            break;
        }
        p->pos++;
        if (n == cap) { cap *= 2; cols = xrealloc(cols, sizeof(Column) * cap); }
        cols[n].name = xstrdup(cname);
        cols[n].type = type;
        n++;
    } while (accept_sym(p, ','));

    if (ok && !(expect_sym(p, ')') && expect_end(p))) ok = 0;
    if (ok && find_table(name)) {
        perr(p, "table '%s' already exists", name);
        ok = 0;
    }
    if (!ok) {
        for (int k = 0; k < n; k++) free(cols[k].name);
        free(cols);
        return 0;
    }

    Table *t = xmalloc(sizeof *t);
    memset(t, 0, sizeof *t);
    t->name = xstrdup(name);
    t->cols = cols;
    t->ncols = n;
    db_add_table(t);
    printf("Table '%s' created with %d column%s.\n", t->name, n, n == 1 ? "" : "s");
    return 1;
}

/* DROP TABLE name; */
static int exec_drop(Parser *p) {
    if (!expect_kw(p, "TABLE")) return 0;
    Table *t = expect_table(p);
    if (!t || !expect_end(p)) return 0;
    for (int i = 0; i < db.ntables; i++)
        if (db.tables[i] == t) {
            memmove(&db.tables[i], &db.tables[i + 1], sizeof(Table *) * (db.ntables - i - 1));
            db.ntables--;
            break;
        }
    printf("Table '%s' dropped.\n", t->name);
    free_table(t);
    return 1;
}

/* INSERT INTO name VALUES (v1, v2, ...); */
static int exec_insert(Parser *p) {
    if (!expect_kw(p, "INTO")) return 0;
    Table *t = expect_table(p);
    if (!t) return 0;
    if (!expect_kw(p, "VALUES") || !expect_sym(p, '(')) return 0;

    /* Collect values first; the table is only modified after full validation. */
    int n = 0, cap = t->ncols + 1;
    Value *vals = xmalloc(sizeof(Value) * cap);
    int ok = 1;
    do {
        if (n == cap) { cap *= 2; vals = xrealloc(vals, sizeof(Value) * cap); }
        if (!parse_literal(p, &vals[n])) { ok = 0; break; }
        n++;
    } while (accept_sym(p, ','));
    if (ok && !(expect_sym(p, ')') && expect_end(p))) ok = 0;
    if (ok && n != t->ncols) {
        perr(p, "table '%s' has %d column%s but %d value%s supplied", t->name, t->ncols,
             t->ncols == 1 ? "" : "s", n, n == 1 ? " was" : "s were");
        ok = 0;
    }
    for (int c = 0; ok && c < n; c++)
        if (!check_type(p, &t->cols[c], &vals[c])) ok = 0;
    if (!ok) { free(vals); return 0; }

    Value *row = xmalloc(sizeof(Value) * t->ncols);
    for (int c = 0; c < t->ncols; c++) row[c] = value_copy(&vals[c]);
    free(vals);
    table_add_row(t, row);
    printf("1 row inserted into '%s'.\n", t->name);
    return 1;
}

/* SELECT * | col, col FROM name [WHERE col = value]; */
static int exec_select(Parser *p) {
    /* Column names are read before we know the table, so remember token positions. */
    int star = 0, first = p->pos, ncolnames = 0;
    if (accept_sym(p, '*')) star = 1;
    else {
        do {
            if (!expect_ident(p, "'*' or column name")) return 0;
            ncolnames++;
        } while (accept_sym(p, ','));
    }
    if (!expect_kw(p, "FROM")) return 0;
    Table *t = expect_table(p);
    if (!t) return 0;

    int ncols = star ? t->ncols : ncolnames;
    int *cols = xmalloc(sizeof(int) * (ncols ? ncols : 1));
    for (int k = 0; k < ncols; k++) {
        if (star) { cols[k] = k; continue; }
        const char *cname = p->tl->v[first + 2 * k].text; /* names are separated by ',' */
        if ((cols[k] = find_col(t, cname)) < 0) {
            perr(p, "no such column: '%s' in table '%s'", cname, t->name);
            free(cols);
            return 0;
        }
    }

    Where w;
    if (!parse_where(p, t, &w) || !expect_end(p)) { free(cols); return 0; }

    Value **match = xmalloc(sizeof(Value *) * (t->nrows ? t->nrows : 1));
    int nmatch = 0;
    for (int r = 0; r < t->nrows; r++)
        if (row_matches(t->rows[r], &w)) match[nmatch++] = t->rows[r];
    print_rows(t, cols, ncols, match, nmatch);
    free(match);
    free(cols);
    return 1;
}

/* UPDATE name SET col = value [, col = value] [WHERE col = value]; */
static int exec_update(Parser *p) {
    Table *t = expect_table(p);
    if (!t || !expect_kw(p, "SET")) return 0;

    int n = 0;
    int *setcols = xmalloc(sizeof(int) * (t->ncols + 1));
    Value *setvals = xmalloc(sizeof(Value) * (t->ncols + 1));
    int ok = 1;
    do {
        int c = expect_column(p, t);
        if (c < 0) { ok = 0; break; }
        for (int k = 0; k < n; k++)
            if (setcols[k] == c) { perr(p, "column '%s' is assigned twice", t->cols[c].name); ok = 0; }
        if (!ok) break;
        if (!expect_sym(p, '=') || !parse_literal(p, &setvals[n]) || !check_type(p, &t->cols[c], &setvals[n])) {
            ok = 0;
            break;
        }
        setcols[n++] = c;
    } while (n < t->ncols && accept_sym(p, ','));

    Where w;
    if (ok && !(parse_where(p, t, &w) && expect_end(p))) ok = 0;
    if (!ok) { free(setcols); free(setvals); return 0; }

    int count = 0;
    for (int r = 0; r < t->nrows; r++) {
        Value *row = t->rows[r];
        if (!row_matches(row, &w)) continue;
        for (int k = 0; k < n; k++) {
            Value *cell = &row[setcols[k]];
            if (cell->type == TYPE_TEXT) free(cell->s);
            *cell = value_copy(&setvals[k]);
        }
        count++;
    }
    free(setcols);
    free(setvals);
    printf("%d row%s updated.\n", count, count == 1 ? "" : "s");
    return count > 0;
}

/* DELETE FROM name [WHERE col = value]; */
static int exec_delete(Parser *p) {
    if (!expect_kw(p, "FROM")) return 0;
    Table *t = expect_table(p);
    if (!t) return 0;
    Where w;
    if (!parse_where(p, t, &w) || !expect_end(p)) return 0;

    int kept = 0, count = 0;
    for (int r = 0; r < t->nrows; r++) {
        if (row_matches(t->rows[r], &w)) { free_row(t->rows[r], t->ncols); count++; }
        else t->rows[kept++] = t->rows[r];
    }
    t->nrows = kept;
    printf("%d row%s deleted.\n", count, count == 1 ? "" : "s");
    return count > 0;
}

/* ======================================================================
 *  Dot commands
 * ====================================================================== */

static void print_help(void) {
    puts("Service commands:\n"
         "  .help                 Show this help\n"
         "  .tables               List tables\n"
         "  .schema [table]       Show table structure\n"
         "  .exit                 Exit the program (also .quit or Ctrl+D)\n"
         "\n"
         "SQL commands (one line each, must end with ';', keywords are case-insensitive):\n"
         "  CREATE TABLE students (id INTEGER, name TEXT, age INTEGER);\n"
         "  INSERT INTO students VALUES (1, 'Ani', 20);\n"
         "  SELECT * FROM students;\n"
         "  SELECT name, age FROM students WHERE id = 1;\n"
         "  UPDATE students SET age = 21 WHERE id = 1;\n"
         "  DELETE FROM students WHERE name = 'Aram';\n"
         "  DROP TABLE students;\n"
         "\n"
         "Types: INTEGER (whole number), TEXT (in single quotes: 'Ani').\n"
         "WHERE supports one condition: column = value.\n"
         "UPDATE/DELETE without WHERE affect all rows.");
}

static void print_schema(const Table *t) {
    printf("CREATE TABLE %s (", t->name);
    for (int c = 0; c < t->ncols; c++)
        printf("%s%s %s", c ? ", " : "", t->cols[c].name, type_name(t->cols[c].type));
    printf(");   -- %d row%s\n", t->nrows, t->nrows == 1 ? "" : "s");
}

/* Returns 0 if the program should exit. */
static int run_dot_command(const char *line) {
    char cmd[64] = "", arg[128] = "";
    sscanf(line, "%63s %127s", cmd, arg);
    if (strcmp(cmd, ".exit") == 0 || strcmp(cmd, ".quit") == 0) return 0;
    if (strcmp(cmd, ".help") == 0) { print_help(); return 1; }
    if (strcmp(cmd, ".tables") == 0) {
        if (db.ntables == 0) puts("(no tables)");
        for (int i = 0; i < db.ntables; i++) puts(db.tables[i]->name);
        return 1;
    }
    if (strcmp(cmd, ".schema") == 0) {
        if (arg[0]) {
            Table *t = find_table(arg);
            if (t) print_schema(t);
            else printf("Error: no such table: '%s'\n", arg);
        } else {
            if (db.ntables == 0) puts("(no tables)");
            for (int i = 0; i < db.ntables; i++) print_schema(db.tables[i]);
        }
        return 1;
    }
    printf("Error: unknown command '%s'. Type .help for help.\n", cmd);
    return 1;
}

/* ======================================================================
 *  SQL dispatch
 * ====================================================================== */

static void run_sql(const char *line) {
    TokList tl = { 0 };
    char err[512];
    if (!tokenize(line, &tl, err, sizeof err)) {
        printf("Error: %s\n", err);
        toklist_free(&tl);
        return;
    }

    Parser p = { &tl, 0, "" };
    Token *first = peek(&p);
    int changed = 0;

    if (first->kind == TK_END) { toklist_free(&tl); return; }

    /* Gives a clear message when the user forgot the ';'. */
    if (tl.n < 2 || tl.v[tl.n - 2].kind != TK_SYM || tl.v[tl.n - 2].text[0] != ';') {
        if (!strchr(line, ';')) {
            printf("Error: missing ';' at the end of the command.\n");
            toklist_free(&tl);
            return;
        }
    }

    if (accept_kw(&p, "CREATE")) changed = exec_create(&p);
    else if (accept_kw(&p, "INSERT")) changed = exec_insert(&p);
    else if (accept_kw(&p, "SELECT")) exec_select(&p);
    else if (accept_kw(&p, "UPDATE")) changed = exec_update(&p);
    else if (accept_kw(&p, "DELETE")) changed = exec_delete(&p);
    else if (accept_kw(&p, "DROP")) changed = exec_drop(&p);
    else {
        char buf[128];
        perr(&p, "unknown command %s. Supported: CREATE, INSERT, SELECT, UPDATE, DELETE, DROP. Type .help for help.",
             describe(first, buf, sizeof buf));
    }

    if (p.err[0]) printf("Error: %s\n", p.err);
    else if (changed) save_db(); /* save after every successful change */
    toklist_free(&tl);
}

/* ======================================================================
 *  Main loop
 * ====================================================================== */

int main(int argc, char **argv) {
    if (argc > 2) {
        fprintf(stderr, "Usage: %s [database.json]\n", argv[0]);
        return 1;
    }
    if (argc == 2) db_path = argv[1];

    puts("MiniSQLite");
    puts("Type .help for help.");
    load_db();
    putchar('\n');

    char *line = NULL;
    size_t cap = 0;
    for (;;) {
        printf("minidb> ");
        fflush(stdout);
        ssize_t len = getline(&line, &cap, stdin);
        if (len < 0) { putchar('\n'); break; } /* Ctrl+D */

        /* trim spaces and the newline */
        while (len > 0 && isspace((unsigned char)line[len - 1])) line[--len] = '\0';
        char *s = line;
        while (isspace((unsigned char)*s)) s++;
        if (*s == '\0') continue;

        if (*s == '.') {
            if (!run_dot_command(s)) break;
        } else {
            run_sql(s);
        }
    }
    free(line);
    for (int i = 0; i < db.ntables; i++) free_table(db.tables[i]);
    free(db.tables);
    puts("Bye!");
    return 0;
}
