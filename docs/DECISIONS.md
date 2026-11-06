# Cutefish Store — Registre des décisions

Ce document conserve les décisions d'architecture de Cutefish Store, leur
justification et les questions encore ouvertes. Il accompagne
[ARCHITECTURE.md](ARCHITECTURE.md), qui décrit la conception elle-même.

## Conventions

Chaque décision porte un identifiant stable, qui ne change jamais, même si la
décision est remplacée.

| Statut | Signification |
|---|---|
| **Validée** | Décidée par le porteur du projet. Ne se remet en cause que sur un problème technique concret et argumenté. |
| **Proposée** | Recommandée par la phase 0.2, en attente de validation. |
| **Ouverte** | Question identifiée, sans décision. |
| **Remplacée** | Conservée pour l'historique ; renvoie à la décision qui la remplace. |

Niveau de preuve des justifications, repris de l'audit de la phase 0.1 :

- **[CODE]** : vérifié dans le code des dépôts CutefishOS ;
- **[DOC]** : documentation officielle ;
- **[MESURÉ]** : mesuré sur une Debian 13 (AppStream 1.0.5, PackageKit 1.3.1,
  libflatpak 1.16.6, SQLite 3.46.1) ;
- **[HYPOTHÈSE]** : non vérifié, à confirmer.

---

## Décisions validées

### D-001 — Ambition hybride

- **Statut** : Validée (phase 0.2).
- **Décision** : Cutefish Store est d'abord une application native Cutefish.
  Son cœur technique est indépendant de l'environnement graphique, pour
  permettre d'autres interfaces et un portage vers d'autres environnements
  Linux. X11 n'est pas exigé en v1.
- **Conséquences** :
  - aucune dépendance graphique (Wayland, Skia, EGL) hors de l'interface ;
  - le contrat entre interface et cœur est l'API D-Bus, pas un en-tête C++.

### D-002 — Service utilisateur séparé dès la v1

- **Statut** : Validée (phase 0.2).
- **Décision** : un service utilisateur, sur le bus de session, héberge le
  cœur. L'interface lui parle exclusivement par D-Bus.
- **Justification** :
  - GLib (libflatpak, libappstream) reste hors de l'interface ;
  - un plantage de backend ne ferme pas la fenêtre ;
  - les transactions survivent à la fermeture de l'interface ;
  - les mises à jour se vérifient sans fenêtre ouverte.

### D-003 — Formats de la v1 : APT/DEB et Flatpak

- **Statut** : Validée (phase 0.2).
- **Décision** : seuls APT/DEB (via PackageKit) et Flatpak sont intégrés en
  v1. Snap, AppImage, RPM et les dépôts éditeurs viendront plus tard.
- **Conséquence** : l'abstraction des backends est conçue pour deux
  implémentations réelles, pas pour des formats hypothétiques.

### D-004 — AppStream via libappstream, GLib isolée

- **Statut** : Validée (phase 0.2), précisée par P-002.
- **Décision** : les catalogues AppStream (DEP-11 Debian, XML Flathub) sont
  lus avec libappstream. GLib ne doit jamais être imposée à l'interface.

### D-005 — Flatpak en installation utilisateur par défaut

- **Statut** : Validée (phase 0.2).
- **Décision** : les applications Flatpak s'installent dans l'installation
  utilisateur (`~/.local/share/flatpak`). L'architecture garde la porte
  ouverte à l'installation système.
- **Justification** :
  - aucune élévation de privilèges n'est nécessaire ;
  - c'est cohérent avec un service sans privilèges.

### D-006 — Interface en C++20 avec cutefish-toolkit

- **Statut** : Validée (phase 0.2).
- **Décision** : l'interface utilise C++20 et `Cutefish::Toolkit`, pas Qt.
- **Justification** [CODE] : l'écosystème Cutefish actuel n'utilise plus Qt.
  Le toolkit est un toolkit maison (Skia, Wayland), et seul ce choix donne une
  intégration réelle (thème, flou, menu global).

### D-007 — APT via PackageKit en D-Bus direct (sd-bus)

- **Statut** : Validée (phase 0.2).
- **Décision** : le backend APT parle à `org.freedesktop.PackageKit` en D-Bus,
  via le wrapper sd-bus de `Cutefish::ToolkitBase`, sans packagekit-glib2.
- **Justification** [CODE][MESURÉ] :
  - PackageKit fournit déjà le démon privilégié, polkit, la file de
    transactions et la gestion des verrous ;
  - le parler en D-Bus évite GLib dans ce backend.

---

## Décisions proposées (phase 0.2, à valider)

### P-001 — Un seul service, bibliothèques internes statiques, pas de plugins

