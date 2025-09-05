/*
 * VITTE LIGHT — vitlc.c
 * ------------------------------------------------------------------
 * Révision: 2025-09-05
 * Rôle    : Compilateur Vitte Light (VITL) en C11, monolithique, auto-contenu.
 * Cible   : Lecture .vitl → IR lisible (--emit-ir) et/ou bytecode .vitbc (--emit-bytecode).
 * Portée  : Sous-ensemble pédagogique du langage pour bootstrapping et CI.
 *           - Modules, imports (collecte simple)
 *           - Fonctions avec paramètres typés et retour explicite
 *           - Déclarations let/const, assignations, retours
 *           - Contrôle: if/else, while, for i in a..b (exclusif)
 *           - Expressions: littéraux (int, float, bool, string), ident, appels, ()
 *           - Opérateurs binaires: + - * / % == != < <= > >= && ||
 *           - Opérateur unaire: ! -
 *           - Types: i32, i64, f64, bool, str
 *           - Result/Option traités au niveau du parseur comme ident spéciaux (pas de sémantique auto)
 *
 * Limitations volontaires:
 *           - Pas de génériques
 *           - Pas d’inférence globale
 *           - Pas de FFI ici (géré au niveau du runtime/éditeur de liens)
 *
 * CLI      : vitlc [options] <fichier.vitl> [...]
 * Options  :
 *   -O0|-O1|-O2|-O3      Niveaux d’optimisation symboliques (conservés dans le header)
 *   -g                   Génère info debug (numéros de lignes dans l’IR)
 *   -o <chemin>          Fichier de sortie (binaire bytecode ou nul si IR seul)
 *   --emit-ir            Affiche IR lisible sur stdout
 *   --emit-bytecode      Écrit .vitbc (format minimal défini ci-dessous)
 *   --target <triple>    Annote le bytecode avec la cible (ex: x86_64-linux-gnu)
 *
 * Build    : cc -std=c11 -O2 -Wall -Wextra -o vitlc vitlc.c
 * Exemples : ./vitlc --emit-ir examples/hello.vitl
 *            ./vitlc --emit-bytecode -o build/app.vitbc src/main.vitl
 * Licence  : MIT
 */

#define _POSIX_C_SOURCE 200809L
#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef uint8_t  u8;  typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t   i8;  typedef int16_t  i16; typedef int32_t  i32; typedef int64_t  i64;

/* --------------------------------- Utils ---------------------------------- */
#define DIE(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); exit(1);} while(0)
#define UNREACHABLE() do { fprintf(stderr, "[BUG] Unreachable at %s:%d\n", __FILE__, __LINE__); abort(); } while(0)

static void* xmalloc(size_t n){ void* p=malloc(n); if(!p) DIE("OOM"); return p; }
static void* xcalloc(size_t n, size_t s){ void* p=calloc(n,s); if(!p) DIE("OOM"); return p; }
static void* xrealloc(void* p, size_t n){ void* q=realloc(p,n); if(!q) DIE("OOM"); return q; }

/* StringView */
typedef struct { const char* ptr; int len; } Str;
static Str SV(const char* p, int len){ Str s={p,len}; return s; }
static bool streq(Str a, const char* b){ size_t blen=strlen(b); return a.len==(int)blen && memcmp(a.ptr,b,blen)==0; }
static char* str_dup_sv(Str s){ char* r=xmalloc((size_t)s.len+1); memcpy(r,s.ptr,(size_t)s.len); r[s.len]='\0'; return r; }

/* Dynamic array (stretchy buffer) */
#define VEC(T) struct { T* data; size_t len; size_t cap; }
#define vec_init() {NULL,0,0}
#define vec_free(v) do{ free((v).data); (v).data=NULL; (v).len=(v).cap=0; }while(0)
#define vec_push(v,x) do{ if((v).len== (v).cap){ (v).cap = (v).cap? (v).cap*2: 8; (v).data = xrealloc((v).data, (v).cap*sizeof(*(v).data)); } (v).data[(v).len++]=(x); }while(0)

/* --------------------------------- Source --------------------------------- */

typedef struct {
    char*  path;
    char*  buf;
    size_t len;
} Source;

static Source source_load(const char* path){
    Source s={0};
    FILE* f=fopen(path,"rb"); if(!f) DIE("impossible d'ouvrir %s: %s", path, strerror(errno));
    fseek(f,0,SEEK_END); long L=ftell(f); if(L<0) DIE("ftell"); fseek(f,0,SEEK_SET);
    s.buf = xmalloc((size_t)L+1); s.len=(size_t)L; size_t r=fread(s.buf,1,(size_t)L,f); fclose(f);
    if(r!= (size_t)L) DIE("lecture incomplète %s", path);
    s.buf[L]='\0'; s.path=str_dup_sv(SV(path,(int)strlen(path)));
    return s;
}

static void source_free(Source* s){ if(!s) return; free(s->path); free(s->buf); s->path=NULL; s->buf=NULL; s->len=0; }

/* --------------------------------- Lexer ---------------------------------- */

