
VITTE LIGHT — GUIDE UTILISATEUR COMPLET
Révision: 2025-09-05
──────────────────────────────────────────────

0) INTRODUCTION — VITTE LIGHT
─────────────────────────────
Vitte Light (abrégé **VITL**) est un langage minimaliste, dérivé du langage Vitte, 
conçu pour allier **syntaxe moderne**, **simplicité d’usage** et **outillage complet**.  
Il vise trois profils de développeurs : débutants, intermédiaires, professionnels.

─────────────────────────────
0.1 Philosophie du langage
─────────────────────────────
— Minimalisme : garder un cœur réduit de fonctionnalités, faciles à apprendre.  
— Cohérence : syntaxe claire, pas de règles cachées, erreurs explicites.  
— Polyvalence : exécution immédiate via VM, compilation native, intégration en C.  
— Sécurité : gestion mémoire déterministe (références comptées), 
  erreurs explicites (Result, panic).  
— Portabilité : fonctionne sur Linux, BSD, macOS, Windows.  

─────────────────────────────
0.2 Pour qui est fait VITL ?
─────────────────────────────
**Débutant**  
— Découvrir la programmation avec une syntaxe simple et lisible.  
— Ne pas se perdre dans la complexité (pointeurs, mémoire manuelle, GC lourd).  
— Apprendre progressivement des concepts modernes : immutabilité, Result, match.  

**Développeur intermédiaire**  
— Écrire des scripts rapides et compacts pour automatiser des tâches.  
— Construire de petites applications CLI portables.  
— Profiter d’un formateur, d’un linter et de tests intégrés.  

**Professionnel**  
— Compiler en exécutable natif optimisé (-O3).  
— Exploiter le FFI C stable pour réutiliser des bibliothèques existantes.  
— Intégrer VITL dans des workflows CI/CD et packager des outils distribuables.  
— Inspecter IR et bytecode (`--emit-ir`, `--emit-bytecode`) pour analyser la compilation.  

─────────────────────────────
0.3 Domaines d’utilisation
─────────────────────────────
1. **Écriture rapide de scripts CLI**  
   — Remplacer Bash/Python dans certains cas avec un langage typé.  
   — Automatiser des tâches système, parser des fichiers, manipuler du texte.  

2. **Compilation en exécutables natifs**  
   — Créer de petits binaires autonomes, rapides à exécuter.  
   — Idéal pour distribution, packaging, embarqué.  
   — Compatible multi-plateformes (Linux, BSD, macOS, Windows).  

3. **Langage embarqué (FFI C)**  
   — Intégrer VITL dans des projets existants en C.  
   — Charger du code utilisateur compilé en bytecode `.vitbc`.  
   — Fournir un langage de script extensible pour un moteur ou un outil.  

─────────────────────────────
0.4 Points forts de VITL
─────────────────────────────
✓ Syntaxe lisible et unifiée (identique à Vitte).  
✓ Gestion mémoire simple et prévisible (Rc/Weak).  
✓ Erreurs explicites avec `Result` et propagation `?`.  
✓ Standard library (I/O, fichiers, chaînes, maths, temps, vecteurs).  
✓ Outils intégrés : formateur (`fmt`), linter (`check`), tests (`test`), doc (`doc`).  
✓ Exécution directe ou compilation native optimisée.  
✓ FFI C stable et facile à utiliser.  

─────────────────────────────
0.5 Exemple rapide (Hello World)
─────────────────────────────
Code :
  module app.main
  import std.io

  fn main() -> i32 {
    std.io::println("Bonjour Vitte Light")
    return 0
  }

Exécution :
  vitl run src/main.vitl

Compilation :
  vitl build -O2 -o build/app src/main.vitl
  ./build/app

─────────────────────────────
Résumé
— **Débutant** : apprendre la programmation moderne sans complexité inutile.  
— **Intermédiaire** : écrire des scripts compacts, robustes et portables.  
— **Pro** : bénéficier d’un compilateur performant, d’un FFI C stable et d’une toolchain complète.  
— Trois usages principaux : scripting CLI, compilation native, intégration embarquée.  



──────────────────────────────────────────────

1) STRUCTURE DE PROJET — GUIDE COMPLET
──────────────────────────────────────
Un projet VITL repose sur une organisation stricte des dossiers et fichiers.
Cette rigueur facilite la navigation, la compilation et l’intégration dans des 
outils externes (CMake, Meson, CI/CD, IDE).

──────────────────────────────────────
1.1 Arborescence type minimale
──────────────────────────────────────
/src      → code source principal (.vitl)
/libs     → bibliothèques natives C/FI (.c, .so, .dll, .a)
/build    → binaires compilés, artefacts intermédiaires, bytecode (.vitbc)

Exemple :
  mon_projet/
    src/
      main.vitl
      util/str.vitl
    libs/
      mylib.c
      mylib.a
    build/
      app            (binaire natif)
      app.vitbc      (bytecode compilé)

──────────────────────────────────────
1.2 Convention des modules
──────────────────────────────────────
— Un fichier correspond à un module unique.
— Le chemin disque reflète le nom du module.
— Le nom de module s’écrit en `snake.case`.

Exemple :
  Fichier : src/std/io.vitl
  Contenu : module std.io

Règle : **hiérarchie dossier = hiérarchie de module**.

Bénéfices :
— Import clair et intuitif :
     import std.io
     import util.str
— Évite les conflits de noms.
— Favorise la réutilisation de modules indépendants.

──────────────────────────────────────
1.3 Organisation recommandée
──────────────────────────────────────
Niveaux typiques d’un projet :

/src
  app/              → logique applicative (main, modules spécifiques)
  std/              → standard library locale/complémentaire
  util/             → utilitaires internes (par ex. parsing, formatage)

/libs
  externe/          → wrappers C tiers (sqlite.c, ssl.c…)
  interne/          → code C maison à exposer via FFI

/build
  debug/            → builds avec -O0 -g
  release/          → builds optimisés avec -O3
  docs/             → documentation générée par `vitl doc`

──────────────────────────────────────
1.4 Raison d’être de la rigueur
──────────────────────────────────────
Débutant
— Vous savez toujours où chercher vos fichiers.
— Le projet reste simple à lire, même avec plusieurs sources.

Intermédiaire
— Séparation claire entre code applicatif et librairies natives.
— Possibilité de réutiliser un module sans embarquer tout le projet.
— Tests plus faciles : `vitl test` explore /src et repère vos fichiers.

Pro
— Packaging plus simple (chaque dossier a un rôle précis).
— CI/CD : scripts de build ciblent `/src` et exportent dans `/build`.
— Compatibilité naturelle avec outils externes (CMake, Meson, Ninja).
— FFI C propre : tout ce qui est natif est dans `/libs`, sans polluer `/src`.

──────────────────────────────────────
1.5 Variantes de structure
──────────────────────────────────────
Projet simple (script unique)
  /src/main.vitl

Projet moyen (appli CLI avec libs perso)
  /src/
    main.vitl
    cli/args.vitl
    math/geom.vitl
  /libs/
    fastmath.c
  /build/

Projet complexe (grosse appli avec tests et doc)
/src/
  app/
    main.vitl
    net/http.vitl
    net/tcp.vitl
  core/
    error.vitl
    config.vitl
/tests/
  test_math.vitl
  test_net.vitl
/libs/
  sqlite/
    sqlite3.c
/build/
  debug/
  release/
  docs/

──────────────────────────────────────
1.6 Conseils pratiques
──────────────────────────────────────
— Toujours commencer un fichier source par :
     module chemin.nom
— Grouper les imports immédiatement après.
— Garder /src pour le code VITL, /libs pour le C natif, /build pour les binaires.
— Ajouter /tests si le projet grandit, et y placer uniquement des modules de tests.
— Ne jamais mélanger artefacts compilés et sources dans le même dossier.

──────────────────────────────────────
Résumé
✓ /src : code VITL (modules).  
✓ /libs : code natif pour FFI.  
✓ /build : binaires et docs.  
✓ Nom module = chemin fichier.  
✓ Structure stricte = lisibilité, maintenabilité, intégration CI/CD.


──────────────────────────────────────────────
2) OUTILS EN LIGNE DE COMMANDE — GUIDE COMPLET
──────────────────────────────────────────────
Vitte Light (VITL) propose un environnement de compilation et d’exécution 
via une CLI (Command Line Interface). Selon la distribution, tu peux avoir :

— Un binaire unique `vitl`
   → combine compilation, VM, outils (fmt, check, test, doc).
— Un couple `vitlc` (compilateur) et `vitlv` (machine virtuelle)
   → approche plus modulaire, utile si tu veux séparer build et exécution.

──────────────────────────────────────────────
2.1 Exécution immédiate (VM/JIT)
──────────────────────────────────────────────
Commande :
  vitl run src/main.vitl
