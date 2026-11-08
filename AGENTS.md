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
   étapes et leur état, §16 service v0.1, §17 backend PackageKit.

## État (clôture de l'étape F)

- Étapes A à F faites et validées (SERVICE.md §15).
- Prochaine étape : G (backend Flatpak utilisateur) ou F2 (gestionnaire de
  transactions : `Install`, `Remove`, `Update`), au choix du propriétaire.
  **Ne commencer aucune étape sans son accord explicite.**
- Décisions encore proposées, à faire valider : D-027, D-029, P-001, P-009,
  P-011, P-013. Questions ouvertes notables : O-001 (licence), O-014
  (formatage automatique).
- Limites connues et points jamais vérifiés : SERVICE.md §16.4 et §17.6,
  ARCHITECTURE.md §7.2 (« Reste à tester »).

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
