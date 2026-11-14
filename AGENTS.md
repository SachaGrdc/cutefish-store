# Consignes pour les agents (Cutefish Store)

Ce fichier dit où en est le projet et comment y travailler. Il ne remplace
pas la documentation : il renvoie vers elle. Le propriétaire du projet
valide chaque étape et chaque décision ; un agent propose, il ne décide pas.

## À lire, dans cet ordre

1. [README.md](README.md) : ce qui existe, dépendances, construction.
2. [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) : la conception d'ensemble.
3. [docs/DECISIONS.md](docs/DECISIONS.md) : décisions **validées** (D-xxx,
   P-xxx validées), **proposées** (section « Décisions proposées »), et
   questions ouvertes (tableau O-xxx, Q-xxx). Une décision validée ne se
   remet en cause que sur un problème technique démontré, présenté au
   propriétaire.
4. [docs/CORE.md](docs/CORE.md) : le cœur `store-core` et ses contrats.
5. [docs/CATALOG.md](docs/CATALOG.md) : catalogue SQLite et indexeur.
6. [docs/SERVICE.md](docs/SERVICE.md) : le service D-Bus ; §15 plan des
   étapes et leur état, §16 service v0.1, §17 backend PackageKit, §18
   gestionnaire de transactions (F2).

## Le projet en bref

Cutefish Store : une boutique d'applications pour le bureau Cutefish (Debian
13), qui présente un catalogue unique (AppStream DEP-11 de la distribution
et Flathub) et installe par les gestionnaires natifs (PackageKit pour les
paquets deb, libflatpak pour Flatpak), sans composant privilégié propre.
C++20, CMake, SQLite (FTS5), libappstream (indexeur), Cutefish Toolkit
(`ToolkitBase`, couche hôte seulement), D-Bus (sd-bus via le Toolkit).

| Dossier | Contenu |
|---|---|
| `src/core` | `store-core` : modèles, catalogue SQLite, plans, confirmation, `Transaction`, `state.sqlite`, contrat `Backend` (CORE.md) |
| `src/indexer` | `cutefish-store-indexer` : AppStream → catalogue candidat (CATALOG.md) |
| `src/backend-packagekit` | Logique PackageKit et profil Debian, sans D-Bus |
| `src/backend-flatpak` | `store-backend-flatpak` : logique pure, magasin libflatpak, backend Flatpak (SERVICE.md §19) |
| `src/service` | `cutefish-store-service` : D-Bus `org.cutefish.Store`, transport PackageKit, inventaire, gestionnaire de transactions (SERVICE.md) |
| `tests/` | GoogleTest par composant ; `tests/service` lance des bus privés et un PackageKit simulé ; `tests/backend-flatpak` un faux magasin Flatpak |
| `data/` | Fichiers d'activation D-Bus et unité systemd utilisateur |

## État (fin de la session de passation)

| Étape | Contenu | Commits principaux |
|---|---|---|
| Phases 0.x | Conception, décisions, prototypes | `574d58a` … `5e3201c` |
| Core v0.1–v0.2, indexeur | Modèles, catalogue SQLite/FTS5, indexeur | `ada8e9b` … `d130382` |
| A, B | Plans, confirmation, `Transaction`, `state.sqlite` | `36cb575` … `def4947` |
| C | Catalogue v0.3 (empreintes, navigation, état installé) | `783c8a7` … `b52fdd5` |
| D | Contrat `Backend` complet | `4346051`, `4e714aa` |
| E | Service D-Bus v0.1 en lecture | `ed56113`, `d017c02` |
| F | Backend PackageKit, intégration en lecture, D-028, D-029 | `59084c2` … `40d3c7b` |
| F2 | Gestionnaire de transactions (`Install`, `Remove`, `Update`, `UpdateAll`) | `5ef279f`, tests F2 `6329b90` … `8b00b7a` |
| G | Backend Flatpak autonome (`store-backend-flatpak`) | `feat(backend-flatpak): …` |
| H.1 | Registre multi-backends et sélection (service) | `feat(service): …` |
| H.2 | Inventaire unifié (installés, mises à jour, `UpdateCount`, écritures SQLite), finalisation, coexistence PackageKit | `fix(state): …`, `feat(service): …`, `feat(backend-packagekit): …` |