ou :
  vitlv run src/main.vitl

Explications :
— Le code source est lu, compilé en bytecode, puis exécuté directement par la VM.
— Idéal pour tester rapidement un script sans passer par la compilation native.
— La VM vérifie aussi certains invariants (sécurité mémoire, erreurs de syntaxe).

Cas d’usage :
— Débutant : écrire un petit script et voir le résultat instantanément.
— Intermédiaire : prototypage rapide avant intégration.
— Pro : usage pour tests automatisés dans CI/CD (exécution sans build).

Erreur fréquente :
— Oublier le chemin du fichier → `vitl run` sans argument affiche l’aide.

──────────────────────────────────────────────
2.2 Compilation en exécutable natif
──────────────────────────────────────────────
Commande :
  vitl build -O2 -o build/app src/main.vitl
ou :
  vitlc -O2 -o build/app src/main.vitl

Explications :
— Génère un binaire exécutable natif (Linux, BSD, macOS, Windows selon cible).
— -O2 active des optimisations standards (taille/rapidité équilibrée).
— Le résultat se trouve dans `/build` par convention.

Avantages :
— Démarrage instantané (pas besoin de VM).
— Binaire autonome, facile à distribuer.
— Plus de performance pour des calculs lourds.

Cas d’usage :
— Intermédiaire : distribuer un outil CLI interne.
— Pro : créer un exécutable pour production, packagé avec CI/CD.

──────────────────────────────────────────────
2.3 Outils supplémentaires
──────────────────────────────────────────────
**Formateur automatique**  
  vitl fmt src/  
— Réorganise imports, corrige indentation, uniformise accolades.  
— Conseillé pour tous : garde un style homogène entre équipes.

**Analyse statique**  
  vitl check src/  
— Détecte variables inutilisées, branches mortes, conversions implicites.  
— Intermédiaire/Pro : indispensable avant commit ou merge.

**Tests unitaires**  
  vitl test  
— Exécute tous les blocs `test "nom" { ... }` définis dans le code.  
— Débutant : apprendre à valider ses fonctions.  
— Pro : automatiser la validation dans pipelines CI.

**Documentation automatique**  
  vitl doc src/ -o build/docs.txt  
— Extrait les commentaires `///` en documentation utilisateur.  
— Pro : permet de livrer une doc synchronisée avec le code.

──────────────────────────────────────────────
2.4 Options utiles
──────────────────────────────────────────────
— `-O0` : pas d’optimisation, build rapide pour debug.  
— `-O1` : optimisation légère.  
— `-O2` : compromis vitesse/taille (valeur par défaut).  
— `-O3` : optimisations agressives (inlining, unroll).  
— `-g`  : inclut symboles de debug, nécessaire pour `std.debug::backtrace`.  

Exemple debug :
  vitl build -O0 -g -o build/app src/main.vitl
  ./build/app

— `--emit-ir` : imprime l’IR (Intermediate Representation) lisible par humain.  
— `--emit-bytecode` : génère un fichier `.vitbc` (format binaire standardisé).  

Cas d’usage :
— Étudiant/chercheur : `--emit-ir` pour étudier la compilation.  
— Pro : `--emit-bytecode` pour intégrer VITBC dans un pipeline VM.

──────────────────────────────────────────────
2.5 Bonnes pratiques CLI
──────────────────────────────────────────────
Débutant :
— Utiliser `vitl run` pour tester rapidement.
— Ajouter `vitl fmt` pour s’habituer au style standard.

Intermédiaire :
— Compiler avec `vitl build -O2 -g` pour avoir à la fois optimisation et debug.
— Vérifier le code avec `vitl check` avant tout commit.

Pro :
— Intégrer `vitl test` et `vitl check` dans une pipeline CI/CD.
— Distribuer des exécutables compilés avec `-O3` pour prod.
— Générer docs avec `vitl doc` pour livrer aux utilisateurs.

──────────────────────────────────────────────
2.6 Erreurs fréquentes
──────────────────────────────────────────────
— E0001 : fichier introuvable → chemin incorrect dans la commande.
— Permission denied → oublier `chmod +x` sur un binaire généré.
— Mauvais ordre d’options → `-o build/app` doit suivre la commande `build`.

──────────────────────────────────────────────
Résumé
— `vitl run` : exécution directe dans la VM.  
— `vitl build` : compilation en exécutable natif.  
— `vitl fmt`, `vitl check`, `vitl test`, `vitl doc` : outillage quotidien.  
— Options -O et -g : choisir entre debug et performance.  
— Outils pensés pour couvrir du **prototype rapide** jusqu’au **binaire distribué**.



──────────────────────────────────────────────

3) SYNTAXE : DÉCOUVERTE PROGRESSIVE
────────────────────────────────────
But
— Apprendre les briques de base de Vitte Light (VITL).
— Comprendre la logique derrière chaque construction.
— Éviter les erreurs classiques de syntaxe ou de typage.

────────────────────────────────────
3.1 Commentaires
────────────────
Types de commentaires :
— Ligne unique : commence par `//`, ignore tout jusqu’à la fin de la ligne.
— Bloc : délimité par `/* … */`, peut couvrir plusieurs lignes.

Exemple :
  // ceci est un commentaire simple
  let x = 42  // vous pouvez aussi commenter après une instruction

  /*
     commentaire multi-lignes
     utile pour désactiver temporairement un bloc de code
  */

Bonne pratique
— Utiliser `//` pour notes rapides.
— Utiliser `/* … */` pour documenter un algorithme ou désactiver du code.

────────────────────────────────────
3.2 Variables et constantes
───────────────────────────
Déclaration :
— `const` : valeur immuable et connue à la compilation.
— `let`   : variable liée à une valeur. Par défaut immuable.
— `let mut` : variable mutable.

Exemples :
  const PI: f64 = 3.14159     // constante avec type explicite
  let mut compteur: i32 = 0   // variable modifiable
  let message = "Salut"       // type inféré automatiquement = str

Inférence de type
— Le compilateur déduit le type si ce n’est pas ambigu.
— Pour une API publique, toujours annoter le type explicitement.

Erreur fréquente
— Oublier `mut` :
    let n = 0
    n = n + 1    // erreur → variable non mutable
  Correct :
    let mut n = 0
    n = n + 1

────────────────────────────────────
3.3 Fonctions
────────────────
Syntaxe :
  fn nom(param: Type, ...) -> TypeRetour {
    // corps
    return valeur
  }

Exemple :
  fn add(a: i32, b: i32) -> i32 {
    return a + b
  }

Notes
— Le mot-clé `fn` introduit une fonction.
— Paramètres typés explicitement.
— Le `return` indique la valeur renvoyée.
— Si la fonction ne renvoie rien : `-> ()`.

Variante courte
  fn square(x: i32) -> i32 { return x*x }

Erreur fréquente
— Oublier `-> TypeRetour` :
    fn bad(x: i32) { return x*x }   // erreur : manque le type

────────────────────────────────────
3.4 Structures de contrôle
──────────────────────────
Conditionnelle `if`
  if x > 0 {
    println("positif")
  } else {
    println("non positif")
  }

Boucle `for`
— Itère sur un intervalle ou une collection.
— Intervalle exclusif `0..10` → 0 à 9.
— Intervalle inclusif `0..=10` → 0 à 10.

Exemple :
  for i in 0..10 {
    println(i.to_string())
  }

Boucle `while`
  let mut n = 0
  while n < 5 {
    println(n.to_string())
    n = n + 1
  }

Erreur fréquente
— Boucle infinie non voulue (oublier d’incrémenter un compteur).

────────────────────────────────────
3.5 Match (pattern matching puissant)
────────────────────────────────────
`match` compare une valeur à différents motifs.

Exemple simple :
  match valeur {
    0 => println("zéro"),
    1 | 2 => println("petit"),   // alternative avec |
    _ => println("autre")        // _ = cas par défaut
  }

Exemple avec enum :
  enum Status { Ok, Err(str) }

  let s = Status::Err("oops")

  match s {
    Status::Ok => println("ok"),
    Status::Err(msg) => println("erreur: " + msg),
  }

Avantages
— Plus expressif que des if/else imbriqués.
— Forçage à couvrir tous les cas (sauf si `_` utilisé).

Erreur fréquente
— Oublier un cas dans un `match` exhaustif :
    enum Color { Red, Green, Blue }
    match c {
      Color::Red => println("rouge"),
      Color::Green => println("vert")
    }
  → erreur : cas `Blue` manquant

────────────────────────────────────
3.6 Types de base et littéraux
──────────────────────────────
Entiers :
— Décimaux : 0, 42, 1_000
— Hexa : 0xFF
— Octal : 0o755
— Binaire : 0b1010

Flottants :
— 3.14, 2.0e-3

