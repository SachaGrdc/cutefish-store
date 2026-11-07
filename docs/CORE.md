# Cutefish Store — Le cœur (`store-core`), v0.3

`store-core` est la bibliothèque qui porte la logique métier de la boutique.
Elle applique les décisions de [ARCHITECTURE.md](ARCHITECTURE.md) et de
[DECISIONS.md](DECISIONS.md) ; en cas de divergence, ce sont ces deux
documents qui font foi.

## Rôle

- Modéliser ce que la boutique manipule : applications, offres, sources,
  versions, éléments installés, recherches.
- Valider tout ce qui entre : un objet du modèle n'existe que s'il est
  valide.
- Définir les contrats des composants : catalogue, état installé, backends.
- Depuis la v0.2 : le catalogue SQLite (écriture d'un candidat, validation,
  remplacement atomique, lecture et recherche), décrit dans
  [CATALOG.md](CATALOG.md).
- Depuis la v0.3a : les plans de transaction, la politique de confirmation,
  la comparaison de plans, la machine d'états des transactions et le choix
  de l'offre proposée par défaut (D-009). Ce sont des fonctions pures : elles
  décident, mais n'appellent aucun backend.
- Depuis la v0.3 (catalogue) : empreintes des sources et décision de
  réimport, navigation (`browse`, `listCategories`), état installé lu avec
  le catalogue ([CATALOG.md](CATALOG.md) §11).
- Depuis la v0.3b : `state.sqlite` (`StateStore`) : cache de l'état
  installé, journal des transactions, dates de rafraîchissement, rétention
  D-018 et reprise après un arrêt.

Le cœur ne dépend que de C++20, de la bibliothèque standard et de SQLite
(P-006) :

