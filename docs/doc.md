<!doctype html>
<html lang="fr">
<head>
<meta charset="utf-8" />
<meta name="viewport" content="width=device-width,initial-scale=1" />
<title>VITTE LIGHT — Guide utilisateur complet (Révision 2025-09-05)</title>
<meta name="color-scheme" content="light dark" />
<style>
  :root{
    --bg:#ffffff; --fg:#0f172a; --muted:#64748b; --card:#f8fafc;
    --border:#e2e8f0; --accent:#2563eb; --code:#111827;
  }
  @media (prefers-color-scheme: dark){
    :root{
      --bg:#0b1020; --fg:#e8eef6; --muted:#9aa7bd; --card:#0f152a;
      --border:#1f2a44; --accent:#60a5fa; --code:#e6edf3;
    }
  }
  *{box-sizing:border-box}
  body{
    margin:0; background:var(--bg); color:var(--fg);
    font:16px/1.7 system-ui,-apple-system,Segoe UI,Roboto,Inter,Arial,"Noto Sans";
  }
  a{ color:var(--accent); text-decoration:none }
  a:hover{ text-decoration:underline }
  header{
    padding:24px; border-bottom:1px solid var(--border);
    background:linear-gradient(180deg, rgba(99,102,241,.08), transparent);
  }
  .container{ max-width:1000px; margin:0 auto; padding:24px }
  .intro{ display:flex; gap:18px; align-items:flex-end; flex-wrap:wrap }
  h1{ margin:0; font-size:clamp(24px,3vw,36px); letter-spacing:.2px }
  .rev{ color:var(--muted) }
  nav.toc{
    background:var(--card); border:1px solid var(--border); border-radius:12px;
    padding:14px 16px; margin:20px 0 8px 0;
  }
  nav.toc strong{ font-size:13px; color:var(--muted); text-transform:uppercase; letter-spacing:.08em }
  nav.toc ul{ margin:8px 0 0 0; padding:0 0 0 16px }
  section{ margin:28px 0 }
  h2{ margin:0 0 10px 0; padding-top:10px; border-top:1px solid var(--border) }
  h3{ margin:18px 0 8px 0 }
  p{ margin:.6rem 0 }
  ul,ol{ margin:.4rem 0 .8rem 1.25rem }
  .card{
    background:var(--card); border:1px solid var(--border); border-radius:12px; padding:16px;
  }
  code{
    font-family:ui-monospace,SFMono-Regular,Menlo,Consolas,Monaco,monospace;
    background:color-mix(in srgb, var(--card) 85%, transparent);
    color:var(--code); padding:.15em .35em; border-radius:6px; border:1px solid var(--border)
  }
  pre{
    margin:.75rem 0; padding:14px 16px; border-radius:12px;
    background:var(--card); border:1px solid var(--border); overflow:auto
  }
  pre code{ background:transparent; border:0; padding:0; color:var(--code) }
  hr{ border:0; border-top:1px dashed var(--border); margin:16px 0 }
  .note{ color:var(--muted); font-size:.95rem }