Booléens :
— true, false

Caractères :
— 'a', '\n'

Chaînes :
— "texte normal"
— r"brut sans échappement"

────────────────────────────────────
3.7 Bonnes pratiques de style
─────────────────────────────
Débutant
— Déclarer vos variables avec `let` et types simples.
— Utiliser `println` pour vérifier les valeurs.

Intermédiaire
— Isoler les tests dans des fonctions et utiliser `assert`.
— Toujours traiter tous les cas d’un `match`.

Pro
— Factoriser vos contrôles avec `match` au lieu d’empiler des `if`.
— Éviter les conversions implicites ; soyez explicite avec `as`.

────────────────────────────────────
Résumé
— Commentaires : `//` ou `/* … */`.
— Variables : immuables par défaut, `mut` si nécessaire.
— Fonctions : `fn nom(param: Type) -> Type`.
— Contrôle : `if`, `while`, `for`.
— Pattern matching : `match`, exhaustif et clair.


──────────────────────────────────────────────

4) PREMIERS EXEMPLES — DÉTAILLÉS
────────────────────────────────
Objectif
— T’aider à écrire, exécuter, compiler et tester trois programmes “types”.
— Montrer les erreurs fréquentes et leurs corrections.
— Donner des variantes utiles pour aller un cran plus loin.

Pré-requis
— Arbo conseillée :
    /src
      main.vitl        (exemple débutant)
      cat.vitl         (exemple intermédiaire)
      geom.vitl        (exemple pro)
— Exécution VM :
    vitl run src/main.vitl
— Compilation native :
    vitl build -O2 -o build/app src/main.vitl


4.1 Débutant → programme minimal
────────────────────────────────
But
— Afficher du texte. Vérifier que la toolchain fonctionne.

Code (src/main.vitl)
  module app.main
  import std.io

  fn main() -> i32 {
    std.io::println("Bonjour Vitte Light")
    return 0
  }

Explications
— `module app.main` : identifie le module du fichier.
— `import std.io` : active les fonctions d’E/S console.
— `main() -> i32` : point d’entrée, retourne un code de sortie.
— `println(...)` : écrit sur stdout puis ajoute un saut de ligne.

Exécution (VM)
  vitl run src/main.vitl

Compilation (binaire)
  vitl build -O2 -o build/hello src/main.vitl
  ./build/hello

Sortie attendue
  Bonjour Vitte Light

Erreurs fréquentes
— E0001 symbole inconnu : oublier `import std.io`.
— E0002 types incompatibles : concaténer nombre + string sans `.to_string()`.

Variante A — afficher une variable
  fn main() -> i32 {
    let n = 42
    std.io::println("n=" + n.to_string())
    return 0
  }

Variante B — codes d’erreur
  fn main() -> i32 {
    if 2 + 2 != 4 {
      std.io::eprintln("arithmétique cassée")
      return 1  // code d’erreur générique
    }
    return 0
  }


4.2 Intermédiaire → lire un fichier
───────────────────────────────────
But
— Lire un fichier texte passé en argument. Gérer les erreurs proprement.

Code (src/cat.vitl)
  module app.cat
  import std.{fs, io, cli, str}

  fn main() -> i32 {
    let argv = cli::args()
    if str::len(argv) < 2 {
      io::eprintln("usage: cat <fichier>")
      return 2  // convention: 2 = erreur d’usage
    }
    let path = argv[1]
    if !fs::exists(path) {
      io::eprintln("introuvable: " + path)
      return 3  // convention: 3 = I/O
    }
    let contenu = fs::read_to_string(path)?   // propage l’erreur I/O
    io::print(contenu)
    return 0
  }

Explications
— `cli::args()` : récupère les arguments CLI sous forme de [String].
— `str::len(argv)` : taille. On exige au moins 2 éléments (binaire + fichier).
— `fs::exists(path)` : évite une erreur inutile avant lecture.
— `read_to_string(path)?` : renvoie Result<String, Error>. `?` propage l’erreur au runtime sous forme d’un code ≠ 0.
— `return 2/3` : codes de sortie standardisés.

Exécution
  vitl run src/cat.vitl -- data.txt

Sorties possibles
— OK : affiche le contenu du fichier.
— Erreur d’usage : `usage: cat <fichier>` avec code 2.
— Fichier manquant : `introuvable: ...` avec code 3.

Erreurs fréquentes
— E2001 fichier non trouvé : oublier le test `exists`.
— E0002 concat de String + i32 sans `.to_string()`.

Variante A — lecture binaire
  // Pour fichiers non-UTF8
  let bytes = fs::read(path)?        // [u8]
  io::println("taille=" + bytes.len().to_string())

Variante B — chronométrer l’opération
  let t0 = std.time::now()
  let contenu = fs::read_to_string(path)?
  io::println("ms=" + (std.time::now() - t0).to_string())

Variante C — “cat” multi-fichiers avec boucle
  for i in 1..str::len(argv) {
    let p = argv[i]
    if !fs::exists(p) { io::eprintln("skip: " + p); continue }
    io::print(fs::read_to_string(p)?)
  }


4.3 Pro → struct, impl, méthodes
────────────────────────────────
But
— Définir un type, ajouter des méthodes, calculer une norme, tester.

Code (src/geom.vitl)
  module app.geom
  import std.{io, math}

  struct Vec2 { x: f64, y: f64 }

  impl Vec2 {
    fn norm(&self) -> f64 {
      return (self.x*self.x + self.y*self.y).sqrt()
    }
    fn dot(&self, o: Vec2) -> f64 {
      return self.x*o.x + self.y*o.y
    }
  }

  fn main() -> i32 {
    let v = Vec2 { x: 3.0, y: 4.0 }
    io::println(v.norm().to_string())  // 5
    return 0
  }

Explications
— `struct Vec2 { x, y }` : type produit par l’utilisateur.
— `impl Vec2 { … }` : bloc de méthodes associé à Vec2.
— `&self` : emprunt en lecture, pas de copie complète.
— `sqrt()` : méthode flt; import indirect via std.math.

Erreurs fréquentes
— Typo : écrire `self.xself.x` au lieu de `self.x*self.x`.
— Oublier `import std.math` si l’impl demande des fonctions flottantes.

Tests intégrés
  test "norm classique 3-4-5" {
    let v = Vec2 { x:3.0, y:4.0 }
    assert(v.norm() == 5.0)
  }
  test "dot produit orthogonal" {
    let a = Vec2 { x:1.0, y:0.0 }
    let b = Vec2 { x:0.0, y:1.0 }
    assert(a.dot(b) == 0.0)
  }

Variante A — parsing CLI vers f64
  import std.{cli, str}
  fn parse_f64(s: str) -> Result<f64, str> { return str::to_float(s) }
  fn main() -> i32 {
    let a = cli::args()
    if str::len(a) != 3 { io::eprintln("usage: geom <x> <y>"); return 2 }
    let x = parse_f64(a[1])?    // ? propage une Err("…")
    let y = parse_f64(a[2])?
    let v = Vec2 { x:x, y:y }
    io::println(v.norm().to_string())
    return 0
  }

Variante B — mini-bench
  fn bench(n: i32) -> i32 {
    let mut acc:f64 = 0.0
    let base = Vec2 { x:1.234, y:5.678 }
    for i in 0..n {
      let v = Vec2 { x: base.x + (i as f64), y: base.y - (i as f64) }
      acc = acc + v.norm()
    }
    // utiliser acc pour éviter l’élimination par l’optimiseur
    std.io::println("acc=" + acc.to_string())
    return 0
  }

  fn main() -> i32 {
    let t0 = std.time::now()
    _ = bench(100000)
    std.io::println("ms=" + (std.time::now()-t0).to_string())
    return 0
  }


4.4 Bonnes pratiques transverses
────────────────────────────────
Style
— `module` en tête, imports regroupés.
— snake_case pour fonctions/variables; CamelCase pour types.
— Types explicites pour API publiques.

Erreurs et robustesse
— `?` pour propager les erreurs I/O et parsing.
— Codes de sortie standard : 0 OK, 1 panic, 2 usage, 3 I/O, 4 FFI.

Organisation
— Un fichier = un module; découper tôt si le fichier grossit.
— Tests en fin de fichier, liés aux fonctions du module.

Perf
— Utiliser `std.time::now()` pour mesurer.
— Éviter les copies inutiles grâce aux slices `[T]`.
— Pré-allouer avec `std.vec::with_capacity` si la taille est connue.

Sécurité
— Pas d’`unsafe` dans ces exemples; réserver `unsafe` au FFI.
— Toujours convertir les chaînes en `CString` avant d’appeler du C.

Raccourcis utiles
— Formatage auto : `vitl fmt src/`
— Lint : `vitl check src/`
— Tests : `vitl test`
— IR lisible : `vitl build --emit-ir -o /dev/null src/geom.vitl`


