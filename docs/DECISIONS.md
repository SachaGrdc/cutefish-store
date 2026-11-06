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
  - le cœur ne dépend d'aucune bibliothèque Cutefish (P-006) ;
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
- **Précisions ultérieures** : le backend s'appelle `backend-packagekit`
  (P-001) ; seul son transport D-Bus, dans la couche hôte, utilise
  `ToolkitBase` (P-006).

### D-008 — Nom D-Bus `org.cutefish.Store` (tranche O-002)

- **Statut** : Validée (propriétaire, après la matrice de validation de la
  phase 0.2).
- **Décision** : le nom de bus est `org.cutefish.Store`, l'objet principal
  `/org/cutefish/Store`, les interfaces `org.cutefish.Store1` et
  `org.cutefish.Store1.Transaction` ; le même préfixe sert d'identifiant
  d'application (`org.cutefish.Store`).
- **Portée** : ce choix **anticipe** une intégration dans l'écosystème
  Cutefish. Il ne signifie pas que CutefishOS a adopté le projet : aucune
  adoption officielle n'est acquise à ce jour.
- **Conséquence** : si l'intégration ne se faisait pas, un renommage serait
  nécessaire ; il est peu coûteux avant la première publication et coûteux
  après (clients, fichiers `.service`, identifiant AppStream).

### D-009 — Flatpak est la source privilégiée (tranche O-004)

- **Statut** : Validée (propriétaire, après la matrice de validation de la
  phase 0.2).
- **Décision** :
  - quand une application existe à la fois en paquet de la distribution (DEB)
    et sur Flathub, l'offre Flatpak est proposée par défaut ;
  - l'utilisateur peut toujours choisir explicitement le paquet DEB : toutes
    les offres sont affichées sur la page de l'application, et l'API installe
    l'offre que le client désigne ;
  - une offre déjà installée reste l'offre mise en avant pour cette
    application ;
  - sans remote Flathub activée, il n'y a pas d'offre Flatpak et le paquet DEB
    est proposé.
- **Ce que la préférence ne signifie pas** : elle n'affirme pas que la version
  Flatpak est plus récente ni meilleure. Les versions diffèrent selon la
  source, et un paquet Debian peut recevoir des correctifs de sécurité sans
  changer de version amont. L'interface affiche la version et la provenance de
  chaque offre, sans comparer les versions entre formats.
- **Détails** : ARCHITECTURE §6.4.

### D-010 — Mises à jour : applications seulement (tranche O-005)

- **Statut** : Validée (propriétaire, après la matrice de validation de la
  phase 0.2).
- **Décision** :
  - la boutique gère les mises à jour des **applications** qu'elle connaît :
    paquets de la distribution qui portent une application du catalogue, et
    applications Flatpak (avec les runtimes dont elles dépendent) ;
  - elle ne propose **pas** de gestion générale des mises à jour système APT :
    ni mise à jour complète du système, ni bibliothèques, noyau ou paquets
    sans application, ni mises à jour hors ligne ;
  - mettre à jour une application installée via APT n'est pas une mise à jour
    du système : seuls ses paquets, et les dépendances que PackageKit doit
    mettre à jour pour eux, figurent dans le plan.
- **Conséquence** : les mises à jour système restent du ressort de l'outil de
  la distribution (APT, mises à jour automatiques ou autre).
- **Détails** : ARCHITECTURE §5.4 et §7.2.

### P-006 — Cœur indépendant de Cutefish, `ToolkitBase` limitée à la couche hôte

- **Statut** : Validée avec modification (propriétaire, après la matrice de
  validation de la phase 0.2). L'identifiant `P-006` est conservé.