</style>
</head>
<body>
  <header>
    <div class="container intro">
      <h1>VITTE LIGHT — Guide utilisateur complet</h1>
      <span class="rev">Révision&nbsp;: 2025-09-05</span>
    </div>
  </header>

  <main class="container">
    <nav class="toc" aria-label="Table des matières">
      <strong>Sommaire</strong>
      <ul>
        <li><a href="#intro">0) Introduction</a>
          <ul>
            <li><a href="#philosophie">0.1 Philosophie</a></li>
            <li><a href="#pour-qui">0.2 Pour qui&nbsp;?</a></li>
            <li><a href="#domaines">0.3 Domaines d'utilisation</a></li>
            <li><a href="#points-forts">0.4 Points forts</a></li>
            <li><a href="#hello">0.5 Exemple rapide</a></li>
          </ul>
        </li>
        <li><a href="#structure">1) Structure de projet</a></li>
        <li><a href="#cli">2) Outils en ligne de commande</a></li>
        <li><a href="#syntaxe">3) Syntaxe : découverte progressive</a></li>
        <li><a href="#exemples">4) Premiers exemples</a></li>
        <li><a href="#memoire">5) Mémoire et sécurité</a></li>
        <li><a href="#erreurs">6) Gestion des erreurs</a></li>
        <li><a href="#ffi">7) Interopérabilité C (FFI)</a></li>
        <li><a href="#stdlib">8) Stdlib — aperçu</a></li>
        <li><a href="#style">9) Style et bonnes pratiques</a></li>
        <li><a href="#niveaux">10) Conseils par niveau</a></li>
        <li><a href="#limites">11) Limites (version Light)</a></li>
        <li><a href="#codes-sortie">12) Codes de sortie</a></li>
        <li><a href="#diagnostics">13) Diagnostics courants</a></li>
        <li><a href="#checklist">14) Checklist avant de livrer</a></li>
      </ul>
    </nav>

    <!-- 0) INTRO -->
    <section id="intro">
      <h2>0) Introduction — VITTE LIGHT</h2>
      <p><strong>Vitte Light</strong> (abrégé <strong>VITL</strong>) est un langage minimaliste, dérivé du langage Vitte, conçu pour allier <strong>syntaxe moderne</strong>, <strong>simplicité d’usage</strong> et <strong>outillage complet</strong>. Il vise trois profils de développeurs&nbsp;: débutants, intermédiaires, professionnels.</p>

      <section id="philosophie">
        <h3>0.1 Philosophie du langage</h3>
        <ul>
          <li><strong>Minimalisme</strong>&nbsp;: garder un cœur réduit de fonctionnalités, faciles à apprendre.</li>
          <li><strong>Cohérence</strong>&nbsp;: syntaxe claire, pas de règles cachées, erreurs explicites.</li>
          <li><strong>Polyvalence</strong>&nbsp;: exécution immédiate via VM, compilation native, intégration en C.</li>
          <li><strong>Sécurité</strong>&nbsp;: gestion mémoire déterministe (références comptées), erreurs explicites (<code>Result</code>, <code>panic</code>).</li>
          <li><strong>Portabilité</strong>&nbsp;: fonctionne sur Linux, BSD, macOS, Windows.</li>
        </ul>
      </section>

      <section id="pour-qui">
        <h3>0.2 Pour qui est fait VITL&nbsp;?</h3>
        <p><strong>Débutant</strong></p>
        <ul>
          <li>Découvrir la programmation avec une syntaxe simple et lisible.</li>
          <li>Éviter la complexité (pointeurs, mémoire manuelle, GC lourd).</li>
          <li>Apprendre progressivement&nbsp;: immutabilité, <code>Result</code>, <code>match</code>.</li>
        </ul>
        <p><strong>Développeur intermédiaire</strong></p>
        <ul>
          <li>Scripts rapides et compacts pour automatiser des tâches.</li>
          <li>Petites applications CLI portables.</li>
          <li>Formateur, linter et tests intégrés.</li>
        </ul>
        <p><strong>Professionnel</strong></p>
        <ul>
          <li>Compilation native optimisée (<code>-O3</code>).</li>
          <li>FFI C stable pour réutiliser des bibliothèques existantes.</li>
          <li>Intégration CI/CD, packaging, inspection IR/bytecode (<code>--emit-ir</code>, <code>--emit-bytecode</code>).</li>
        </ul>
      </section>

      <section id="domaines">
        <h3>0.3 Domaines d'utilisation</h3>
        <ol>
          <li><strong>Écriture rapide de scripts CLI</strong> : remplacement ciblé de Bash/Python, automatisation, parsing, texte.</li>
          <li><strong>Compilation en exécutables natifs</strong> : binaires autonomes, distribution, embarqué, multi-plateformes.</li>
          <li><strong>Langage embarqué (FFI C)</strong> : intégration dans des projets C, chargement de <code>.vitbc</code>, scripts extensibles.</li>
        </ol>
      </section>

      <section id="points-forts">
        <h3>0.4 Points forts</h3>
        <ul>
          <li>Syntaxe lisible et unifiée (identique à Vitte).</li>
          <li>Gestion mémoire <code>Rc</code>/<code>Weak</code> simple et prévisible.</li>
          <li>Erreurs explicites <code>Result</code> et propagation <code>?</code>.</li>
          <li>Stdlib couvrant I/O, fichiers, chaînes, maths, temps, vecteurs.</li>
          <li>Outils intégrés : <code>fmt</code>, <code>check</code>, <code>test</code>, <code>doc</code>.</li>
          <li>Exécution directe ou compilation native optimisée.</li>
          <li>FFI C stable et facile.</li>
        </ul>
      </section>

      <section id="hello">
        <h3>0.5 Exemple rapide (Hello World)</h3>
<pre><code>module app.main
import std.io

fn main() -&gt; i32 {
  std.io::println("Bonjour Vitte Light")
  return 0
}
</code></pre>
        <p><strong>Exécution</strong> : <code>vitl run src/main.vitl</code></p>
        <p><strong>Compilation</strong> : <code>vitl build -O2 -o build/app src/main.vitl &amp;&amp; ./build/app</code></p>
        <p class="note"><strong>Résumé</strong> — Débutant : apprendre sans complexité inutile · Intermédiaire : scripts compacts et portables · Pro : compilateur performant, FFI C stable, toolchain complète.</p>
      </section>
    </section>

    <hr />

    <!-- 1) STRUCTURE -->
    <section id="structure">
      <h2>1) Structure de projet — Guide complet</h2>
      <p>Un projet VITL repose sur une organisation stricte des dossiers et fichiers pour faciliter navigation, compilation et intégration (CMake, Meson, CI/CD, IDE).</p>

      <h3>1.1 Arborescence type minimale</h3>
<pre><code>/src   → code source principal (.vitl)
 /libs → bibliothèques natives C/FFI (.c, .so, .dll, .a)
 /build→ binaires, artefacts intermédiaires, bytecode (.vitbc)

mon_projet/
  src/
    main.vitl
    util/str.vitl
  libs/
    mylib.c
    mylib.a
  build/
    app        (binaire natif)
    app.vitbc  (bytecode)
</code></pre>

      <h3>1.2 Convention des modules</h3>
      <ul>
        <li>Un fichier = un module.</li>
        <li>Chemin disque = nom de module.</li>
        <li>Nom en <code>snake.case</code>.</li>
      </ul>
<pre><code>Fichier : src/std/io.vitl
Contenu : module std.io
</code></pre>
      <p>Imports clairs : <code>import std.io</code>, <code>import util.str</code>. Évite les conflits, favorise la réutilisation.</p>

      <h3>1.3 Organisation recommandée</h3>
<pre><code>/src
  app/  → logique applicative
  std/  → std locale/complémentaire
  util/ → utilitaires internes
/libs
  externe/ → wrappers C tiers
  interne/ → code C maison via FFI
/build
  debug/   → -O0 -g
  release/ → -O3
  docs/    → `vitl doc`
</code></pre>

      <h3>1.4 Raison d’être</h3>
      <p>Débutant&nbsp;: lisibilité · Intermédiaire&nbsp;: séparation claire, tests faciles · Pro&nbsp;: packaging, CI/CD, FFI propre.</p>

      <h3>1.5 Variantes de structure</h3>
<pre><code>Projet simple : /src/main.vitl

Projet moyen :
  /src main.vitl  cli/args.vitl  math/geom.vitl
  /libs fastmath.c