- ni Cutefish, ni Qt, ni GTK, ni GLib, ni sd-bus, ni PackageKit, ni
  libflatpak, ni libappstream (confinée à l'indexeur, `src/indexer/`) ;
- SQLite est lié en privé : aucun en-tête SQLite n'apparaît dans l'API
  publique ;
- aucune boucle d'événements, aucun thread, aucune horloge, aucun accès
  réseau ; il écrit seulement le catalogue et `state.sqlite` qu'on lui
  désigne, et lit en plus les fichiers de métadonnées des sources pour en
  calculer l'empreinte (`probeSource`) ;
- libcurl, prévu par P-006, n'est pas encore lié : il arrivera avec le cache
  d'images.

Il peut donc être repris tel quel par une autre couche hôte, un autre
environnement de bureau ou un outil en ligne de commande.

## Organisation

```text
CMakeLists.txt                     projet, options BUILD_TESTING et CUTEFISH_STORE_SANITIZERS
src/core/
├── CMakeLists.txt                 cible store-core (bibliothèque statique)
├── include/cutefish/store/        API publique : seul dossier visible des utilisateurs
└── src/                           implémentation et en-têtes internes (text.h, sqlite.h, catalogschema.h)
src/indexer/                       importeur AppStream (CATALOG.md)
tests/core/                        GoogleTest, implémentations de test (fakes.h, fixtures.h, catalogfixture.h)
tests/indexer/                     tests de l'importeur et catalogues réduits
tests/perf/                        store-catalog-bench (hors ctest)
```

- Namespace `cutefish::store`, fichiers en minuscules sans séparateur
  (P-013). Pas d'en-tête SPDX tant que la licence n'est pas choisie (O-001),
  pas de `.clang-format` (O-014).
- Les en-têtes publics sont autonomes : un test les compile chacun seul.
- Les détails d'implémentation (`cutefish::store::detail`) restent dans
  `src/core/src/`.

Les images viendront dans `src/core/src/` avec le composant correspondant.

## Erreurs et résultats

- **`Error`** (`error.h`) : un `ErrorCode` et un message de diagnostic.
  - Codes : `InvalidArgument`, `NotFound`, `NotSupported`, `Conflict`,
    `Cancelled`, `Backend`, `Internal`.
  - Une erreur `Backend` porte aussi une `BackendFailure`, qui reprend les
    codes de transaction du §5.6 : `auth-denied`, `auth-cancelled`,
    `network`, `no-space`, `untrusted`, `dependency`, `conflict`, `locked`,
    `interrupted`, `backend-unavailable`.
  - On décide d'après le code, jamais d'après le message, qui n'est ni
    traduit ni destiné à être analysé.
- **`Result<T>`** (`result.h`) : une valeur ou une `Error`. C'est la version
  minimale, en C++20, de `std::expected` (C++23). Les erreurs attendues ne
  passent jamais par des exceptions.
- `toString()` donne des noms stables en minuscules, ceux de l'API D-Bus.

## Modèles

Chaque modèle se construit par une fonction `create()`, qui prend une
structure `…Spec` et rend un `Result`. Les chaînes sont **déplacées** dans
l'objet, sans copie. Une fois créé, l'objet est immuable.

| Modèle | En-tête | Responsabilité |
|---|---|---|
| `AppId`, `OfferId`, `SourceId` | `identifiers.h` | Identifiants typés. `AppId` est l'identifiant AppStream **normalisé** (§6.4) : `fromComponentId` retire le suffixe hérité `.desktop`, `parse` n'accepte que la forme normalisée. `OfferId` et `SourceId` sont opaques : `parse` vérifie seulement leur forme (1 à 255 octets ASCII imprimables) |
| `Source` | `source.h` | Origine d'une offre : un dépôt de la distribution ou une remote Flatpak d'une installation. Backend, périmètre, nature (`distribution`, `flathub`, `third-party`), origine, libellé, URL, priorité. Lecture seule : la gestion des dépôts n'est pas du ressort du cœur en v0.1 |
| `Offer` | `offer.h` | Une façon concrète d'installer une application : un paquet deb (PackageKit) ou une ref Flatpak, depuis une source. Garde sa provenance (source, nature, origine, périmètre). Construit son identifiant : `packagekit:<paquet>` ou `flatpak:<user\|system>:<remote>:<ref>` |
| `Application` | `application.h` | Le produit présenté par la boutique, distinct de tout paquet : nom, résumé, description en blocs, référence d'icône, catégories, alias, et au moins une offre |
| `InstalledItem` | `installed.h` | Ce qu'un backend signale comme installé : offre, application éventuelle, backend, périmètre, version, origine |
| `SearchQuery`, `AppSummary`, `SearchPage` | `search.h` | Requête de recherche validée, ligne de résultat, page de résultats liée à une génération du catalogue |

Règles principales :

- formats : seulement `deb` et `flatpak` (D-003) ; un format s'ajoutera avec
  le backend qui l'installe ;
- une offre deb vient d'une source PackageKit, une offre Flatpak d'une
  source Flatpak ; une source PackageKit est toujours `system` ;
- nom de paquet deb conforme à la politique Debian ; ref Flatpak de la forme
  `app|runtime/ID/arch/branche`, dont l'architecture est extraite ;
- une même application peut avoir des offres deb, Flatpak utilisateur et
  Flatpak système, et plusieurs branches : elles restent distinctes ;
- textes : UTF-8 valide, sans caractère de contrôle, longueurs bornées
  (§9.4) ; icônes locales en chemin absolu sans `..`, icônes distantes en
  `https` seulement (§9.5) ;
- pas de doublon d'alias, de catégorie ou d'offre dans une application.

## Versions

Une `Version` porte toujours son **schéma** (`version.h`) :

| Schéma | Usage | Ordre |
|---|---|---|
| `Debian` | Version d'un paquet deb, `[epoch:]amont[-révision]` | Règles de dpkg |
| `Upstream` | Version déclarée par le projet amont (AppStream), affichée pour une offre Flatpak | Aucun ordre fiable |

`compare(a, b)` rend un `Result<std::weak_ordering>` :

- deux versions `Debian` : l'ordre de dpkg (époque, puis partie amont, puis
  révision ; `~` passe avant tout, même la fin de la chaîne ; les suites de
  chiffres se comparent numériquement). Les résultats ont été vérifiés
  contre `dpkg --compare-versions` (dpkg 1.22.22) ;