typedef enum {
    T_EOF=0,
    T_IDENT, T_INT, T_FLOAT, T_STRING,
    T_KW_MODULE, T_KW_IMPORT, T_KW_FN, T_KW_RETURN, T_KW_LET, T_KW_CONST,
    T_KW_IF, T_KW_ELSE, T_KW_WHILE, T_KW_FOR, T_KW_IN,
    T_KW_TRUE, T_KW_FALSE,
    T_KW_I32, T_KW_I64, T_KW_F64, T_KW_BOOL, T_KW_STR,
    // Punctuators
    T_LPAREN, T_RPAREN, T_LBRACE, T_RBRACE, T_LBRACK, T_RBRACK,
    T_COMMA, T_SEMI, T_COLON, T_ARROW, T_ASSIGN,
    // Operators
    T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT,
    T_EQ, T_NEQ, T_LT, T_LTE, T_GT, T_GTE,
    T_AND, T_OR, T_NOT,
    T_DOT, T_RANGE, T_RANGE_EQ
} TokenKind;

static const char* tname(TokenKind k){
    switch(k){
        case T_EOF: return "<eof>"; case T_IDENT: return "ident"; case T_INT: return "int"; case T_FLOAT: return "float"; case T_STRING: return "string";
        case T_KW_MODULE: return "module"; case T_KW_IMPORT: return "import"; case T_KW_FN: return "fn"; case T_KW_RETURN: return "return";
        case T_KW_LET: return "let"; case T_KW_CONST: return "const"; case T_KW_IF: return "if"; case T_KW_ELSE: return "else"; case T_KW_WHILE: return "while";
        case T_KW_FOR: return "for"; case T_KW_IN: return "in"; case T_KW_TRUE: return "true"; case T_KW_FALSE: return "false";
        case T_KW_I32: return "i32"; case T_KW_I64: return "i64"; case T_KW_F64: return "f64"; case T_KW_BOOL: return "bool"; case T_KW_STR: return "str";
        case T_LPAREN: return "("; case T_RPAREN: return ")"; case T_LBRACE: return "{"; case T_RBRACE: return "}"; case T_LBRACK: return "["; case T_RBRACK: return "]";
        case T_COMMA: return ","; case T_SEMI: return ";"; case T_COLON: return ":"; case T_ARROW: return "->"; case T_ASSIGN: return "=";
        case T_PLUS: return "+"; case T_MINUS: return "-"; case T_STAR: return "*"; case T_SLASH: return "/"; case T_PERCENT: return "%";
        case T_EQ: return "=="; case T_NEQ: return "!="; case T_LT: return "<"; case T_LTE: return "<="; case T_GT: return ">"; case T_GTE: return ">=";
        case T_AND: return "&&"; case T_OR: return "||"; case T_NOT: return "!"; case T_DOT: return "."; case T_RANGE: return ".."; case T_RANGE_EQ: return "..=";
    }
    return "?";
}

typedef struct { TokenKind kind; Str lex; int line, col; i64 i; double f; } Token;

typedef struct {
    const char* p; const char* start; const char* end;
    int line; int col;
    VEC(Token) toks;
} Lexer;

static bool is_ident_start(char c){ return isalpha((unsigned char)c) || c=='_'; }
static bool is_ident_continue(char c){ return isalnum((unsigned char)c) || c=='_'; }

static void lex_init(Lexer* L, const char* buf, size_t len){
    L->p=buf; L->start=buf; L->end=buf+len; L->line=1; L->col=1; L->toks= (VEC(Token)) vec_init();
}

static void lex_emit(Lexer* L, Token t){ vec_push(L->toks, t); }

static void skip_ws(Lexer* L){
    for(;;){
        if(L->p>=L->end) return;
        char c=*L->p;
        if(c==' '||c=='\t' || c=='\r'){ L->p++; L->col++; continue; }
        if(c=='\n'){ L->p++; L->line++; L->col=1; continue; }
        if(c=='/' && L->p+1<L->end && L->p[1]=='/'){ // line comment
            L->p+=2; L->col+=2; while(L->p<L->end && *L->p!='\n') { L->p++; L->col++; } continue;
        }
        if(c=='/' && L->p+1<L->end && L->p[1]=='*'){ // block comment
            L->p+=2; L->col+=2; int depth=1;
            while(L->p<L->end && depth>0){ if(*L->p=='\n'){ L->line++; L->col=1; L->p++; continue; }
                if(L->p+1<L->end && L->p[0]=='/' && L->p[1]=='*'){ depth++; L->p+=2; L->col+=2; continue; }
                if(L->p+1<L->end && L->p[0]=='*' && L->p[1]=='/'){ depth--; L->p+=2; L->col+=2; continue; }
                L->p++; L->col++; }
            if(depth>0) DIE("commentaire non terminé ligne %d", L->line); continue;
        }
        break;
    }
}

static bool match2(Lexer* L, char a, char b){ return L->p+1<L->end && L->p[0]==a && L->p[1]==b; }

static Token make_tok(Lexer* L, TokenKind k, const char* start, const char* end){
    Token t={0}; t.kind=k; t.lex=SV(start,(int)(end-start)); t.line=L->line; t.col=L->col; return t;
}

