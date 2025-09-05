collecte de cycles automatique.

Conséquences --- Les cycles forts (A → B → A) **ne sont pas libérés**
automatiquement. --- Les performances sont prévisibles (pas de pause
GC), mais l'architecture doit éviter les cycles.

Stratégies de conception Débutant --- Préférer des structures
arborescentes sans références en arrière. --- Passer des données par
valeur quand c'est petit et fréquent (i32, f64...).

Intermédiaire --- Utiliser **Weak** pour les pointeurs "retour" (parent)
et **Rc** pour les pointeurs "avant" (enfants). --- Documenter la
topologie (qui possède quoi). Une seule direction en **fort**, l'autre
en **faible**.

Pro --- Introduire des *zones de vie* et des frontières claires entre
propriétaires et observateurs. --- Auditer régulièrement (revue de code)
les graphes d'objets susceptibles de former des cycles. --- Si
nécessaire, encapsuler un allocateur/arena côté C via FFI pour des
patterns haute fréquence (pools, slab).

Exemple (rupture de cycle) Mauvais (cycle fort): // A::child -\>
Rc`<B>`{=html}, B::parent -\> Rc`<A>`{=html} (cycle) Correct (parent
faible): // A::child -\> Rc`<B>`{=html}, B::parent -\> Weak`<A>`{=html}

Checklist \[ \] Les relations parents/enfants évitent-elles les cycles
forts ?\
\[ \] Les valeurs volumineuses sont-elles partagées via Rc et lues
majoritairement ?\
\[ \] Les buffers temporaires élevés en fréquence sont-ils réutilisés
(éviter l'allocation répétée) ?

─────────────────────────────────────────────── 11.3 Génériques partiels
uniquement ────────────────────────────────── Ce que cela signifie ---
Les types/génériques existent de façon limitée (principalement dans la
stdlib). --- Moins de métaprogrammation que dans Vitte "complet" ou
Rust.

Conséquences --- API "générique" parfois moins flexible. --- Possibles
duplications de code quand les types varient beaucoup.

Stratégies de conception Débutant --- Commencer avec des types concrets
(i32, f64, String). Éviter d'abstraire trop tôt. --- Favoriser des
fonctions simples et spécialisées.

Intermédiaire --- Factoriser par **interfaces de données** (formats
texte, lignes, enregistrements) au lieu de viser la généricité
type-paramétrée. --- Utiliser des adaptateurs minces : convertisseurs
`to_string()`, `parse()` côté std.str.

Pro --- Déporter la généricité "lourde" dans une lib C/Rust accessible
via FFI C (sur quelques points chauds seulement). --- Stabiliser vos
**types de domaine** (DTO) et isoler les conversions aux frontières de
module. --- Intégrer des tests de non-régression lors des changements de
type.

Checklist \[ \] Les fonctions publiques ont-elles des signatures
**stables** et explicites ?\
\[ \] Les conversions de types sont-elles centralisées et testées ?\
\[ \] Les points nécessitant de la généricité forte sont-ils délégués à
FFI si critique ?

─────────────────────────────────────────────── 11.4 FFI limité au C
(ABI C) ──────────────────────────── Ce que cela signifie --- Seule
l'interface binaire C est supportée nativement. --- C++/Rust/Go/etc.
doivent exposer un **shim C** (`extern "C"`) pour être appelés.

Conséquences --- Pas de passage direct d'objets complexes non-C. ---
Convention d'appel et représentation mémoire = C.

Stratégies d'intégration Débutant --- S'en tenir aux appels simples :
fonctions C pures, entrées/sorties primitives, buffers. --- Toujours
construire les chaînes avec `std.c::CString::from_str` avant de passer
au C.

Intermédiaire --- Écrire un **shim C** propre : API plate, types opaques
(`void*` + fonctions d'accès), fonctions `create()/destroy()`. ---
Vérifier les codes retour et convertir vers `Result`.

Pro --- Définir une **ABI stable** (semver) côté C, tests d'intégration
croisés (VITL ↔ C). --- Mesurer l'overhead d'appels FFI et grouper les
opérations pour amortir le coût. --- Gérer explicitement
l'allocation/désallocation croisée (qui possède le buffer ? qui le
libère ?).

Exemple de pattern FFI C (shim): struct Obj; Obj\* obj_create(int cap);
void obj_destroy(Obj*); int obj_push(Obj*, const char\* s); // 0=OK,
\<0=err int obj_len(const Obj\*);

VITL: extern "C" { fn obj_create(cap:i32)-\>*mut void fn
obj_destroy(p:*mut void)-\>void fn obj_push(p:*mut void, s:*const
char)-\>i32 fn obj_len(p:\*const void)-\>i32 } // Construire CString,
appeler dans unsafe, vérifier codes retour.

Checklist \[ \] Le shim expose-t-il uniquement des types C stables (int,
double, void*, char*) ?\
\[ \] Les responsabilités d'allocation/libération sont-elles documentées
?\
\[ \] Chaque appel renvoie-t-il un code d'erreur testable, converti en
`Result` côté VITL ?

─────────────────────────────────────────────── 11.5 Décider quand
"sortir" des limites ───────────────────────────────────────────── Règle
pratique --- Si la fonctionnalité manque et que l'**algorithme** suffit
à compenser → rester 100% VITL. --- Si les contraintes sont
structurelles (threads, généricité lourde, GC) → **isoler** la partie
critique dans une lib C/Rust avec shim C et piloter depuis VITL.

Matrice rapide --- Performance CPU brute → déléguer le cœur à C/Rust
(FFI).\
--- I/O intensif séquentiel → VITL convient, optimiser le format et les
buffers.\
--- Concurrence massive → multi-processus ou runtime externe via FFI.\
--- Modèles de données dynamiques et très polymorphes → fixer des DTO
stables + shims.

─────────────────────────────────────────────── 11.6 Contrats de qualité
autour des limites ──────────────────────────────────────────── ---
Tests : pour chaque contour, fournir au moins 1 test succès + 1 test
erreur. --- Logs : enrichir les messages d'erreur avec contexte
(fichier, taille, chemin). --- Docs : annoter les fonctions "limites"
avec `///` précisant invariants et coûts. --- Bench : garder un
micro-benchmark des chemins FFI et des I/O.

Fin de la section 11.

──────────────────────────────────────────────

12) CODES DE SORTIE ──────────────────── 0 → succès\
    1 → erreur générique/panic\
    2 → erreur d'usage CLI\
    3 → erreur I/O\
    4 → erreur FFI