Projet complexe :
  /src app/main.vitl  net/http.vitl  net/tcp.vitl  core/error.vitl  config.vitl
  /tests test_math.vitl  test_net.vitl
  /libs/sqlite/sqlite3.c
  /build/{debug,release}
  /docs
</code></pre>

      <h3>1.6 Conseils pratiques</h3>
      <ul>
        <li><code>module chemin.nom</code> en tête de fichier, puis les <code>import</code>.</li>
        <li><code>/src</code> pour VITL, <code>/libs</code> pour C, <code>/build</code> pour binaires.</li>
        <li><code>/tests</code> dédié si le projet grandit.</li>
        <li>Ne pas mélanger artefacts et sources.</li>
      </ul>

      <p class="card"><strong>Résumé</strong> — <code>/src</code> : VITL · <code>/libs</code> : FFI · <code>/build</code> : binaires/docs · Nom module = chemin · Rigueur = lisibilité/CI.</p>
    </section>

    <hr />

    <!-- 2) CLI -->
    <section id="cli">
      <h2>2) Outils en ligne de commande — Guide complet</h2>
      <p>Selon la distribution :</p>
      <ul>
        <li><strong>Binaire unique</strong> <code>vitl</code> (compilation, VM, <code>fmt</code>, <code>check</code>, <code>test</code>, <code>doc</code>).</li>
        <li><strong>Couple</strong> <code>vitlc</code> (compilateur) + <code>vitlv</code> (VM) pour séparer build/exécution.</li>
      </ul>

      <h3>2.1 Exécution immédiate (VM/JIT)</h3>
<pre><code>vitl run src/main.vitl
# ou
vitlv run src/main.vitl
</code></pre>
      <ul>
        <li>Compile en bytecode puis exécute dans la VM (vérifs sécurité/syntaxe).</li>
        <li>Idéal pour tester vite, prototyper, CI sans build.</li>
        <li>Erreur fréquente : oublier le chemin (affiche l’aide).</li>
      </ul>

      <h3>2.2 Compilation en exécutable natif</h3>
<pre><code>vitl build -O2 -o build/app src/main.vitl
# ou
vitlc -O2 -o build/app src/main.vitl
</code></pre>
      <ul>
        <li>Binaire natif, autonome, rapide.</li>
      </ul>

      <h3>2.3 Outils supplémentaires</h3>
<pre><code>vitl fmt src/             # formateur
vitl check src/           # analyse statique
vitl test                 # exécute les blocs `test "nom" { ... }`
vitl doc src/ -o build/docs.txt  # documentation à partir de ///</code></pre>

      <h3>2.4 Options utiles</h3>
      <ul>
        <li><code>-O0</code>, <code>-O1</code>, <code>-O2</code> (défaut), <code>-O3</code></li>
        <li><code>-g</code> pour debug/backtrace</li>
        <li><code>--emit-ir</code>, <code>--emit-bytecode</code></li>
      </ul>
<pre><code>vitl build -O0 -g -o build/app src/main.vitl &amp;&amp; ./build/app</code></pre>

      <h3>2.5 Bonnes pratiques CLI</h3>
      <ul>
        <li>Débutant : <code>vitl run</code> + <code>vitl fmt</code>.</li>
        <li>Intermédiaire : <code>vitl build -O2 -g</code> + <code>vitl check</code>.</li>
        <li>Pro : <code>test</code>/<code>check</code> en CI, livrer <code>-O3</code>, générer la doc.</li>
      </ul>

      <h3>2.6 Erreurs fréquentes</h3>
      <ul>
        <li><strong>E0001</strong> fichier introuvable.</li>
        <li><strong>Permission denied</strong> : penser à <code>chmod +x</code>.</li>
        <li>Ordre d’options : <code>-o build/app</code> après <code>build</code>.</li>
      </ul>
    </section>

    <hr />

    <!-- 3) SYNTAXE -->
    <section id="syntaxe">
      <h2>3) Syntaxe : découverte progressive</h2>
      <p><strong>But</strong> — Apprendre les briques de base de VITL, comprendre la logique, éviter les erreurs classiques.</p>

      <h3>3.1 Commentaires</h3>
      <ul>
        <li>Ligne : <code>//</code> jusqu’à la fin de ligne.</li>
        <li>Bloc : <code>/* ... */</code> multi-lignes.</li>
      </ul>
<pre><code>// commentaire
let x = 42  // commentaire en fin de ligne

/* commentaire multi-lignes */
</code></pre>

      <h3>3.2 Variables et constantes</h3>
<pre><code>const PI: f64 = 3.14159
let mut compteur: i32 = 0
let message = "Salut"  // inférence: str

// Erreur : variable non mutable
let n = 0
n = n + 1

// Correct
let mut n = 0
n = n + 1
</code></pre>

      <h3>3.3 Fonctions</h3>
<pre><code>fn add(a: i32, b: i32) -&gt; i32 { return a + b }

// variante
fn square(x: i32) -&gt; i32 { return x * x }

// Erreur : manque le type de retour
fn bad(x: i32) { return x * x }
</code></pre>

      <h3>3.4 Structures de contrôle</h3>
<pre><code>if x &gt; 0 { println("positif") } else { println("non positif") }

for i in 0..10 { println(i.to_string()) }  // 0..9
for i in 0..=10 { println(i.to_string()) } // 0..10

let mut n = 0
while n &lt; 5 { println(n.to_string()); n = n + 1 }
</code></pre>

      <h3>3.5 <code>match</code> (pattern matching)</h3>
<pre><code>match valeur {
  0 =&gt; println("zéro"),
  1 | 2 =&gt; println("petit"),
  _ =&gt; println("autre"),
}