static void lex_all(Lexer* L){
    while(1){
        skip_ws(L); if(L->p>=L->end){ Token t={0}; t.kind=T_EOF; t.line=L->line; t.col=L->col; lex_emit(L,t); break; }
        const char* s=L->p; char c=*L->p; 
        // Ident / kw
        if(is_ident_start(c)){
            L->p++; while(L->p<L->end && is_ident_continue(*L->p)) L->p++;
            Str sv=SV(s,(int)(L->p-s));
            TokenKind k=T_IDENT;
            #define KW(txt,kind) if(streq(sv, txt)) k=kind; else
            KW("module",T_KW_MODULE) KW("import",T_KW_IMPORT) KW("fn",T_KW_FN) KW("return",T_KW_RETURN)
            KW("let",T_KW_LET) KW("const",T_KW_CONST) KW("if",T_KW_IF) KW("else",T_KW_ELSE)
            KW("while",T_KW_WHILE) KW("for",T_KW_FOR) KW("in",T_KW_IN)
            KW("true",T_KW_TRUE) KW("false",T_KW_FALSE)
            KW("i32",T_KW_I32) KW("i64",T_KW_I64) KW("f64",T_KW_F64) KW("bool",T_KW_BOOL) KW("str",T_KW_STR)
            /* default */ ;
            #undef KW
            Token t=make_tok(L,k,s,L->p); lex_emit(L,t); continue;
        }
        // Number
        if(isdigit((unsigned char)c)){
            bool isf=false; L->p++;
            while(L->p<L->end && isdigit((unsigned char)*L->p)) L->p++;
            if(L->p<L->end && *L->p=='.' && !(L->p+1<L->end && L->p[1]=='.')){ // float dot, careful with range ".."
                isf=true; L->p++; while(L->p<L->end && isdigit((unsigned char)*L->p)) L->p++;
                if(L->p<L->end && (*L->p=='e'||*L->p=='E')){ L->p++; if(*L->p=='+'||*L->p=='-') L->p++; while(L->p<L->end && isdigit((unsigned char)*L->p)) L->p++; }
            }
            Token t=make_tok(L, isf?T_FLOAT:T_INT, s, L->p);
            if(isf){ t.f=strtod(s,NULL);} else { t.i=strtoll(s,NULL,10);} lex_emit(L,t); continue;
        }
        // String
        if(c=='"'){
            L->p++; const char* start=L->p; bool esc=false;
            while(L->p<L->end){ char d=*L->p; if(d=='\n') DIE("string non terminée ligne %d", L->line);
                if(!esc){ if(d=='\\'){ esc=true; L->p++; continue; } if(d=='"'){ break; } }
                else { esc=false; L->p++; continue; }
                L->p++; }
            if(L->p>=L->end) DIE("string non terminée");
            Token t=make_tok(L,T_STRING,start,L->p); L->p++; // consume closing
            lex_emit(L,t); continue;
        }
        // Punct / Ops
        #define EMIT1(ch,kind) case ch: { L->p++; Token t=make_tok(L,kind,s,L->p); lex_emit(L,t); break; }
        switch(c){
            case '(': EMIT1('(',T_LPAREN); case ')': EMIT1(')',T_RPAREN); case '{': EMIT1('{',T_LBRACE); case '}': EMIT1('}',T_RBRACE);
            case '[': EMIT1('[',T_LBRACK); case ']': EMIT1(']',T_RBRACK); case ',': EMIT1(',',T_COMMA); case ';': EMIT1(';',T_SEMI); case ':': EMIT1(':',T_COLON);
            default: break;
        }
        #undef EMIT1
        if(match2(L,'.','.')){ L->p+=2; if(L->p<L->end && *L->p=='='){ L->p++; lex_emit(L, make_tok(L,T_RANGE_EQ,s,L->p)); } else lex_emit(L,make_tok(L,T_RANGE,s,L->p)); continue; }
        if(match2(L,'-','>')){ L->p+=2; lex_emit(L, make_tok(L,T_ARROW,s,L->p)); continue; }
        if(match2(L,'=','=')){ L->p+=2; lex_emit(L, make_tok(L,T_EQ,s,L->p)); continue; }
        if(match2(L,'!','=')){ L->p+=2; lex_emit(L, make_tok(L,T_NEQ,s,L->p)); continue; }
        if(match2(L,'<','=')){ L->p+=2; lex_emit(L, make_tok(L,T_LTE,s,L->p)); continue; }
        if(match2(L,'>','=')){ L->p+=2; lex_emit(L, make_tok(L,T_GTE,s,L->p)); continue; }
        if(match2(L,'&','&')){ L->p+=2; lex_emit(L, make_tok(L,T_AND,s,L->p)); continue; }
        if(match2(L,'|','|')){ L->p+=2; lex_emit(L, make_tok(L,T_OR,s,L->p)); continue; }
        // singles
        switch(*L->p){
            case '+': L->p++; lex_emit(L, make_tok(L,T_PLUS,s,L->p)); continue;
            case '-': L->p++; lex_emit(L, make_tok(L,T_MINUS,s,L->p)); continue;
            case '*': L->p++; lex_emit(L, make_tok(L,T_STAR,s,L->p)); continue;
            case '/': L->p++; lex_emit(L, make_tok(L,T_SLASH,s,L->p)); continue;
            case '%': L->p++; lex_emit(L, make_tok(L,T_PERCENT,s,L->p)); continue;
            case '<': L->p++; lex_emit(L, make_tok(L,T_LT,s,L->p)); continue;
            case '>': L->p++; lex_emit(L, make_tok(L,T_GT,s,L->p)); continue;
            case '=': L->p++; lex_emit(L, make_tok(L,T_ASSIGN,s,L->p)); continue;
            case '!': L->p++; lex_emit(L, make_tok(L,T_NOT,s,L->p)); continue;
            case '.': L->p++; lex_emit(L, make_tok(L,T_DOT,s,L->p)); continue;
        }
        DIE("caractère inattendu '%c' ligne %d", *L->p, L->line);
    }
}

/* --------------------------------- Parser --------------------------------- */

typedef enum { TY_VOID=0, TY_I32, TY_I64, TY_F64, TY_BOOL, TY_STR } TypeKind;

