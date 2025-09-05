VITTE LIGHT — GUIDE UTILISATEUR COMPLET
Révision: 2025-09-05
──────────────────────────────────────────────

0) OBJECTIF ET PUBLIC
Vitte Light (VITL) est un langage minimaliste dérivé de Vitte.  
Il conserve la même syntaxe mais réduit la complexité pour être:
- accessible aux débutants qui découvrent la programmation,
- pratique pour les développeurs intermédiaires qui veulent des scripts rapides,
- utile aux professionnels qui recherchent un langage compilé simple, portable, avec FFI C.

Philosophie: 
- syntaxe lisible et cohérente,
- compilation rapide,
- intégration facile avec l’écosystème C.

──────────────────────────────────────────────
1) STRUCTURE DE PROJET ET FICHIERS
Extension: `.vitl`  
Encodage: UTF-8 (sans BOM) → standard dans tous les éditeurs modernes.

Organisation type:
  /src    → code source (.vitl)
  /libs   → bibliothèques natives (.c, .a, .so, .dll) pour FFI
  /build  → binaires et artefacts compilés

Bonne pratique: séparer **code** et **dépendances natives** pour simplifier la maintenance.  

Convention de modules:
- Nom en snake_case,
- Chemin hiérarchique = dossier.  
Exemple: `module std.io` correspond à `std/io.vitl`.

──────────────────────────────────────────────
2) OUTILS EN LIGNE DE COMMANDE
Selon votre installation, vous aurez:
- soit `vitl` binaire unique,
- soit `vitlc` (compilateur) + `vitlv` (VM).

Modes d’usage:
- **Exécution directe (JIT/VM)**  
  `vitl run src/main.vitl`  
  rapide, utile pour scripts et tests.

- **Compilation vers binaire natif**  
  `vitl build -O2 -o build/app src/main.vitl`  
  production, distribution, intégration système.  

Commandes utiles:
- `vitl fmt` → formateur de code (style unifié).
- `vitl check` → analyse statique (lint).
- `vitl test` → exécute les tests internes.
- `vitl doc`  → extrait la doc des commentaires `///`.

Options fréquentes:
- `-O0..O3` → optimisation (0=debug, 3=max perf).
- `-g` → inclure symboles debug.
- `--emit-ir` → montre l’IR lisible.
- `--emit-bytecode` → génère `.vitbc`.

──────────────────────────────────────────────
3) SYNTAXE DE BASE
La syntaxe est volontairement proche de Rust, C et Swift pour être familière.

Commentaires: `//` ou `/* ... */`  
Identificateurs: lettres/chiffres/underscore, pas d’espaces.

Types primitifs:
- entiers: `i32`, `u64`…
- flottants: `f32`, `f64`
- booléens: `true`/`false`
- chaînes: `"texte"`
- caractères: `'a'`
- optionnels: `Option<T>`

Déclarations:
const PI: f64 = 3.1415
let mut compteur: i32 = 0
let message = "Salut" // inférence automatique

makefile
Copier le code

Fonctions:
fn add(a: i32, b: i32) -> i32 {
return a+b
}

makefile
Copier le code

Contrôle:
if x > 0 { ... } else { ... }
for i in 0..10 { ... } // 0..10 exclusif
match val {
0 => println("zéro"),
_ => {}
}

vbnet
Copier le code

──────────────────────────────────────────────
4) EXEMPLES POUR CHAQUE NIVEAU
Débutant → apprendre la structure:
module app.main
import std.io

fn main() -> i32 {
std.io::println("Bonjour le monde");
return 0
}

yaml
Copier le code

Intermédiaire → travailler avec des fichiers:
module app.cat
import std.{fs, io, cli}

fn main() -> i32 {
let args = cli::args()
if std.len(args) < 2 {
io::eprintln("usage: cat <fichier>")
return 2
}
let contenu = fs::read_to_string(args[1])?
io::print(contenu)
return 0
}

rust
Copier le code

Pro → struct + impl + méthodes:
module app.geom

struct Vec2 { x: f64, y: f64 }

impl Vec2 {
fn norm(&self) -> f64 {
return (self.xself.x + self.yself.y).sqrt()
}
}

fn main() -> i32 {
let v = Vec2{ x:3.0, y:4.0 }
std.io::println(v.norm().to_string()) // 5
return 0
}