enum Status { Ok, Err(str) }
let s = Status::Err("oops")
match s {
  Status::Ok =&gt; println("ok"),
  Status::Err(msg) =&gt; println("erreur:" + msg),
}
</code></pre>

      <h3>3.6 Types de base et littéraux</h3>
      <ul>
        <li>Entiers : <code>0</code>, <code>42</code>, <code>1_000</code>, <code>0xFF</code>, <code>0o755</code>, <code>0b1010</code></li>
        <li>Flottants : <code>3.14</code>, <code>2.0e-3</code></li>
        <li>Booléens : <code>true</code>, <code>false</code></li>
        <li>Caractères : <code>'a'</code>, <code>'\n'</code></li>
        <li>Chaînes : <code>"..."</code>, <code>r"..."</code></li>
      </ul>

      <h3>3.7 Bonnes pratiques de style</h3>
      <p>Débutant : <code>let</code> + <code>println</code> · Intermédiaire : <code>assert</code>, <code>match</code> exhaustif · Pro : conversions explicites avec <code>as</code>.</p>

      <p class="card"><strong>Résumé</strong> — Commentaires <code>//</code>/<code>/*...*/</code> · variables immuables par défaut (<code>mut</code> si besoin) · fonctions <code>fn</code> · contrôles <code>if</code>/<code>while</code>/<code>for</code> · <code>match</code> exhaustif.</p>
    </section>

    <hr />

    <!-- 4) EXEMPLES -->
    <section id="exemples">
      <h2>4) Premiers exemples — détaillés</h2>
      <p><strong>Objectif</strong> — Écrire, exécuter, compiler et tester trois programmes types, avec erreurs fréquentes et variantes.</p>
      <p><strong>Pré-requis</strong> — Arbo conseillée : <code>/src</code> avec <code>main.vitl</code>, <code>cat.vitl</code>, <code>geom.vitl</code>.</p>

      <h3>4.1 Débutant → programme minimal</h3>
<pre><code>// src/main.vitl
module app.main
import std.io

fn main() -&gt; i32 {
  std.io::println("Bonjour Vitte Light")
  return 0
}
</code></pre>
<pre><code># Exécution (VM)
vitl run src/main.vitl

# Compilation (binaire)
vitl build -O2 -o build/hello src/main.vitl
./build/hello

# Sortie attendue
Bonjour Vitte Light
</code></pre>
      <ul>
        <li>E0001 symbole inconnu : oublier <code>import std.io</code>.</li>
        <li>E0002 types incompatibles : concaténer nombre + string sans <code>.to_string()</code>.</li>
      </ul>
<pre><code>// Variante A
fn main() -&gt; i32 {
  let n = 42
  std.io::println("n=" + n.to_string())
  return 0
}

// Variante B (codes d'erreur)
fn main() -&gt; i32 {
  if 2 + 2 != 4 {
    std.io::eprintln("arithmétique cassée")
    return 1
  }
  return 0
}
</code></pre>

      <h3>4.2 Intermédiaire → lire un fichier</h3>
<pre><code>// src/cat.vitl
module app.cat
import std.{fs, io, cli, str}

fn main() -&gt; i32 {
  let argv = cli::args()
  if str::len(argv) &lt; 2 {
    io::eprintln("usage: cat &lt;fichier&gt;")
    return 2
  }
  let path = argv[1]
  if !fs::exists(path) {
    io::eprintln("introuvable:" + path)
    return 3
  }
  let contenu = fs::read_to_string(path)?  // propage l'erreur I/O
  io::print(contenu)
  return 0
}
</code></pre>
<pre><code># Exécution
vitl run src/cat.vitl -- data.txt
</code></pre>
      <ul>
        <li>OK : affiche le contenu.</li>
        <li>Erreur d’usage : code 2.</li>
        <li>Fichier manquant : code 3.</li>
      </ul>
<pre><code>// Variante A (binaire)
let bytes = fs::read(path)?
io::println("taille=" + bytes.len().to_string())

// Variante B (chrono)
let t0 = std.time::now()
let contenu = fs::read_to_string(path)?
io::println("ms=" + (std.time::now() - t0).to_string())

// Variante C (multi-fichiers)
for i in 1..str::len(argv) {
  let p = argv[i]
  if !fs::exists(p) { io::eprintln("skip:" + p); continue }
  io::print(fs::read_to_string(p)?)
}
</code></pre>

      <h3>4.3 Pro → struct, impl, méthodes</h3>
<pre><code>// src/geom.vitl
module app.geom
import std.{io, math}

struct Vec2 { x: f64, y: f64 }

impl Vec2 {
  fn norm(&amp;self) -&gt; f64 { return (self.x*self.x + self.y*self.y).sqrt() }
  fn dot(&amp;self, o: Vec2) -&gt; f64 { return self.x*o.x + self.y*o.y }
}

fn main() -&gt; i32 {
  let v = Vec2 { x: 3.0, y: 4.0 }
  io::println(v.norm().to_string()) // 5
  return 0
}

// Tests intégrés
test "norm classique 3-4-5" {
  let v = Vec2 { x:3.0, y:4.0 }
  assert(v.norm() == 5.0)
}
test "dot produit orthogonal" {
  let a = Vec2 { x:1.0, y:0.0 }
  let b = Vec2 { x:0.0, y:1.0 }
  assert(a.dot(b) == 0.0)
}
</code></pre>
<pre><code>// Variante A (parse CLI)
import std.{cli, str}
fn parse_f64(s: str) -&gt; Result&lt;f64, str&gt; { return str::to_float(s) }
fn main() -&gt; i32 {
  let a = cli::args()
  if str::len(a) != 3 {
    io::eprintln("usage: geom &lt;x&gt; &lt;y&gt;"); return 2
  }
  let x = parse_f64(a[1])?
  let y = parse_f64(a[2])?
  let v = Vec2 { x:x, y:y }
  io::println(v.norm().to_string())
  return 0
}