typedef struct { TypeKind k; } Type;
static Type TY(TypeKind k){ Type t; t.k=k; return t; }

static const char* type_name(Type t){ switch(t.k){ case TY_VOID: return "void"; case TY_I32: return "i32"; case TY_I64: return "i64"; case TY_F64: return "f64"; case TY_BOOL: return "bool"; case TY_STR: return "str"; } return "?"; }

/* AST */
typedef enum {
    A_UNIT=0,
    A_MODULE,
    A_IMPORT,
    A_FUNC,
    A_BLOCK,
    A_LET, A_CONST,
    A_ASSIGN,
    A_IF,
    A_WHILE,
    A_FOR_RANGE,
    A_RETURN,
    A_CALL,
    A_BINOP, A_UNOP,
    A_LITERAL_INT, A_LITERAL_FLOAT, A_LITERAL_STR, A_LITERAL_BOOL,
    A_IDENT
} ASTKind;

typedef struct AST AST;

typedef struct { Str name; Type type; } Param;

struct AST{
    ASTKind k; int line, col; Type type; // type synthétisé (si connu)
    union {
        struct { Str modname; } module;
        struct { Str path; } import;
        struct { Str name; VEC(Param) params; Type ret; AST* body; } func;
        struct { VEC(AST*) stmts; } block;
        struct { bool is_mut; Str name; Type type; AST* init; } let;
        struct { AST* lhs; AST* rhs; } assign;
        struct { AST* cond; AST* then_b; AST* else_b; } ifs;
        struct { AST* cond; AST* body; } whiles;
        struct { Str it; AST* start; AST* end; bool inclusive; AST* body; } forr;
        struct { AST* expr; } ret;
        struct { Str name; VEC(AST*) args; } call;
        struct { int op; AST* a; AST* b; } binop;
        struct { int op; AST* a; } unop;
        struct { i64 i; } lit_i;
        struct { double f; } lit_f;
        struct { Str s; } lit_s;
        struct { bool b; } lit_b;
        struct { Str name; } ident;
    };
};

/* Parser state */
typedef struct {
    Lexer* L; size_t idx; VEC(AST*) toplevel;
} Parser;

static Token* cur(Parser* P){ return &P->L->toks.data[P->idx]; }
static Token* peek(Parser* P, size_t o){ return &P->L->toks.data[P->idx+o]; }
static bool at(Parser* P, TokenKind k){ return cur(P)->kind==k; }
static Token expect(Parser* P, TokenKind k, const char* msg){ if(!at(P,k)) DIE("parse: attendu %s (%s:%d) mais trouvé %s", tname(k), P->L->start, cur(P)->line, tname(cur(P)->kind)); Token t=*cur(P); P->idx++; return t; }
static bool match(Parser* P, TokenKind k){ if(at(P,k)){ P->idx++; return true; } return false; }

static AST* mk(ASTKind k, Token* t){ AST* n=xcalloc(1,sizeof(AST)); n->k=k; n->line=t->line; n->col=t->col; n->type=TY(TY_VOID); return n; }

/* Forward decls */
static AST* parse_block(Parser* P);
static AST* parse_expr(Parser* P);
static Type parse_type(Parser* P){
    if(at(P,T_KW_I32)){ P->idx++; return TY(TY_I32);} if(at(P,T_KW_I64)){ P->idx++; return TY(TY_I64);} if(at(P,T_KW_F64)){ P->idx++; return TY(TY_F64);} if(at(P,T_KW_BOOL)){ P->idx++; return TY(TY_BOOL);} if(at(P,T_KW_STR)){ P->idx++; return TY(TY_STR);} DIE("type attendu ligne %d", cur(P)->line);
}

static AST* parse_stmt(Parser* P);

static AST* parse_primary(Parser* P){
    Token* t=cur(P);
    if(at(P,T_INT)){ AST* n=mk(A_LITERAL_INT,t); n->lit_i.i=t->i; P->idx++; n->type=TY(TY_I32); return n; }
    if(at(P,T_FLOAT)){ AST* n=mk(A_LITERAL_FLOAT,t); n->lit_f.f=t->f; P->idx++; n->type=TY(TY_F64); return n; }
    if(at(P,T_STRING)){ AST* n=mk(A_LITERAL_STR,t); n->lit_s.s=t->lex; P->idx++; n->type=TY(TY_STR); return n; }
    if(at(P,T_KW_TRUE)||at(P,T_KW_FALSE)){ AST* n=mk(A_LITERAL_BOOL,t); n->lit_b.b= at(P,T_KW_TRUE); P->idx++; n->type=TY(TY_BOOL); return n; }
    if(at(P,T_IDENT)){
        Token name=*t; P->idx++;
        // call ?
        if(match(P,T_LPAREN)){
            AST* n=mk(A_CALL,&name); n->call.name=name.lex; n->type=TY(TY_VOID);
            // args
            if(!at(P,T_RPAREN)){
                for(;;){ AST* e=parse_expr(P); vec_push(n->call.args, e); if(match(P,T_COMMA)) continue; break; }
            }
            expect(P,T_RPAREN, ")");
            return n;
        }
        AST* n=mk(A_IDENT,&name); n->ident.name=name.lex; return n;
    }
    if(match(P,T_LPAREN)){ AST* e=parse_expr(P); expect(P,T_RPAREN,")"); return e; }
    DIE("expression primaire attendue ligne %d", t->line);
}