- **F2** : implémentée et testée contre le PackageKit simulé ; le détail de
  ce qui est testé et de ce qui reste hors de portée est dans SERVICE.md
  §18. **Le propriétaire doit encore valider F2.** Les reprises au
  démarrage (rattachement, transaction perdue, `planning` interrompu),
  l'annulation d'une transaction `running` par D-Bus, l'annulation pendant
  la seconde simulation, plusieurs transactions en file et la parente
  `UpdateAll` (dont l'échec partiel) ont désormais des tests. Les
  transactions réelles, le dialogue polkit, le verrou dpkg et debconf
  restent à vérifier en VM.
- **G** : le backend Flatpak est implémenté comme bibliothèque autonome
  (SERVICE.md §19), testé sur un faux magasin (25 tests). Il refuse tout
  ajout de remote déclenché par une transaction (P-011), filtre les remotes
  désactivés du catalogue, et libère ses `GError`.
- **H.1** : le service porte désormais plusieurs backends (SERVICE.md §20) :
  registre `Backends` (un `Backend` + un `Inventory` par `BackendKind`),
  sélection par `offer_id` sans bascule silencieuse, propriété D-Bus
  `Backends`, sémantique `degraded` seulement quand tous les backends sont
  inutilisables, et construction optionnelle de PackageKit et Flatpak dans
  `main.cpp` (`--flatpak`, compatible sans libflatpak).
- **H.2** : inventaire unifié (SERVICE.md §21) : normalisation de ce que
  chaque backend liste (offres étrangères, doublons), `GetUpdates` et
  `UpdateCount` tirés d'une seule liste triée par `offer_id`, signaux sans
  doublon, un thread d'E/S partagé par les inventaires avec nouvelle
  tentative bornée, `busy_timeout` fixé dès l'ouverture de `state.sqlite`.
  Finalisation : écritures d'inventaire qui ne bloquent jamais le thread
  principal (destruction sans attente, nouvelles tentatives par minuteur),
  erreurs SQLite classées, reprise bornée et signalée (`inventory-error`),
  remplacement atomique de tous les scopes d'un backend ; le journal reste
  synchrone sur le thread principal (décision du propriétaire, SERVICE.md
  §21.5). Coexistence avec les mises à jour système de Cutefish Settings
  (SERVICE.md §22) : activité des autres programmes dans PackageKit
  (`activity` de `Backends`), actions deb refusées en `locked` pendant une
  modification du système, Flatpak non concerné. Audit final : une instance
  qui s'arrête ne valide plus aucune écriture d'inventaire (garde de
  validation SQLite, arrêt avant la libération du nom), pour qu'une
  nouvelle instance ne voie jamais un inventaire plus ancien écrit après le
  sien (SERVICE.md §21.5, « Arrêt et redémarrage »).
  **Le propriétaire doit encore valider H.2.** Prochaines étapes : H.3
  (transactions), H.4 (`UpdateAll`), H.5 (sources et API D-Bus), H.6
  (moniteur Flatpak), H.7 (catalogue unifié).
  **Ne commencer aucune étape sans l'accord explicite du propriétaire.**
- **Décisions encore proposées**, à faire valider (ne pas les valider
  soi-même) : D-027, D-029, P-001, P-009 (sa précision PackageKit est
  validée ; le propriétaire applique aussi la règle « une opération
  modificatrice à la fois par backend »), P-011, P-013. Le propriétaire a
  cité P-001 comme règle de travail à l'étape F, sans changer son statut.
  Questions ouvertes notables : O-001 (licence), O-014 (formatage).
- Limites connues et points jamais vérifiés : SERVICE.md §16.4, §17.6 et
  §18, ARCHITECTURE.md §7.2 (« Reste à tester »).

## Reprendre le projet (nouvel agent)

1. `git status`, `git log --oneline -15` : l'arbre doit être propre ; la
   branche `main` a des commits locaux non poussés (ne pas pousser).
2. Lire ce fichier, puis les documents de la section suivante.
3. Construire et lancer toute la suite (section « Construire et tester ») ;
   noter le nombre de tests réellement passés avant tout changement.
4. Demander au propriétaire la validation de F2 et des décisions proposées,
   puis l'étape à commencer.
5. Ne rien supposer de l'historique des conversations : tout ce qui compte
   est dans le dépôt. En cas de doute sur une règle, demander.

## Règles du propriétaire

- **Aucune commande root.** Aucune installation, suppression ou mise à jour
  réelle de paquets sur la machine sans autorisation explicite ; les
  transactions réelles se testent dans une VM.
- **Git** : `git status` avant chaque étape ; commits petits et cohérents,
  messages conventionnels (`feat(service): …`), diff complet relu avant
  chaque commit ; aucun fichier temporaire, artefact de construction ou
  secret ; ne jamais réécrire l'historique ; **ne rien pousser** sans
  autorisation explicite. Identité des commits :
  `Sacha Grandcolas <68822946+SachaGrdc@users.noreply.github.com>`.
- Pas de fichier `LICENSE` ni d'en-têtes SPDX tant qu'O-001 est ouverte ;
  pas de `.clang-format` (O-014).
- Le dépôt des prototypes `~/Dev/cutefish-store-prototypes` se lit, ne se
  modifie pas.
- Si une décision importante manque, s'arrêter et demander.
- Un test n'est réussi que s'il a réellement été exécuté ; dire ce qui est
  mesuré, simulé ou non vérifié.

## Règles d'architecture

- Le cœur (`src/core`) ne dépend ni de PackageKit, ni de D-Bus, ni de
  Cutefish Toolkit (P-006) ; seule la couche hôte (`src/service`) utilise le
  Toolkit.
- Logique générique, profil de distribution et transport restent séparés
  (P-001) : `src/backend-packagekit` (logique et profil Debian, sans D-Bus),
  transport et adaptateurs dans `src/service`. Le service n'utilise un
  backend que par le contrat `Backend` (`backend.h`) ; seul `main.cpp`
  choisit lequel.
- Aucun composant privilégié propre (P-007) : polkit et les gestionnaires de
  paquets autorisent.
- Plan et exécution restent distincts : `mayExecute()` avant tout
  changement ; ne jamais présenter une simulation comme identique à
  l'exécution (D-017, `verifiesExecutedPlan`).
- Thread principal jamais bloqué ; complétions et événements postés dans
  l'ordre, jamais depuis l'appel (CORE.md, contrat de `Backend`).

## Conventions

C++20, CMake, Ninja ; conventions de P-013 (fichiers en minuscules sans
séparateur, membres `m_`). Code et commentaires en anglais, documentation
en français. Erreurs par `Result<T>` et `Error` (CORE.md).

## Construire et tester

```bash
cmake -S . -B build -G Ninja -DBUILD_TESTING=ON
cmake --build build
ctest --test-dir build --output-on-failure -j4
```

- Clang en `-Werror` : configurer explicitement, sinon le dossier peut être
  recréé avec GCC sans le dire :
  `cmake -S . -B build-clang -G Ninja -DBUILD_TESTING=ON -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_CXX_FLAGS=-Werror`.
- ASan et UBSan : `cmake -S . -B build-asan -G Ninja -DBUILD_TESTING=ON -DCUTEFISH_STORE_SANITIZERS=ON`.
- Les tests D-Bus lancent leur propre `dbus-daemon` sur un bus privé ; ils
  ne touchent jamais au vrai packagekitd (`--packagekit off`, ou un
  PackageKit simulé, `tests/service/fakepackagekit.h`).
- Tests contre le vrai packagekitd, hors `ctest`, requêtes et simulations
  seulement (D-026) :
  `CUTEFISH_STORE_REAL_PACKAGEKIT=1 build/tests/service/store-pkbackend-real-tests`.
- Après les tests, vérifier qu'aucun processus ne reste :
  `pgrep -af 'cutefish-store-service|cutefish-store-indexer'`. Avec
  `pkill -f` ou `pgrep -f`, écrire le motif `[b]uild/…` pour ne pas viser le
  shell qui lance la commande.