- tout le reste, y compris deux versions `Upstream` ou deux schémas
  différents : `ErrorCode::NotSupported`. L'appelant affiche alors les deux
  versions sans prétendre que l'une est plus récente (D-009). Les mises à
  jour Flatpak se décident par commit, pas par version.

Précisions :

- l'ordre est faible : `1.0` et `0:1.0` sont **équivalents** mais pas
  identiques. `operator==` compare l'identité (schéma et texte), pas l'ordre ;
- l'analyse Debian est stricte : ce que dpkg accepte avec un simple
  avertissement (partie amont qui ne commence pas par un chiffre, caractère
  invalide) est refusé, pour qu'une version soit soit ordonnée correctement,
  soit rejetée ;
- deux textes `Upstream` identiques ne sont pas déclarés équivalents : rien
  ne dit qu'ils désignent la même construction.

## Interfaces publiques

| Interface | En-tête | Implémentation prévue | Appels |
|---|---|---|---|
| `Catalog` | `catalog.h` | `SqliteCatalog` (`sqlitecatalog.h`), implémentée en v0.2, étendue en v0.3 ([CATALOG.md](CATALOG.md)) | Synchrones, sur les threads de lecture de l'hôte : `generation`, `application` (par id ou alias), `search`, `summaries`, `browse`, `listCategories`, `sources` ; avec `state.sqlite` attachée, `installed` et `installedOnly` |
| `InstalledState` | `installedstate.h` | `StateStore` (thread principal) et `SqliteInstalledState` (threads de lecture), implémentées en v0.3b (`statestore.h`) | Synchrones : `items`, `find`, `forApplication`. Ne fait jamais attendre PackageKit ou libflatpak (P-009) |
| `Backend` | `backend.h` | `backend-packagekit`, `backend-flatpak` | Asynchrones (§7.1) : `availability`, `queryInstalled`, `sources` |

Contrat de `Backend` :

- les méthodes sont appelées depuis le thread principal de l'hôte et rendent
  la main aussitôt ;
- la `Completion` est appelée **une seule fois**, sur le thread principal,
  par la fonction `post` fournie par l'hôte (§2) ;
- elle n'est **jamais** appelée depuis l'intérieur de la méthode, ce qui
  évite toute réentrance ;
- les échecs du gestionnaire de paquets sont des erreurs `Backend` avec leur
  `BackendFailure`.

Les opérations de planification, d'exécution, d'annulation, de mises à
jour et de notification de changement (§7.1) seront ajoutées à `Backend` à
l'étape D (SERVICE.md §15) ; les types qu'elles échangent (`Plan`) existent
depuis la v0.3a.

`tests/core/fakes.h` contient des implémentations en mémoire de chaque
interface, utilisées par les tests ; elles ne font pas partie de
`store-core`.

Autres API publiques du catalogue (v0.2, [CATALOG.md](CATALOG.md)) :
`CatalogWriter` (`catalogwriter.h`), qui écrit un catalogue candidat, et
`inspectCatalog` / `replaceCatalog` (`catalogfile.h`), qui le valident et le
mettent en service.

## Plans, confirmation et transactions (v0.3a)

Ces types appliquent P-008, D-011 et D-014 sans rien savoir de Cutefish, de
D-Bus, de polkit, de PackageKit ni de libflatpak. Le cœur **décrit** ce qui
doit être confirmé ; il n'authentifie personne. L'autorisation reste aux
gestionnaires de paquets et à polkit (ARCHITECTURE §9).

### `Plan` (`plan.h`)