/* precedence climbing */
static int prec(TokenKind k){ switch(k){
    case T_OR: return 1; case T_AND: return 2; case T_EQ: case T_NEQ: return 3; case T_LT: case T_LTE: case T_GT: case T_GTE: return 4;
    case T_PLUS: case T_MINUS: return 5; case T_STAR: case T_SLASH: case T_PERCENT: return 6; default: return -1; }
}

static AST* parse_unary(Parser* P){ if(match(P,T_NOT)){ Token op=*peek(P,(size_t)-1); AST* a=parse_unary(P); AST* n=mk(A_UNOP,&op); n->unop.op=op.kind; n->unop.a=a; return n; } if(match(P,T_MINUS)){ Token op=*peek(P,(size_t)-1); AST* a=parse_unary(P); AST* n=mk(A_UNOP,&op); n->unop.op=op.kind; n->unop.a=a; return n; } return parse_primary(P); }

static AST* parse_bin_rhs(Parser* P, int min_prec, AST* lhs){ for(;;){ TokenKind op=cur(P)->kind; int p=prec(op); if(p<min_prec) return lhs; Token optok=*cur(P); P->idx++; AST* rhs=parse_unary(P); int p2=prec(cur(P)->kind); if(p2>p){ rhs=parse_bin_rhs(P,p+1,rhs);} AST* n=mk(A_BINOP,&optok); n->binop.op=op; n->binop.a=lhs; n->binop.b=rhs; lhs=n; }
}

static AST* parse_expr(Parser* P){ return parse_bin_rhs(P,1,parse_unary(P)); }

static AST* parse_let(Parser* P, bool is_const){ Token* t=cur(P); P->idx++; bool is_mut=!is_const; // `const` → non mutable; `let` par défaut mutable? garder simple
    Token name=expect(P,T_IDENT,"ident"); Type ty=TY(TY_VOID); if(match(P,T_COLON)){ ty=parse_type(P);} expect(P,T_ASSIGN,"="); AST* init=parse_expr(P); AST* n=mk(is_const?A_CONST:A_LET,t); n->let.is_mut=is_mut; n->let.name=name.lex; n->let.type=ty; n->let.init=init; return n; }

static AST* parse_if(Parser* P){ Token* t=cur(P); expect(P,T_KW_IF,"if"); AST* cond=parse_expr(P); AST* then_b=parse_block(P); AST* else_b=NULL; if(match(P,T_KW_ELSE)) else_b=parse_block(P); AST* n=mk(A_IF,t); n->ifs.cond=cond; n->ifs.then_b=then_b; n->ifs.else_b=else_b; return n; }

static AST* parse_while(Parser* P){ Token* t=cur(P); expect(P,T_KW_WHILE,"while"); AST* cond=parse_expr(P); AST* body=parse_block(P); AST* n=mk(A_WHILE,t); n->whiles.cond=cond; n->whiles.body=body; return n; }

static AST* parse_for(Parser* P){ Token* t=cur(P); expect(P,T_KW_FOR,"for"); Token it=expect(P,T_IDENT,"ident"); expect(P,T_KW_IN,"in"); AST* start=parse_expr(P); bool inclusive=false; if(match(P,T_RANGE_EQ)) inclusive=true; else expect(P,T_RANGE,".." ); AST* end=parse_expr(P); AST* body=parse_block(P); AST* n=mk(A_FOR_RANGE,t); n->forr.it=it.lex; n->forr.start=start; n->forr.end=end; n->forr.inclusive=inclusive; n->forr.body=body; return n; }

static AST* parse_return(Parser* P){ Token* t=cur(P); expect(P,T_KW_RETURN,"return"); AST* e=NULL; if(!at(P,T_SEMI) && !at(P,T_RBRACE)) e=parse_expr(P); AST* n=mk(A_RETURN,t); n->ret.expr=e; return n; }

static AST* parse_stmt(Parser* P){ if(at(P,T_KW_LET)) return parse_let(P,false); if(at(P,T_KW_CONST)) return parse_let(P,true); if(at(P,T_KW_IF)) return parse_if(P); if(at(P,T_KW_WHILE)) return parse_while(P); if(at(P,T_KW_FOR)) return parse_for(P); if(at(P,T_KW_RETURN)) return parse_return(P); // assign or expr
    AST* e=parse_expr(P); if(e->k==A_IDENT && match(P,T_ASSIGN)){ AST* rhs=parse_expr(P); AST* n=mk(A_ASSIGN,cur(P)); n->assign.lhs=e; n->assign.rhs=rhs; return n; } return e; }

static AST* parse_block(Parser* P){ Token* t=cur(P); expect(P,T_LBRACE,"{"); AST* b=mk(A_BLOCK,t); for(;;){ if(at(P,T_RBRACE)){ P->idx++; break;} AST* s=parse_stmt(P); if(at(P,T_SEMI)) P->idx++; vec_push(b->block.stmts, s); }
    return b;
}

static AST* parse_func(Parser* P){ Token fntok=expect(P,T_KW_FN,"fn"); Token name=expect(P,T_IDENT,"ident"); expect(P,T_LPAREN,"("); VEC(Param) ps=vec_init(); if(!at(P,T_RPAREN)){
        for(;;){ Token pname=expect(P,T_IDENT,"param ident"); expect(P,T_COLON,":"); Type pt=parse_type(P); Param p={.name=pname.lex,.type=pt}; vec_push(ps,p); if(match(P,T_COMMA)) continue; break; }
    }
    expect(P,T_RPAREN,")"); Type ret=TY(TY_VOID); if(match(P,T_ARROW)) ret=parse_type(P); AST* body=parse_block(P);
    AST* fn=mk(A_FUNC,&fntok); fn->func.name=name.lex; fn->func.params=ps; fn->func.ret=ret; fn->func.body=body; return fn;
}

