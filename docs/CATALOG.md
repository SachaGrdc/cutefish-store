# Cutefish Store — Le catalogue local (Core v0.2)

Le catalogue est l'instantané local des applications que la boutique peut
proposer : un fichier SQLite, reconstruit en bloc à partir des catalogues
AppStream des sources, puis remplacé atomiquement. Ce document décrit ce qui
est implémenté. [ARCHITECTURE.md](ARCHITECTURE.md) (§6) et
[DECISIONS.md](DECISIONS.md) font foi pour les décisions ; [CORE.md](CORE.md)
décrit les modèles du cœur.

Niveau de preuve : **[MESURÉ]** sur la machine d'audit (Debian 13, SQLite
3.46.1, AppStream 1.0.5, libxml2 2.9.14, GCC 14), **[CODE]**,
**[DOC]**, **[À VALIDER]**.

## 1. Architecture

```text
catalogues AppStream                cutefish-store-indexer (processus éphémère)
  DEP-11 YAML (Debian)  ─────────►    store-indexer : libappstream, GLib, libxml2
  XML (Flathub)                       └─► CatalogWriter (store-core, SQLite)
                                              │ fichier candidat
                                              ▼
                                      replaceCatalog() : validation, fsync, rename()
                                              │
                                              ▼
                                       catalog.sqlite ◄── SqliteCatalog (lecteurs, immutable)
```

| Élément | Où | Dépendances | Rôle |
|---|---|---|---|
| `CatalogWriter` | `store-core` (`catalogwriter.h`) | SQLite | Écrit un catalogue candidat : sources, composants, index de recherche, métadonnées |
| `inspectCatalog`, `replaceCatalog` | `store-core` (`catalogfile.h`) | SQLite, POSIX | Valident un fichier et le mettent en service |
| `SqliteCatalog` | `store-core` (`sqlitecatalog.h`) | SQLite | Implémente l'interface `Catalog` en lecture seule |
| `store-indexer` | `src/indexer/` | libappstream, GLib, libxml2, `store-core` | Lit les catalogues AppStream et alimente le `CatalogWriter` |
| `cutefish-store-indexer` | `src/indexer/main.cpp` | `store-indexer` | Exécutable lancé par le service (P-002) ; aussi utilisable à la main |

- **GLib reste dans l'indexeur.** Aucun en-tête de SQLite, de GLib ni de
  libappstream n'apparaît dans l'API publique de `store-core` : SQLite y est
  lié en `PRIVATE`, derrière des classes à implémentation cachée (pimpl).
- **Nouvelles dépendances**, toutes prévues par l'architecture (§12.1) :
  - SQLite ≥ 3.34 avec FTS5 et `trigram`, dans `store-core` ;
  - libappstream ≥ 1.0 dans l'indexeur, qui apporte GLib et GIO ;
  - libxml2 dans l'indexeur, pour lire le XML en flux et convertir les
    descriptions. La bibliothèque libxml2 est une dépendance de
    libappstream, mais **pas ses fichiers de développement** : `libxml2-dev`
    doit être installé à part.
- L'interface `Catalog` du Core v0.1 n'a pas changé. Les modèles non plus,
  à une correction près : une ref Flatpak dont l'ID se termine par
  `.desktop` est acceptée (`app/chat.delta.desktop/…`, publiée par Flathub).

### Dépendances

Paquets Debian 13 nécessaires pour construire le projet et ses tests :

```bash
sudo apt install build-essential cmake ninja-build pkg-config \
    libsqlite3-dev libappstream-dev libxml2-dev libgtest-dev
```

| Paquet | Pour | Version utilisée |
|---|---|---|
| `build-essential`, `cmake`, `ninja-build`, `pkg-config` | Compilation (GCC 14, CMake ≥ 3.22) | 12.12, 3.31.6, 1.12.1, 1.8.1 |
| `libsqlite3-dev` | `store-core` | 3.46.1 |
| `libappstream-dev` (apporte `libglib2.0-dev`) | Indexeur | 1.0.5 |
| `libxml2-dev` | Indexeur | 2.9.14 |
| `libgtest-dev` | Tests (`-DBUILD_TESTING=ON`) | 1.16.0 |