markdown
Copier le code

──────────────────────────────────────────────
5) MÉMOIRE ET SÉCURITÉ
- **Automatique:** modèle Rc<T> (référence comptée).  
  Pas besoin de `free()`.  
- **Unsafe:** pointeurs bruts pour FFI. À éviter pour les débutants.  
- **Strings:** `str` = immuable, `String` = mutable.  
- **Slices:** vues sur tableaux.  

Bonne pratique: utiliser Rc et les containers de la stdlib.  

──────────────────────────────────────────────
6) ERREURS ET PANICS
Type standard: `Result<T,E>`  
Exemple:
fn f() -> Result<i32,str> { return Result::Ok(42) }

let r = f()
match r {
Result::Ok(v) => println(v.to_string()),
Result::Err(e) => eprintln(e)
}

go
Copier le code

L’opérateur `?` propage l’erreur:
let contenu = fs::read_to_string("f.txt")?

mathematica
Copier le code

──────────────────────────────────────────────
7) FFI C
Interop simple avec bibliothèques C.

Déclarer:
extern "C" { fn puts(msg: *const char) -> i32 }

makefile
Copier le code

Appeler:
let cstr = std.c::CString::from_str("Hello\n")
unsafe { puts(cstr.as_ptr()) }

makefile
Copier le code

Compilation:
vitl build -L libs -lc -o build/app src/main.vitl

diff
Copier le code

──────────────────────────────────────────────
8) STDLIB RAPIDE
- std.io    → print, println, eprintln
- std.fs    → read/write fichiers
- std.str   → len, split, find, replace
- std.math  → abs, sqrt, sin, cos
- std.time  → now, sleep_ms
- std.cli   → args, exit
- std.vec   → vecteurs dynamiques
- std.debug → assert, backtrace
- std.err   → Error
- std.c     → FFI utilitaires

──────────────────────────────────────────────
9) STYLE & LINT
`vitl fmt` impose des règles de lisibilité:  
- module en tête  
- imports regroupés  
- snake_case pour variables/fonctions  
- CamelCase pour types  

`vitl check` détecte variables inutilisées, branches mortes, incohérences.

──────────────────────────────────────────────
10) CONSEILS PAR NIVEAU
Débutant:
- commencez avec `print`/`println`,
- utilisez `let` sans type explicite pour simplifier.

Intermédiaire:
- testez vos fonctions avec `test "..."`,
- manipulez fichiers et erreurs avec `?`.

Pro:
- explorez FFI,
- activez optimisations `-O3`,
- utilisez `--emit-ir` pour analyser le code généré.

──────────────────────────────────────────────
11) LIMITES ACTUELLES
- pas de threads,
- pas de GC (seulement Rc),
- FFI limité au C,
- génériques partiels seulement.

──────────────────────────────────────────────
12) CODES DE SORTIE
0 → succès  
1 → erreur générique/panic  
2 → erreur d’usage CLI  
3 → erreur I/O  
4 → erreur FFI

──────────────────────────────────────────────
13) CHECKLIST AVANT BUILD
[ ] module app.main défini  
[ ] fn main()->i32 existe  
[ ] imports nécessaires uniquement  
[ ] vitl test passe  
[ ] build/app généré  

──────────────────────────────────────────────
FIN DU GUIDE
──────────────────────────────────────────────
Veux-tu que je décline ce guide en 3 versions séparées (Débutant, Intermédiaire, Pro) ou garder une seule version unique enrichie comme ci-dessus ?







Vous avez dit :
plus de texte plus explicatif


ChatGPT a dit :
Compris. Voici une version enrichie du guide Vitte Light : beaucoup plus longue, avec du texte explicatif pensé pour débutants, intermédiaires et pros. Toujours en TXT, mais structuré comme un manuel de formation.

markdown
Copier le code
VITTE LIGHT — GUIDE UTILISATEUR DÉTAILLÉ
Révision : 2025-09-05
──────────────────────────────────────────────

0) INTRODUCTION
────────────────
Vitte Light (abrégé VITL) est un langage minimaliste qui reprend la syntaxe du langage Vitte, 
mais vise la simplicité et la rapidité.  

- Pour un **débutant**, VITL est une porte d’entrée vers la programmation moderne : 
  syntaxe claire, concepts simples, règles cohérentes.
