README — VITTE LIGHT (VITL)
Révision 2025-09-05

──────────────────────────────────────────────────────────────────────────────
0) DESCRIPTION
Vitte Light (VITL) est une version compacte du langage Vitte. 
Même syntaxe canonique, mais runtime réduit.  
Objectifs: 
- scripts CLI rapides, 
- binaires compacts,
- intégration simple en C.

──────────────────────────────────────────────────────────────────────────────
1) CARACTÉRISTIQUES
- Syntaxe 100% identique à Vitte (let, fn, struct, enum, impl, match, slices).
- Toolchain unique: `vitl` (ou `vitlc` + `vitlv`).
- Compilation native → ELF/Mach-O/PE, ou exécution via VM.
- Stdlib réduite: std.io, std.fs, std.str, std.math, std.time, std.cli, std.vec.
- Mémoire: comptage de références (Rc/Weak).
- Gestion erreurs: Result<T,E>, propagation avec `?`, panic.
- Tests intégrés: `test` + `assert`.
- FFI C stable (extern "C").
- Formatteur intégré (`vitl fmt`).
- Cross-compilation avec `--target`.

──────────────────────────────────────────────────────────────────────────────
2) INSTALLATION
Dépendances: GCC/Clang, make, libc.
Exemple:

    git clone https://github.com/example/vitte-light.git
    cd vitte-light
    make
    sudo make install

Ensuite `vitl` disponible dans $PATH.

──────────────────────────────────────────────────────────────────────────────
3) UTILISATION BASIQUE
Exécuter directement:

    vitl run src/main.vitl

Compiler en binaire natif:

    vitl build -O2 -o build/app src/main.vitl

Autres:

    vitl fmt src/              # formater
    vitl check src/            # analyse statique
    vitl test                  # tests
    vitl doc src/ -o docs.txt  # extraction doc

──────────────────────────────────────────────────────────────────────────────
4) STRUCTURE DE PROJET
project/
 ├─ src/
 │   └─ main.vitl
 ├─ build/
 └─ libs/    # librairies C optionnelles pour FFI

──────────────────────────────────────────────────────────────────────────────
5) EXEMPLE MINIMAL
src/main.vitl :

    module app.main
    import std.io

    fn main() -> i32 {
      std.io::println("Hello, Vitte Light")
      return 0
    }

Exécution:

    vitl run src/main.vitl

Résultat:

    Hello, Vitte Light

──────────────────────────────────────────────────────────────────────────────
6) GRAMMAIRE RÉSUMÉE (EBNF)
  Module    = "module", Ident, {".", Ident} ;
  Import    = "import", Path, [ "{" , Idents , "}" ] ;
  Const     = "const", Ident, ":", Type, "=", Expr ;
  Let       = "let", ["mut"], Ident, [":", Type], "=", Expr ;
  Fn        = "fn", Ident, "(", [Params], ")", ["->", Type], Block ;
  Struct    = "struct", Ident, "{" , Fields , "}" ;
  Enum      = "enum", Ident, "{" , Variants , "}" ;
  Impl      = "impl", Ident, Block ;
  Block     = "{" , {Stmt} , "}" ;
  Stmt      = Let | Expr | Return | If | While | For | Match ;

──────────────────────────────────────────────────────────────────────────────
7) STDLIB DISPONIBLE
- std.io    : print, println, eprint, eprintln
- std.fs    : read_to_string, read, write_string, exists
- std.str   : len, split, find, replace, to_int, to_float
- std.math  : abs, sqrt, sin, cos, pow
- std.time  : now, sleep_ms
- std.cli   : args, env, exit
- std.vec   : with_capacity, push, pop, len
- std.debug : assert, dump
- std.err   : Error
- std.c     : CString, malloc/free (unsafe)

──────────────────────────────────────────────────────────────────────────────
8) FFI C (OPTIONNEL)
Déclaration:

    extern "C" {
      fn puts(msg: *const char) -> i32
    }

Appel:

    let cstr = std.c::CString::from_str("Hello\n")
    unsafe { _ = puts(cstr.as_ptr()) }

Build avec libC:

    vitl build -L libs -lc -o build/app src/main.vitl

──────────────────────────────────────────────────────────────────────────────
9) TESTS INTÉGRÉS
    fn add(a: i32, b: i32) -> i32 { return a + b }

    test "sum works" {
      assert(add(2,2) == 4)
    }

Exécution:

    vitl test

──────────────────────────────────────────────────────────────────────────────
10) ERREURS ET SORTIES
Codes retour:
- 0 succès
- 1 panic / erreur générique
- 2 erreur usage CLI
- 3 erreur I/O
- 4 erreur FFI/lib dynamique

Exemple de propagation d’erreur:

    fn cat(path: str) -> Result<str, str> {
      return std.fs::read_to_string(path)
    }

    fn main() -> i32 {
      let argv = std.cli::args()
      let txt = cat(argv[1])?   // `?` remonte l’erreur
      std.io::print(txt)
      return 0
    }

──────────────────────────────────────────────────────────────────────────────
11) DIAGNOSTICS COURANTS
E0001 symbole inconnu → vérifier import
E0002 types incompatibles → cast explicite
E0003 variable non init
E1001 appel FFI hors `unsafe`
E2001 fichier non trouvé

──────────────────────────────────────────────────────────────────────────────
12) CHECKLIST AVANT RELEASE
[ ] module app.main défini
[ ] main() -> i32 présent
[ ] imports minimaux
[ ] build avec -O2 ou -g
[ ] tests passent
[ ] binaire généré dans /build

──────────────────────────────────────────────────────────────────────────────
LICENCE
MIT License.