────────────────────────────────────────────── 11) DIAGNOSTICS COURANTS
--- VERSION DÉTAILLÉE ────────────────────────────────────────────

But --- Donner pour chaque code : symptôme, causes probables,
correctifs, exemples. --- Cible débutants → comprendre le message.
Intermédiaires → corriger vite. Pros → outillage.

Outils utiles --- Lint : vitl check src/ --- Build debug : vitl build -g
-O0 -o build/app src/main.vitl --- IR/BC : vitl build --emit-ir \| vitl
build --emit-bytecode --- Traces : std.debug::backtrace() \|
panic("msg") --- Recherche : vitl doc src/ -o build/docs.txt

Notation --- « Mauvais » = exemple fautif. « Correct » = correction
minimale.

E0001 : symbole inconnu ─────────────────────── Symptôme --- Le
compilateur ne trouve pas une fonction, un type, un module ou une
constante.

Causes fréquentes --- Import manquant ou chemin de module incorrect. ---
Typo dans le nom (majuscules/minuscules). --- Déplacement de fichier
sans mise à jour de `module` en tête. --- API renommée lors d'une
refactorisation.

Correctifs standard --- Ajouter l'import précis ou corriger le chemin.
--- Vérifier que `module` en tête correspond au chemin disque. ---
Lancer `vitl fmt` pour regrouper et stabiliser les imports.