Sans l'indexeur (`-DCUTEFISH_STORE_BUILD_INDEXER=OFF`), `libappstream-dev`
et `libxml2-dev` ne sont pas nécessaires. Si un paquet manque, la
configuration CMake s'arrête en nommant le paquet Debian à installer.

**Comment ces dépendances ont été obtenues** : aucune n'a été contournée.
Les en-têtes de SQLite et d'AppStream manquaient au début du Core v0.2 ; le
propriétaire a installé `libsqlite3-dev` et `libappstream-dev` le 7 novembre
2026 (`apt install libsqlite3-dev libappstream-dev`, journal
`/var/log/apt/history.log`) avant toute compilation, à la demande de cette
étape. `libxml2-dev`, `libglib2.0-dev` et `libgtest-dev` étaient déjà
installés. Les déclarations recopiées à la main des prototypes de la phase
0.3 n'ont jamais été utilisées dans ce dépôt.

**Vérification** [MESURÉ, 7 novembre 2026] : `scripts/check-clean-build.sh`
construit et teste le projet, sans root, dans une vue du système (bubblewrap)
où `/usr/include` et les dossiers pkg-config et CMake ne contiennent que les
fichiers des paquets ci-dessus et de leurs dépendances (203 paquets installés
dans cette fermeture). Résultat : configuration, compilation et 138 tests
réussis. Retirer un seul des paquets `libsqlite3-dev`, `libappstream-dev`,
`libxml2-dev` ou `libgtest-dev` fait échouer la configuration avec le
message attendu. Limite : les bibliothèques d'exécution et les outils
restent ceux de l'hôte ; ce n'est pas une installation Debian vierge, qui
n'a pas pu être testée sans machine virtuelle ni conteneur [À VALIDER].

## 2. Schéma SQLite

Version de schéma 1, aussi écrite dans `PRAGMA user_version` (§7).