Ce qu'une transaction fera, tel que le backend l'a calculé (simulation
PackageKit, signal `ready` d'une `FlatpakTransaction`). Un plan appartient à
**un** backend et **une** installation, et porte le type de la transaction
(`install`, `remove`, `update`). Chaque `PlanItem` :

| Champ | Sens |
|---|---|
| `operation` | `install`, `update`, `downgrade`, `remove` |
| `kind` | `application`, `runtime`, `package` |
| `requested` | Demandé par l'utilisateur, ou ajouté par le gestionnaire (dépendance, runtime, suppression en cascade) |
| `package` | Nom de paquet ou ref Flatpak complète, unique dans le plan |
| `fromVersion`, `toVersion` | Versions avant et après, si connues |
| `origin`, `revision` | Dépôt ou remote ; identifiant de construction immuable (commit Flatpak) |
| `downloadSize` | Taille annoncée |
| `permissions`, `oldPermissions` | Chaînes stables choisies par le backend, triées et dédoublonnées par `Plan::create` |
| `addsSource`, `rebaseFrom`, `trusted` | Ajout d'une remote, remplacement en fin de vie, authentification |

Règles de `Plan::create` : au moins un élément demandé ; un élément demandé
fait ce que la transaction demande ; pas de version cible pour une
suppression ni de version précédente pour une installation ; schéma de
version du backend ; PackageKit toujours en système. Les éléments ajoutés
peuvent tout faire : une mise à jour peut installer un runtime (D-014).

### Politique de confirmation (`confirmation.h`)

`assessPlan(plan, policy)` rend les raisons (`ConfirmationReasons`) et, pour
chacune, l'élément qui la cause (`ConfirmationConcern`), pour que
l'interface montre par exemple le runtime système qu'une mise à jour
installerait.

| Raison | Règle, élément par élément |
|---|---|
| `removes-packages` | Suppression non demandée |
| `new-permissions` | Permissions absentes avant l'opération (toutes, pour une installation) |
| `new-source`, `rebase`, `untrusted` | Indicateurs de l'élément |
| `system-scope` | Installation ou suppression, demandée ou non, dans l'installation système d'un backend de `policy.sharedSystemScope` (Flatpak par défaut, D-011), **quel que soit le type de transaction** : un runtime installé par une mise à jour système compte (D-014) |
| `plan-changed` | Jamais produite ici : vient de la comparaison |

Les paquets de la distribution ne sont pas dans `sharedSystemScope` par
défaut : P-008 ne demande pas de confirmation pour eux. C'est une donnée de
la politique, pas une règle codée en dur.

### Comparaison de plans

`comparePlans(accepted, fresh)` applique la comparaison sémantique de P-008
(seconde passe Flatpak, nouvelle simulation PackageKit) :

| Différence | Verdict |
|---|---|
| Type, backend ou installation différents | Nouvelle confirmation, rien d'autre n'est comparé |
| Élément ajouté (dépendance comprise) | Nouvelle confirmation |
| Élément ajouté par le gestionnaire, qui installait ou mettait à jour, disparu | Compatible (déjà satisfait) |
| Tout autre élément disparu (demandé, ou suppression) | Nouvelle confirmation |
| Opération, nature, demande, origine, révision, version changées | Nouvelle confirmation |
| Permission nouvelle par rapport au plan accepté | Nouvelle confirmation |
| Ajout d'une source, rebase changé, perte d'authentification | Nouvelle confirmation |
| Permissions seulement retirées, risque réduit, taille changée | Compatible |

Tout ce qui n'est pas explicitement inoffensif exige une confirmation. Les
différences sont triées par paquet puis par nature : le résultat ne dépend
pas de l'ordre des éléments.

### Machine d'états (`transaction.h`)

`Transaction` enregistre et décide ; l'hôte la pilote depuis son thread
principal. États et transitions : ARCHITECTURE §3.5. Règle de D-017 : la
transaction reste en `planning` jusqu'à la vérification du plan d'exécution ;
`running` signifie toujours que le backend peut modifier le système, et il
n'existe pas de retour de `running` vers `awaiting-confirmation`.

- `planReady(plan)` : plan calculé sans rien exécuter (première passe
  Flatpak, simulation PackageKit). Sans plan accepté : `awaiting-confirmation`
  si les règles l'exigent, sinon le plan est accepté tel quel et la
  transaction **reste en `planning`**. Avec un plan accepté (après
  `confirm()`) : comparaison ; reste en `planning` si le plan est
  compatible, sinon `awaiting-confirmation` avec `plan-changed`.
- `confirm()` : le plan présenté devient le plan accepté
  (`acceptedPlan()`), retour en `planning`.
- `verifyExecutionPlan(plan)` : le plan que le backend va réellement
  exécuter (seconde passe Flatpak dans `ready`, nouvelle simulation
  PackageKit), comparé au plan accepté. Compatible : `running`, et seulement
  alors le backend exécute. Sinon : `awaiting-confirmation` avec
  `plan-changed`, rien n'est exécuté (par exemple un runtime système apparu,
  D-014). Refusé sans plan accepté, et refusé en `running`.