Exemples Mauvais: module app.main fn main()-\>i32 {
std.io::pritnln("hi") // typo return 0 }

Correct: module app.main import std.io fn main()-\>i32 {
std.io::println("hi") return 0 }

Astuce pro --- Préférer `import std.{io, fs}` pour expliciter le
périmètre. --- `vitl check` pointe la ligne et la colonne exactes.

E0002 : types incompatibles ─────────────────────────── Symptôme --- Une
expression du type A est passée là où B est attendu.

Causes fréquentes --- Addition mélangeant `i32` et `f64`. --- Appel
d'API avec paramètre de type attendu différent. --- Comparaison stricte
entre types non convertibles.

Correctifs standard --- Ajouter un cast explicite `as` quand la perte
d'info est acceptée. --- Adapter la signature de la fonction ou
l'appelant pour unifier les types. --- Introduire une conversion
préalable (ex. `to_string()`).

Exemples Mauvais: let n:i32 = 3 let x:f64 = 0.5 let y = n + x // E0002

Correct (1 --- cast local): let n:i32 = 3 let x:f64 = 0.5 let y = (n as
f64) + x

Correct (2 --- normaliser l amont): let n:f64 = 3.0 let x:f64 = 0.5 let
y = n + x

Autres patterns --- Chaînes: std.io::println("n=" + n) // E0002
std.io::println("n=" + n.to_string()) // OK

Astuce pro --- Stabiliser les types aux frontières d'API publiques
(types explicites). --- `vitl check` repère aussi les conversions
implicites dangereuses.

E0003 : variable non initialisée ────────────────────────────────
Symptôme --- Utilisation d'une variable qui n'a pas reçu de valeur sur
tous les chemins.

Causes fréquentes --- Déclaration séparée de l'initialisation. ---
Chemin `if/else` non exhaustif. --- Retour prématuré avant affectation.

Correctifs standard --- Initialiser à la déclaration. --- Rendre les
branches exhaustives. --- Restructurer en `match` pour couvrir tous les
cas.

Exemples Mauvais: let mut acc:i32 if cond { acc = 1 } // cond=false →
acc non assignée std.io::println(acc.to_string()) // E0003

Correct: let mut acc:i32 = 0 if cond { acc = 1 }
std.io::println(acc.to_string())

Autre: let x:i32 if a { x = 1 } else { x = 2 } // ici OK car exhaustif

Astuce pro --- Éviter les « variables sentinelles »; préférer un `match`
retournant une valeur.

E1001 : appel FFI non-safe hors bloc unsafe
──────────────────────────────────────────── Symptôme --- Appel d'une
fonction C ou manipulation de pointeurs bruts sans `unsafe`.

Causes fréquentes --- Oubli du bloc `unsafe { ... }`. --- Conversion de
chaîne en `CString` non vérifiée. --- Passage d'un pointeur invalide à
une API C.

Correctifs standard --- Envelopper *uniquement* l'appel risqué dans
`unsafe`. --- Construire les `CString` via `std.c::CString::from_str`.
--- Assurer la durée de vie des buffers passés au C.

Exemples Mauvais: extern "C" { fn puts(msg:\*const char)-\>i32 } fn
main()-\>i32 { let s = std.c::CString::from_str("Hi`\n`{=tex}")
puts(s.as_ptr()) // E1001 return 0 }

Correct: extern "C" { fn puts(msg:\*const char)-\>i32 } fn main()-\>i32
{ let s = std.c::CString::from_str("Hi`\n`{=tex}") unsafe { \_ =
puts(s.as_ptr()) } return 0 }

Astuce pro --- Réduire la surface `unsafe` au strict minimum. ---
Valider les tailles et null-terminators côté VITL avant l'appel.

E2001 : fichier introuvable (I/O) ─────────────────────────────────
Symptôme --- Échec d'ouverture/lecture d'un fichier par la stdlib
(erreur au runtime).

Causes fréquentes --- Chemin relatif interprété depuis un répertoire
inattendu. --- Fichier non copié ou supprimé. --- Droits insuffisants,
montage absent.

Correctifs standard --- Tester l'existence avant lecture. --- Utiliser
des chemins absolus pour les ressources système. --- Gérer l'erreur avec
`Result` et message contextuel.

Exemples Mauvais: let txt = std.fs::read_to_string("data/config.txt")?
// E2001

Correct: let path = "data/config.txt" if !std.fs::exists(path) {
std.io::eprintln("config manquante:" + path) return 3 } let txt =
std.fs::read_to_string(path)?

Annexe A --- Diagnostics supplémentaires courants
─────────────────────────────────────────────── E0100 : variable non
utilisée --- Contexte: le code compile mais une variable n est jamais
lue. --- Correction: supprimer la variable, ou prefixer `_var` pour
marquer volontaire.

E0101 : code inatteignable --- Contexte: instructions après `return` ou
après un `match` exhaustif. --- Correction: retirer la portion morte ou
factoriser les branches.

E0201 : division par zéro détectable --- Contexte: `x / 0` littéral
connu au compile-time. --- Correction: vérifier dénominateur, valider en
amont.

E0301 : dépassement d'index (runtime) --- Contexte: `v[i]` hors bornes.
--- Correction: tester `i < v.len()`, ou utiliser `get(i)` qui retourne
`Option`.

E0401 : pattern `match` non exhaustif --- Contexte: manque un cas,
surtout avec `enum`. --- Correction: ajouter `_ => {...}` ou toutes les
variantes.

E0501 : conflit mutabilité simple --- Contexte: tentative d'écrire dans
une donnée non `mut`. --- Correction: déclarer `let mut x` ou
cloner/retourner une nouvelle valeur.

E0601 : format de chaîne invalide --- Contexte: concat de types
non-string sans `to_string()`. --- Correction: convertir avant
concaténation.

E0701 : échec FFI (résultat négatif) --- Contexte: fonction C renvoie
code erreur. --- Correction: vérifier le code retour, convertir en
`Result::Err(...)` côté VITL.

E0801 : UTF-8 invalide lors d une lecture texte --- Contexte: contenu
binaire lu via `read_to_string`. --- Correction: utiliser `std.fs::read`
(bytes) puis décoder conditionnellement.

Annexe B --- Stratégie de triage (checklist rapide)
────────────────────────────────────────────────── 1) Lire le message
complet et la ligne ciblée. 2) Lancer `vitl check` pour lints
supplémentaires. 3) Si import/symbole: vérifier `module` et `import`. 4)
Si types: normaliser les types en amont, cast explicite si nécessaire.
5) Si non initialisé: initialiser à la déclaration ou rendre les
branches exhaustives. 6) Si FFI: entourer par `unsafe`, valider les
pointeurs et durées de vie. 7) Si I/O: controler `std.fs::exists`,
chemins absolus, droits. 8) Recompiler avec `-g` et activer
`std.debug::backtrace()` en cas de crash. 9) Ajouter des `test` minimaux
pour reproduire le bug. 10) Documenter la cause et le correctif en
commentaire `///` si API publique.