| Table | Contenu | Contraintes et index |
|---|---|---|
| `meta` | `schema`, `generation`, `search_locales`, `offers:<source>` (nombre d'offres par source) | `key` clé primaire, `WITHOUT ROWID` |
| `sources` | `source_id`, `backend`, `scope`, `kind`, `origin`, `label`, `url`, `priority`, `enabled` | `source_id` unique ; `CHECK` sur les valeurs énumérées |
| `apps` | Une ligne par application : `app_id` normalisé, `verified` ; `id` sert de `rowid` aux index FTS5 | `app_id` unique |
| `aliases` | Identifiant alternatif → application | `alias` clé primaire, clé étrangère vers `apps`, index sur `app` |
| `app_categories` | (catégorie, application) | Clé primaire composée, index sur `app` |
| `offers` | Une ligne par composant AppStream importé : `offer_id`, application, source, `component_id` d'origine, `format`, `package`, `version`, `architecture`, `developer`, icône | Unique sur (`app`, `offer_id`) ; clés étrangères vers `apps` et `sources` ; index sur `source` |
| `texts` | Textes d'une offre : (offre, champ, langue) → valeur. Champs : nom, résumé, description, mots-clés | Clé primaire (`offer`, `field`, `locale`), `WITHOUT ROWID` ; index partiel `value COLLATE NOCASE WHERE field = 0` pour les noms |
| `search` | FTS5 sans contenu (`content=''`) : `name`, `keywords`, `summary`, `description`, `developer` ; `unicode61 remove_diacritics 2`, `prefix='2 3'` | `rowid` = `apps.id` |
| `search_names` | FTS5 sans contenu, `trigram remove_diacritics 1`, noms seulement | `rowid` = `apps.id` |

Choix :

- **Les textes appartiennent aux offres**, pas aux applications : chaque
  source garde son nom, son résumé, sa description et son icône. Rien n'est
  écrasé à la fusion (prototype 3 : les descriptions diffèrent pour 59 % des
  applications présentes dans les deux sources).
- **Descriptions** : stockées en blocs (paragraphe, élément de liste,
  élément de liste numérotée) dans une seule valeur, séparés par U+001E ;
  chaque bloc commence par un marqueur de ponctuation et U+001F, que le
  tokenizer de recherche ignore. Jamais de HTML.
- **Mots-clés** : une valeur par langue, un mot-clé par ligne.
- **Version d'une offre deb : vide.** DEP-11 ne contient pas la version du
  paquet ; elle viendra de PackageKit, dans `state.sqlite`. Les offres
  Flatpak portent la première version amont déclarée.
- **Pas d'état local** : rien sur ce qui est installé, ni préférence. Tout
  cela appartient à `state.sqlite` (P-003), qui n'est pas implémenté.
- **L'identifiant d'offre n'est unique que par application** : un même paquet
  Debian peut porter plusieurs applications (deux composants DEP-11, même
  `Package:`), et donc la même offre `packagekit:<paquet>` sous deux
  applications.

## 3. Import AppStream

Choix repris du prototype 3 [MESURÉ alors, confirmé ici] :

- **`AsMetadata`, pas `AsPool`** : `AsPool` dédoublonne par `data_id`, qui
  n'inclut pas la branche Flatpak, et perdait 703 composants Flathub.
- Seuls les composants `desktop-application` sont importés (§6.1 de
  l'architecture). Les autres sont comptés comme ignorés.
- **Textes non traduits lus explicitement** : avec la langue `ALL`, les
  accesseurs simples suivent la langue du processus. Le nom et le résumé
  viennent de l'entrée `C` de leurs tables ; la description est lue après
  `as_component_set_context_locale("C")`.
- **Provenance** : la source est celle que l'indexeur reçoit en argument
  (identifiant, backend, périmètre, nature, origine, priorité), jamais
  l'attribut `origin` du fichier, générique pour Flathub (`flatpak`).

Lecture de chaque composant (`src/indexer/importer.cpp`) :

| Donnée | Debian (DEP-11) | Flathub (XML) |
|---|---|---|
| Offre | `Package:` → offre deb, sans version | `<bundle type="flatpak">` → ref ; version = première `<release>` |
| Nom, résumé, mots-clés | Toutes les langues, depuis les tables d'AppStream | Idem |
| Description | `C`, plus les langues demandées (`--locale`) ou toutes (`--all-description-locales`) ; une traduction identique au texte `C` n'est pas stockée | Idem |
| Icône | Icône en cache présente sur disque (64 px, puis 128 px), sous `icons=…` | Idem ; à défaut URL `https`, puis nom d'icône du thème |
| Autres | Catégories, `<provides><id>`, développeur | Idem, plus `flathub::verification::verified` |

Les espaces (indentation YAML ou XML, tabulations) sont normalisés ; le
balisage des descriptions est converti en blocs par libxml2. Un balisage
illisible devient un paragraphe sans balises.

**Lecture en flux.** Le prototype 3 analysait chaque fichier d'un bloc :
463 MiB de pic pour Flathub. L'indexeur lit maintenant les fichiers par lots
de composants :

- XML : le lecteur en flux de libxml2 sérialise chaque `<component>`, et
  `AsMetadata` analyse les lots sous une copie de l'élément racine et de ses
  attributs ;
- DEP-11 : le fichier, décompressé à la volée par GIO, est découpé sur ses
  lignes `---`. Les clés de premier niveau sont en colonne 0 et les scalaires
  de bloc indentés : une ligne `---` en colonne 0 sépare toujours deux
  documents. Chaque lot est l'en-tête suivi de N composants ;
- `--batch N` (20 par défaut ; 0 analyse le fichier entier).

Le contenu produit est identique quelle que soit la taille des lots :
empreinte SHA-256 des applications, offres, textes, catégories et alias
comparée pour des lots de 0 (fichier entier), 20 et 500 composants sur
Flathub, et de 0, 1, 20 et 200 sur Debian [MESURÉ].

**Robustesse.** Un composant invalide est ignoré, compté sous une raison,
et l'import continue :

| Cas | Traitement |
|---|---|
| Composant d'un autre type | Ignoré : « not a desktop application » |
| Pas d'identifiant, pas de paquet, pas de bundle Flatpak | Ignoré, raison dédiée |
| Identifiant AppStream invalide, pas de nom `C`, paquet ou ref invalide | Refusé par le `CatalogWriter` (`invalid-argument`) |
| Même offre deux fois pour une application | Refusée (`conflict`) |
| Traduction invalide (UTF-8, longueur, langue mal formée), catégorie invalide, icône invalide | Retirée et comptée ; le composant est gardé |
| Pas d'icône, pas de résumé, pas de version | Accepté tel quel |
| Fichier absent, illisible, tronqué ou mal formé | Source signalée « illisible » ; ce qui a été lu est gardé ; les autres sources continuent |

Le rapport de l'indexeur donne, par source, le nombre de composants lus,
importés, et ignorés par raison, avec quelques exemples. Sur les catalogues
réels du 7 novembre 2026 [MESURÉ] : Debian `main` 2 515 composants, 2 045
importés, 470 d'un autre type ; Flathub 4 828 composants, 3 349 importés,
1 479 d'un autre type ; 3 traductions retirées.

### Contrat de l'indexeur

`cutefish-store-indexer` écrit un catalogue **candidat**. Écrire un candidat
et le mettre en service sont deux étapes distinctes, et le code de sortie
les distingue :

| Code | Sans `--replace` | Avec `--replace ACTIF` |
|---|---|---|
| 0 | Candidat **écrit et complet** (`user_version` = 1). Il n'est **ni validé, ni comparé au catalogue actif, ni installé** | Candidat écrit, validé (`replaceCatalog`) et **installé** à la place de `ACTIF` |
| 1 | Aucun candidat utilisable : l'écriture a échoué ; un fichier laissé à `--output` est incomplet (`user_version` = 0) et refusé par tous les lecteurs | Idem ; `ACTIF` inchangé |
| 2 | Erreur d'utilisation (argument inconnu, manquant ou mal formé, `--output` identique à `ACTIF`) ; rien n'est écrit | Idem |
| 3 | — | Candidat écrit mais **refusé** par la validation ; `ACTIF` inchangé, octet pour octet ; candidat gardé pour diagnostic |

- La dernière ligne de la sortie standard répète l'issue sous une forme
  lisible par un programme : `result=written|installed|refused|failed
  sources=N unreadable_sources=M`.
- **Une source illisible ne change pas le code de sortie.** Elle est
  signalée (`readable=0` dans le rapport, `unreadable_sources`), et c'est la
  validation qui refuse un candidat qui a perdu une source du catalogue
  actif. Un code 0 sans `--replace` ne dit donc rien de la qualité du
  contenu.
- `--output` est supprimé puis recréé : ce ne doit jamais être le catalogue
  actif. Avec `--replace`, le même chemin (après résolution) pour les deux est
  refusé (code 2).
- Les nombres mal formés (`--batch beaucoup`) sont des erreurs
  d'utilisation, pas un arrêt brutal du programme.

Règles pour les appelants :

- **le service** lance l'indexeur **sans** `--replace` et considère le code 0
  comme « candidat disponible », jamais comme « catalogue à jour ». Il appelle
  ensuite lui-même `replaceCatalog()`, qui seul valide et installe
  (ARCHITECTURE §6.2, étapes 4 et 5) ; le service n'existe pas encore ;
- `--replace` sert aux usages manuels et aux tests ; l'API C++ sépare de la
  même façon `CatalogWriter::finish()` (le fichier est complet) et
  `replaceCatalog()` (il est validé et en service) ;
- appelant actuel du dépôt : `scripts/measure-catalog.sh`, sans `--replace` ;
  il n'installe rien et ses catalogues sont ouverts par `SqliteCatalog`, qui
  refuse un fichier incomplet.

Tests : `tests/indexer/testcli.cpp` lance le binaire et vérifie chaque code,
la ligne `result=`, l'absence d'installation sans `--replace`, l'actif
inchangé après un refus, et le refus de `--output` égal à l'actif.

## 4. Provenance, doublons et identifiants

- **Une offre par composant**, avec sa source et son identifiant AppStream
  d'origine (`component_id`). Les composants ne sont jamais fusionnés entre
  eux : ils sont regroupés sous l'application de même identifiant normalisé
  (§6.4 de l'architecture).
- **Identifiants hérités** : `org.gnome.gitg.desktop` et `org.gnome.gitg`
  donnent la même application ; la forme d'origine reste dans
  `component_id`. Une recherche par l'ancienne forme passe par
  `AppId::fromComponentId`.
- **Branches Flatpak** : `org.qgis.qgis` `lts` et `stable` sont deux offres.
- **Même lanceur, identifiants différents** (`org.kde.kwalletmanager5`,
  `org.kde.kwalletmanager`) : deux applications, jamais fusionnées ; le nom
  seul ne fusionne jamais rien.
- **`<provides><id>`, décision D-012** (validée par le propriétaire ; elle
  remplace la règle 2 du §6.4 de l'architecture, qui fusionnait deux
  applications liées par un `<provides><id>`) :
  - l'identifiant est normalisé (suffixe `.desktop` retiré) ;
  - il devient un alias seulement si une seule application le revendique et
    qu'aucune application ne porte déjà cet identifiant, quel que soit
    l'ordre d'import ; sinon il est ignoré et compté comme ambigu ;
  - un identifiant invalide est compté à part ; l'identifiant du composant
    lui-même est ignoré ;
  - il ne fusionne jamais deux applications et ne déplace aucune offre ; la
    recherche essaie les identifiants d'application avant les alias ;
  - catalogues réels [MESURÉ] : 225 alias gardés, 79 ambigus, aucun
    invalide ;
  - tests : `tests/core/testaliases.cpp`.
- 444 applications ont une offre Debian et une offre Flathub [MESURÉ],
  comme mesuré pendant la phase 0.2.

**Textes affichés : priorité éditoriale** (validée par le propriétaire ;
confirmée en phase 0.4 comme indépendante de l'offre proposée) : pour chaque
champ, le texte de l'offre dont la source a la plus haute priorité
éditoriale (`Source::priority`) parmi celles qui le fournissent dans la
meilleure langue disponible ; à priorité égale, l'identifiant d'offre
départage. Les offres d'une application sont rendues dans le même ordre.

**Offre proposée par défaut** : règle distincte (D-009 : installée, puis
Flathub, puis la distribution, puis une remote Flatpak tierce), fondée sur la
nature des sources et l'état installé. Elle n'est pas dans le catalogue :
`Offer` ne porte pas de marque « par défaut », et l'ordre des offres rendu par
le catalogue suit la priorité éditoriale, **pas** la priorité
d'installation. La fonction qui l'applique est prévue en Core v0.3
(SERVICE.md §14 et §15, étape A).

## 5. Localisation

- Stockage : noms, résumés et mots-clés dans **toutes** les langues des
  catalogues (355 langues de noms sur les données réelles) ; descriptions en
  `C` et dans les langues passées à l'indexeur.
- Lecture : `SqliteCatalog` reçoit une liste de langues ; chacune apporte
  ses replis (`fr_FR.UTF-8` → `fr_FR` → `fr`), puis `C`. Les langues sont
  stockées sans encodage.
- Recherche : l'index contient `C` et les langues demandées à la
  construction (`fr_FR` apporte aussi `fr`).

Coûts mesurés pour les deux sources [MESURÉ, médiane de 3] :

| Descriptions stockées | Taille | Durée | Pic mémoire |
|---|---|---|---|
| `C` seulement | 25,0 Mo | 4,4 s | 38 MiB |
| `C` + français | 28,2 Mo | 4,8 s | 38 MiB |
| Toutes les langues disponibles | 58,1 Mo | 4,9 s | 38 MiB |

Répartition des 25 Mo [MESURÉ] : textes 11,4 Mo (dont noms et résumés
2,1 Mo, mots-clés 1,2 Mo, descriptions `C` 2,8 Mo), index FTS5 5,0 Mo,
index des noms 1,4 Mo, offres 1,3 Mo.

**Politique retenue (D-013, tranche O-009)** : noms, résumés et mots-clés
dans toutes les langues ; descriptions en `C` et en français. Le service
lancera l'indexeur avec `--locale fr` (catalogue de 28,2 Mo mesuré). Utilisé
seul, l'indexeur ne choisit aucune langue : sans `--locale`, seules les
descriptions `C` sont stockées.

## 6. Recherche FTS5

La saisie de l'utilisateur n'est jamais utilisée comme syntaxe FTS5 : elle
est découpée en mots (comme le tokenizer découpe l'ASCII), chaque mot est
mis entre guillemets et suivi de `*`, au plus 8 mots. Les guillemets, `%`,
`_`, `NEAR`, `OR` sont du texte comme un autre.

**Classement en rangs**, chaque application n'apparaissant qu'une fois, au
premier rang qui la trouve :

| Rang | Correspondance | Moyen | Ordre dans le rang |
|---|---|---|---|
| 1 | Nom exact | Index `texts_names`, comparaison `NOCASE` (casse ASCII), toutes langues | Nom le plus court, puis `app_id` |
| 2 | Préfixe du nom | Même index, `LIKE 'saisie%'` | Idem |
| 3 | Mots | FTS5 `search`, préfixes, sans accents ni casse | `bm25` pondéré : nom 10, mots-clés 5, résumé 2, description 0,5, développeur 1 ; puis `app_id` |
| 4 | Sous-chaîne du nom | FTS5 `search_names` (trigrammes), dès 3 caractères | `bm25`, puis `app_id` |
| 5 | Nom approché | Trigrammes de la saisie (au plus 200 candidats), puis distance d'édition ≤ 2 | Distance, puis `app_id` |

- Le rang 5 n'est calculé **que si les rangs 1 à 4 ne trouvent rien** : une
  faute de frappe ne pollue pas les résultats d'une saisie correcte.
- Les rangs 1 et 2 sont insensibles à la casse ASCII seulement ; une saisie
  accentuée différemment est trouvée par le rang 3.
- Résultats : des applications, jamais des paquets ; les offres s'obtiennent
  par `application(id)`.
- Filtres : catégorie, backend. `installedOnly` est refusé (`NotSupported`)
  tant que `state.sqlite` n'existe pas ; `installed` et `hasUpdate` sont
  faux dans les résumés.
- La liste classée de la dernière recherche est gardée : changer de page ne
  relance pas la recherche. Le service applique déjà « la dernière requête
  gagne » côté interface (§10.3).

## 7. Reconstruction atomique

Séquence (ARCHITECTURE §6.2) :

1. `CatalogWriter::create` supprime un ancien candidat et ses fichiers
   annexes, crée le schéma, `user_version = 0`.
2. Écriture dans **une transaction**, sans journal ni fsync
   (`journal_mode = OFF`, `synchronous = OFF`) : le fichier n'est lu par
   personne.
3. `finish()` : index FTS5, `optimize`, métadonnées, `COMMIT`, `ANALYZE`.
4. Puis `journal_mode = DELETE`, `synchronous = FULL`, et **`user_version =
   1` dans une transaction séparée**, journalisée : SQLite écrit le journal,
   le synchronise, écrit la page 1, synchronise le fichier — ce qui force
   aussi sur disque toutes les pages écrites avant —, puis supprime le
   journal [DOC SQLite, « Atomic Commit »]. Un arrêt brutal avant la fin
   laisse soit `user_version = 0`, soit un journal « chaud ».
5. `replaceCatalog(candidat, actif)` :
   - refuse un candidat accompagné d'un fichier `-journal` ou `-wal` ;
   - l'ouvre en lecture seule **sans** `immutable` : `user_version` attendu,
     `PRAGMA quick_check`, `PRAGMA foreign_key_check`, présence des tables,
     réponse des deux index FTS5 ;
   - le compare au catalogue actif s'il est lisible : chaque source doit
     garder au moins 50 % de ses offres, sauf si elle a été retirée
     volontairement (`removedSources`) ;
   - `fsync` du candidat, `rename()` sur le catalogue actif (même dossier),
     `fsync` du dossier.
6. En cas d'échec, rien ne change : l'actif reste en place, octet pour octet
   [MESURÉ par les tests].

**Garanties sur lesquelles repose la sécurité des lecteurs** — le renommage
seul n'y suffirait pas :

- le fichier en service n'est **jamais modifié en place** : il n'est
  remplacé que par `rename()`, qui substitue atomiquement l'entrée du
  dossier [DOC POSIX] ;
- le candidat est complet, fermé, sans journal ni WAL au moment du
  renommage : SQLite associe le journal au nom du fichier, et un journal
  laissé derrière un fichier renommé pourrait être appliqué au mauvais
  fichier [DOC SQLite, « How To Corrupt An SQLite Database File », §2] ;
- les lecteurs ouvrent le fichier en lecture seule avec `immutable=1` :
  SQLite ne prend aucun verrou et ne cherche aucun journal, ce qui n'est
  valable que parce que le fichier ne change jamais [DOC SQLite, URI
  `immutable`] ;
- une connexion ouverte garde l'inode de l'ancien fichier jusqu'à sa
  fermeture [DOC POSIX] : elle continue de lire un instantané cohérent ;
  le service ouvre de nouvelles connexions après `CatalogChanged`.

Tests [MESURÉ, `tests/core/testcatalogfile.cpp`] : candidat absent,
construction interrompue, processus terminé en cours d'écriture (`fork`),
fichier tronqué, fichier corrompu, journal résiduel, source disparue, source
réduite à moins de la moitié, dossiers différents : tous refusés, l'actif
inchangé. Remplacement réussi, y compris d'un actif illisible. Un lecteur
ouvert avant le remplacement garde la génération 1 et ses offres Flathub ;
un nouveau lecteur voit la génération 2. Quatre threads lecteurs qui
cherchent pendant vingt remplacements successifs : aucune erreur, aucune
génération qui change sous une connexion ouverte.

Non testé : coupure de courant réelle, système de fichiers sans `rename()`
atomique (réseau), crash pendant le `rename()` lui-même [À VALIDER].

## 8. Performances mesurées

Machine d'audit, catalogues du système le 7 novembre 2026 (Debian `main` :
fichier du 8 août 2025, 8,0 Mo compressé ; Flathub : 11,0 Mo compressé), cache
de pages chaud. **Ce sont des mesures sur une machine, pas des garanties.**
Reproduire : `scripts/measure-catalog.sh build 3`.

### Construction

| Cas | Durée | Pic mémoire | Prototype 3 |
|---|---|---|---|
| Debian seul, fichier entier | 1,2 s | 83 MiB | 0,49 s d'analyse, 59 MiB |
| Debian seul, par lots de 20 | 1,3 s | 22 MiB | — |
| Flathub seul, fichier entier | 1,9 s | 461 MiB | 1,06 s d'analyse, 459 MiB |
| Flathub seul, par lots de 20 | 2,9 s | 36 MiB | — |
| Les deux sources, par lots de 20 | 4,4 à 4,7 s | 37 à 39 MiB | 2,3 s, 471 MiB |

- Cible de l'architecture (§10.1) : indexation < 10 s et pic < 300 Mo.
  **Les deux sont tenues sur cette machine** ; la seconde ne l'était pas
  avec la lecture en bloc du prototype 3.
- La lecture en flux coûte environ 1 s de plus pour Flathub (sérialisation
  libxml2 et analyses répétées), et divise le pic mémoire par 12.
- La construction est plus longue que dans le prototype 3 : toutes les
  traductions des mots-clés sont stockées, l'index des noms est plus large,
  et la lecture se fait en flux.

### Lecture et recherche

Catalogue des deux sources, descriptions françaises, lu en `C`, 500 requêtes
par mode tirées du catalogue (graine fixe) [MESURÉ] :

| Mesure | p50 | p95 | p99 | Trouvé dans les 10 premiers |
|---|---|---|---|---|
| Ouverture | 0,8 ms | — | — | — |
| Nom exact | 0,17 ms | 0,64 ms | 4,0 ms | 500/500 |
| Préfixe (2 à 5 caractères, frappe) | 0,77 ms | 5,6 ms | 8,4 ms | 428/500 |
| Mots courants (33 mots anglais et français) | 1,6 ms | 2,6 ms | 3,4 ms | — |
| Sous-chaîne de 4 caractères | 0,15 ms | 2,1 ms | 7,7 ms | 422/500 |
| Nom avec une faute | 0,68 ms | 2,0 ms | 2,7 ms | 478/500 |
| Page suivante d'une recherche | 0,001 ms | 1,2 ms | 1,3 ms | — |
| Application avec ses offres | 0,024 ms | 0,038 ms | 0,043 ms | — |
| Résumés d'une page de 60 | 0,91 ms | 1,0 ms | — | — |
| 4 lecteurs simultanés, recherches par préfixe | 1,3 ms | 8,4 ms | 13,5 ms | 0 erreur |

- Lu en français, les temps augmentent de 20 à 50 % (davantage de langues
  candidates) ; la page de 60 résumés passe à 1,4 ms.
- Le maximum isolé (≈ 30 ms) correspond aux premières requêtes d'une
  connexion (cache de pages SQLite à remplir).
- Comparaison au prototype 3 (préfixe p95 3,5 ms, LIMIT 60) : le classement
  en rangs calcule la liste complète pour donner le total exact, d'où un
  p95 de 5,6 ms ; il trouve plus souvent l'application attendue (428 contre
  381 sur 500). La cible « frappe → affichage p95 < 50 ms » (§10.1) inclut
  D-Bus et l'interface, non mesurés.

## 9. Tests

| Ensemble | Fichiers | Ce qui est couvert |
|---|---|---|
| Alias (D-012) | `tests/core/testaliases.cpp` | Chaque cas de la règle du §4 |
| Catalogue | `tests/core/testcatalog.cpp`, `catalogfixture.h` | Construction, compteurs et rejets, application à plusieurs offres, provenance, identifiants hérités, branches, même lanceur, alias, langues, recherche par rang (exacte, préfixe, mots, accents, sous-chaîne, fautes, sans résultat, syntaxe FTS5), filtres, pages, résumés, fichiers non valides |
| Remplacement | `tests/core/testcatalogfile.cpp` | §7 |
| Indexeur (contrat) | `tests/indexer/testcli.cpp` | Codes de sortie et ligne `result=` (§3, « Contrat de l'indexeur ») |
| Import | `tests/indexer/testimport.cpp`, `testmarkup.cpp`, `data/` | DEP-11 et XML Flathub réduits, en clair et compressés ; mêmes résultats pour toutes les tailles de lots ; source absente, fichiers tronqués, contenu illisible ; balisage des descriptions |
| Intégration | `scripts/measure-catalog.sh`, `tests/perf/catalogbench.cpp` | Catalogues réels du système, hors `ctest` |

Les tests unitaires n'ont besoin ni du réseau ni de privilèges : 138 tests,
réussis avec GCC, avec ASan et UBSan [MESURÉ].

## 10. Limites connues

- **D-009** : l'offre par défaut n'est pas encore calculée (Core v0.3) ; le
  catalogue n'en stocke pas.
- Composants autres que `desktop-application` (console, polices, modules
  complémentaires) non importés.
- Rangs 1 et 2 sensibles aux accents (casse ASCII seulement).
- Pas d'état installé : `installedOnly` refusé, `installed` et `hasUpdate`
  toujours faux (Core v0.3, `state.sqlite`).
- Captures d'écran, versions publiées, URL, licence, classification du
  contenu non stockées (`app_details` du §6.3, à venir avec la page de
  détails).
- Plausibilité du remplacement : 50 % par source, choisi par l'architecture,
  non éprouvé sur des actualisations réelles.
- Mesures sur une seule machine, cache de pages chaud.