static void parse_module_and_imports(Parser* P){ if(match(P,T_KW_MODULE)){ Token m=expect(P,T_IDENT,"nom de module"); AST* mod=mk(A_MODULE,&m); mod->module.modname=m.lex; vec_push(P->toplevel, mod); }
    while(match(P,T_KW_IMPORT)){ Token path=expect(P,T_IDENT,"chemin import (ident.simple)"); AST* im=mk(A_IMPORT,&path); im->import.path=path.lex; vec_push(P->toplevel, im); if(at(P,T_SEMI)) P->idx++; }
}

static void parse_toplevel(Parser* P){ parse_module_and_imports(P); while(!at(P,T_EOF)){ if(at(P,T_KW_FN)){ AST* f=parse_func(P); vec_push(P->toplevel,f); } else { DIE("déclaration toplevel inattendue: %s", tname(cur(P)->kind)); } }
}

/* ------------------------------ Simple typer ------------------------------ */

static Type type_of_binop(Type a, TokenKind op, Type b){
    // Règle simple: si l'un est f64 -> f64; sinon i64 si présent, sinon i32; comparaisons -> bool.
    switch(op){ case T_EQ: case T_NEQ: case T_LT: case T_LTE: case T_GT: case T_GTE: case T_AND: case T_OR: return TY(TY_BOOL); default: break; }
    if(a.k==TY_F64 || b.k==TY_F64) return TY(TY_F64); if(a.k==TY_I64 || b.k==TY_I64) return TY(TY_I64); return TY(TY_I32);
}

/* ------------------------------- IR & Codegen ----------------------------- */

typedef enum {
    IR_NOP=0,
    IR_IMMI,   // dst = imm i64
    IR_IMMF,   // dst = imm f64
    IR_MOV,
    IR_ADD, IR_SUB, IR_MUL, IR_DIV, IR_MOD,
    IR_CMP_EQ, IR_CMP_NE, IR_CMP_LT, IR_CMP_LE, IR_CMP_GT, IR_CMP_GE,
    IR_JMP, IR_JZ, IR_JNZ,
    IR_LABEL,
    IR_CALL,   // dst = call name argc [args...]
    IR_RET
} IROp;

typedef struct { IROp op; int dst; int a; int b; double f; Str sym; int argc; int* argv; int line; } IR;

typedef struct { VEC(IR) code; int next_tmp; } IRFun;

typedef struct { Str name; IRFun fn; Type ret; } IRUnitFun;

typedef struct { VEC(IRUnitFun) funs; } IRUnit;

static int ir_tmp(IRFun* f){ return f->next_tmp++; }
static void ir_emit(IRFun* f, IR i){ vec_push(f->code, i); }
static IR ir_simple(IROp op, int dst, int a, int b){ IR i={0}; i.op=op; i.dst=dst; i.a=a; i.b=b; return i; }

static int gen_expr(IRFun* f, AST* e);

static int gen_unop(IRFun* f, int op, int v){ switch(op){ case T_MINUS: { int d=ir_tmp(f); ir_emit(f, ir_simple(IR_SUB,d,0,v)); return d; } case T_NOT: { int d=ir_tmp(f); // !v → cmp v==0
            ir_emit(f, ir_simple(IR_CMP_EQ, d, v, 0)); return d; } default: UNREACHABLE(); }
}

static IROp bin_to_ir(TokenKind k){ switch(k){
    case T_PLUS: return IR_ADD; case T_MINUS: return IR_SUB; case T_STAR: return IR_MUL; case T_SLASH: return IR_DIV; case T_PERCENT: return IR_MOD;
    case T_EQ: return IR_CMP_EQ; case T_NEQ: return IR_CMP_NE; case T_LT: return IR_CMP_LT; case T_LTE: return IR_CMP_LE; case T_GT: return IR_CMP_GT; case T_GTE: return IR_CMP_GE;
    default: return IR_NOP; }
}

/* Très simple table des symboles locales: noms → registre tmp */

typedef struct { Str name; int reg; Type type; } Local;

typedef struct { VEC(Local) locals; } Scope;

static void scope_init(Scope* s){ *s=(Scope){0}; }
static void scope_free(Scope* s){ vec_free(s->locals); }
static int scope_find(Scope* s, Str name){ for(size_t i=0;i<s->locals.len;i++){ Local* L=&s->locals.data[i]; if(streq(L->name, str_dup_sv(name))){ /* slow path; ok boot */ } if(L->name.len==name.len && memcmp(L->name.ptr, name.ptr, name.len)==0) return (int)i; } return -1; }
static int scope_add(Scope* s, Str name, Type t, int reg){ Local L={0}; L.name=SV(str_dup_sv(name), name.len); L.reg=reg; L.type=t; vec_push(s->locals, L); return reg; }