──────────────────────────────────────────────

5) MÉMOIRE ET SÉCURITÉ — GUIDE DÉTAILLÉ
────────────────────────────────────────
Objectif
— Expliquer comment la mémoire est gérée dans Vitte Light (VITL).
— Montrer les avantages du modèle choisi et les limites.
— Donner des stratégies adaptées à chaque niveau d’expérience.

────────────────────────────────────────
5.1 Modèle mémoire en mode sûr
──────────────────────────────
— VITL ne possède pas de garbage collector complexe.
— La gestion est assurée par **références comptées** (Rc<T>).
— Quand le compteur de références tombe à zéro, l’objet est libéré immédiatement.
— Pas de pause GC, pas de collecte asynchrone → comportement prévisible.

Exemple :
  let a = Rc<int>::new(42)
  let b = a.clone()        // compteur fort = 2
  a.drop()                 // compteur fort = 1
  b.drop()                 // compteur fort = 0 → libération immédiate

Avantages
— Simplicité : pas de `malloc`/`free` manuel pour l’utilisateur.
— Déterministe : pas de latence due à un GC qui s’exécute en arrière-plan.
— Sécurité : la mémoire est libérée dès qu’elle n’est plus accessible.

Limites
— Les cycles de références (A→B→A) ne sont pas libérés automatiquement.
— À éviter en structurant vos données avec Rc/Weak.

────────────────────────────────────────
5.2 Immutabilité et mutabilité
──────────────────────────────
— `str` est **immuable** (chaîne littérale ou constante).
— Pour une chaîne modifiable, utiliser `String` :
    let mut s = String::from("abc")
    s.push("d")  // devient "abcd"

— Règle générale :
  - Immuable par défaut.
  - Ajouter `mut` uniquement quand nécessaire.

Avantages
— Facilite le raisonnement : une valeur immuable ne change pas dans une autre partie du code.
— Réduit les bugs liés aux effets de bord.

────────────────────────────────────────
5.3 Collections et slices
────────────────────────
— Les **slices** `[T]` sont des vues sur un tableau ou un vecteur :
  Elles permettent de parcourir ou de lire sans recopier les données.

Exemple :
  let v:[i32] = [1,2,3,4]
  for x in &v { io::println(x.to_string()) }

— Les vecteurs dynamiques (`std.vec`) allouent sur le tas et grandissent au besoin.
— Toujours utiliser `vec::with_capacity` si vous connaissez la taille prévue.

────────────────────────────────────────
5.4 Mode unsafe et pointeurs bruts
──────────────────────────────────
— Le mot-clé `unsafe` permet d’utiliser des pointeurs C (*T, *mut T).
— À réserver aux appels FFI et aux cas bas niveau où Rc ne suffit pas.
— Dans un bloc unsafe, vous assumez :
  - Validité du pointeur.
  - Durée de vie correcte de l’objet.
  - Alignement mémoire conforme.

Exemple minimal FFI :
  extern "C" { fn puts(msg:*const char)->i32 }
  let cstr = std.c::CString::from_str("hi\n")?
  unsafe { puts(cstr.as_ptr()) }

Bonne pratique
— Encapsuler les appels `unsafe` dans une fonction VITL sûre, pour isoler le risque.

────────────────────────────────────────
5.5 Débutant, intermédiaire, pro
──────────────────────────────────
Débutant
— Pas besoin de `free()`, Rc s’occupe de tout.
— Utilisez `String` si vous devez modifier une chaîne.
— Évitez tout `unsafe`.

Intermédiaire
— Apprenez à utiliser Rc/Weak pour éviter les cycles.
— Utilisez les slices `[T]` pour passer des vues sur des données sans recopier.
— Familiarisez-vous avec la différence entre `str` et `String`.

Pro
— Combinez Rc avec des pointeurs faibles (Weak) pour des graphes complexes.
— Exploitez le FFI en encapsulant soigneusement le code `unsafe`.
— Si besoin, utilisez des allocateurs personnalisés côté C et liez-les via FFI.

────────────────────────────────────────
5.6 Erreurs courantes à éviter
──────────────────────────────
— Créer des cycles de références Rc → fuite mémoire silencieuse.
— Passer une String VITL temporaire directement au C → pointeur dangling.
— Oublier `mut` sur une variable qui doit être modifiée → erreur de compilation.
— Utiliser `unsafe` sans encapsulation → propagation de bugs difficiles à tracer.

────────────────────────────────────────
5.7 Récapitulatif
─────────────────
✓ Gestion sûre et déterministe via Rc/Weak.  
✓ Pas de GC, pas de `malloc`/`free` manuel côté VITL.  
✓ str immuable, String mutable.  
✓ Slices pour éviter les copies inutiles.  
✓ unsafe réservé aux experts pour FFI/pointeurs.  


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

7) INTEROPÉRABILITÉ C (FFI) — GUIDE AVANCÉ
───────────────────────────────────────────
But
— Appeler du code C depuis Vitte Light (VITL) en sécurité.
— Définir un « shim C » propre et portable.
— Gérer chaînes, buffers, erreurs, et l’édition de liens.

Principes
— ABI supportée : C uniquement.
— Toute interaction mémoire non vérifiée est `unsafe`.
— Réduire la surface `unsafe` au strict minimum.
— Toujours documenter qui alloue et qui libère.

7.1 Déclaration et appel minimal
────────────────────────────────
Déclarer une fonction C exposée :
  extern "C" { fn puts(msg: *const char) -> i32 }

Construire une CString et appeler :
  let cstr = std.c::CString::from_str("Hello C!\n")?
  unsafe { _ = puts(cstr.as_ptr()) }

Lien à la lib C standard (exemple) :
  vitl build -O2 -L libs -lc -o build/app src/main.vitl

Notes
— `CString::from_str` échoue si le texte contient un octet nul `\0`.
— `puts` attend une chaîne C null-terminée. Ne jamais passer `str` brut.

7.2 Chaînes et ownership
────────────────────────
Entrée VITL → C :
— Convertir avec `CString::from_str`.
— Conserver la CString vivante pendant tout l’appel C (durée de vie).

Retour C → VITL :
— Si la lib C **renvoie** `const char*` pointant sur un buffer interne,
  copier immédiatement côté VITL et **ne pas** libérer côté VITL.
— Si la lib C **alloue** et te transfère la propriété (ex: `char*` via malloc),
  convertir en String puis libérer via `c::free(ptr)` si c’est ton contrat.

Pattern de contrat (recommandé) côté C :
— Fournir des paires `create/destroy`, `dup/free`, et préciser l’allocateur.

7.3 Buffers binaires et slices
──────────────────────────────
Passage d’un slice VITL `[u8]` vers C :
— Passer `ptr` et `len` séparément :
    extern "C" { fn sha256(data:*const u8, len:usize, out:*mut u8) -> i32 }
— Créer un buffer de sortie côté VITL et passer `as_mut_ptr()` :
    let mut out:[u8] = [0; 32]
    unsafe { _ = sha256(input.as_ptr(), input.len(), out.as_mut_ptr()) }

Règles
— Toujours passer `len`. Ne jamais supposer qu’un buffer est null-terminé.
— Vérifier les bornes côté VITL avant l’appel. Ne jamais dépasser `out` cap.

7.4 Structs : opacité plutôt que couplage
───────────────────────────────────────────
Éviter d’échanger des `struct` C inline (alignement/ABI cross-platform).
Préférer un **handle opaque** :

C (shim) :
  typedef struct Obj Obj;
  Obj* obj_create(int cap);
  void obj_destroy(Obj*);
  int  obj_push(Obj*, const char* s);   // 0 ok, <0 err
  int  obj_len(const Obj*);

VITL :
  extern "C" {
    fn obj_create(cap:i32)->*mut void
    fn obj_destroy(p:*mut void)->void
    fn obj_push(p:*mut void, s:*const char)->i32
    fn obj_len(p:*const void)->i32
  }
  // Convertir str→CString, wrapper en API VITL sûre.
  // Envelopper les appels dans `unsafe { … }`.

7.5 Erreurs et mapping Result
─────────────────────────────
Convention côté C :
— Retour `0` = succès. `<0` = code d’erreur. `>0` = taille/compte utile.

Mapping côté VITL :
  fn c_call_ok(code:i32) -> Result<(),str> {
    if code == 0 { return Result::Ok(()) }
    return Result::Err("ffi: call failed")
  }

Exemple robuste :
  let s = std.c::CString::from_str("hi")?
  let r = unsafe { obj_push(h, s.as_ptr()) }
  _ = c_call_ok(r)?   // propage l’erreur proprement

Astuce pro
— Centraliser les conversions (code C → err::Error) dans un module `ffi.err`.

