# VitteLight — VM Tooling & CLI

> Petite VM / toolkit C (C17) : parsing, UTF-8 utils, hash, IO, arena/pool alloc, CLI démos.

[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
![Language](https://img.shields.io/badge/lang-C17-informational)
![OS](https://img.shields.io/badge/OS-macOS%20%7C%20Linux%20%7C%20Windows-forestgreen)
![Build](https://img.shields.io/badge/build-Makefile-brightgreen)
![CI](https://img.shields.io/badge/CI-local%20or%20GH%20Actions-lightgrey)
![Homebrew](https://img.shields.io/badge/brew-ready-black)
![PkgConfig](https://img.shields.io/badge/pkg--config-vittelight.pc-yellow)

---

## Dernière release

**v0.1.0** — tag `v0.1.0`  
Archive source : `vitte-light-0.1.0.tar.gz`  
SHA256 (archive) : `…à remplacer par la valeur générée par make brew-archive…`

Précompilés (optionnel si vous en publiez) :
- macOS (x86_64/arm64) : `vitte-cli` (~120–180 KB)
- Linux (x86_64) : `vitte-cli` (~110–170 KB)
- Windows (x86_64) : `vitte-cli.exe` (~150–230 KB)

Changements majeurs :
- Nouveau CLI `vitte-cli` (sous‐commandes `info`, `rand`, `hash`, `cat`, `json`, `utf8`, `freq`, `bench`, `ansi`, `demo`)
- Bibliothèque statique/dynamique `libvittelight` + header public `core/api.h`
- Alloc mémoire avancée (arena, pool, pages), StringBuilder, vecteurs dynamiques, hash64, I/O simples
- Helpers UTF-8 (encode/decode), writer JSON, ANSI helpers
- Makefile *cross-platform* + génération `pkg-config` + recette Homebrew

──────────────────────────────────────────────────────────────────────────────
## DESCRIPTION
Vitte Light (VITL) est une version compacte du langage Vitte. 
Même syntaxe canonique, mais runtime réduit.  
Objectifs: 
- scripts CLI rapides, 
- binaires compacts,
- intégration simple en C.

──────────────────────────────────────────────────────────────────────────────
## CARACTÉRISTIQUES
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
## INSTALLATION
Dépendances: GCC/Clang, make, libc.
Exemple:
``` bash
    git clone https://github.com/example/vitte-light.git
    cd vitte-light
    make
    sudo make install
```
Ensuite `vitl` disponible dans $PATH.

──────────────────────────────────────────────────────────────────────────────
##U TILISATION BASIQUE
Exécuter directement:
``` bash
    vitl run src/main.vitl

Compiler en binaire natif:

    vitl build -O2 -o build/app src/main.vitl

Autres:

    vitl fmt src/              # formater
    vitl check src/            # analyse statique
    vitl test                  # tests
    vitl doc src/ -o docs.txt  # extraction doc
```
──────────────────────────────────────────────────────────────────────────────
## STRUCTURE DE PROJET
``` markdown
project/
 ├─ src/
 │   └─ main.vitl
 ├─ build/
 └─ libs/    # librairies C optionnelles pour FFI
```
──────────────────────────────────────────────────────────────────────────────
## EXEMPLE MINIMAL
src/main.vitl :
``` vitl
    module app.main
    import std.io

    fn main() -> i32 {
      std.io::println("Hello, Vitte Light")
      return 0
    }
```
Exécution:
``` vitl
    vitl run src/main.vitl

Résultat:

    Hello, Vitte Light
```
──────────────────────────────────────────────────────────────────────────────
## GRAMMAIRE RÉSUMÉE (EBNF)
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
## STDLIB DISPONIBLE
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
## FFI C (OPTIONNEL)
Déclaration:
``` vitl
    extern "C" {
      fn puts(msg: *const char) -> i32
    }

Appel:
``` vitl
    let cstr = std.c::CString::from_str("Hello\n")
    unsafe { _ = puts(cstr.as_ptr()) }
```
``` vitte
Build avec libC:

    vitl build -L libs -lc -o build/app src/main.vitl
```
──────────────────────────────────────────────────────────────────────────────
## TESTS INTÉGRÉS
``` vitl
    fn add(a: i32, b: i32) -> i32 { return a + b }

    test "sum works" {
      assert(add(2,2) == 4)
    }

Exécution:

    vitl test

──────────────────────────────────────────────────────────────────────────────
## ERREURS ET SORTIES
Codes retour:
- 0 succès
- 1 panic / erreur générique
- 2 erreur usage CLI
- 3 erreur I/O
- 4 erreur FFI/lib dynamique

Exemple de propagation d’erreur:
``` vitl
    fn cat(path: str) -> Result<str, str> {
      return std.fs::read_to_string(path)
    }

    fn main() -> i32 {
      let argv = std.cli::args()
      let txt = cat(argv[1])?   // `?` remonte l’erreur
      std.io::print(txt)
      return 0
    }
```

──────────────────────────────────────────────────────────────────────────────
## DIAGNOSTICS COURANTS
E0001 symbole inconnu → vérifier import
E0002 types incompatibles → cast explicite
E0003 variable non init
E1001 appel FFI hors `unsafe`
E2001 fichier non trouvé

──────────────────────────────────────────────────────────────────────────────
## CHECKLIST AVANT RELEASE
``` markdown
[ ] module app.main défini
[ ] main() -> i32 présent
[ ] imports minimaux
[ ] build avec -O2 ou -g
[ ] tests passent
[ ] binaire généré dans /build
```
──────────────────────────────────────────────────────────────────────────────
LICENCE
MIT License.