// Variante B (mini-bench)
fn bench(n: i32) -&gt; i32 {
  let mut acc:f64 = 0.0
  let base = Vec2 { x:1.234, y:5.678 }
  for i in 0..n {
    let v = Vec2 { x: base.x + (i as f64), y: base.y - (i as f64) }
    acc = acc + v.norm()
  }
  std.io::println("acc=" + acc.to_string())
  return 0
}
fn main() -&gt; i32 {
  let t0 = std.time::now()
  _ = bench(100000)
  std.io::println("ms=" + (std.time::now()-t0).to_string())
  return 0
}
</code></pre>

      <h3>4.4 Bonnes pratiques transverses</h3>
      <ul>
        <li>Style : <code>module</code> en tête, imports groupés, <code>snake_case</code> pour fonctions/variables, <code>CamelCase</code> pour types.</li>
        <li>Erreurs : <code>?</code> pour I/O et parsing, codes de sortie (0,1,2,3,4).</li>
        <li>Organisation : un fichier = un module; tests en fin de fichier.</li>
        <li>Perf : <code>time::now()</code>, slices <code>[T]</code>, <code>vec::with_capacity</code>.</li>
        <li>Sécurité : pas d’<code>unsafe</code> hors FFI; utiliser <code>CString</code> pour le C.</li>
      </ul>
    </section>

    <hr />

    <!-- 5) MEMOIRE -->
    <section id="memoire">
      <h2>5) Mémoire et sécurité — guide détaillé</h2>

      <h3>5.1 Modèle mémoire en mode sûr</h3>
      <ul>
        <li>Pas de GC complexe, gestion via références comptées <code>Rc&lt;T&gt;</code>.</li>
        <li>Libération immédiate quand le compteur tombe à zéro.</li>
      </ul>
<pre><code>let a = Rc&lt;int&gt;::new(42)
let b = a.clone()  // forts = 2
a.drop()           // forts = 1
b.drop()           // forts = 0 → libération
</code></pre>
      <p>Avantages : simplicité, déterminisme, sécurité. Limites : cycles <em>forts</em> non collectés → utiliser <code>Weak</code>.</p>

      <h3>5.2 Immutabilité et mutabilité</h3>
<pre><code>// str immuable ; String mutable
let mut s = String::from("abc")
s.push("d")  // "abcd"
</code></pre>

      <h3>5.3 Collections et slices</h3>
<pre><code>let v:[i32] = [1,2,3,4]
for x in &amp;v { io::println(x.to_string()) }
</code></pre>
      <p>Pré-allouer avec <code>vec::with_capacity</code> quand la taille est connue.</p>

      <h3>5.4 Mode <code>unsafe</code> et pointeurs bruts</h3>
<pre><code>extern "C" { fn puts(msg:*const char)-&gt;i32 }
let cstr = std.c::CString::from_str("hi\n")?
unsafe { puts(cstr.as_ptr()) }
</code></pre>
      <p>Encapsuler l’<code>unsafe</code> dans des fonctions VITL sûres.</p>

      <h3>5.5 Par niveau</h3>
      <p>Débutant : pas de <code>free()</code>, éviter <code>unsafe</code>. · Intermédiaire : <code>Rc/Weak</code>, slices, <code>str</code> vs <code>String</code>. · Pro : graphes via <code>Weak</code>, FFI encapsulé, allocateurs custom côté C si besoin.</p>

      <h3>5.6 Erreurs courantes</h3>
      <ul>
        <li>Cycles <code>Rc</code> → fuite.</li>
        <li>Passer une <code>String</code> temporaire au C → pointeur dangling.</li>
        <li>Oublier <code>mut</code> → erreur compilation.</li>
        <li><code>unsafe</code> non encapsulé.</li>
      </ul>

      <h3>5.7 Récapitulatif</h3>
      <p>Gestion sûre via <code>Rc/Weak</code> · pas de GC · <code>str</code> immuable / <code>String</code> mutable · slices évitent les copies · <code>unsafe</code> réservé au FFI.</p>
    </section>

    <hr />

    <!-- 6) ERREURS -->
    <section id="erreurs">
      <h2>6) Gestion des erreurs</h2>
      <p>Type standard : <code>Result&lt;T, E&gt;</code> avec <code>Ok(T)</code> / <code>Err(E)</code>. Propagation avec <code>?</code>.</p>
<pre><code>let contenu = fs::read_to_string("config.txt")?
</code></pre>
<pre><code>match fs::read_to_string("config.txt") {
  Result::Ok(txt) =&gt; println(txt),
  Result::Err(e)  =&gt; eprintln(e),
}
</code></pre>
      <p><code>panic("msg")</code> : erreur irrécupérable, arrêt du programme.</p>
    </section>

    <hr />

    <!-- 7) FFI -->
    <section id="ffi">
      <h2>7) Interopérabilité C (FFI) — guide avancé</h2>
      <p>But — Appeler du code C en sécurité, définir un shim C portable, gérer chaînes/buffers/erreurs/édition de liens.</p>

      <h3>7.1 Déclaration et appel minimal</h3>
<pre><code>extern "C" { fn puts(msg: *const char) -&gt; i32 }
let cstr = std.c::CString::from_str("Hello C!\n")?
unsafe { _ = puts(cstr.as_ptr()) }

// lien à la libc (ex.)
vitl build -O2 -L libs -lc -o build/app src/main.vitl
</code></pre>

      <h3>7.2 Chaînes et ownership</h3>
      <ul>
        <li>Entrée VITL→C : construire via <code>CString::from_str</code>, conserver la durée de vie.</li>
        <li>Retour C→VITL : copier si buffer interne, sinon libérer avec l’allocateur documenté (ex. <code>c::free</code>).</li>
      </ul>

      <h3>7.3 Buffers binaires et slices</h3>