- `succeed()` n'est possible qu'en `running`, donc après cette
  vérification.
- **Limite PackageKit** : la vérification porte sur une simulation, qui est
  une transaction distincte de l'exécution ; elle réduit la fenêtre entre
  plan et exécution sans la fermer (D-017). Seule la seconde passe Flatpak
  vérifie le plan de la transaction exécutée elle-même.
- Une transition interdite rend `Conflict` et ne change rien ; un plan d'un
  autre type rend `InvalidArgument`.
- Ni délai ni horloge : le délai de confirmation de 10 minutes (D-019) est
  appliqué par l'hôte, qui appelle `cancel()`.

## Offre proposée par défaut (`defaultoffer.h`, D-009)

`defaultOffer(offers, installed)` choisit, dans l'ordre :

1. une offre installée (son identifiant figure dans `installed`) ;
2. le rang `offerRank()` : Flathub, puis deb de la distribution, puis
   Flatpak d'une remote tierce, puis les offres non classées (deb d'un dépôt
   APT tiers, remote Flatpak marquée « distribution ») ;
3. l'installation utilisateur avant l'installation système (D-005) ;
4. le plus petit identifiant d'offre.

La clé est totale : le résultat ne dépend ni de l'ordre des offres rendu par
SQLite, ni de l'ordre de `installed`. Elle n'utilise jamais
`Source::priority`, qui ne règle que les textes affichés (priorité
éditoriale, CATALOG §4). La fonction est appliquée à la lecture et jamais
stockée, puisqu'elle dépend de l'état installé.

## `state.sqlite` (v0.3b, `statestore.h`)

Ce que la boutique sait du système et de son propre travail (P-003,
SERVICE.md §8). Un seul écrivain, `StateStore`, sur le thread principal de
l'hôte ; des lecteurs `SqliteInstalledState`, un par thread de lecture.

