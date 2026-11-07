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
| **Validée avec modification** | Proposition validée par le porteur du projet sous une forme modifiée ; l'ancienne version est résumée dans la décision. |
| **Proposée** | Recommandée par la phase 0.2, en attente de validation. |
| **Ouverte** | Question identifiée, sans décision. |
| **Tranchée** | Question ouverte résolue par une décision validée, dont l'identifiant est indiqué. |
| **Remplacée** | Conservée pour l'historique ; renvoie à la décision qui la remplace. |

Niveau de preuve des justifications, repris de l'audit de la phase 0.1 :

- **[CODE]** : vérifié dans le code des dépôts CutefishOS ;
- **[DOC]** : documentation officielle ;
- **[MESURÉ]** : mesuré sur une Debian 13 (AppStream 1.0.5, PackageKit 1.3.1,
  libflatpak 1.16.6, SQLite 3.46.1, polkit 126) ;
- **[SYMBOLE]** : symbole présent dans la bibliothèque installée ; signature
  et sémantique à confirmer ;
- **[HYPOTHÈSE]** : non vérifié, à confirmer ;
- **[À VALIDER]** : point de conception à confirmer par un prototype ou une
  mesure.

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
- **Précision ultérieure** : les Flatpaks déjà installés au niveau système sont
  gérés dès la v1 (D-011) ; les **nouvelles** installations restent en
  utilisateur par défaut.

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
- **Base expérimentale** (prototype 4, phase 0.3, validé par le propriétaire
  sous réserve des tests non réalisés) : un client asynchrone sd-bus, sur un
  thread dédié avec sa propre boucle d'événements, a interrogé PackageKit
  1.3.1 (backend `apt`) sans jamais bloquer le thread appelant (attente
  maximale 6 ms) [MESURÉ sur la machine d'audit]. Requêtes en lecture et
  transactions simulées seulement ; détails dans ARCHITECTURE §7.2.

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
- **Précisions du prototype 4** [MESURÉ, backend APT de PackageKit 1.3.1] :
  - les entrées de `GetUpdates` dont la version proposée est identique à la
    version installée sont retirées de la liste ;
  - `GetUpdates` et `UpdatePackages` ne se comportent pas exactement de la
    même façon : la liste est toujours confirmée par une simulation
    `UpdatePackages(ONLY_TRUSTED | SIMULATE)` ;
  - la liste de `GetUpdates` mélange applications, composants du bureau et
    bibliothèques : le filtrage par les offres d'application du catalogue
    est indispensable.
- **Détails** : ARCHITECTURE §5.4 et §7.2.

### D-011 — Flatpaks système détectés, affichés et gérés en v1 (tranche O-006)

- **Statut** : Validée (propriétaire, après la matrice de validation de la
  phase 0.2).
- **Décision** :
  - la v1 détecte, affiche et gère les applications Flatpak de l'installation
    **système** par défaut (`/var/lib/flatpak`), en plus de l'installation
    utilisateur ;
  - les deux périmètres restent distincts partout : identifiant d'offre
    (`flatpak:user:…`, `flatpak:system:…`), affichage, plan, journal ;
  - « gérer » couvre la mise à jour et la suppression des applications
    installées en système, et la mise à jour du catalogue de leurs remotes ;
  - les nouvelles installations se font en utilisateur (D-005) ; proposer une
    nouvelle installation en système reste une question ouverte (O-013) ;
  - les remotes système sont en lecture seule en v1 (les modifier demande
    l'action `configure-remote`, `auth_admin_keep`).
- **Mécanisme** : libflatpak, dans le service sans privilège, délègue les
  écritures sur l'installation système à `flatpak-system-helper` (root,
  activé par D-Bus sur le bus système), qui interroge polkit [MESURÉ :
  binaire, fichier d'activation et actions polkit présents].
- **Les autorisations ne sont pas uniformes** [MESURÉ sur Debian 13] :
  - sans mot de passe pour l'utilisateur actif : `app-update`,
    `runtime-update`, `appstream-update`, `metadata-update`, `update-remote`,
    `modify-repo` ;
  - `auth_admin_keep` par défaut : `app-install`, `runtime-install`,
    `app-uninstall`, `runtime-uninstall`, `configure-remote`, `configure`,
    `install-bundle` ;
  - mais une règle Debian (`org.freedesktop.Flatpak.rules`) accorde sans mot
    de passe l'installation, la désinstallation et `modify-repo` aux membres
    actifs et locaux du groupe `sudo`.
  - Conséquence : le même geste demande un mot de passe à un utilisateur et
    pas à un autre. La boutique ne présume jamais d'une autorisation et traite
    les refus et annulations d'authentification.
- **Politique propre à la boutique** : une installation ou une suppression en
  système touche tous les utilisateurs. Le plan porte alors la raison
  `system-scope` et exige une confirmation explicite, même si polkit n'affiche
  aucun dialogue (P-008).
- **Détails** : ARCHITECTURE §7.4.

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

### P-008 — Transactions en deux temps : plan, puis confirmation

- **Statut** : Validée (propriétaire, phase 0.3, après le prototype 1). Le
  mécanisme Flatpak retenu est la **double passe** (mécanisme B), avec une
  comparaison **sémantique** des plans. L'identifiant `P-008` est conservé.
- **Décision** : toute transaction calcule d'abord un plan :
  - pour PackageKit, `SIMULATE` ;
  - pour Flatpak, le signal `ready` d'une première `FlatpakTransaction`.

  Le cœur applique une politique de confirmation commune. Une confirmation
  explicite est exigée si le plan :
  - supprime d'autres paquets ;
  - ajoute des permissions Flatpak ;
  - ajoute une source (remote) ;
  - change de remote (fin de vie avec rebase) ;
  - installe ou supprime dans l'installation Flatpak système (D-011).
- **Mécanisme Flatpak : double passe** :
  1. construire une première transaction ;
  2. la résoudre et copier le plan dans le signal `ready`, qui retourne
     `FALSE` : rien n'est téléchargé ni installé ;
  3. présenter le plan à l'utilisateur ;
  4. attendre la confirmation **sans conserver de transaction active** ;
  5. après `Confirm`, reconstruire une seconde transaction ;
  6. comparer son plan au plan confirmé, dans son signal `ready` ;
  7. exécuter seulement si les différences sont acceptables ; sinon,
     interrompre et redemander une confirmation avec le nouveau plan.
- **Règles de comparaison** (sémantique, pas une égalité stricte) :
  - **nouvelle confirmation nécessaire** si une opération apparaît (y compris
    une dépendance supplémentaire), si le commit, la remote ou le type d'une
    opération change, si de nouvelles permissions sont demandées, ou si une
    autre différence modifie substantiellement ce qui sera installé ou
    supprimé ;
  - **sans conséquence** : la disparition d'une opération parce qu'elle est
    déjà satisfaite (par exemple un runtime installé entre-temps) ;
  - **toute différence inconnue ou impossible à classer** entraîne un refus
    d'exécution et une nouvelle confirmation ;
  - aucune opération non couverte par la confirmation de l'utilisateur n'est
    exécutée silencieusement.
- **Justification** :
  - les décisions de sécurité ne dépendent pas de l'interface utilisée ;
  - on évite les suppressions en cascade silencieuses ;
  - la double passe laisse libres le thread et la file des transactions
    Flatpak pendant que l'utilisateur réfléchit, ne garde aucune transaction
    suspendue (question non testée pour `flatpak-system-helper`, D-011), et
    suit le même modèle que PackageKit : planifier, confirmer, exécuter.
- **Base expérimentale** : prototype 1, dépôt local
  `~/Dev/cutefish-store-prototypes/flatpak-confirm/` (commit `352aeaf`,
  documents `PROTOCOL.md`, `RESULTS-A.md`, `RESULTS-B.md`, `COMPARISON.md`).
  Environnement : Debian 13, Flatpak 1.16.6, installation **utilisateur**
  temporaire (`FLATPAK_USER_DIR`), dépôt local `file://` sans réseau ni GPG,
  attentes courtes, un seul client simulé. Constats [MESURÉ dans cet
  environnement] :
  - les deux mécanismes ont fonctionné ;
  - le mécanisme A (blocage dans `ready`) occupe le thread GLib des
    transactions pendant toute l'attente ; le mécanisme B le laisse
    disponible ;
  - la seconde passe détecte un changement de commit et de permissions
    survenu entre les passes ;
  - une comparaison stricte produit une fausse alerte quand une dépendance
    est déjà satisfaite entre les passes ;
  - tous les signaux observés sont émis sur le thread qui appelle
    `flatpak_transaction_run` ;
  - aucun verrou de fichier n'a été observé pendant l'attente dans les
    scénarios testés.
- **Restant à tester** : réseau réel et coût de la seconde résolution sur
  Flathub, remotes GPG et HTTP, plusieurs clients D-Bus, installation système
  avec `flatpak-system-helper`.
- **Précisions PackageKit** (prototype 4, validées par le propriétaire sous
  réserve des tests non réalisés) :
  - la simulation (`SIMULATE`) donne pour chaque paquet l'action, la version
    et l'origine ; sur la machine d'audit, elle a donné les mêmes plans
    qu'`apt-get -s` [MESURÉ] ;
  - toute suppression est précédée d'une simulation : le backend APT testé
    ignore `allow_deps=false` et supprime aussi les paquets qui dépendent du
    paquet demandé [MESURÉ, CODE]. Le service présente ces paquets
    supplémentaires à l'utilisateur et ne suppose jamais qu'une suppression
    se limite au paquet demandé ;
  - la version d'un `package_id` n'est pas une garantie : une version
    inexistante est remplacée sans erreur [MESURÉ]. Le service vérifie la
    version réellement présente dans le plan et refuse, ou redemande une
    confirmation, si le plan ne correspond pas à la demande ;
  - **ce n'est pas une double passe** au sens Flatpak : une simulation
    PackageKit est une transaction distincte de l'exécution, qui recalcule
    tout et n'émet pas son plan avant de l'appliquer [CODE]. La nouvelle
    simulation faite avant l'exécution réduit la fenêtre sans la fermer ;
    la correspondance entre simulation et transaction réelle n'est **pas
    garantie** et n'a pas été testée.
- **Alternative écartée** : mécanisme A, blocage dans `ready`. Il exécute
  exactement le commit présenté, mais bloque toute la file Flatpak (P-009)
  jusqu'à la décision ou au délai maximal, et laisse une transaction
  suspendue dont le comportement avec le helper système n'est pas connu.
- **Détails** : [ARCHITECTURE §7.3](ARCHITECTURE.md#73-backend-flatpak-libflatpak)
  et [§5.5](ARCHITECTURE.md#55-annulation-concurrence-et-ordre).

---

## Décisions proposées (phase 0.2, à valider)

### P-001 — Un seul service, bibliothèques internes statiques, pas de plugins

- **Statut** : Proposée, corrigée après la matrice de validation. Le point
  « backend `backend-packagekit`, qui parle à PackageKit par D-Bus avec
  sd-bus » est **validé** (propriétaire, phase 0.3, après le prototype 4) ;
  le reste de la décision reste proposé.
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

- **Statut** : Proposée, justification corrigée après la matrice de
  validation. C'est un écart par rapport à une lecture littérale de « service
  seul écrivain », expliqué ci-dessous.
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
- **Justification**, par ordre d'importance :
  1. ce sont des données venues du réseau, donc non fiables : un plantage du
     parseur ne doit pas arrêter le service ni ses transactions ;
  2. libappstream, et la GLib qu'elle tire, ne sont chargées que dans
     l'indexeur ;
  3. un processus qui se termine rend toute sa mémoire au système, quelle
     qu'en soit la quantité ; dans un service résident, la fragmentation du
     tas glibc en garderait une partie (le gestionnaire de fichiers Cutefish
     appelle `malloc_trim` pour cette raison [CODE]) ;
  4. l'indexeur peut tourner à basse priorité (`nice`, classe d'E/S idle).
- **Ce que les mesures disent, et ne disent pas** :
  - [MESURÉ] `appstreamcli convert` sur le catalogue Flathub (4 827
    composants, XML de 50 Mo) atteint un pic de 459 Mo RSS en 1,7 s. Cette
    commande analyse **et** sérialise en YAML : elle ne prouve pas que
    l'analyse seule consomme autant ;
  - [MESURÉ] `appstreamcli search`, qui passe par `AsPool` et son cache
    libxmlb, culmine à 52 Mo ;
  - la consommation réelle de l'indexeur est donc **inconnue** et sera
    mesurée sur le prototype. L'argument mémoire n'est plus le premier
    argument : les points 1 et 2 suffisent à justifier le processus séparé.
- **Correction sur `AsPool`** : la version précédente affirmait qu'`AsPool`
  masquerait la provenance des composants. C'est **faux** dans le cas
  vérifié : [MESURÉ] `appstreamcli get app.drey.Dialect`, qui interroge le
  pool de métadonnées, renvoie deux
  composants distincts portant le même identifiant, celui de Debian (paquet
  `dialect`) et celui de Flathub (bundle
  `flatpak:app/app.drey.Dialect/x86_64/stable`). L'API expose aussi
  `as_component_get_origin` et `as_component_get_data_id` [SYMBOLE]. Le choix
  entre `AsPool` (cache libxmlb) et `AsMetadata` (un fichier à la fois) est
  laissé au prototype, selon la mémoire, la durée, la conservation de la
  provenance et la sélection des langues.
- **Détails** : [ARCHITECTURE §6.2](ARCHITECTURE.md#62-chaîne-dindexation).
- **À mesurer** :
  - durée totale de l'indexation Debian + Flathub (objectif initial < 10 s en
    arrière-plan) ;
  - pic mémoire réel de l'indexeur, avec `AsPool` et avec `AsMetadata`.

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

- **Statut** : Proposée, corrigée après la matrice de validation. Écart par
  rapport à l'audit 0.1, qui suggérait une lecture SQLite directe par
  l'interface pour la recherche.
- **Décision** : l'interface n'ouvre aucune base de données du service, aucun
  fichier d'état et aucune connexion réseau. La recherche est un appel D-Bus
  asynchrone ; le service interroge l'index FTS5 sur un thread de lecture.
  Les seuls fichiers que l'interface lit sont des images désignées par le
  service, selon le contrat d'accès aux fichiers (P-005).
- **Correction** : la version précédente disait que l'interface n'ouvre
  « aucun fichier du service », ce qui contredisait P-005 (chemins d'images
  dans le cache du service).
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

- **Statut** : Proposée, précisée après la matrice de validation.
- **Décision** :
  - le service gère le téléchargement (libcurl, HTTPS uniquement), le cache
    disque et la déduplication des captures d'écran et des icônes distantes ;
  - l'interface reçoit des chemins de fichiers locaux et les affiche avec
    `ImageView`, qui décode sur un thread [CODE] ;
  - **contrat d'accès aux fichiers** : les chemins transmis désignent soit
    des icônes des catalogues (`/var/lib/swcatalog/icons`, dossiers appstream
    des installations Flatpak), soit des fichiers du cache d'images du
    service. Ce sont des fichiers partagés **en lecture seule** : le client ne
    les modifie jamais ; le service écrit chaque fichier de cache de façon
    atomique et ne le modifie plus ensuite ; un fichier peut disparaître après
    éviction, et le client le redemande alors ;
  - ces chemins ne sont **pas** lisibles par un client placé dans un bac à
    sable. Ce n'est pas un objectif de la v1 ; l'évolution prévue est le
    passage de descripteurs de fichiers (type D-Bus `h`).
- **Justification** :
  - un seul cache pour toutes les interfaces ;
  - une seule politique réseau (taille maximale, délais, HTTPS, mode hors
    ligne) ;
  - l'interface n'a pas besoin de libcurl.
- **Contrepartie** : une API d'annulation est nécessaire pour ne pas
  télécharger les images d'une liste qu'on a quittée
  ([ARCHITECTURE §5.2, Images](ARCHITECTURE.md#images)).
- **Détails du contrat d'accès** :
  [ARCHITECTURE §5.8](ARCHITECTURE.md#58-accès-des-clients-aux-fichiers-images-et-icônes).

### P-007 — Aucun composant privilégié propre en v1

- **Statut** : Proposée.
- **Décision** : la v1 ne livre aucun helper root. Les opérations
  privilégiées sont entièrement déléguées :
  - à PackageKit (APT) ;
  - à libflatpak et `flatpak-system-helper` pour l'installation Flatpak
    système (D-011).
- **Justification** : les dépôts éditeurs, qui nécessitent un helper
  privilégié (PackageKit ne sait pas ajouter un dépôt avec sa clé [CODE]),
  sont hors périmètre v1 (D-003). Sans eux, aucun code privilégié n'est
  nécessaire.

### P-009 — Transactions sérialisées par backend, parallèles entre backends

- **Statut** : Proposée. La précision PackageKit ci-dessous est **validée**
  (propriétaire, phase 0.3, après le prototype 4).
- **Décision** :
  - au plus une transaction modificatrice active par backend et par
    installation ;
  - PackageKit et Flatpak peuvent s'exécuter en parallèle ;
  - les lectures du catalogue et de `state.sqlite` (recherche, détails,
    état installé affiché) ne sont jamais bloquées par une transaction.
- **Précision PackageKit (validée)** :
  - sur le backend APT testé, PackageKit sérialise **toutes** ses
    transactions, consultations et recherches comprises : `Resolve`,
    `GetUpdates`, `GetDetails` attendent la fin de la transaction en cours
    [MESURÉ ; CODE : le backend APT ne déclare pas le parallélisme, et le
    planificateur rend alors toutes les transactions exclusives] ;
  - le service utilise donc une **file unique pour toutes les requêtes
    PackageKit** sur ce backend, et pas seulement pour les opérations
    modificatrices ;
  - une transaction modificatrice active peut retarder les consultations :
    l'interface est servie depuis `state.sqlite`, la recherche interactive
    depuis le catalogue SQLite/FTS5, et PackageKit n'est jamais interrogé à
    chaque frappe ; `Resolve` et `GetDetails` sont groupés ;
  - ce constat porte sur le backend APT de PackageKit 1.3.1. Il n'est pas
    généralisé aux autres backends PackageKit sans preuve.
- **Justification** :
  - PackageKit sérialise de toute façon les transactions APT et gère le
    verrou dpkg [DOC ; MESURÉ, prototype 4] ;
  - chaque transaction du backend APT recharge le cache APT (≈ 0,45 s sur la
    machine d'audit [MESURÉ, une seule machine, pas une garantie]) : des
    requêtes groupées et peu nombreuses coûtent bien moins que des requêtes
    individuelles répétées ;
  - un seul `FlatpakTransaction` à la fois évite les contentions de verrou
    du dépôt OSTree et simplifie l'annulation.

### P-010 — Cycle de vie du service : activation D-Bus, timer, arrêt sur inactivité

- **Statut** : Proposée, règle d'inactivité précisée après la matrice de
  validation.
- **Décision** :
  - le service est activé par D-Bus (bus de session) et décrit par une unité
    systemd utilisateur (`Type=dbus`) ;
  - un timer systemd utilisateur le démarre périodiquement pour rafraîchir
    les catalogues et vérifier les mises à jour ;
  - **règle d'inactivité** :
    1. un *client connu* est une connexion D-Bus (nom unique) qui a appelé au
       moins une méthode du service, lecture des propriétés comprise. Le
       service suit sa disparition (`NameOwnerChanged`). Un simple abonné aux
       signaux n'est pas compté : l'interface lit les propriétés dès son
       démarrage ;
    2. le service reste actif tant qu'au moins un client connu est connecté ;
    3. il ne s'arrête jamais pendant une transaction non terminale (y compris
       `queued` et `awaiting-confirmation`), une indexation ou un
       rafraîchissement ;
    4. il s'arrête après un délai défini suivant la disparition du dernier
       client, quand plus rien n'est en cours : 5 minutes proposées
       [À VALIDER]. Un démarrage par le timer, sans client, suit la même
       règle une fois le travail terminé ;
    5. les téléchargements d'images en file sont abandonnés à l'arrêt ; un
       fichier en cours d'écriture est supprimé (§5.8).
- **Justification** :
  - zéro mémoire consommée quand la boutique ne sert pas ;
  - modèle identique aux services de cutefish-core et de l'installeur
    (activation D-Bus et arrêt sur inactivité [CODE]) ;
  - une fenêtre ouverte garde le service actif : elle continue de recevoir
    les signaux de progression et de catalogue.

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
  calculator, filemanager et settings. Les noms de fichiers suivent les
  applications Cutefish ; le toolkit, lui, utilise le `snake_case`
  (`list_view.h`) : c'est un choix assumé, pas une convention unique de
  l'écosystème.
- **Vérification du formatage automatique** (demandée après la matrice de
  validation) :
  - [CODE, API GitHub, 6 novembre 2026] aucun des 25 dépôts de
    l'organisation `cutefishos` n'a de `.clang-format` à la racine ;
  - [CODE, clones locaux de 19 dépôts, `cutefish-webkit` exclu car c'est un
    portage de WebKit] aucun `.clang-format`, `_clang-format`, `.clang-tidy`
    ni `.editorconfig` dans les trois premiers niveaux d'arborescence ;
  - la CI partagée (`cutefishos/.github`) ne contient aucune vérification de
    formatage ;
  - **conclusion** : il n'existe pas de convention amont de formatage
    automatique à reprendre. Ce document n'en invente pas. Ajouter un
    `.clang-format` propre à Cutefish Store serait une décision du projet
    (O-014), à présenter comme telle et non comme une règle de Cutefish.

---

## Questions ouvertes et tranchées

| Id | Statut | Question | Pourquoi elle compte | Quand trancher |
|---|---|---|---|---|
| O-001 | Ouverte | **Licence** du projet (le dépôt n'a pas de `LICENSE`). L'écosystème Cutefish est en BSD-3-Clause [CODE]. Le choix revient au propriétaire. | En-têtes SPDX, contributions, compatibilité avec libflatpak/libappstream (LGPL-2.1+, liaison dynamique) | Avant le premier fichier source |
| O-002 | Tranchée → D-008 | **Espace de noms D-Bus et identifiant d'application**. | Nom de bus, chemins d'objets, fichiers `.service`, identifiant AppStream de la boutique | — |
| O-003 | Ouverte | **Packaging** : dans `cutefishos/packaging` (convention Cutefish [CODE]) ou dans un dossier `packaging/` propre au dépôt ? | Arborescence, CI, publication | Avant la première publication |
| O-004 | Tranchée → D-009 | **Source préférée par défaut** quand une application existe en DEB et en Flatpak. | Ce que propose le bouton « Installer » | — |
| O-005 | Tranchée → D-010 | **Mises à jour système APT**. | Périmètre de l'écran « Mises à jour » | — |
| O-006 | Tranchée → D-011 | **Flatpaks déjà installés en système**. | Éviter de proposer un doublon utilisateur ; périmètre polkit | — |
| O-007 | Ouverte | **Dépôts éditeurs** (ex. Claude Desktop) : version cible, gouvernance du manifeste, helper privilégié. | Seul cas qui demande du code privilégié | Après la v1 |
| O-008 | Ouverte | **`ToolkitBase` hors Cutefish** : demander en amont une option de build « base seule » et/ou un paquet séparé. | Ne concerne plus que la couche hôte et le transport PackageKit : le cœur, l'indexeur et l'adaptateur Flatpak n'en dépendent pas (P-006) | Avant un portage de la couche hôte |
| O-009 | Ouverte | **Langues indexées** : langue de session et `C` seulement, ou toutes les langues configurées ? | Taille de l'index, temps d'indexation | Pendant le prototype de l'indexeur |
| O-010 | Ouverte | **Serveur de notifications** : quel serveur `org.freedesktop.Notifications` est disponible sous Cutefish ? Non vérifié en 0.1. | Notifications de mises à jour en arrière-plan | Phase 0.3 |
| O-011 | Ouverte | **Contenu éditorial** (mises en avant, sélections) : source et format. | Écran d'accueil | Avant le design de l'accueil |
| O-012 | Ouverte | **Fréquence du rafraîchissement en arrière-plan** et comportement sur connexion limitée (NetworkManager `Metered`). | Consommation réseau | Phase 0.3 |
| O-013 | Ouverte | **Nouvelles installations Flatpak en système** : la boutique doit-elle permettre de choisir l'installation système pour une nouvelle application, et pour qui ? | Effet sur tous les utilisateurs ; autorisations différentes selon le groupe `sudo` (D-011) | Après la v1, ou avant si le propriétaire le souhaite |
| O-014 | Ouverte | **Formatage automatique** : faut-il un `.clang-format` propre au projet, et lequel ? Cutefish n'en fournit aucun (P-013). | Homogénéité du code, vérification en CI | Avant le premier fichier source |