7.6 Callbacks et user_data
──────────────────────────
Modèle classique C :
  typedef void (*cb_t)(const char* msg, void* user);
  void run_with_cb(cb_t cb, void* user);

VITL :
  // On ne déclare pas directement des closures VITL vers C.
  // Pattern : exposer côté C une table de trampolines ou un registre.
  // C conserve `user` (opaque) et rappelle via cb_t.
  // VITL fournit un handle `user` obtenu d’un shim C (ex: table indexée).

Recommandation
— Laisser la gestion des callbacks dans le shim C.
— VITL envoie un identifiant opaque (`void*`/index) et reçoit des événements
  retransformés en appels VITL par polling ou par API pull.

7.7 Linking : -L, -l, rpath, static
────────────────────────────────────
Dossiers :
— `-L <dir>` : où chercher les libs.
— `-lfoo`    : lie `libfoo.so` ou `libfoo.a`.

Exemples
— Dylib partagée :
    vitl build -O2 -L libs -lfoo -o build/app src/main.vitl
— Statique (si dispo) :
    vitl build -O2 -L libs -Wl,-Bstatic -lfoo -Wl,-Bdynamic -o build/app src/main.vitl

Chargement à l’exécution
— OpenBSD/Unix : variable d’env `LD_LIBRARY_PATH` ou rpath :
    vitl build ... -Wl,-rpath,/chemin/vers/libs
— Windows : `.dll` dans le même dossier que l’exe ou dans le `PATH`.

Bonnes pratiques
— Versionner `libs/` dans le repo pour reproductibilité (si licences OK).
— Épingler des versions de libs et documenter le hash/commit.

7.8 Cross-plateforme : tailles et ABI
─────────────────────────────────────
— Utiliser des types C stables (`int32_t`, `uint64_t`, `size_t`) côté C.
— Rester cohérent côté VITL (`i32`, `u64`, `usize`).
— Éviter les `long`/`unsigned long` (taille change entre plateformes).
— Attention à l’alignement et à `#pragma pack` → déconseillé.

7.9 Sécurité mémoire : règles d’or
──────────────────────────────────
— Aucune écriture hors borne dans des buffers passés à C.
— Jamais de double free : un seul propriétaire libère.
— Chaînes : toujours null-terminées côté C.
— Durée de vie : ne pas garder de pointeur C vers une String VITL temporaire.
— Sur échec FFI, remettre l’objet VITL dans un état cohérent (ou le détruire).

Checklist rapide
[ ] Qui alloue ? Qui libère ? Écrit dans la doc.  
[ ] Les tailles sont passées séparément et validées.  
[ ] Tous les appels `unsafe` sont encapsulés dans des fonctions sûres.  
[ ] Codes d’erreur convertis en `Result` avec message contextuel.  
[ ] Tests d’intégration VITL↔C (succès et erreurs).

7.10 Exemple complet : wrapper « safe » autour d’une lib C
──────────────────────────────────────────────────────────
C (shim, libfoo) :
  typedef struct Foo Foo;
  Foo* foo_create(int cap);
  void foo_destroy(Foo*);
  int  foo_push(Foo*, const char* s);   // 0 ok, -1 err
  int  foo_len(const Foo*);

VITL (API sûre) :
  extern "C" {
    fn foo_create(cap:i32)->*mut void
    fn foo_destroy(h:*mut void)->void
    fn foo_push(h:*mut void, s:*const char)->i32
    fn foo_len(h:*const void)->i32
  }

  struct Foo { handle:*mut void }

  fn Foo::new(cap:i32) -> Result<Foo,str> {
    let h = unsafe { foo_create(cap) }
    if h == std.ptr::null_mut() { return Result::Err("ffi: create failed") }
    return Result::Ok(Foo{ handle:h })
  }

  fn Foo::push(&mut self, s:str) -> Result<(),str> {
    let cstr = std.c::CString::from_str(s)?
    let rc = unsafe { foo_push(self.handle, cstr.as_ptr()) }
    if rc != 0 { return Result::Err("ffi: push failed") }
    return Result::Ok(())
  }

  fn Foo::len(&self) -> i32 {
    return unsafe { foo_len(self.handle) }
  }

  fn Foo::close(&mut self) -> () {
    if self.handle != std.ptr::null_mut() {
      unsafe { foo_destroy(self.handle) }
      self.handle = std.ptr::null_mut()
    }
  }

  // Usage :
  // let mut f = Foo::new(128)?
  // _ = f.push("abc")?
  // io::println(f.len().to_string())
  // f.close()

7.11 Stratégie d’équipe et CI
─────────────────────────────
— Figer l’interface C (headers) et ajouter des tests d’intégration.
— Sur chaque MR/PR, builder le shim C et l’exécutable VITL dans CI.
— Publier les artefacts : lib partagée + header + empreinte (sha256).
— Documenter rpath/LD_LIBRARY_PATH et répertoires `libs/`.

7.12 Quand FFI n’est pas le bon outil
─────────────────────────────────────
— Appels ultra-fréquents très fins → regroupes-les (batch) côté C.
— Sérialisation lourde objet par objet → définis un format binaire compact.
— Besoin de threads natifs → faire tourner un runtime externe via shim.

Résumé
— L’FFI C de VITL est simple et sûr si le contrat est clair.
— Opaque handles, CString, ptr+len, Result, surface `unsafe` minimale.
— Documenter allocation, libération, et versions de libs.


──────────────────────────────────────────────

8) STDLIB — APERÇU DÉTAILLÉ
────────────────────────────
But
— Donner une vue opérationnelle des modules standard essentiels.
— Exposer signatures usuelles, comportements, erreurs courantes.
— Conseils selon profils: débutant, intermédiaire, pro.

Convention
— Toutes les fonctions qui touchent au système retournent Result<…>.
— L’opérateur `?` propage l’erreur vers l’appelant.
— Les extraits supposent: `import std.{io, fs, str, math, time, cli, vec, debug, err, c}`.


8.1 std.io — E/S console
────────────────────────
API courante
— print(x: str|String|affichable)       -> ()
— println(x: …)                         -> ()
— eprint(x: …)  (stderr)                -> ()
— eprintln(x: …) (stderr)               -> ()

Exemples
  io::println("Hello")
  io::eprintln("fatal: config manquante")

Bonnes pratiques
— Préférer `println` pour tracer les étapes clés.
— Pour concat, convertir: `"n=" + n.to_string()`.

Pièges
— Trop de logs = bruit. En prod, centraliser les messages d’erreur.


8.2 std.fs — Fichiers et chemins
────────────────────────────────
API courante
— read_to_string(path: str)             -> Result<String, err::Error>
— write_string(path: str, data: str)    -> Result<(), err::Error>
— read(path: str)                       -> Result<[u8], err::Error>
— write(path: str, bytes: [u8])         -> Result<(), err::Error>
— exists(path: str)                     -> bool

Exemples
  let cfg = fs::read_to_string("config.toml")?
  _ = fs::write_string("out.txt", "ok\n")?

  if !fs::exists("data.bin") {
    io::eprintln("absent: data.bin"); return 3
  }

Débutant
— Utiliser `read_to_string` pour du texte uniquement.
Intermédiaire
— Pour binaire, préférer `read`/`write` en bytes.
Pro
— Standardiser des chemins absolus pour la prod; vérifier droits.

Pièges
— Chemins relatifs dépendants du répertoire courant.
— Fichiers non UTF-8 → utiliser `read` puis décoder.


8.3 std.str — Chaînes et utilitaires
────────────────────────────────────
API courante
— len(s: str|String)                    -> i32
— split(s: str, sep: char)              -> [String]
— find(s: str, needle: str)             -> i32    // -1 si absent
— replace(s: str, from: str, to: str)   -> String
— to_int(s: str)                        -> Result<i64, str>
— to_float(s: str)                      -> Result<f64, str>

Exemples
  let n = str::len("abc")                 // 3
  let parts = str::split("a b c", ' ')
  let idx = str::find("abc", "b")         // 1
  let out = str::replace("a_b", "_", "-") // "a-b"
  let v = str::to_int("42")?              // i64

Bonnes pratiques
— Vérifier les Result de parse avec `?` et un message d’erreur clair.
— Normaliser les entrées (trim, lower/upper) avant comparaison.

Pièges
— Concat de non-strings sans `.to_string()` → E0002.


8.4 std.math — Maths usuelles
──────────────────────────────
API courante
— abs(x: num)                            -> num
— sqrt(x: f64)                           -> f64
— sin/cos(x: f64)                        -> f64
— pow(x: f64, y: f64)                    -> f64

Exemples
  let d = math::sqrt(3.0*3.0 + 4.0*4.0)  // 5.0
  let a = math::abs(-12)                 // 12

