
──────────────────────────────────────────────

6) STANDARD LIBRARY
───────────────────
Résumé des modules principaux :

- std.io    : print, println, eprintln
- std.fs    : read_to_string, write_string
- std.str   : len, split, find, replace
- std.math  : abs, sqrt, sin, cos, pow
- std.time  : now, sleep_ms
- std.cli   : args, env, exit
- std.vec   : push, pop, len
- std.debug : assert, backtrace
- std.c     : CString, malloc/free (unsafe)

──────────────────────────────────────────────

7) MÉMOIRE ET SÉCURITÉ
──────────────────────
Mode sûr :
- Gestion automatique via Rc<T>.
- Pas de GC global → prévisible.

Mode unsafe :
- Pointeurs bruts.
- Obligatoire pour appels C sensibles.

Bonne pratique :
- Débutant : ne jamais utiliser unsafe.
- Intermédiaire : slices et Rc.
- Pro : FFI, contrôle mémoire avancé.

──────────────────────────────────────────────

8) OUTILS EN LIGNE DE COMMANDE
──────────────────────────────
Exécution directe :
  vitl run src/main.vitl

Compilation :
  vitl build -O2 -o build/app src/main.vitl

Formateur :
  vitl fmt src/

Analyse statique :
  vitl check src/

Tests :
  vitl test

Documentation :
  vitl doc src/ -o build/docs.txt

──────────────────────────────────────────────

9) STYLE DE CODE
────────────────
- module en tête de fichier.
- imports regroupés.
- snake_case pour variables/fonctions.
- CamelCase pour structs/enums.
- Types explicites aux API publiques.
- Utiliser `vitl fmt` pour homogénéiser.

──────────────────────────────────────────────

10) DIAGNOSTICS COURANTS
────────────────────────
E0001 : symbole inconnu → mauvais import.  
E0002 : types incompatibles → ajouter cast.  
E0003 : variable non initialisée.  
E1001 : appel FFI hors bloc unsafe.  
E2001 : fichier non trouvé (I/O).  

──────────────────────────────────────────────

11) CONSEILS PAR NIVEAU
────────────────────────
Débutant :
- Travaillez avec `println` pour valider votre code.
- Écrivez des programmes courts, testez souvent.
- Formatez avec `vitl fmt`.

Intermédiaire :
- Créez modules séparés pour chaque fonctionnalité.
- Ajoutez des `test` pour éviter les régressions.
- Utilisez `?` pour simplifier gestion d’erreurs.

Pro :
- Activez optimisations `-O3` pour production.
- Inspectez IR avec `--emit-ir` pour optimiser.
- Écrivez wrappers FFI pour interop C.

──────────────────────────────────────────────

12) LIMITES ACTUELLES (VERSION LIGHT)
─────────────────────────────────────
- Pas de threads natifs.
- Pas de garbage collector complet (seulement Rc).
- Génériques limités aux containers standard.
- FFI stable uniquement avec C.

──────────────────────────────────────────────

13) CODES DE SORTIE
────────────────────
0 → succès  
1 → erreur générique/panic  
2 → erreur d’usage CLI  
3 → erreur I/O  
4 → erreur FFI  

──────────────────────────────────────────────

14) CHECKLIST RAPIDE
────────────────────
[ ] module app.main défini  
[ ] fn main()->i32 présent  
[ ] imports minimaux  
[ ] tests passent (`vitl test`)  
[ ] binaire généré dans /build  

──────────────────────────────────────────────
FIN DU README
──────────────────────────────────────────────