- Pour un **développeur intermédiaire**, c’est un langage efficace pour écrire des scripts 
  rapides, compacts et portables.
- Pour un **professionnel**, VITL propose un compilateur performant, une VM, un FFI C stable 
  et des outils de build sérieux.

VITL est conçu pour être utilisé à trois échelles :
1. **Écriture rapide de scripts** (usage CLI, prototypage).
2. **Compilation en exécutables natifs** (performance, distribution).
3. **Intégration comme langage embarqué** dans des projets C (FFI simple).

──────────────────────────────────────────────

1) STRUCTURE DE PROJET
──────────────────────
Un projet VITL se construit toujours autour d’un dossier clair.

Arborescence type :
  /src    → votre code source (.vitl)
  /libs   → bibliothèques natives C (.c, .so, .dll, .a)
  /build  → binaires compilés et artefacts intermédiaires

Convention de modules :
- Un fichier correspond à un module.
- Le nom du module reflète le chemin du fichier.
- Exemple : `std/io.vitl` → `module std.io`

Pourquoi cette rigueur ?
- Débutant : vous savez toujours où chercher.
- Intermédiaire : vous séparez le code applicatif de la librairie.
- Pro : facilite le packaging, la CI/CD et l’intégration avec CMake/Meson.

──────────────────────────────────────────────

2) OUTILS EN LIGNE DE COMMANDE
───────────────────────────────
VITL peut fonctionner de deux manières :
- soit avec un binaire unique `vitl`,
- soit avec un couple `vitlc` (compilateur) et `vitlv` (machine virtuelle).

Exécution immédiate (VM/JIT) :
  vitl run src/main.vitl
  vitlv run src/main.vitl

Compilation en exécutable natif :
  vitl build -O2 -o build/app src/main.vitl
  vitlc -O2 -o build/app src/main.vitl

Outils supplémentaires :
- `vitl fmt` → formateur automatique (uniformise le style).
- `vitl check` → vérifie votre code (lint statique).
- `vitl test` → lance vos tests unitaires.
- `vitl doc` → génère documentation à partir de commentaires `///`.

Options utiles :
- `-O0/-O1/-O2/-O3` : optimisation (O0=debug rapide, O3=max perf).
- `-g` : debug avec symboles.
- `--emit-ir` : affiche l’IR lisible.
- `--emit-bytecode` : génère fichier `.vitbc`.

──────────────────────────────────────────────

3) SYNTAXE : DÉCOUVERTE PROGRESSIVE
────────────────────────────────────
Commentaires :
  // ligne unique
  /* bloc multi-lignes */

Variables et constantes :
const PI: f64 = 3.14159 // constante typée
let mut compteur: i32 = 0 // variable mutable
let message = "Salut" // inférence de type

yaml
Copier le code

Fonctions :
fn add(a: i32, b: i32) -> i32 {
return a + b
}

nginx
Copier le code

Structures de contrôle :
if x > 0 { println("positif") }
else { println("non positif") }

for i in 0..10 { println(i.to_string()) } // 0..10 exclusif

sql
Copier le code

Match (pattern matching puissant) :
match valeur {
0 => println("zéro"),
1 | 2 => println("petit"),
_ => println("autre")
}

yaml
Copier le code

──────────────────────────────────────────────

4) PREMIERS EXEMPLES
────────────────────
Débutant → programme minimal :
module app.main
import std.io

fn main() -> i32 {
std.io::println("Bonjour Vitte Light")
return 0
}

yaml
Copier le code

Intermédiaire → lire un fichier :
module app.cat
import std.{fs, io, cli}

fn main() -> i32 {
let argv = cli::args()
if std.len(argv) < 2 {
io::eprintln("usage: cat <fichier>")
return 2
}
let contenu = fs::read_to_string(argv[1])?
io::print(contenu)
return 0
}

rust
Copier le code

Pro → struct, impl, méthodes :
module app.geom

struct Vec2 { x: f64, y: f64 }

impl Vec2 {
fn norm(&self) -> f64 {
return (self.xself.x + self.yself.y).sqrt()
}
}

fn main() -> i32 {
let v = Vec2 { x: 3.0, y: 4.0 }
std.io::println(v.norm().to_string()) // affiche 5
return 0
}