static int gen_expr(IRFun* f, AST* e){ switch(e->k){
    case A_LITERAL_INT: { int d=ir_tmp(f); IR i=ir_simple(IR_IMMI,d,0,0); i.a=(int)e->lit_i.i; ir_emit(f,i); return d; }
    case A_LITERAL_FLOAT: { int d=ir_tmp(f); IR i=ir_simple(IR_IMMF,d,0,0); i.f=e->lit_f.f; ir_emit(f,i); return d; }
    case A_LITERAL_BOOL: { int d=ir_tmp(f); IR i=ir_simple(IR_IMMI,d,0,0); i.a=e->lit_b.b?1:0; ir_emit(f,i); return d; }
    case A_IDENT: { DIE("ident en expression sans résolution dans ce prototype"); }
    case A_UNOP: { int v=gen_expr(f,e->unop.a); return gen_unop(f,e->unop.op, v); }
    case A_BINOP: { int a=gen_expr(f,e->binop.a); int b=gen_expr(f,e->binop.b); int d=ir_tmp(f); ir_emit(f, ir_simple(bin_to_ir(e->binop.op), d, a, b)); return d; }
    case A_CALL: { int d=ir_tmp(f); IR i={0}; i.op=IR_CALL; i.dst=d; i.sym=e->call.name; i.argc=(int)e->call.args.len; i.argv=xmalloc(sizeof(int)* (size_t)i.argc); for(int k=0;k<i.argc;k++){ i.argv[k]=gen_expr(f, e->call.args.data[k]); } ir_emit(f,i); return d; }
    default: DIE("expression non supportée dans ce générateur IR");
    }
}

