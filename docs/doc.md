VITTE LIGHT — GUIDE UTILISATEUR (TXT)
Révision: 2025-09-05

──────────────────────────────────────────────────────────────────────────────
0) OBJECTIF
Vitte Light (VITL) est un langage minimal inspiré de Vitte, même syntaxe de base.
Cible: programmes CLI rapides, scripts compilés, embeddings C.
Ce document couvre: structure de projet, CLI, syntaxe, stdlib, FFI, exemples.

──────────────────────────────────────────────────────────────────────────────
1) FICHIERS, EXTENSIONS, ENCODAGE
- Source: .vitl
- Encodage: UTF-8 sans BOM
- Convention modules: noms en snake.case, chemins hiérarchiques
  Exemple: module std.io correspond à std/io.vitl

Arbo simple:
  /src
    main.vitl
  /libs           # libs .c/.a/.so/.dll si FFI
  /build          # artefacts

──────────────────────────────────────────────────────────────────────────────
2) OUTILS EN LIGNE DE COMMANDE
Selon distribution, soit binaire unique `vitl`, soit couple `vitlc` (compileur),
`vitlv` (VM/runner). Les deux formes ci-dessous sont supportées.

Exécution directe (JIT/VM):
  vitl run src/main.vitl
ou:
  vitlv run src/main.vitl

Compilation → exécutable natif:
  vitl build -O2 -o build/app src/main.vitl
ou:
  vitlc -O2 -o build/app src/main.vitl

Autres:
  vitl fmt path/                  # formateur
  vitl check src/                 # analyse statique
  vitl test                       # lance tests intégrés
  vitl doc src/ -o build/docs.txt # extrait commentaires ///

Options communes:
  -O0/-O1/-O2/-O3, -g, --emit-ir, --emit-bytecode, --no-stdlib, -I <dir>, -L <dir>, -l<name>

──────────────────────────────────────────────────────────────────────────────
3) SYNTAXE ESSENTIELLE (IDENTIQUE À VITTE)
Lexique:
  - Ident: [A-Za-z_][A-Za-z0-9_]*
  - Commentaires: // ligne, /* bloc */
  - Séparateurs: ; optionnel en fin de ligne si non ambigu

Littéraux:
  - Entiers: 0, 42, 1_000, 0xFF, 0o755, 0b1010
  - Flottants: 3.14, 1e-9
  - Bool: true, false
  - Char: 'a', '\n'
  - String: "texte", r"brut"

Types de base:
  i8 i16 i32 i64  u8 u16 u32 u64  f32 f64  bool char  str
  arrays: [T; N]   slices: [T]    tuples: (T1, T2)    option: Option<T>
  pointeurs bas niveau si activés: *T, *mut T   (mode unsafe)

Déclarations:
  module app.main                  // un par fichier
  import std.io
  import std.{fs, time}            // import groupé

  const PI: f64 = 3.1415926535
  let mut x: i32 = 0
  let y = 123                      // inférence

Fonctions:
  fn add(a: i32, b: i32) -> i32 { return a + b }
  fn main() -> i32 {
    std.io::println("Hello");
    return 0
  }

Contrôle:
  if cond { ... } else { ... }
  while cond { ... }
  for i in 0..10 { ... }           // 0..10 exclusif, 0..=10 inclusif
  match v {
    0 => std.io::println("zero"),
    1 | 2 => std.io::println("small"),
    _ => {}
  }

Structures et enums:
  struct Point { x: f64, y: f64 }
  enum Result<T, E> { Ok(T), Err(E) }

Méthodes et impl:
  impl Point {
    fn norm(&self) -> f64 { return (self.x*self.x + self.y*self.y).sqrt() }
  }

Erreurs:
  fn f() -> Result<i32, str> { return Result::Ok(42) }
  let r = f()
  match r { Result::Ok(v) => {}, Result::Err(e) => std.io::eprintln(e) }

Assertions/tests:
  assert(x == 4)
  test "sum works" { assert(add(2,2) == 4) }

──────────────────────────────────────────────────────────────────────────────
4) ENTRÉE/SORTIE (STD)
E/S console:
  std.io::print("txt")
  std.io::println("txt")
  std.io::eprintln("err")

Fichiers:
  let data = std.fs::read_to_string("file.txt")?
  std.fs::write_string("out.txt", "hello")?

Temps:
  let now = std.time::now()

Chaînes:
  let s = "hi " + name
  let n = std.str::len(s)
  let parts = std.str::split(s, ' ')

Args, exit:
  let argv = std.cli::args()
  std.sys::exit(0)

`?` propage l’erreur sous forme Result; équivaut à match court.

──────────────────────────────────────────────────────────────────────────────
5) GESTION MÉMOIRE
- Automatique par comptage de références (Rc) en mode sûr.
- `unsafe` autorise pointeurs bruts et FFI manuel.
- Slices vues non propriétaires; strings immuables `str`, mutables via `String`.

Exemples rapides:
  let mut v: [i32] = std.vec::with_capacity(16)
  v.push(1); v.push(2)
  for x in &v { std.io::println(x.to_string()) }

──────────────────────────────────────────────────────────────────────────────
6) ERREURS ET TRAITEMENT
Type canonique: Result<T, E>
- Fonctions std retournent Result avec E=std.err::Error ou str.
- Opérateur `?` remonte l’erreur.
- `panic("msg")` pour erreurs irrécupérables; exit code ≠ 0.

──────────────────────────────────────────────────────────────────────────────
7) FFI C (OPTIONNEL)
Déclarer signatures externes:
  extern "C" {
    fn puts(msg: *const char) -> i32
  }

Appel:
  let cstr = std.c::CString::from_str("Hello\n")
  unsafe { _ = puts(cstr.as_ptr()) }