- **Statut** : Proposée.
- **Décision** :
  - un seul service (`cutefish-store-service`) ;
  - le code est découpé en bibliothèques statiques internes : `common`,
    `core`, `backend-apt`, `backend-flatpak`, `service` ;
  - les backends sont enregistrés à la compilation (options CMake), sans
    chargement dynamique ni ABI de plugin.
- **Justification** :
  - il n'y a que deux backends en v1 ;
  - un système de plugins `dlopen` imposerait une ABI stable, du versionnage
    et une validation au chargement, sans bénéfice tant que les backends sont
    livrés avec le service ;
  - le découpage en bibliothèques suffit à tester chaque partie isolément.
  - C'est la même organisation que les applications Cutefish
    (`calculator-engine`, `calculator-ui`) [CODE].
- **Alternatives écartées** :
  - un service par backend : plus d'IPC, plus de cycles de vie, aucun gain ;
  - des plugins dynamiques : complexité prématurée.

### P-002 — Indexation AppStream dans un sous-processus éphémère

- **Statut** : Proposée. C'est un écart par rapport à une lecture littérale
  de « service seul écrivain », expliqué ci-dessous.
- **Décision** :
  - le parsing AppStream est confié à `cutefish-store-indexer`, un exécutable
    lancé par le service à la demande ;
  - il lit les catalogues avec libappstream et écrit un **instantané candidat**
    du catalogue dans un fichier temporaire, puis se termine ;
  - le service valide cet instantané (version de schéma, `PRAGMA
    integrity_check`, nombre de composants plausible), puis le met en place par
    renommage atomique.
  - Le service reste le seul écrivain de la base **en service** ; l'indexeur
    ne touche jamais la base en cours d'utilisation.
- **Justification** :
  - [MESURÉ] le parsing complet du catalogue Flathub par libappstream
    (`appstreamcli convert`, 4 827 composants, XML de 50 Mo) atteint un pic de
    **459 Mo RSS** en 1,7 s. Dans un service résident, la fragmentation du tas
    glibc garderait une partie de cette mémoire après coup (le gestionnaire de
    fichiers Cutefish appelle déjà `malloc_trim` pour cette raison [CODE]) ;
  - ce sont des données réseau non fiables : un plantage du parseur ne tue pas
    le service ;
  - libappstream, et la GLib qu'elle tire, n'est chargée que dans l'indexeur ;
  - l'indexeur peut tourner à basse priorité (`nice`, classe d'E/S idle).