Bonnes pratiques
— Rester en `f64` pour stabilité. Convertir les entiers en amont.

Pièges
— Racine d’un négatif → NaN. Vérifier les bornes avant `sqrt`.


8.5 std.time — Horloge et temporisation
────────────────────────────────────────
API courante
— now()                                  -> i64   // ms depuis epoch
— sleep_ms(ms: i32)                      -> ()

Exemples
  let t0 = time::now()
  heavy()
  io::println((time::now() - t0).to_string() + " ms")

Bonnes pratiques
— Mesurer les sections critiques pour guider l’optimisation.
— Pour temporiser une boucle, utiliser `sleep_ms` plutôt qu’un spin.

Pièges
— Supposer un ordre total strict entre timestamps sur machines variées.
— Dépendances temporelles dans les tests → stabiliser via fixtures.


8.6 std.cli — Arguments et environnement
────────────────────────────────────────
API courante
— args()                                 -> [String]
— env(key: str)                          -> Option<String>
— exit(code: i32)                        -> !

Exemples
  let argv = cli::args()
  if str::len(argv) < 2 { io::eprintln("usage: app <file>"); return 2 }
  match cli::env("HOME") {
    Some(h) => io::println(h),
    None => io::eprintln("HOME non défini")
  }

Bonnes pratiques
— Valider tôt les options; retourner 2 en cas d’usage invalide.

Pièges
— Utiliser `exit` dans des libs. Réserver `exit` au binaire `app.main`.


8.7 std.vec — Vecteurs dynamiques
──────────────────────────────────
API courante
— with_capacity(n: i32)                  -> [T]
— push(v: &mut [T], x: T)                -> ()
— pop(v: &mut [T])                       -> Option<T>
— len(v: [T])                            -> i32
— get(v: [T], i: i32)                    -> Option<&T>

Exemples
  let mut v:[i32] = vec::with_capacity(16)
  v.push(1); v.push(2)
  for x in &v { io::println(x.to_string()) }
  match v.pop() { Some(x) => io::println("last="+x.to_string()), None => {} }

Bonnes pratiques
— Accéder via `get(i)` si l’index peut dépasser; sinon tester `i < vec::len(v)`.

Pièges
— Hypothèses implicites sur la capacité → réserver avec `with_capacity` si connu.


8.8 std.debug — Assertions et backtrace
───────────────────────────────────────
API courante
— assert(cond: bool)                      -> ()
— dump(x: affichable)                     -> ()   // aide au debug
— backtrace()                             -> ()   // si -g et supporté

Exemples
  debug::assert(sum([1,2,3]) == 6)
  debug::dump("state=INIT")

Bonnes pratiques
— Assertions pour invariants internes; messages clairs à l’échec.
— Activer `-g` en debug pour des traces exploitables.

Pièges
— Laisser des `dump` verbeux en prod. Garder les traces essentielles.


8.9 std.err — Erreurs canoniques
─────────────────────────────────
Concept
— `err::Error` rassemble des informations d’échec (catégorie, message, contexte).
— La stdlib retourne souvent `Result<T, err::Error>`.

Motifs communs
— I/O: chemins, droits, formats.
— FFI: codes retour négatifs, pointeurs invalides.

Bonnes pratiques
— Enrichir l’erreur avec contexte applicatif avant de la remonter.
— Normaliser les codes de sortie (0 OK, 1 panic, 2 usage, 3 I/O, 4 FFI).

Pièges
— Déguiser un panic en `Err` ou inversement. Séparer fatal/récupérable.


8.10 std.c — Pont C et mémoire bas niveau
──────────────────────────────────────────
API courante
— CString::from_str(s: str)              -> Result<CString, str>
— malloc(size: usize)                    -> *mut u8        // unsafe
— free(ptr: *mut u8)                     -> ()             // unsafe

Interop
— Déclarer:
    extern "C" { fn puts(msg:*const char) -> i32 }
— Appeler:
    let cmsg = c::CString::from_str("Hello\n")?
    unsafe { _ = puts(cmsg.as_ptr()) }

Bonnes pratiques
— Réduire la surface `unsafe` au strict minimum.
— Documenter qui alloue/libère chaque buffer.
— Pour strings, toujours passer par `CString::from_str`.

Pièges
— Passer un `str` non null-terminé côté C.
— Oublier `free` sur mémoire allouée côté C quand c’est votre responsabilité.


8.11 Patterns composés — recettes rapides
──────────────────────────────────────────
Lecture fichier + parse int avec erreurs explicites
  fn load_threshold(path: str) -> Result<i64, err::Error> {
      let s = fs::read_to_string(path)?
      match str::to_int(str::replace(s, "\n", "")) {
          Result::Ok(v) => return Result::Ok(v),
          Result::Err(_) => return Result::Err(err::Error::new("parse int échoué")),
      }
  }

Timer simple autour d’une fonction
  fn timed<F>(label: str, f: F) -> ()
  where F: fn()->()
  {
      let t0 = time::now()
      f()
      io::println(label + ": " + (time::now()-t0).to_string() + " ms")
  }

CLI robuste (usage + codes de sortie)
  fn main() -> i32 {
      let a = cli::args()
      if str::len(a) < 2 {
          io::eprintln("usage: app <input>")
          return 2
      }
      if !fs::exists(a[1]) {
          io::eprintln("E2001: introuvable → " + a[1])
          return 3
      }
      // …
      return 0
  }


8.12 Conseils par niveau
────────────────────────
Débutant
— Commencer avec io/fs/str/cli uniquement.
— Toujours vérifier `exists` avant `read_to_string`.

Intermédiaire
— Structurer les erreurs avec `Result` et messages utiles.
— Mesurer avec `time::now()` pour cibler l’optimisation.

Pro
— Envelopper les APIs C via `std.c` avec un shim C stable.
— Centraliser la gestion d’erreurs `err::Error` + mapping vers codes de sortie.
— Garder un niveau de logs cohérent et mesuré.


──────────────────────────────────────────────
9) STYLE ET BONNES PRATIQUES — GUIDE DÉTAILLÉ
─────────────────────────────────────────────

But
— Donner une cohérence à tous les projets Vitte Light.
— Aider les débutants à écrire du code lisible dès le départ.
— Permettre aux intermédiaires de maintenir des bases de code claires.
— Donner aux pros un style stable, compatible avec l’outillage (fmt, lint).

─────────────────────────────────────────────
9.1 Structure générale des fichiers
───────────────────────────────────
1) Toujours commencer par le `module` :
   — C’est la “carte d’identité” du fichier.
   — Exemple : fichier `src/math/linear.vitl` → `module app.math.linear`

2) Grouper immédiatement les `import` :
   — Tous les imports en bloc, sans code entre eux.
   — Les regrouper par hiérarchie.
   — Exemple :
     ```
     module app.main
     import std.{io, fs}
     import app.math.linear
     ```

3) Code du module ensuite :
   — Constantes
   — Types (struct, enum)
   — Implémentations
   — Fonctions
   — Tests

─────────────────────────────────────────────
9.2 Nommage (variables, fonctions, types)
─────────────────────────────────────────────
Convention :
— snake_case → variables, fonctions locales, noms de fichiers.
— CamelCase → structs, enums, modules publics.

Exemples :
  let user_name = "Alice"
  fn compute_area(w:i32, h:i32) -> i32 { return w*h }

  struct Rectangle { width:i32, height:i32 }
  enum Status { Ok, Failed }

Pourquoi ?
— snake_case : lisible et courant dans la majorité des langages modernes.
— CamelCase : met en évidence les types et les concepts de haut niveau.

─────────────────────────────────────────────
9.3 Constantes et immutabilité
─────────────────────────────────────────────
— Toujours utiliser `const` pour les valeurs fixes :



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

11) LIMITES (VERSION LIGHT) — VERSION DÉTAILLÉE
───────────────────────────────────────────────
But
— Clarifier ce qui n’existe pas encore dans Vitte Light (VITL), pourquoi, et comment contourner proprement.
— Donner des recettes adaptées aux débutants, intermédiaires, pros.

Résumé court
— Pas de threads natifs.
— Pas de GC complet (seulement Rc/Weak).
— Génériques partiels uniquement.
— FFI limité au C (ABI C).

───────────────────────────────────────────────
11.1 Pas de threads natifs
──────────────────────────
Ce que cela signifie
— Aucune API VITL pour créer des threads. Pas de synchronisation (mutex, condvar) dans la stdlib VITL.

Conséquences
— Pas de parallélisme CPU intra-processus directement en VITL.
— Les tâches concurrentes doivent être séquencées ou externalisées.

Stratégies de contournement
Débutant
— Sérialiser le travail : découper en étapes, mesurer, optimiser l’algorithme.
— Utiliser des fichiers temporaires ou des pipes simples pour enchaîner des outils externes (exécutés via le shell/OS).

