/* ============================================================================
   lex.h — API du lexer Vitte/Vitl (C17)
   - Lexe l’EBNF « Vitte/Vitl » (identique pour les deux langages)
   - Identifiants, nombres (10/16/2, float + exp, underscores), bool, char,
   string
   - Commentaires //, //!, (slash-star)
 */

#ifndef VT_LEX_H
#define VT_LEX_H
#pragma once

#include <stddef.h> /* size_t */
#include <stdint.h> /* uint32_t, uint64_t */

#ifdef __cplusplus
extern "C" {
#endif

/* ----------------------------------------------------------------------------
   Export
---------------------------------------------------------------------------- */
#ifndef VT_LEX_API
#define VT_LEX_API extern
#endif

  /* ----------------------------------------------------------------------------
     Types publics
  ----------------------------------------------------------------------------
*/
  typedef enum vt_tok_kind {
    /* Spéciaux */
    TK_EOF = 0,
    TK_ERROR,

    /* Ident / littéraux */
    TK_IDENT,
    TK_INT,
    TK_FLOAT,
    TK_BOOL,
    TK_CHAR,
    TK_STRING,

    /* Mots-clés */
    TK_KW_module,
    TK_KW_import,
    TK_KW_use,
    TK_KW_as,
    TK_KW_pub,
    TK_KW_const,
    TK_KW_let,
    TK_KW_mut,
    TK_KW_fn,
    TK_KW_return,
    TK_KW_if,
    TK_KW_else,
    TK_KW_match,
    TK_KW_while,
    TK_KW_for,
    TK_KW_in,
    TK_KW_break,
    TK_KW_continue,
    TK_KW_type,
    TK_KW_impl,
    TK_KW_where,
    TK_KW_test,
    TK_KW_true,
    TK_KW_false,

    /* Opérateurs d’affectation composés */
    TK_EQ,
    TK_PLUSEQ,
    TK_MINUSEQ,
    TK_MULEQ,
    TK_DIVEQ,
    TK_MODEQ,
    TK_SHLEQ,
    TK_SHREQ,
    TK_ANDEQ,
    TK_XOREQ,
    TK_OREQ,

    /* Opérateurs logiques/bitwise/relatifs */
    TK_LOR,
    TK_LAND,
    TK_BOR,
    TK_BXOR,
    TK_BAND,
    TK_EQEQ,
    TK_NEQ,
    TK_LT,
    TK_LTE,
    TK_GT,
    TK_GTE,
    TK_SHL,
    TK_SHR,
    TK_PLUS,
    TK_MINUS,
    TK_STAR,
    TK_SLASH,
    TK_PERCENT,
    TK_BANG,
    TK_AMP,

    /* Ranges */
    TK_DOT,
    TK_DOTDOT,
    TK_DOTDOTEQ,

    /* Délimiteurs */
    TK_LP,
    TK_RP,
    TK_LB,
    TK_RB,
    TK_LC,
    TK_RC,
    TK_COMMA,
    TK_COLON,
    TK_SEMI,
    TK_DCOLON,
    TK_ARROW,
    TK_FATARROW
  } vt_tok_kind;

  /* Position (1-based pour ligne/col) */
  typedef struct {
    uint32_t line;
    uint32_t col;
    uint32_t offset; /* 0-based octet dans le buffer source */
  } vt_src_pos;

  /* Infos numériques (remplies pour TK_INT / TK_FLOAT) */
  typedef struct {
    int is_neg;   /* réservé (pourrait rester 0) */
    uint64_t u64; /* entier non signé interne */
    double f64;   /* flottant */
  } vt_num;

  /* Jeton */
  typedef struct {
    vt_tok_kind kind;
    vt_src_pos pos;  /* position du début du lexème */
    const char* lex; /* pointeur dans le buffer source (non null-terminated) */
    uint32_t len;    /* longueur du lexème en octets */
    vt_num num;      /* pour TK_INT / TK_FLOAT */
    int bool_val;    /* pour TK_BOOL (true/false) */
  } vt_token;

  /* Lexer (public pour allocation stack/heap par l’appelant) */
  typedef struct {
    const char* src;
    size_t len;
    size_t cur;
    uint32_t line;
    uint32_t col;

    /* Lookahead simple */
    vt_token la;
    int has_la;

    /* Options (réservé) */
    int keep_doc;

    /* Buffer interne pour formatage d’erreurs */
    char errbuf[256];
  } vt_lexer;

  /* ----------------------------------------------------------------------------
     API
  ----------------------------------------------------------------------------
*/

  /* Initialise le lexer à partir d’un buffer mémoire (non copié). */
  VT_LEX_API void vt_lex_init(vt_lexer * lx, const char* src, size_t len);

  /* Charge un fichier (lit en mémoire) et initialise le lexer.
     Retourne 0 si OK, -1 si erreur (errno renseigné).
     À la fin, appeler vt_lex_dispose_from_file() pour libérer le buffer. */
  VT_LEX_API int vt_lex_init_from_file(vt_lexer * lx, const char* path);

  /* Libère la mémoire allouée par vt_lex_init_from_file(). */
  VT_LEX_API void vt_lex_dispose_from_file(vt_lexer * lx);

  /* Récupère le prochain token (ignore espaces/commentaires). */
  VT_LEX_API vt_token vt_lex_next(vt_lexer * lx);

  /* Lookahead (1-token) sans consommer. */
  VT_LEX_API vt_token vt_lex_peek(vt_lexer * lx);

  /* Consomme et vérifie qu’un token spécifique est présent.
     - k : genre attendu
     - out : optionnel, reçoit le token consommé
     - err : optionnel, reçoit un message formaté pointant la position du token
     inattendu Retourne 1 si OK, 0 sinon. */
  VT_LEX_API int vt_lex_expect(vt_lexer * lx, vt_tok_kind k, vt_token * out,
                               const char** err);

  /* Formate un message d’erreur multi-lignes (ligne source + caret).
     Le pointeur retourné est valide jusqu’au prochain appel modifiant le lexer.
   */
  VT_LEX_API const char* vt_lex_format_error(vt_lexer * lx, const char* msg,
                                             vt_src_pos p);

  /* Décodage des littéraux string/char (unescape).
     - vt_lex_decode_string: copie le contenu (sans guillemets) dans `out`.
       Retourne 1 si OK, 0 si erreur (buffer insuffisant, escape invalide, …).
       outlen (optionnel) reçoit la longueur écrite.
     - vt_lex_decode_char: extrait un seul code-unit (ASCII) depuis un TK_CHAR.
       Retourne 1 si OK, 0 sinon. */
  VT_LEX_API int vt_lex_decode_string(const vt_token* t, char* out,
                                      uint32_t outcap, uint32_t* outlen);
  VT_LEX_API int vt_lex_decode_char(const vt_token* t, char* out);

  /* Utilitaires debug */
  VT_LEX_API const char* vt_tok_name(vt_tok_kind k);
  VT_LEX_API void vt_tok_dump(const vt_token* t);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* VT_LEX_H */