Lien:
  vitl build -L libs -lc -o build/app src/main.vitl
  # ou -l<libname> pour lib<name>.so/.dll/.a

──────────────────────────────────────────────────────────────────────────────
8) EXEMPLES CONCIS

8.1 Hello CLI
  module app.main
  import std.io
  fn main() -> i32 { std.io::println("Hello, Vitte Light"); return 0 }

8.2 Somme, tests
  module app.math
  fn sum(xs: [i32]) -> i32 {
    let mut s = 0
    for x in xs { s = s + x }
    return s
  }
  test "sum" { assert(sum([1,2,3]) == 6) }

8.3 Lecture fichier avec `?`
  module app.cat
  import std.{fs, io}
  fn main() -> i32 {
    let argv = std.cli::args()
    if std.len(argv) < 2 { io::eprintln("usage: cat <file>"); return 2 }
    let txt = fs::read_to_string(argv[1])?
    io::print(txt)
    return 0
  }

8.4 Enum et match
  module app.http
  enum Code { Ok, NotFound, Other(i32) }
  fn descr(c: Code) -> str {
    match c { Code::Ok => "200", Code::NotFound => "404", Code::Other(x) => "X" }
  }

8.5 Struct + méthodes
  module app.geo
  struct Vec2 { x: f64, y: f64 }
  impl Vec2 { fn dot(&self, o: Vec2) -> f64 { return self.x*o.x + self.y*o.y } }

──────────────────────────────────────────────────────────────────────────────
9) FORMATTEUR ET LINT
- `vitl fmt` aligne imports, retire espaces, normalise accolades.
- `vitl check` signale variables non utilisées, branches mortes, conversions implicites.

Règles de style abrégées:
- Module au début de fichier, imports regroupés.
- Types explicites aux API publiques.
- snake_case pour fonctions/vars, CamelCase pour types.

──────────────────────────────────────────────────────────────────────────────
10) DIAGNOSTICS COURANTS
E0001: symbole inconnu
  → vérifier import ou nom du module
E0002: types incompatibles
  → caster explicitement: x as i32
E0003: variable non initialisée
E1001: appel FFI non-safe hors bloc unsafe
E2001: fichier introuvable (std.fs::read_*)

──────────────────────────────────────────────────────────────────────────────
11) BUILD AVANCÉ
IR/Bytecode:
  --emit-ir         # imprime IR lisible
  --emit-bytecode   # écrit .vitbc (VITBC) dans /build

Optimisations:
  -O2 par défaut; -O3 active inlining agressif et unroll
Débogage:
  -g + std.debug::backtrace() si supporté

Cross-build:
  vitlc --target x86_64-linux-gnu -O2 -o build/app src/main.vitl

──────────────────────────────────────────────────────────────────────────────
12) MINI GRAMMAIRE (EBNF RÉSUMÉ)
  Module    = "module", Ident, {".", Ident} ;
  Import    = "import", Path, [ "{", Ident, {",", Ident}, "}" ] ;
  Decl      = Const | Let | Fn | Struct | Enum | Impl ;
  Const     = "const", Ident, ":", Type, "=", Expr ;
  Let       = "let", ["mut"], Ident, [":", Type], "=", Expr ;
  Fn        = "fn", Ident, "(", [Params], ")", ["->", Type], Block ;
  Struct    = "struct", Ident, "{", Fields, "}" ;
  Enum      = "enum", Ident, "{", Variants, "}" ;
  Impl      = "impl", Ident, Block ;
  Block     = "{", {Stmt}, "}" ;
  Stmt      = Let | Expr | Return | If | While | For | Match | ";"
  Return    = "return", [Expr] ;
  If        = "if", Expr, Block, ["else", Block] ;
  While     = "while", Expr, Block ;
  For       = "for", Ident, "in", Expr, Block ;
  Match     = "match", Expr, "{", Arms, "}" ;

──────────────────────────────────────────────────────────────────────────────
13) STDLIB (SURVOL MINIMAL)
  std.io    : print, println, eprint, eprintln
  std.fs    : read_to_string, read, write_string, write, exists
  std.str   : len, split, find, replace, to_int, to_float
  std.math  : abs, sqrt, sin, cos, pow
  std.time  : now, sleep_ms
  std.cli   : args, env, exit
  std.vec   : with_capacity, push, pop, len, get
  std.err   : Error, display
  std.debug : assert, dump, backtrace
  std.c     : CString, malloc/free (unsafe)

──────────────────────────────────────────────────────────────────────────────
14) CONVENTIONS D’EXIT
  0  succès
  1  erreur générique/panic
  2  erreur d’usage CLI
  3  I/O
  4  FFI/chargement lib

──────────────────────────────────────────────────────────────────────────────
15) RECETTES RAPIDES

Lecture JSON en texte brut (sans parser dédié):
  let raw = std.fs::read_to_string("cfg.json")?
  if std.str::find(raw, "\"enabled\": true") >= 0 { std.io::println("on") }

Mesure de durée:
  let t0 = std.time::now()
  heavy()
  std.io::println((std.time::now() - t0).to_string() + " ms")

──────────────────────────────────────────────────────────────────────────────
16) LIMITES ACTUELLES (MODE LIGHT)
  - Pas de threads natifs.
  - GC absent; modèle Rc + scopes.
  - Génériques limités aux containers std (selon build).
  - FFI stable C uniquement.

──────────────────────────────────────────────────────────────────────────────
17) CHECKLIST PROJET
  [ ] module app.main défini
  [ ] main() -> i32 présent
  [ ] imports minimaux
  [ ] build avec -O2 ou -g suivant cible
  [ ] tests passent: vitl test
  [ ] binaire dans /build

FIN.