<pre><code>extern "C" { fn sha256(data:*const u8, len:usize, out:*mut u8) -&gt; i32 }
let mut out:[u8] = [0; 32]
unsafe { _ = sha256(input.as_ptr(), input.len(), out.as_mut_ptr()) }
</code></pre>

      <h3>7.4 Structs : opacité</h3>
<pre><code>// C (shim)
typedef struct Obj Obj;
Obj* obj_create(int cap);
void obj_destroy(Obj*);
int  obj_push(Obj*, const char*);
int  obj_len(const Obj*);

// VITL
extern "C" {
  fn obj_create(cap:i32)-&gt;*mut void
  fn obj_destroy(p:*mut void)-&gt;void
  fn obj_push(p:*mut void, s:*const char)-&gt;i32
  fn obj_len(p:*const void)-&gt;i32
}
</code></pre>

      <h3>7.5 Erreurs et mapping <code>Result</code></h3>
<pre><code>fn c_call_ok(code:i32) -&gt; Result&lt;(),str&gt; {
  if code == 0 { return Result::Ok(()) }
  return Result::Err("ffi: call failed")
}
</code></pre>

      <h3>7.6 Callbacks et <code>user_data</code></h3>
      <p>Gérer les callbacks dans le shim C, VITL fournit un identifiant opaque.</p>

      <h3>7.7 Linking : <code>-L</code>, <code>-l</code>, rpath, static</h3>
<pre><code># Dylib partagée
vitl build -O2 -L libs -lfoo -o build/app src/main.vitl

# Statique (si dispo)
vitl build -O2 -L libs -Wl,-Bstatic -lfoo -Wl,-Bdynamic -o build/app src/main.vitl

# rpath
vitl build ... -Wl,-rpath,/chemin/vers/libs
</code></pre>

      <h3>7.8 Cross-plateforme</h3>
      <p>Types stables (<code>int32_t</code>, <code>uint64_t</code>, <code>size_t</code>), éviter <code>long</code>, attention à l’alignement.</p>

      <h3>7.9 Sécurité mémoire : règles d’or</h3>
      <ul>
        <li>Bornes vérifiées, pas de double free.</li>
        <li>Chaînes null-terminées côté C.</li>
        <li>Ne pas garder de pointeur C vers une <code>String</code> temporaire.</li>
      </ul>

      <h3>7.10 Exemple complet : wrapper « safe »</h3>
<pre><code>// C (shim libfoo)
typedef struct Foo Foo;
Foo* foo_create(int cap); void foo_destroy(Foo*);
int foo_push(Foo*, const char*); int foo_len(const Foo*);

// VITL
extern "C" {
  fn foo_create(cap:i32)-&gt;*mut void
  fn foo_destroy(h:*mut void)-&gt;void
  fn foo_push(h:*mut void, s:*const char)-&gt;i32
  fn foo_len(h:*const void)-&gt;i32
}
struct Foo { handle:*mut void }
fn Foo::new(cap:i32) -&gt; Result&lt;Foo,str&gt; {
  let h = unsafe { foo_create(cap) }
  if h == std.ptr::null_mut() { return Result::Err("ffi: create failed") }
  return Result::Ok(Foo{ handle:h })
}
fn Foo::push(&amp;mut self, s:str) -&gt; Result&lt;(),str&gt; {
  let cstr = std.c::CString::from_str(s)?
  let rc = unsafe { foo_push(self.handle, cstr.as_ptr()) }
  if rc != 0 { return Result::Err("ffi: push failed") }
  return Result::Ok(())
}
fn Foo::len(&amp;self) -&gt; i32 { return unsafe { foo_len(self.handle) } }
fn Foo::close(&amp;mut self) -&gt; () {
  if self.handle != std.ptr::null_mut() {
    unsafe { foo_destroy(self.handle) }
    self.handle = std.ptr::null_mut()
  }
}
</code></pre>

      <h3>7.11 Stratégie d’équipe et CI</h3>
      <p>Figer l’interface C, tests d’intégration, builder shim + exécutable, publier artefacts (lib+header+sha), documenter rpath/paths.</p>

      <h3>7.12 Quand FFI n’est pas le bon outil</h3>
      <p>Regrouper les appels (batch), formats binaires compacts, runtime externe pour threads natifs.</p>

      <p class="card"><strong>Résumé</strong> — ABI C, handles opaques, <code>CString</code>, <code>ptr+len</code>, <code>Result</code>, surface <code>unsafe</code> minimale.</p>
    </section>

    <hr />

    <!-- 8) STDLIB -->
    <section id="stdlib">
      <h2>8) Stdlib — aperçu détaillé</h2>
      <p>Convention — Fonctions système retournent <code>Result&lt;...&gt;</code>, <code>?</code> propage. Extraits supposent : <code>import std.{io, fs, str, math, time, cli, vec, debug, err, c}</code>.</p>

      <h3>8.1 std.io — E/S console</h3>
      <p>API : <code>print</code>, <code>println</code>, <code>eprint</code>, <code>eprintln</code>.</p>
<pre><code>io::println("Hello")
io::eprintln("fatal: config manquante")
</code></pre>

      <h3>8.2 std.fs — Fichiers et chemins</h3>
<pre><code>let cfg = fs::read_to_string("config.toml")?
_ = fs::write_string("out.txt", "ok\n")?
if !fs::exists("data.bin") { io::eprintln("absent: data.bin"); return 3 }
</code></pre>

      <h3>8.3 std.str — Chaînes et utilitaires</h3>
<pre><code>let n = str::len("abc")     // 3
let parts = str::split("a b c", ' ')
let idx = str::find("abc","b") // 1
let out = str::replace("a_b","_","-") // "a-b"
let v = str::to_int("42")?   // i64
</code></pre>

      <h3>8.4 std.math — Maths</h3>
<pre><code>let d = math::sqrt(3.0*3.0 + 4.0*4.0) // 5.0
let a = math::abs(-12) // 12
</code></pre>

      <h3>8.5 std.time — Horloge</h3>