Annexe C --- Exemples de messages enrichis côté application
────────────────────────────────────────────────────────── --- I/O
robuste: let path = argv\[1\] if !std.fs::exists(path) {
std.io::eprintln("E2001: fichier introuvable →" + path)
std.io::eprintln("Astuce: lancer depuis la racine du projet ou passer un
chemin absolu.") return 3 }

--- FFI robuste: let msg = std.c::CString::from_str("ok`\n`{=tex}") if
msg.is_err() { return Result::Err("E0601: CString invalide (caractère
nul)") } unsafe { \_ = puts(msg.unwrap().as_ptr()) }

Annexe D --- Bonnes pratiques pour éviter les diagnostics
──────────────────────────────────────────────────────── --- Imports
précis et groupés; `vitl fmt` après chaque session. --- Types explicites
aux frontières d'API (fonctions `pub`). --- Tests unitaires pour chaque
module; couvrir les erreurs attendues. --- Logs de contexte avec codes
d'erreur stables (E-codes ci-dessus). --- Limiter `unsafe` et l'isoler;
documenter les invariants attendus. --- Préférer `Option`/`Result` aux
valeurs sentinelles magiques. --- Valider l'entrée utilisateur tôt; fail
fast avec message clair.

Fin de la section 10.

14) CHECKLIST AVANT DE LIVRER ───────────────────────────── \[ \] module
    app.main défini\
    \[ \] fn main()-\>i32 présent\
    \[ \] imports réduits au nécessaire\
    \[ \] vitl test passe avec succès\
    \[ \] build/app généré dans /build

──────────────────────────────────────────────

FIN DU GUIDE ─────────────────────────────────────────────