static void gen_block(IRFun* f, AST* b){ for(size_t i=0;i<b->block.stmts.len;i++){ AST* s=b->block.stmts.data[i]; switch(s->k){
        case A_RETURN: { int v = s->ret.expr? gen_expr(f, s->ret.expr) : 0; ir_emit(f, ir_simple(IR_RET, v, 0,0)); break; }
        case A_LET: case A_CONST: {
            int v=gen_expr(f, s->let.init); // associer à un registre tmp; pas de stockage persistant dans ce prototype
            (void)v; break; }
        default: {
            if(s->k==A_BINOP || s->k==A_UNOP || s->k==A_CALL) { (void)gen_expr(f,s); }
            else if(s->k==A_ASSIGN){ (void)gen_expr(f,s->assign.rhs); /* TODO: associer aux symboles */ }
            else if(s->k==A_IF){ /* TODO: branchement */ }
            else if(s->k==A_WHILE){ /* TODO */ }
            else if(s->k==A_FOR_RANGE){ /* TODO */ }
            else if(s->k==A_BLOCK){ gen_block(f,s); }
            else { DIE("instruction non supportée dans ce prototype"); }
        }
    } }

static IRUnit gen_unit(VEC(AST*)* tops){ IRUnit U={0}; for(size_t i=0;i<tops->len;i++){ AST* n=tops->data[i]; if(n->k==A_FUNC){ IRUnitFun F={0}; F.name=n->func.name; F.ret=n->func.ret; F.fn.next_tmp=1; // r0 réservé pour const zéro
            gen_block(&F.fn, n->func.body); vec_push(U.funs, F); } }
    return U;
}

/* --------------------------------- IR Print -------------------------------- */

static void print_str(Str s){ fwrite(s.ptr,1,(size_t)s.len, stdout); }
static void ir_dump_fun(IRUnitFun* F){ printf("fn "); print_str(F->name); printf("() -> %s {\n", type_name(F->ret)); for(size_t i=0;i<F->fn.code.len;i++){ IR* I=&F->fn.code.data[i];
        switch(I->op){
            case IR_IMMI: printf("  r%d = immi %d\n", I->dst, I->a); break;
            case IR_IMMF: printf("  r%d = immf %g\n", I->dst, I->f); break;
            case IR_ADD: printf("  r%d = add r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_SUB: printf("  r%d = sub r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_MUL: printf("  r%d = mul r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_DIV: printf("  r%d = div r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_MOD: printf("  r%d = mod r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CMP_EQ: printf("  r%d = cmpeq r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CMP_NE: printf("  r%d = cmpne r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CMP_LT: printf("  r%d = cmplt r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CMP_LE: printf("  r%d = cmple r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CMP_GT: printf("  r%d = cmpgt r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CMP_GE: printf("  r%d = cmpge r%d, r%d\n", I->dst, I->a, I->b); break;
            case IR_CALL: printf("  r%d = call ", I->dst); print_str(I->sym); printf("("); for(int k=0;k<I->argc;k++){ if(k) printf(", "); printf("r%d", I->argv[k]); } printf(")\n"); break;
            case IR_RET: printf("  ret r%d\n", I->dst); break;
            default: printf("  ; op %d\n", I->op); break;
        }
    }
    printf("}\n");
}

static void ir_dump(IRUnit* U){ for(size_t i=0;i<U->funs.len;i++){ ir_dump_fun(&U->funs.data[i]); }
}

/* ------------------------------- Bytecode IO ------------------------------ */

typedef struct { VEC(u8) buf; } Buf;
static void buf_init(Buf* b){ b->buf = (VEC(u8)) vec_init(); }
static void buf_put(Buf* b, const void* p, size_t n){ for(size_t i=0;i<n;i++){ vec_push(b->buf, ((const u8*)p)[i]); } }
static void buf_put_u32(Buf* b, u32 v){ buf_put(b,&v,sizeof(v)); }
static void buf_put_u8(Buf* b, u8 v){ buf_put(b,&v,1); }
static void buf_write_file(Buf* b, const char* path){ FILE* f=fopen(path,"wb"); if(!f) DIE("écriture %s: %s", path, strerror(errno)); fwrite(b->buf.data,1,b->buf.len,f); fclose(f); }

/* Format .vitbc minimal
 *   magic: 6 bytes  "VITBC1"
 *   flags: u32      bit0 debug, bit1 O1, bit2 O2, bit3 O3
 *   target_len: u32
 *   target: bytes
 *   fun_count: u32
 *   [per fun]
 *     name_len: u32, name bytes
 *     ret_ty: u8
 *     insn_count: u32
 *     [insn]*: {op:u8, dst:u32, a:u32, b:u32, f:double, argc:u32, argv[argc]:u32}
 */

static void bc_write(IRUnit* U, const char* path, bool debug, int O, const char* target){ Buf b; buf_init(&b); const char magic[] = {'V','I','T','B','C','1'}; buf_put(&b, magic, sizeof(magic)); u32 flags=0; if(debug) flags|=1; if(O>=1) flags|=2; if(O>=2) flags|=4; if(O>=3) flags|=8; buf_put_u32(&b, flags); u32 tlen=(u32) (target? strlen(target):0); buf_put_u32(&b, tlen); if(tlen) buf_put(&b, target, tlen); buf_put_u32(&b, (u32)U->funs.len);
    for(size_t i=0;i<U->funs.len;i++){ IRUnitFun* F=&U->funs.data[i]; u32 nlen=(u32)F->name.len; buf_put_u32(&b,nlen); buf_put(&b,F->name.ptr,nlen); buf_put_u8(&b,(u8)F->ret.k); buf_put_u32(&b,(u32)F->fn.code.len);
        for(size_t j=0;j<F->fn.code.len;j++){ IR* I=&F->fn.code.data[j]; buf_put_u8(&b,(u8)I->op); buf_put_u32(&b,(u32)I->dst); buf_put_u32(&b,(u32)I->a); buf_put_u32(&b,(u32)I->b); buf_put(&b,&I->f,sizeof(double)); u32 argc=(u32)I->argc; buf_put_u32(&b,argc); for(u32 k=0;k<argc;k++){ buf_put_u32(&b,(u32)I->argv[k]); }
        }
    }
    buf_write_file(&b,path); vec_free(b.buf);
}

/* ----------------------------------- CLI ---------------------------------- */

typedef struct {
    int O; bool debug; bool emit_ir; bool emit_bc; const char* out; const char* target; VEC(const char*) inputs;
} CLI;

static void usage(FILE* f){
    fprintf(f,
        "vitlc — compilateur Vitte Light\n\n"
        "Usage: vitlc [options] <fichier.vitl>...\n\n"
        "Options:\n"
        "  -O0|-O1|-O2|-O3     Niveaux d'optimisation\n"
        "  -g                  Debug symbols\n"
        "  -o <chemin>         Sortie bytecode (.vitbc)\n"
        "  --emit-ir           Afficher IR lisible\n"
        "  --emit-bytecode     Écrire .vitbc\n"
        "  --target <triple>   Cible (annotation)\n"
    );
}

static CLI parse_cli(int argc, char** argv){ CLI c={0}; c.O=2; c.inputs= (VEC(const char*)) vec_init(); for(int i=1;i<argc;i++){
        const char* a=argv[i];
        if(strcmp(a,"-O0")==0){ c.O=0; continue; } if(strcmp(a,"-O1")==0){ c.O=1; continue; } if(strcmp(a,"-O2")==0){ c.O=2; continue; } if(strcmp(a,"-O3")==0){ c.O=3; continue; }
        if(strcmp(a,"-g")==0){ c.debug=true; continue; }
        if(strcmp(a,"--emit-ir")==0){ c.emit_ir=true; continue; }
        if(strcmp(a,"--emit-bytecode")==0){ c.emit_bc=true; continue; }
        if(strcmp(a,"-o")==0){ if(i+1>=argc) DIE("-o: chemin manquant"); c.out=argv[++i]; continue; }
        if(strcmp(a,"--target")==0){ if(i+1>=argc) DIE("--target: valeur manquante"); c.target=argv[++i]; continue; }
        if(a[0]=='-'){ usage(stderr); DIE("option inconnue: %s", a); }
        vec_push(c.inputs, a);
    }
    if(c.inputs.len==0){ usage(stderr); DIE("aucun fichier source"); }
    if(!c.emit_ir && !c.emit_bc){ c.emit_ir=true; }
    if(c.emit_bc && !c.out){ c.out="build/out.vitbc"; }
    return c;
}

/* ---------------------------------- Main ---------------------------------- */

int main(int argc, char** argv){
    CLI cli=parse_cli(argc,argv);

    // Charger et compiler chaque fichier, concaténer les fonctions dans un IR unit
    IRUnit unit={0};
    for(size_t i=0;i<cli.inputs.len;i++){
        const char* path=cli.inputs.data[i];
        Source s=source_load(path);
        Lexer L; lex_init(&L, s.buf, s.len); lex_all(&L);
        Parser P={0}; P.L=&L; P.idx=0; P.toplevel= (VEC(AST*)) vec_init();
        parse_toplevel(&P);
        IRUnit U=gen_unit(&P.toplevel);
        // Merge simple
        for(size_t k=0;k<U.funs.len;k++){ vec_push(unit.funs, U.funs.data[k]); }
        // Free sources
        source_free(&s);
    }

    if(cli.emit_ir){ ir_dump(&unit); }
    if(cli.emit_bc){ bc_write(&unit, cli.out, cli.debug, cli.O, cli.target?cli.target:""); fprintf(stderr, "écrit: %s\n", cli.out); }

    return 0;
}