<pre><code>let t0 = time::now()
heavy()
io::println((time::now() - t0).to_string() + " ms")
</code></pre>

      <h3>8.6 std.cli — Arguments/env</h3>
<pre><code>let argv = cli::args()
if str::len(argv) &lt; 2 {
  io::eprintln("usage: app &lt;file&gt;"); return 2
}
match cli::env("HOME") {
  Some(h) =&gt; io::println(h),
  None =&gt; io::eprintln("HOME non défini"),
}
</code></pre>

      <h3>8.7 std.vec — Vecteurs</h3>
<pre><code>let mut v:[i32] = vec::with_capacity(16)
v.push(1); v.push(2)
for x in &amp;v { io::println(x.to_string()) }
match v.pop() { Some(x) =&gt; io::println("last="+x.to_string()), None =&gt; {} }
</code></pre>

      <h3>8.8 std.debug — Assertions</h3>
<pre><code>debug::assert(sum([1,2,3]) == 6)
debug::dump("state=INIT")
</code></pre>

      <h3>8.9 std.err — Erreurs</h3>
      <p><code>err::Error</code> regroupe catégorie/message/contexte. Mapping vers codes de sortie conseillé.</p>

      <h3>8.10 std.c — Pont C</h3>
<pre><code>extern "C" { fn puts(msg:*const char) -&gt; i32 }
let cmsg = c::CString::from_str("Hello\n")?
unsafe { _ = puts(cmsg.as_ptr()) }
</code></pre>

      <h3>8.11 Patterns composés</h3>
<pre><code>fn load_threshold(path: str) -&gt; Result&lt;i64, err::Error&gt; {
  let s = fs::read_to_string(path)?
  match str::to_int(str::replace(s, "\n", "")) {
    Result::Ok(v) =&gt; return Result::Ok(v),
    Result::Err(_) =&gt; return Result::Err(err::Error::new("parse int échoué")),
  }
}
</code></pre>
<pre><code>fn timed&lt;F&gt;(label: str, f: F) -&gt; ()
where F: fn()-&gt;() {
  let t0 = time::now()
  f()
  io::println(label + ":" + (time::now()-t0).to_string() + " ms")
}
</code></pre>

      <h3>8.12 Conseils par niveau</h3>
      <p>Débutant : io/fs/str/cli ; vérifier <code>exists</code>. · Intermédiaire : <code>Result</code> structuré, mesures. · Pro : shims C, <code>err::Error</code> centralisé, logs mesurés.</p>
    </section>

    <hr />

    <!-- 9) STYLE -->
    <section id="style">
      <h2>9) Style et bonnes pratiques — guide détaillé</h2>

      <h3>9.1 Structure générale des fichiers</h3>
      <ol>
        <li><strong>module</strong> en tête (ex. <code>module app.math.linear</code> pour <code>src/math/linear.vitl</code>).</li>
        <li>Imports regroupés immédiatement après (par hiérarchie).</li>
        <li>Ensuite : constantes → types → impl → fonctions → tests.</li>
      </ol>

      <h3>9.2 Nommage</h3>
<pre><code>let user_name = "Alice"
fn compute_area(w:i32, h:i32) -&gt; i32 { return w*h }

struct Rectangle { width:i32, height:i32 }
enum Status { Ok, Failed }
</code></pre>

      <h3>9.3 Constantes et immutabilité</h3>
      <p>Toujours <code>const</code> pour les valeurs fixes ; immuable par défaut, <code>mut</code> au besoin.</p>
    </section>

    <hr />

    <!-- 10) NIVEAUX -->
    <section id="niveaux">
      <h2>10) Conseils par niveau</h2>
      <p><strong>Débutant</strong> : <code>println</code>, programmes &lt; 50 lignes, <code>vitl fmt</code>.</p>
      <p><strong>Intermédiaire</strong> : <code>test</code>, <code>std.fs</code>, maîtriser <code>?</code>.</p>
      <p><strong>Pro</strong> : FFI C, <code>-O3</code>, <code>--emit-ir</code>, CI/CD.</p>
    </section>

    <hr />

    <!-- 11) LIMITES -->
    <section id="limites">
      <h2>11) Limites (version Light) — version détaillée</h2>
      <p>Résumé : pas de threads natifs · pas de GC complet (Rc/Weak) · génériques partiels · FFI limité au C.</p>

      <h3>11.1 Pas de threads natifs</h3>
      <p>Stratégies : séquencer, multi-processus via FFI, pipelines, runtime externe.</p>

      <h3>11.2 Pas de GC complet (Rc/Weak)</h3>
      <p>Éviter les cycles forts, utiliser <code>Weak</code> pour liens retour, auditer les graphes.</p>

      <h3>11.3 Génériques partiels</h3>
      <p>Favoriser types concrets, interfaces de données, déporter la généricité lourde via FFI si critique.</p>

      <h3>11.4 FFI limité au C (ABI C)</h3>
      <p>Shim C requis pour C++/Rust/Go, API plate, handles opaques, responsabilités d’allocation documentées.</p>

      <h3>11.5 Décider quand « sortir »</h3>
      <p>Algorithme suffisant → rester VITL ; contraintes structurelles → isoler via lib externe + FFI.</p>

      <h3>11.6 Contrats de qualité</h3>
      <p>Tests (succès/erreur), logs contextualisés, docs <code>///</code>, micro-benchs I/O/FFI.</p>
    </section>

    <hr />

    <!-- 12) CODES -->
    <section id="codes-sortie">
      <h2>12) Codes de sortie</h2>
      <ul>
        <li><strong>0</strong> → succès</li>
        <li><strong>1</strong> → erreur générique/panic</li>
        <li><strong>2</strong> → erreur d’usage CLI</li>
        <li><strong>3</strong> → erreur I/O</li>
        <li><strong>4</strong> → erreur FFI</li>
      </ul>
    </section>

    <hr />

    <!-- 13) DIAGNOSTICS -->
    <section id="diagnostics">
      <h2>13) Diagnostics courants — version détaillée</h2>
      <p><strong>Outils</strong> : <code>vitl check</code> · <code>vitl build -g -O0</code> · <code>--emit-ir</code>/<code>--emit-bytecode</code> · <code>std.debug::backtrace()</code> · <code>vitl doc</code></p>

      <h3>E0001 : symbole inconnu</h3>