**Ce que la base n'est pas** : la source de vérité de ce qui est installé.
PackageKit et libflatpak le sont. La table `installed` est une copie du
dernier inventaire de chaque backend et installation, pour que l'interface
n'attende jamais ; elle n'est modifiée que par un inventaire complet
(`replaceInstalled`), jamais par une mise à jour optimiste, et ne sert jamais
à décider une opération (c'est le plan du backend qui décide).

| Table | Contenu |
|---|---|
| `meta` | Date de création, `last_refresh`, `reconciled:<backend>:<scope>` |
| `installed` | Cache d'inventaire : offre, backend, installation, application, version, origine |
| `transactions` | Journal : type, backend, installation, état, plan présenté et plan accepté (JSON), confirmation, erreur, référence du backend (chemin PackageKit), dates |
| `transaction_offers` | Offres désignées par le client, dans leur ordre ; supprimées avec la transaction |
| `source_state` | Dernier rafraîchissement réussi de chaque source (D-015) |

Choix de persistance :

- **Cohérence après un arrêt brutal** : journal WAL, `synchronous = FULL`,
  chaque modification dans une seule transaction SQLite (`BEGIN
  IMMEDIATE`). Un inventaire, l'enregistrement d'une transaction (avec ses
  offres et la purge) ou une reprise sont entiers ou absents. `quick_check`
  à l'ouverture.
- **Versions** : `PRAGMA user_version` (1), une migration par version.
  Un fichier plus récent, étranger ou endommagé est refusé **sans être
  modifié** ; le renommer et repartir d'une base neuve est le choix de
  l'hôte (SERVICE.md §8.6).
- **Aucune horloge** (D-019) : toutes les dates (`UnixSeconds`) viennent de
  l'hôte.
- **Plans en JSON versionné** (`"v": 1`) : écrits par le cœur, relus par les
  fonctions JSON de SQLite (`json_each`, `json_extract`), puis validés de
  nouveau par `Plan::create`. Aucune bibliothèque JSON. Un plan illisible
  ou incohérent est une erreur, jamais une donnée acceptée.
- **Transactions** : `TransactionRecord` contient un `TransactionSnapshot`
  (`Transaction::snapshot()` / `Transaction::restore()`). Les raisons de
  confirmation sont recalculées depuis le plan ; seul `plan-changed` est
  gardé. Un enregistrement que `restore` refuserait n'est pas écrit.
- **Rétention (D-018)** : au plus 200 transactions terminées et au plus
  90 jours après leur fin, la plus stricte l'emporte ; appliquée à chaque
  enregistrement d'une transaction terminée (à sa date de fin) et par
  `purgeHistory(now)`. Une transaction non terminée n'est jamais supprimée ;
  les plans restent avec la transaction pendant toute la rétention.
- **Reprise** (`recoverAfterRestart(now)`, SERVICE.md §5.6) : `queued` et
  `planning` → `interrupted` ; `awaiting-confirmation` → `cancelled` ;
  `running` → `interrupted`, sauf une transaction PackageKit dont le chemin
  est connu, rendue à l'hôte pour un rattachement. `reconcile` est vrai si
  une transaction `running` a été interrompue : elle a pu modifier le
  système. Une transaction en `planning` n'a rien exécuté (D-017).
- **Concurrence** : chaque lecture est une seule requête, donc un seul état
  validé (isolation des instantanés WAL) : un lecteur ne voit jamais un
  inventaire à moitié remplacé. Un second écrivain attend (délai de 5 s).
- **Occupation** : `openTransactionFor(offer)` donne la transaction non
  terminée qui porte déjà sur une offre (erreur `Busy`, A §5.5).

## Performance

- Les modèles reçoivent leurs données par déplacement ; les accesseurs
  rendent des références ou des `std::span`, sans copie.
- Énumérations sur un octet ; `BackendSet` est un champ de bits d'un octet.
- Une version Debian est analysée une seule fois, à la création : la
  comparaison ne fait que parcourir les deux chaînes, sans allocation.
- Les listes d'une application (alias, catégories, offres) sont courtes :
  la vérification des doublons les parcourt sans construire d'ensemble.
- Aucune concurrence : le cœur ne crée pas de thread et ses objets ne sont
  pas partagés entre threads, sauf mention contraire.

## Compiler et tester

Prérequis : CMake ≥ 3.22, un compilateur C++20 (GCC 14 ou Clang 19 testés
ici), pkg-config, SQLite avec FTS5 (`libsqlite3-dev`), et pour l'indexeur
libappstream et libxml2 (`libappstream-dev`, `libxml2-dev`) ;
GoogleTest pour les tests (`libgtest-dev`). L'option
`-DCUTEFISH_STORE_BUILD_INDEXER=OFF` construit le cœur sans l'indexeur.
Liste des paquets Debian et vérification : [CATALOG.md](CATALOG.md),
« Dépendances ».

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure
```

Avec AddressSanitizer et UndefinedBehaviorSanitizer :

```bash
cmake -S . -B build-asan -G Ninja -DBUILD_TESTING=ON -DCUTEFISH_STORE_SANITIZERS=ON -DCMAKE_BUILD_TYPE=Debug
cmake --build build-asan
ctest --test-dir build-asan --output-on-failure
```

Sans `BUILD_TESTING=ON` (valeur par défaut `OFF`, comme dans les projets
Cutefish), seule la bibliothèque est construite. Les tests n'ont besoin ni
du réseau, ni de privilèges, ni d'une session Cutefish.