markdown
Copier le code

──────────────────────────────────────────────

5) MÉMOIRE ET SÉCURITÉ
───────────────────────
- En mode sûr → gestion automatique par Rc (références comptées).
- Pas de GC complexe → plus prévisible, idéal pour petits binaires.
- Les chaînes `str` sont immuables. Pour les modifier, utilisez `String`.
- Les slices `[T]` offrent des vues non propriétaires (rapide et sans copie).
- Le mode `unsafe` permet d’utiliser des pointeurs C, réservé aux pros.

Débutant : pas besoin de `free()` ni de penser au ramasse-miettes.  
Intermédiaire : apprenez Rc et slices pour éviter copies inutiles.  
Pro : combinez Rc, FFI et `unsafe` pour contrôler mémoire à bas niveau.

──────────────────────────────────────────────

6) GESTION DES ERREURS
──────────────────────
Type standard : `Result<T, E>`  
- `Ok(T)` → succès avec valeur
- `Err(E)` → erreur avec info

Propagation automatique avec `?` :
let contenu = fs::read_to_string("config.txt")?

yaml
Copier le code

Gestion explicite :
match fs::read_to_string("config.txt") {
Result::Ok(txt) => println(txt),
Result::Err(e) => eprintln(e)
}

scss
Copier le code

`panic("msg")` → erreur irrécupérable, arrêt du programme.

──────────────────────────────────────────────

7) INTEROPÉRABILITÉ C (FFI)
────────────────────────────
Déclarer :
extern "C" { fn puts(msg: *const char) -> i32 }

yaml
Copier le code

Appeler :
let cstr = std.c::CString::from_str("Hello C!\n")
unsafe { puts(cstr.as_ptr()) }

vbnet
Copier le code

Compiler avec une lib externe :
vitl build -L libs -lc -o build/app src/main.vitl

diff
Copier le code

──────────────────────────────────────────────

8) STDLIB — APERÇU
───────────────────
std.io    → print, println, eprintln  
std.fs    → read_to_string, write_string, exists  
std.str   → len, split, find, replace  
std.math  → abs, sqrt, sin, cos, pow  
std.time  → now, sleep_ms  
std.cli   → args, env, exit  
std.vec   → push, pop, len  
std.debug → assert, backtrace  
std.err   → Error  
std.c     → CString, malloc/free  

──────────────────────────────────────────────

9) STYLE ET BONNES PRATIQUES
────────────────────────────
- Toujours déclarer `module` en haut du fichier.
- Grouper vos `import` ensemble.
- snake_case pour variables/fonctions.
- CamelCase pour structs et enums.
- Types explicites pour API publiques.
- Utiliser `vitl fmt` pour garder un style cohérent.

──────────────────────────────────────────────

10) CONSEILS PAR NIVEAU
───────────────────────
Débutant :
- Utilisez `println` pour voir vos résultats.
- Commencez par des programmes courts (< 50 lignes).
- Explorez `vitl fmt` pour apprendre le style standard.

Intermédiaire :
- Utilisez `test` pour vérifier vos fonctions.
- Manipulez fichiers avec `std.fs`.
- Maîtrisez `?` pour simplifier le code d’erreurs.

Pro :
- Intégrez des libs C via FFI.
- Activez optimisations `-O3`.
- Explorez `--emit-ir` pour analyser votre code.
- Combinez VITL avec CI/CD (Makefile, CMake, GitHub Actions).

──────────────────────────────────────────────

11) LIMITES (VERSION LIGHT)
───────────────────────────
- Pas de threads natifs.
- Pas de GC complexe (seulement Rc).
- Génériques partiels uniquement.
- FFI limité au C.

──────────────────────────────────────────────

12) CODES DE SORTIE
────────────────────
0 → succès  
1 → erreur générique/panic  
2 → erreur d’usage CLI  
3 → erreur I/O  
4 → erreur FFI

──────────────────────────────────────────────

13) CHECKLIST AVANT DE LIVRER
─────────────────────────────
[ ] module app.main défini  
[ ] fn main()->i32 présent  
[ ] imports réduits au nécessaire  
[ ] vitl test passe avec succès  
[ ] build/app généré dans /build  

──────────────────────────────────────────────

FIN DU GUIDE
─────────────────────────────────────────────