Intermédiaire
— **Multi-processus** via FFI C (en écrivant un petit shim C qui fork/exec) et communication par *stdin/stdout* (pipes) ou fichiers.
— **Pipelines** : lancer plusieurs instances du binaire VITL et répartir les lots de données.

Pro
— Encapsuler un runtime de threads externe (C, Rust, Go… exposé en C ABI) et appeler ses fonctions via FFI.
— Choisir le bon modèle : *fan-out/fan-in* avec orchestration dans un parent VITL, chaque worker = process natif externe.
— Limiter la granularité : tâches “grosses” pour amortir le coût IPC.

Antipatterns
— Simuler des “threads” avec boucles agressives et I/O bloquantes → gaspillage CPU.
— Multiplier les processus sans contrôle de ressources → contention disque/mémoire.

Checklist
[ ] Avez-vous réellement besoin de parallélisme ?  
[ ] Les parties lourdes sont-elles externalisables dans un binaire C/Rust appelé par VITL ?  
[ ] Les canaux de communication sont-ils bornés et robustes aux erreurs/temps morts ?  

───────────────────────────────────────────────
11.2 Pas de GC complet (seulement Rc/Weak)
────────────────────────────────────────────
Ce que cela signifie
— Gestion mémoire sûre par **références comptées** (Rc<T>) et références faibles (Weak<T>).
— Pas de ramasse-miettes tracing. Pas de déplacement automatique d’objets. Pas de collecte de cycles automatique.

Conséquences
— Les cycles forts (A → B → A) **ne sont pas libérés** automatiquement.
— Les performances sont prévisibles (pas de pause GC), mais l’architecture doit éviter les cycles.

Stratégies de conception
Débutant
— Préférer des structures arborescentes sans références en arrière.
— Passer des données par valeur quand c’est petit et fréquent (i32, f64…).

Intermédiaire
— Utiliser **Weak** pour les pointeurs “retour” (parent) et **Rc** pour les pointeurs “avant” (enfants).
— Documenter la topologie (qui possède quoi). Une seule direction en **fort**, l’autre en **faible**.

Pro
— Introduire des *zones de vie* et des frontières claires entre propriétaires et observateurs.
— Auditer régulièrement (revue de code) les graphes d’objets susceptibles de former des cycles.
— Si nécessaire, encapsuler un allocateur/arena côté C via FFI pour des patterns haute fréquence (pools, slab).

Exemple (rupture de cycle)
Mauvais (cycle fort):
  // A::child -> Rc<B>, B::parent -> Rc<A>  (cycle)
Correct (parent faible):
  // A::child -> Rc<B>, B::parent -> Weak<A>

Checklist
[ ] Les relations parents/enfants évitent-elles les cycles forts ?  
[ ] Les valeurs volumineuses sont-elles partagées via Rc et lues majoritairement ?  
[ ] Les buffers temporaires élevés en fréquence sont-ils réutilisés (éviter l’allocation répétée) ?  

───────────────────────────────────────────────
11.3 Génériques partiels uniquement
──────────────────────────────────
Ce que cela signifie
— Les types/génériques existent de façon limitée (principalement dans la stdlib).
— Moins de métaprogrammation que dans Vitte “complet” ou Rust.

Conséquences
— API “générique” parfois moins flexible.
— Possibles duplications de code quand les types varient beaucoup.

Stratégies de conception
Débutant
— Commencer avec des types concrets (i32, f64, String). Éviter d’abstraire trop tôt.
— Favoriser des fonctions simples et spécialisées.

Intermédiaire
— Factoriser par **interfaces de données** (formats texte, lignes, enregistrements) au lieu de viser la généricité type-paramétrée.
— Utiliser des adaptateurs minces : convertisseurs `to_string()`, `parse()` côté std.str.

Pro
— Déporter la généricité “lourde” dans une lib C/Rust accessible via FFI C (sur quelques points chauds seulement).
— Stabiliser vos **types de domaine** (DTO) et isoler les conversions aux frontières de module.
— Intégrer des tests de non-régression lors des changements de type.

Checklist
[ ] Les fonctions publiques ont-elles des signatures **stables** et explicites ?  
[ ] Les conversions de types sont-elles centralisées et testées ?  
[ ] Les points nécessitant de la généricité forte sont-ils délégués à FFI si critique ?  

───────────────────────────────────────────────
11.4 FFI limité au C (ABI C)
────────────────────────────
Ce que cela signifie
— Seule l’interface binaire C est supportée nativement.
— C++/Rust/Go/etc. doivent exposer un **shim C** (`extern "C"`) pour être appelés.

Conséquences
— Pas de passage direct d’objets complexes non-C.
— Convention d’appel et représentation mémoire = C.

Stratégies d’intégration
Débutant
— S’en tenir aux appels simples : fonctions C pures, entrées/sorties primitives, buffers.
— Toujours construire les chaînes avec `std.c::CString::from_str` avant de passer au C.

Intermédiaire
— Écrire un **shim C** propre : API plate, types opaques (`void*` + fonctions d’accès), fonctions `create()/destroy()`.
— Vérifier les codes retour et convertir vers `Result`.

Pro
— Définir une **ABI stable** (semver) côté C, tests d’intégration croisés (VITL ↔ C).
— Mesurer l’overhead d’appels FFI et grouper les opérations pour amortir le coût.
— Gérer explicitement l’allocation/désallocation croisée (qui possède le buffer ? qui le libère ?).

Exemple de pattern FFI
C (shim):
  struct Obj;
  Obj* obj_create(int cap);
  void  obj_destroy(Obj*);
  int   obj_push(Obj*, const char* s);  // 0=OK, <0=err
  int   obj_len(const Obj*);

VITL:
  extern "C" {
    fn obj_create(cap:i32)->*mut void
    fn obj_destroy(p:*mut void)->void
    fn obj_push(p:*mut void, s:*const char)->i32
    fn obj_len(p:*const void)->i32
  }
  // Construire CString, appeler dans unsafe, vérifier codes retour.

Checklist
[ ] Le shim expose-t-il uniquement des types C stables (int, double, void*, char*) ?  
[ ] Les responsabilités d’allocation/libération sont-elles documentées ?  
[ ] Chaque appel renvoie-t-il un code d’erreur testable, converti en `Result` côté VITL ?  

───────────────────────────────────────────────
11.5 Décider quand “sortir” des limites
─────────────────────────────────────────────
Règle pratique
— Si la fonctionnalité manque et que l’**algorithme** suffit à compenser → rester 100% VITL.
— Si les contraintes sont structurelles (threads, généricité lourde, GC) → **isoler** la partie critique dans une lib C/Rust avec shim C et piloter depuis VITL.

Matrice rapide
— Performance CPU brute → déléguer le cœur à C/Rust (FFI).  
— I/O intensif séquentiel → VITL convient, optimiser le format et les buffers.  
— Concurrence massive → multi-processus ou runtime externe via FFI.  
— Modèles de données dynamiques et très polymorphes → fixer des DTO stables + shims.

───────────────────────────────────────────────
11.6 Contrats de qualité autour des limites
────────────────────────────────────────────
— Tests : pour chaque contour, fournir au moins 1 test succès + 1 test erreur.
— Logs : enrichir les messages d’erreur avec contexte (fichier, taille, chemin).
— Docs : annoter les fonctions “limites” avec `///` précisant invariants et coûts.
— Bench : garder un micro-benchmark des chemins FFI et des I/O.

Fin de la section 11.


──────────────────────────────────────────────

12) CODES DE SORTIE
────────────────────
0 → succès  
1 → erreur générique/panic  
2 → erreur d’usage CLI  
3 → erreur I/O  
4 → erreur FFI

──────────────────────────────────────────────
11) DIAGNOSTICS COURANTS — VERSION DÉTAILLÉE
────────────────────────────────────────────

But
— Donner pour chaque code : symptôme, causes probables, correctifs, exemples.
— Cible débutants → comprendre le message. Intermédiaires → corriger vite. Pros → outillage.

Outils utiles
— Lint :        vitl check src/
— Build debug : vitl build -g -O0 -o build/app src/main.vitl
— IR/BC :       vitl build --emit-ir     | vitl build --emit-bytecode
— Traces :      std.debug::backtrace()   | panic("msg")
— Recherche :   vitl doc src/ -o build/docs.txt

Notation
— « Mauvais » = exemple fautif. « Correct » = correction minimale.


E0001 : symbole inconnu
───────────────────────
Symptôme
— Le compilateur ne trouve pas une fonction, un type, un module ou une constante.

Causes fréquentes
— Import manquant ou chemin de module incorrect.
— Typo dans le nom (majuscules/minuscules).
— Déplacement de fichier sans mise à jour de `module` en tête.
— API renommée lors d’une refactorisation.