- **Décision** :
  - le cœur métier (`store-core`) ne dépend ni de `Cutefish::ToolkitBase`, ni
    de Cutefish, ni d'aucune bibliothèque graphique. Il repose sur C++20 et
    SQLite, plus libcurl pour le téléchargement des images ; aucune autre
    dépendance sans justification ;
  - le cœur ne crée ni boucle d'événements, ni thread, ni connexion D-Bus. Il
    expose des opérations synchrones ; les opérations asynchrones des backends
    lui rendent leurs résultats par une fonction `post` fournie par l'hôte ;
  - la **couche hôte** (`service`) porte la boucle d'événements, D-Bus, les
    pools de threads, les timers, la surveillance de fichiers, le lancement de
    l'indexeur, les réglages et les intégrations propres à Cutefish. C'est
    elle, et elle seule, qui utilise `Cutefish::ToolkitBase` (avec `common`
    pour ses conversions D-Bus, et l'interface) ;
  - l'indexeur ne dépend pas du toolkit : `store-core` et libappstream ;
  - l'adaptateur Flatpak ne dépend pas du toolkit : `store-core`, libflatpak et
    ses propres threads GLib ;
  - l'interface reste native Cutefish avec `Cutefish::Toolkit` (D-006).
- **Ce que la décision ne crée pas** : pas de couche d'abstraction générique
  (ni « plateforme », ni bus abstrait). Le seul point d'injection est la
  fonction `post` ; tout le reste est appelé directement par l'hôte.
- **Justification** :
  - `ToolkitBase` ne se construit pas sans Wayland, EGL et Skia, et elle est
    livrée dans le même paquet Debian que le toolkit graphique [CODE] : en
    dépendre aurait lié le portage du cœur à une décision de l'amont ;
  - l'API du toolkit est jeune (1.0, un seul mainteneur visible [CODE]) ; le
    cœur est la partie qui doit durer le plus ;
  - changer d'avis plus tard aurait touché chaque fichier du cœur ; le
    trancher maintenant ne coûte presque rien.
- **Conséquences** :
  - une autre couche hôte (autre environnement, client en ligne de commande,
    tests) se branche sur le cœur sans le modifier (ARCHITECTURE §2) ;
  - quelques utilitaires simples (cache LRU, journalisation par fonction de
    rappel) sont écrits dans le cœur au lieu d'être repris de `ToolkitBase` ;
  - O-008 n'est plus bloquante : elle ne concerne que la couche hôte.
- **Version initiale, remplacée** : le cœur, les backends, l'indexeur et le
  service devaient tous utiliser `ToolkitBase` (`EventLoop`, `ThreadPool`,
  `dbus`, `json`, `logging`, `Settings`).

---

## Décisions proposées (phase 0.2, à valider)

### P-001 — Un seul service, bibliothèques internes statiques, pas de plugins

- **Statut** : Proposée, corrigée après la matrice de validation.
- **Décision** :
  - un seul service (`cutefish-store-service`) ;
  - le code est découpé en bibliothèques statiques internes : `common`,
    `core`, `backend-packagekit`, `backend-flatpak`, `service` ;
  - les backends sont enregistrés à la compilation (options CMake), sans
    chargement dynamique ni ABI de plugin ;
  - **correction** : le backend autrefois nommé `backend-apt` devient
    `backend-packagekit`. PackageKit ne dépend pas de la distribution ; les
    particularités Debian (catalogue DEP-11, champ « données » des
    `package_id`, classement des dépôts, paquets protégés, format `deb`) sont
    isolées dans un **profil de distribution**. Le code générique ne contient
    aucune donnée propre à Debian (ARCHITECTURE §7.2).
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
  - des plugins dynamiques : complexité prématurée ;
  - un backend « APT » mêlant PackageKit et Debian : il aurait fallu le
    réécrire pour une autre distribution.

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
  - pour PackageKit, `SIMULATE` ;
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
  - PackageKit et Flatpak peuvent s'exécuter en parallèle ;
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

| Id | Statut | Question | Pourquoi elle compte | Quand trancher |
|---|---|---|---|---|
| O-001 | Ouverte | **Licence** du projet (le dépôt n'a pas de `LICENSE`). L'écosystème Cutefish est en BSD-3-Clause [CODE]. Le choix revient au propriétaire. | En-têtes SPDX, contributions, compatibilité avec libflatpak/libappstream (LGPL-2.1+, liaison dynamique) | Avant le premier fichier source |
| O-002 | Tranchée → D-008 | **Espace de noms D-Bus et identifiant d'application**. | Nom de bus, chemins d'objets, fichiers `.service`, identifiant AppStream de la boutique | — |
| O-003 | Ouverte | **Packaging** : dans `cutefishos/packaging` (convention Cutefish [CODE]) ou dans un dossier `packaging/` propre au dépôt ? | Arborescence, CI, publication | Avant la première publication |
| O-004 | Tranchée → D-009 | **Source préférée par défaut** quand une application existe en DEB et en Flatpak. | Ce que propose le bouton « Installer » | — |
| O-005 | Tranchée → D-010 | **Mises à jour système APT**. | Périmètre de l'écran « Mises à jour » | — |
| O-006 | Ouverte | **Flatpaks déjà installés en système** (présents sur la machine d'audit) : affichage seul, ou gestion complète (polkit via `flatpak-system-helper`) dès la v1 ? | Éviter de proposer un doublon utilisateur ; périmètre polkit | Avant l'implémentation du backend Flatpak |
| O-007 | Ouverte | **Dépôts éditeurs** (ex. Claude Desktop) : version cible, gouvernance du manifeste, helper privilégié. | Seul cas qui demande du code privilégié | Après la v1 |
| O-008 | Ouverte | **`ToolkitBase` hors Cutefish** : demander en amont une option de build « base seule » et/ou un paquet séparé. | Ne concerne plus que la couche hôte et le transport PackageKit : le cœur, l'indexeur et l'adaptateur Flatpak n'en dépendent pas (P-006) | Avant un portage de la couche hôte |
| O-009 | Ouverte | **Langues indexées** : langue de session et `C` seulement, ou toutes les langues configurées ? | Taille de l'index, temps d'indexation | Pendant le prototype de l'indexeur |
| O-010 | Ouverte | **Serveur de notifications** : quel serveur `org.freedesktop.Notifications` est disponible sous Cutefish ? Non vérifié en 0.1. | Notifications de mises à jour en arrière-plan | Phase 0.3 |
| O-011 | Ouverte | **Contenu éditorial** (mises en avant, sélections) : source et format. | Écran d'accueil | Avant le design de l'accueil |
| O-012 | Ouverte | **Fréquence du rafraîchissement en arrière-plan** et comportement sur connexion limitée (NetworkManager `Metered`). | Consommation réseau | Phase 0.3 |