<pre><code>// Mauvais
module app.main
fn main()-&gt;i32 {
  std.io::pritnln("hi")
  return 0
}

// Correct
module app.main
import std.io
fn main()-&gt;i32 {
  std.io::println("hi")
  return 0
}
</code></pre>

      <h3>E0002 : types incompatibles</h3>
<pre><code>// Mauvais
let n:i32 = 3
let x:f64 = 0.5
let y = n + x

// Correct (cast local)
let n:i32 = 3
let x:f64 = 0.5
let y = (n as f64) + x

// Chaînes
std.io::println("n=" + n)           // E0002
std.io::println("n=" + n.to_string()) // OK
</code></pre>

      <h3>E0003 : variable non initialisée</h3>
<pre><code>// Mauvais
let mut acc:i32
if cond { acc = 1 }
std.io::println(acc.to_string())

// Correct
let mut acc:i32 = 0
if cond { acc = 1 }
std.io::println(acc.to_string())
</code></pre>

      <h3>E1001 : FFI non-safe hors <code>unsafe</code></h3>
<pre><code>// Mauvais
extern "C" { fn puts(msg:*const char)-&gt;i32 }
fn main()-&gt;i32 {
  let s = std.c::CString::from_str("Hi\n")
  puts(s.as_ptr())  // E1001
  return 0
}

// Correct
extern "C" { fn puts(msg:*const char)-&gt;i32 }
fn main()-&gt;i32 {
  let s = std.c::CString::from_str("Hi\n")
  unsafe { _ = puts(s.unwrap().as_ptr()) }
  return 0
}
</code></pre>

      <h3>E2001 : fichier introuvable (I/O)</h3>
<pre><code>// Correct
let path = "data/config.txt"
if !std.fs::exists(path) {
  std.io::eprintln("config manquante:" + path)
  return 3
}
let txt = std.fs::read_to_string(path)?
</code></pre>

      <h3>Annexe A — Diagnostics supplémentaires</h3>
      <ul>
        <li>E0100 : variable non utilisée → supprimer ou préfixer <code>_</code>.</li>
        <li>E0101 : code inatteignable → retirer ou factoriser.</li>
        <li>E0201 : division par zéro détectable.</li>
        <li>E0301 : dépassement d’index → tester <code>i &lt; v.len()</code> ou <code>get(i)</code>.</li>
        <li>E0401 : <code>match</code> non exhaustif.</li>
        <li>E0501 : conflit de mutabilité.</li>
        <li>E0601 : format de chaîne invalide (manque <code>to_string()</code>).</li>
        <li>E0701 : échec FFI (code &lt; 0).</li>
        <li>E0801 : UTF-8 invalide en lecture texte.</li>
      </ul>

      <h3>Annexe B — Stratégie de triage</h3>
      <ol>
        <li>Lire le message complet et la ligne.</li>
        <li><code>vitl check</code> pour lints.</li>
        <li>Vérifier <code>module</code>/<code>import</code>.</li>
        <li>Normaliser/caster les types.</li>
        <li>Initialiser à la déclaration / branches exhaustives.</li>
        <li>FFI : <code>unsafe</code> + pointeurs/durées validés.</li>
        <li>I/O : <code>exists</code>, chemins, droits.</li>
        <li>Rebuild <code>-g</code> + backtrace.</li>
        <li>Tests minimaux pour reproduire.</li>
        <li>Doc <code>///</code> cause + correctif.</li>
      </ol>

      <h3>Annexe C — Messages enrichis</h3>
<pre><code>// I/O robuste
let path = argv[1]
if !std.fs::exists(path) {
  std.io::eprintln("E2001: fichier introuvable → " + path)
  std.io::eprintln("Astuce: lancer depuis la racine du projet ou passer un chemin absolu.")
  return 3
}
</code></pre>
<pre><code>// FFI robuste
let msg = std.c::CString::from_str("ok\n")
if msg.is_err() { return Result::Err("E0601: CString invalide (caractère nul)") }
unsafe { _ = puts(msg.unwrap().as_ptr()) }
</code></pre>

      <h3>Annexe D — Prévenir les diagnostics</h3>
      <ul>
        <li>Imports précis et groupés ; <code>vitl fmt</code> fréquent.</li>
        <li>Types explicites aux frontières d’API.</li>
        <li>Tests unitaires par module (succès/erreurs attendues).</li>
        <li>Logs contextualisés avec codes d’erreur stables.</li>
        <li><code>unsafe</code> minimal et documenté.</li>
        <li>Préférer <code>Option</code>/<code>Result</code> aux sentinelles.</li>
        <li>Valider l’entrée tôt ; fail fast.</li>
      </ul>
    </section>

    <hr />

    <!-- 14) CHECKLIST -->
    <section id="checklist">
      <h2>14) Checklist avant de livrer</h2>
      <ul>
        <li>[ ] <code>module app.main</code> défini</li>
        <li>[ ] <code>fn main()-&gt;i32</code> présent</li>
        <li>[ ] imports réduits au nécessaire</li>
        <li>[ ] <code>vitl test</code> passe</li>
        <li>[ ] <code>build/app</code> généré dans <code>/build</code></li>
      </ul>
      <p class="note">FIN DU GUIDE</p>
    </section>

  </main>
</body>
</html>