Correctifs standard
— Ajouter l’import précis ou corriger le chemin.
— Vérifier que `module` en tête correspond au chemin disque.
— Lancer `vitl fmt` pour regrouper et stabiliser les imports.

Exemples
Mauvais:
  module app.main
  fn main()->i32 {
    std.io::pritnln("hi")   // typo
    return 0
  }

Correct:
  module app.main
  import std.io
  fn main()->i32 {
    std.io::println("hi")
    return 0
  }

Astuce pro
— Préférer `import std.{io, fs}` pour expliciter le périmètre.
— `vitl check` pointe la ligne et la colonne exactes.


E0002 : types incompatibles
───────────────────────────
Symptôme
— Une expression du type A est passée là où B est attendu.

Causes fréquentes
— Addition mélangeant `i32` et `f64`.
— Appel d’API avec paramètre de type attendu différent.
— Comparaison stricte entre types non convertibles.

Correctifs standard
— Ajouter un cast explicite `as` quand la perte d’info est acceptée.
— Adapter la signature de la fonction ou l’appelant pour unifier les types.
— Introduire une conversion préalable (ex. `to_string()`).

Exemples
Mauvais:
  let n:i32 = 3
  let x:f64 = 0.5
  let y = n + x     // E0002

Correct (1 — cast local):
  let n:i32 = 3
  let x:f64 = 0.5
  let y = (n as f64) + x

Correct (2 — normaliser l amont):
  let n:f64 = 3.0
  let x:f64 = 0.5
  let y = n + x

Autres patterns
— Chaînes:
    std.io::println("n=" + n)        // E0002
    std.io::println("n=" + n.to_string())   // OK

Astuce pro
— Stabiliser les types aux frontières d’API publiques (types explicites).
— `vitl check` repère aussi les conversions implicites dangereuses.


E0003 : variable non initialisée
────────────────────────────────
Symptôme
— Utilisation d’une variable qui n’a pas reçu de valeur sur tous les chemins.

Causes fréquentes
— Déclaration séparée de l’initialisation.
— Chemin `if/else` non exhaustif.
— Retour prématuré avant affectation.

Correctifs standard
— Initialiser à la déclaration.
— Rendre les branches exhaustives.
— Restructurer en `match` pour couvrir tous les cas.

Exemples
Mauvais:
  let mut acc:i32
  if cond { acc = 1 }
  // cond=false → acc non assignée
  std.io::println(acc.to_string())   // E0003

Correct:
  let mut acc:i32 = 0
  if cond { acc = 1 }
  std.io::println(acc.to_string())

Autre:
  let x:i32
  if a { x = 1 } else { x = 2 }
  // ici OK car exhaustif

Astuce pro
— Éviter les « variables sentinelles »; préférer un `match` retournant une valeur.


E1001 : appel FFI non-safe hors bloc unsafe
────────────────────────────────────────────
Symptôme
— Appel d’une fonction C ou manipulation de pointeurs bruts sans `unsafe`.

Causes fréquentes
— Oubli du bloc `unsafe { ... }`.
— Conversion de chaîne en `CString` non vérifiée.
— Passage d’un pointeur invalide à une API C.

Correctifs standard
— Envelopper *uniquement* l’appel risqué dans `unsafe`.
— Construire les `CString` via `std.c::CString::from_str`.
— Assurer la durée de vie des buffers passés au C.

Exemples
Mauvais:
  extern "C" { fn puts(msg:*const char)->i32 }
  fn main()->i32 {
    let s = std.c::CString::from_str("Hi\n")
    puts(s.as_ptr())          // E1001
    return 0
  }

Correct:
  extern "C" { fn puts(msg:*const char)->i32 }
  fn main()->i32 {
    let s = std.c::CString::from_str("Hi\n")
    unsafe { _ = puts(s.as_ptr()) }
    return 0
  }

Astuce pro
— Réduire la surface `unsafe` au strict minimum.
— Valider les tailles et null-terminators côté VITL avant l’appel.


E2001 : fichier introuvable (I/O)
─────────────────────────────────
Symptôme
— Échec d’ouverture/lecture d’un fichier par la stdlib (erreur au runtime).

Causes fréquentes
— Chemin relatif interprété depuis un répertoire inattendu.
— Fichier non copié ou supprimé.
— Droits insuffisants, montage absent.

Correctifs standard
— Tester l’existence avant lecture.
— Utiliser des chemins absolus pour les ressources système.
— Gérer l’erreur avec `Result` et message contextuel.

Exemples
Mauvais:
  let txt = std.fs::read_to_string("data/config.txt")?   // E2001

Correct:
  let path = "data/config.txt"
  if !std.fs::exists(path) {
    std.io::eprintln("config manquante: " + path)
    return 3
  }
  let txt = std.fs::read_to_string(path)?


Annexe A — Diagnostics supplémentaires courants
───────────────────────────────────────────────
E0100 : variable non utilisée
— Contexte: le code compile mais une variable n est jamais lue.
— Correction: supprimer la variable, ou prefixer `_var` pour marquer volontaire.

E0101 : code inatteignable
— Contexte: instructions après `return` ou après un `match` exhaustif.
— Correction: retirer la portion morte ou factoriser les branches.

E0201 : division par zéro détectable
— Contexte: `x / 0` littéral connu au compile-time.
— Correction: vérifier dénominateur, valider en amont.

E0301 : dépassement d’index (runtime)
— Contexte: `v[i]` hors bornes.
— Correction: tester `i < v.len()`, ou utiliser `get(i)` qui retourne `Option`.

E0401 : pattern `match` non exhaustif
— Contexte: manque un cas, surtout avec `enum`.
— Correction: ajouter `_ => {...}` ou toutes les variantes.

E0501 : conflit mutabilité simple
— Contexte: tentative d’écrire dans une donnée non `mut`.
— Correction: déclarer `let mut x` ou cloner/retourner une nouvelle valeur.

E0601 : format de chaîne invalide
— Contexte: concat de types non-string sans `to_string()`.
— Correction: convertir avant concaténation.

E0701 : échec FFI (résultat négatif)
— Contexte: fonction C renvoie code erreur.
— Correction: vérifier le code retour, convertir en `Result::Err(...)` côté VITL.

E0801 : UTF-8 invalide lors d une lecture texte
— Contexte: contenu binaire lu via `read_to_string`.
— Correction: utiliser `std.fs::read` (bytes) puis décoder conditionnellement.


Annexe B — Stratégie de triage (checklist rapide)
──────────────────────────────────────────────────
1) Lire le message complet et la ligne ciblée.
2) Lancer `vitl check` pour lints supplémentaires.
3) Si import/symbole: vérifier `module` et `import`.
4) Si types: normaliser les types en amont, cast explicite si nécessaire.
5) Si non initialisé: initialiser à la déclaration ou rendre les branches exhaustives.
6) Si FFI: entourer par `unsafe`, valider les pointeurs et durées de vie.
7) Si I/O: controler `std.fs::exists`, chemins absolus, droits.
8) Recompiler avec `-g` et activer `std.debug::backtrace()` en cas de crash.
9) Ajouter des `test` minimaux pour reproduire le bug.
10) Documenter la cause et le correctif en commentaire `///` si API publique.


Annexe C — Exemples de messages enrichis côté application
──────────────────────────────────────────────────────────
— I/O robuste:
  let path = argv[1]
  if !std.fs::exists(path) {
    std.io::eprintln("E2001: fichier introuvable → " + path)
    std.io::eprintln("Astuce: lancer depuis la racine du projet ou passer un chemin absolu.")
    return 3
  }

— FFI robuste:
  let msg = std.c::CString::from_str("ok\n")
  if msg.is_err() {
    return Result::Err("E0601: CString invalide (caractère nul)")
  }
  unsafe { _ = puts(msg.unwrap().as_ptr()) }


Annexe D — Bonnes pratiques pour éviter les diagnostics
────────────────────────────────────────────────────────
— Imports précis et groupés; `vitl fmt` après chaque session.
— Types explicites aux frontières d’API (fonctions `pub`).
— Tests unitaires pour chaque module; couvrir les erreurs attendues.
— Logs de contexte avec codes d’erreur stables (E-codes ci-dessus).
— Limiter `unsafe` et l’isoler; documenter les invariants attendus.
— Préférer `Option`/`Result` aux valeurs sentinelles magiques.
— Valider l’entrée utilisateur tôt; fail fast avec message clair.

Fin de la section 10.

14) CHECKLIST AVANT DE LIVRER
─────────────────────────────
[ ] module app.main défini  
[ ] fn main()->i32 présent  
[ ] imports réduits au nécessaire  
[ ] vitl test passe avec succès  
[ ] build/app généré dans /build  

──────────────────────────────────────────────

─────────────────────────────────────────────