- **Détails** : [ARCHITECTURE §6.2](ARCHITECTURE.md#62-chaîne-dindexation).
- **À mesurer** :
  - durée totale de l'indexation Debian + Flathub (objectif initial < 10 s en
    arrière-plan) ;
  - pic mémoire réel de l'indexeur, qui peut différer de `appstreamcli
    convert` [HYPOTHÈSE].

### P-003 — Deux bases SQLite : catalogue (instantané) et état

- **Statut** : Proposée.
- **Décision** :
  - `catalog.sqlite` est un instantané en lecture seule, reconstruit
    entièrement à chaque changement de catalogue et remplacé atomiquement ;
  - `state.sqlite` est la base mutable du service : état installé, mises à
    jour, journal des transactions, préférences, cache d'images.
- **Justification** :
  - les cycles de vie sont différents : le catalogue est reconstruit
    rarement et en bloc, l'état change à chaque transaction ;
  - la reconstruction complète supprime la logique de mise à jour
    incrémentale et ses incohérences ;
  - le remplacement atomique donne une « génération » de catalogue claire,
    exposée par l'API.

### P-004 — Toute la communication interface ↔ service passe par D-Bus, recherche comprise

- **Statut** : Proposée. Écart par rapport à l'audit 0.1, qui suggérait une
  lecture SQLite directe par l'interface pour la recherche.
- **Décision** : l'interface n'ouvre aucun fichier du service. La recherche
  est un appel D-Bus asynchrone ; le service interroge l'index FTS5 sur un
  thread de lecture.
- **Justification** :
  - le schéma SQLite reste privé et peut évoluer ;
  - toutes les interfaces futures (D-001) utilisent le même contrat ;
  - un aller-retour D-Bus local coûte typiquement moins d'une milliseconde
    [HYPOTHÈSE, à mesurer], ce qui est négligeable devant l'objectif p95
    < 50 ms.
- **Détails** : [ARCHITECTURE §5](ARCHITECTURE.md#5-spécification-préliminaire-de-lapi-d-bus).
- **Condition de révision** : si la mesure montre que D-Bus empêche de tenir
  l'objectif de recherche, rouvrir la question (lecture directe en lecture
  seule).

### P-005 — Le service télécharge et met en cache les images

- **Statut** : Proposée.
- **Décision** :
  - le service gère le téléchargement (libcurl, HTTPS uniquement), le cache
    disque et la déduplication des captures d'écran et des icônes distantes ;
  - l'interface reçoit des chemins de fichiers locaux et les affiche avec
    `ImageView`, qui décode sur un thread [CODE].
- **Justification** :
  - un seul cache pour toutes les interfaces ;
  - une seule politique réseau (taille maximale, délais, HTTPS, mode hors
    ligne) ;
  - l'interface n'a pas besoin de libcurl.
- **Contrepartie** : une API d'annulation est nécessaire pour ne pas
  télécharger les images d'une liste qu'on a quittée
  ([ARCHITECTURE §5.2, Images](ARCHITECTURE.md#images)).

### P-006 — Le cœur et le service s'appuient sur `Cutefish::ToolkitBase`

- **Statut** : Proposée.
- **Décision** : `core`, les backends, l'indexeur et le service utilisent
  `Cutefish::ToolkitBase` (`EventLoop`, `ThreadPool`, `Signal`, `dbus`,
  `json`, `logging`, `Settings`). Seule l'interface lie `Cutefish::Toolkit`.
- **Justification** [CODE] :
  - `ToolkitBase` est une bibliothèque distincte du toolkit graphique. Elle
    ne dépend que d'ICU et de libsystemd (« for services and daemons as well
    as applications ») ;
  - les services de cutefish-core (`privacy`, `applicationsfolder`) la lient
    déjà seule ;
  - la réutiliser évite de réécrire une boucle d'événements, un pool de
    threads et un wrapper sd-bus.
- **Risque pour la portabilité** [CODE] :
  - le paquet Debian `cutefish-toolkit` livre les deux bibliothèques
    ensemble ;
  - le `CMakeLists.txt` du toolkit exige Wayland, EGL et Skia même pour
    construire `ToolkitBase`.
  - Porter le service hors de Cutefish demande donc soit de construire le
    toolkit entier, soit d'obtenir en amont une option « base seule »
    (voir O-008).

### P-007 — Aucun composant privilégié propre en v1

- **Statut** : Proposée.
- **Décision** : la v1 ne livre aucun helper root. Les opérations
  privilégiées sont entièrement déléguées :
  - à PackageKit (APT) ;
  - à libflatpak et `flatpak-system-helper` (seulement si l'installation
    système est activée plus tard).
- **Justification** : les dépôts éditeurs, qui nécessitent un helper
  privilégié (PackageKit ne sait pas ajouter un dépôt avec sa clé [CODE]),
  sont hors périmètre v1 (D-003). Sans eux, aucun code privilégié n'est
  nécessaire.

### P-008 — Transactions en deux temps : plan, puis confirmation

- **Statut** : Proposée.
- **Décision** : toute transaction calcule d'abord un plan :
  - pour APT, `SIMULATE` de PackageKit ;
  - pour Flatpak, le signal `ready` de `FlatpakTransaction`.
  Le cœur applique une politique de confirmation commune. Une confirmation
  explicite est exigée si le plan :
  - supprime d'autres paquets ;
  - ajoute des permissions Flatpak ;
  - ajoute une source (remote) ;
  - change de remote (fin de vie avec rebase).
- **Justification** :
  - les décisions de sécurité ne dépendent pas de l'interface utilisée ;
  - on évite les suppressions en cascade silencieuses.
- **Détails** : [ARCHITECTURE §7.3](ARCHITECTURE.md#73-backend-flatpak-libflatpak)
  (double passe Flatpak) et [§5.5](ARCHITECTURE.md#55-annulation-concurrence-et-ordre).

### P-009 — Transactions sérialisées par backend, parallèles entre backends

- **Statut** : Proposée.
- **Décision** :
  - au plus une transaction modificatrice active par backend et par
    installation ;
  - APT et Flatpak peuvent s'exécuter en parallèle ;
  - les lectures (recherche, détails) ne sont jamais bloquées par une
    transaction.
- **Justification** :
  - PackageKit sérialise de toute façon les transactions APT et gère le
    verrou dpkg [DOC] ;
  - un seul `FlatpakTransaction` à la fois évite les contentions de verrou
    du dépôt OSTree et simplifie l'annulation.

### P-010 — Cycle de vie du service : activation D-Bus, timer, arrêt sur inactivité

- **Statut** : Proposée.
- **Décision** :
  - le service est activé par D-Bus (bus de session) et décrit par une unité
    systemd utilisateur (`Type=dbus`) ;
  - un timer systemd utilisateur le démarre périodiquement pour rafraîchir
    les catalogues et vérifier les mises à jour ;
  - il s'arrête après une période d'inactivité (aucune transaction en cours,
    aucun client depuis N minutes).
- **Justification** :
  - zéro mémoire consommée quand la boutique ne sert pas ;
  - modèle identique aux services de cutefish-core et de l'installeur
    (activation D-Bus et arrêt sur inactivité [CODE]).

### P-011 — Remote Flathub utilisateur créée depuis un fichier embarqué

- **Statut** : Proposée.
- **Décision** :
  - si l'installation utilisateur n'a pas de remote Flathub, le service la
    crée à partir d'un `flathub.flatpakrepo` livré avec la boutique (clé GPG
    incluse), jamais téléchargé à l'exécution ;
  - cela se fait uniquement après le consentement de l'utilisateur au premier
    lancement.
- **Justification** :
  - la clé de confiance ne transite pas par le réseau au moment de l'ajout ;
  - ajouter une source tierce reste un choix explicite de l'utilisateur.

### P-012 — API D-Bus versionnée par le nom d'interface

- **Statut** : Proposée.
- **Décision** :
  - les interfaces portent un numéro de version (`…Store1`,
    `…Store1.Transaction`) ;
  - les ajouts compatibles (nouvelles clés `a{sv}`, nouvelles méthodes) ne
    changent pas ce numéro ;
  - une rupture crée `…Store2`, et les deux versions coexistent le temps de
    la migration.
- **Justification** : les dictionnaires `a{sv}` permettent d'étendre l'API
  sans casser les clients existants.

### P-013 — Conventions de code Cutefish

- **Statut** : Proposée.
- **Décision** :
  - C++20, CMake ≥ 3.22, Ninja ;
  - noms de fichiers en minuscules sans séparateur (`transactionmanager.cpp`),
    comme les applications Cutefish ; namespaces en snake_case
    (`cutefish::store`), classes en CamelCase, membres `m_`, callbacks `onXxx` ;
  - en-têtes SPDX, gettext, GoogleTest avec `BUILD_TESTING` désactivé par
    défaut.
- **Justification** [CODE] : ce sont les conventions observées dans
  calculator, filemanager et settings.

---

## Questions ouvertes

| Id | Question | Pourquoi elle compte | Quand trancher |
|---|---|---|---|
| O-001 | **Licence** du projet (le dépôt n'a pas de `LICENSE`). L'écosystème Cutefish est en BSD-3-Clause [CODE]. | En-têtes SPDX, contributions, compatibilité avec libflatpak/libappstream (LGPL-2.1+, liaison dynamique) | Avant le premier fichier source |
| O-002 | **Espace de noms D-Bus et identifiant d'application** : `org.cutefish.Store` suppose un projet adopté par CutefishOS. Sinon, un nom en DNS inverse sous le contrôle du projet (par ex. `io.github.sachagrdc.CutefishStore`). | Nom de bus, chemins d'objets, fichiers `.service`, identifiant AppStream de la boutique | Avant la phase 0.3 |
| O-003 | **Packaging** : dans `cutefishos/packaging` (convention Cutefish [CODE]) ou dans un dossier `packaging/` propre au dépôt ? | Arborescence, CI, publication | Avant la première publication |
| O-004 | **Source préférée par défaut** quand une application existe en DEB et en Flatpak. | Ce que propose le bouton « Installer » | Avant l'implémentation de la page de détail |
| O-005 | **Mises à jour système APT** : la boutique les montre-t-elle toutes (paquets sans AppStream compris) ? Mises à jour hors ligne (`org.freedesktop.PackageKit.Offline`) ? | Périmètre de l'écran « Mises à jour », risque de casser le système | Avant l'implémentation des mises à jour |
| O-006 | **Flatpaks déjà installés en système** (présents sur la machine d'audit) : affichage seul, ou gestion complète (polkit via `flatpak-system-helper`) dès la v1 ? | Éviter de proposer un doublon utilisateur ; périmètre polkit | Avant l'implémentation du backend Flatpak |
| O-007 | **Dépôts éditeurs** (ex. Claude Desktop) : version cible, gouvernance du manifeste, helper privilégié. | Seul cas qui demande du code privilégié | Après la v1 |
| O-008 | **`ToolkitBase` hors Cutefish** : demander en amont une option de build « base seule » et/ou un paquet séparé. | Portabilité du service (D-001) | Avant tout travail de portage |
| O-009 | **Langues indexées** : langue de session et `C` seulement, ou toutes les langues configurées ? | Taille de l'index, temps d'indexation | Pendant le prototype de l'indexeur |
| O-010 | **Serveur de notifications** : quel serveur `org.freedesktop.Notifications` est disponible sous Cutefish ? Non vérifié en 0.1. | Notifications de mises à jour en arrière-plan | Phase 0.3 |
| O-011 | **Contenu éditorial** (mises en avant, sélections) : source et format. | Écran d'accueil | Avant le design de l'accueil |
| O-012 | **Fréquence du rafraîchissement en arrière-plan** et comportement sur connexion limitée (NetworkManager `Metered`). | Consommation réseau | Phase 0.3 |
