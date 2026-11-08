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

### D-009 — Offre proposée par défaut : Flathub, puis la distribution, puis les remotes tiers (tranche O-004)

- **Statut** : Validée (phase 0.2), **précisée par le propriétaire en phase
  0.4** (question Q-2 de [SERVICE.md](SERVICE.md)) : ordre complet, remotes
  Flatpak tiers compris.
- **Décision** :
  - une offre **déjà installée** reste l'offre mise en avant pour cette
    application (inchangé) ;
  - sinon, l'offre proposée par défaut est choisie dans cet ordre :
    1. une offre **Flathub** ;
    2. le paquet du **dépôt officiel de la distribution** (DEB) ;
    3. une offre d'une **remote Flatpak tierce** ;
  - entre deux offres Flathub de la même application, l'une dans
    l'installation utilisateur et l'autre en système : l'offre installée,
    sinon l'offre utilisateur (D-005, ARCHITECTURE §6.4) ;
  - l'utilisateur peut **toujours** choisir une autre offre : toutes les
    offres sont affichées sur la page de l'application, et l'API installe
    l'offre que le client désigne ;
  - sans remote Flathub activée, il n'y a pas d'offre Flathub : le paquet de
    la distribution vient en premier.
- **Indépendance de la priorité éditoriale** : cet ordre choisit **l'offre à
  installer**. Il ne choisit pas les **textes affichés** (nom, résumé,
  description, icône), qui suivent la priorité éditoriale des sources
  (`Source::priority`, CATALOG §4). Les deux règles ne doivent jamais être
  dérivées l'une de l'autre : une application peut afficher les textes de
  Debian et proposer l'offre Flathub.
- **Cas non couvert** : un paquet DEB venu d'un dépôt APT tiers (source
  PackageKit de nature `third-party`). Il n'existe pas dans le catalogue v1,
  qui n'indexe que le DEP-11 de la distribution ; s'il apparaissait, son rang
  serait à trancher avant d'être proposé par défaut [À TRANCHER].
- **Ce que la préférence ne signifie pas** : elle n'affirme pas que la version
  Flatpak est plus récente ni meilleure. Les versions diffèrent selon la
  source, et un paquet Debian peut recevoir des correctifs de sécurité sans
  changer de version amont. L'interface affiche la version et la provenance de
  chaque offre, sans comparer les versions entre formats.
- **Version précédente, remplacée** : « l'offre Flatpak, si une remote
  Flathub est activée ; sinon le paquet de la distribution » ; le rang des
  remotes tierces n'était pas défini.
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

### D-012 — Identifiants `<provides><id>` : alias non ambigus, jamais de fusion

- **Statut** : Validée (propriétaire, après le Core v0.2). Remplace la règle 2
  « Alias explicites » de ARCHITECTURE §6.4, qui fusionnait deux applications
  liées par un `<provides><id>`.
- **Décision** :
  - un identifiant déclaré dans `<provides><id>` est normalisé comme un
    identifiant de composant (suffixe `.desktop` retiré) ;
  - il devient un **alias** de l'application qui le déclare seulement si
    **une seule** application le revendique et si **aucune** application ne
    porte déjà cet identifiant, quel que soit l'ordre d'import ;
  - sinon il est ignoré et compté comme ambigu ; un identifiant invalide est
    compté à part ; un identifiant égal à celui du composant est ignoré ;
  - un `<provides><id>` ne **fusionne jamais** deux applications et ne
    déplace jamais d'offre ; la recherche par identifiant essaie toujours les
    identifiants d'application avant les alias.
- **Justification** :
  - une revendication n'est qu'une correspondance probable : la fusion
    aurait réuni des applications distinctes sur la seule déclaration d'une
    source (par exemple `org.kde.kwalletmanager5` qui déclarerait
    `org.kde.kwalletmanager`) ;
  - la règle est déterministe et indépendante de l'ordre des sources.
- **Mesure** [MESURÉ, catalogues Debian `main` et Flathub du 7 novembre
  2026] : 225 alias gardés, 79 identifiants ambigus, aucun invalide.
- **Tests** : `tests/core/testaliases.cpp` (revendication unique, forme
  `.desktop`, double revendication, identifiant d'une application existante
  dans les deux ordres d'import, même application revendiquant deux fois,
  identifiant propre, identifiants invalides, ordre de recherche).
- **Détails** : [CATALOG.md](CATALOG.md) §4,
  [ARCHITECTURE §6.4](ARCHITECTURE.md#64-identifiants-correspondances-et-dédoublonnage).

### D-013 — Langues du catalogue (tranche O-009)

- **Statut** : Validée (propriétaire, phase 0.4, question Q-3).
- **Décision** :
  - noms, résumés et mots-clés : **toutes** les langues disponibles dans les
    catalogues ;
  - descriptions : le texte original non traduit (`C`) et le **français** ;
  - le mécanisme existant de sélection et de repli est conservé : à la
    lecture, la langue de session, puis sa langue seule (`fr_FR` → `fr`),
    puis `C`.
- **Mise en œuvre** : le service lance l'indexeur avec `--locale fr`. L'index
  de recherche par mots couvre `C` et le français ; les noms exacts et les
  préfixes de noms sont trouvés dans toutes les langues (CATALOG §6).
- **Conséquences** :
  - une session dans une autre langue voit les noms et résumés traduits, et
    la description en `C` ;
  - un changement de langue de session ne demande plus de réindexation : seule
    la lecture change ;
  - coût mesuré : 28,2 Mo contre 25,0 Mo avec `C` seulement, durée
    inchangée à la mesure près [MESURÉ, CATALOG §5].

### D-014 — Mise à jour Flatpak système qui installe un runtime (tranche partielle O-013)

- **Statut** : Validée (propriétaire, phase 0.4, question Q-4).
- **Décision** : une mise à jour d'une application installée **en système**
  peut installer un nouveau runtime système si elle l'exige, aux conditions
  suivantes :
  - le runtime figure dans le plan présenté ;
  - une confirmation explicite est demandée (raison `system-scope`, P-008) ;
  - le mécanisme B (double passe) s'applique ; une différence substantielle
    à la seconde passe exige une nouvelle confirmation ;
  - l'authentification polkit demandée par `flatpak-system-helper` est
    respectée ; selon le code de Flatpak 1.16.6, ce runtime est déployé sous
    l'action `org.freedesktop.Flatpak.app-install` [CODE, prototype 2 ; à
    confirmer en VM] ;
  - **aucune installation silencieuse** d'un runtime système supplémentaire.
- **Ce qui reste ouvert (O-013)** : proposer une **nouvelle** installation
  d'application en système.

### D-015 — Actualisation des catalogues (tranche O-012)

- **Statut** : Validée (propriétaire, phase 0.4, question Q-5).
- **Décision** :
  - **import initial** au premier lancement, quand aucun catalogue n'existe ;
  - **actualisation automatique toutes les 24 heures** ;
  - **vérification de la date du dernier rafraîchissement à l'ouverture** de
    l'interface : au-delà de 24 heures, une actualisation est lancée ;
  - **aucune actualisation automatique sur une connexion limitée** ;
  - **actualisation manuelle toujours possible**, y compris sur une connexion
    limitée ;
  - en cas d'échec, **le catalogue précédent reste en service** (remplacement
    atomique, CATALOG §7) ;
  - **une seule actualisation à la fois** : une demande pendant une
    actualisation est fusionnée avec elle.
- **Mise en œuvre proposée** : [SERVICE.md](SERVICE.md) §3.5.

### D-016 — Dépendances de compilation du service

- **Statut** : Validée (propriétaire, phase 0.4, question Q-1).
- **Décision** : le backend Flatpak utilise `libflatpak-dev` ; le cache
  d'images utilise libcurl dans sa variante OpenSSL (`libcurl4-openssl-dev`).
  Le propriétaire installe ces paquets ; aucune commande root n'est lancée par
  les outils de développement.

### D-017 — Vérification du plan d'exécution et modèle d'états (précise P-008)

- **Statut** : Validée (propriétaire, phase 0.4, après le Core v0.3a).
- **Décision** :
  - une transaction reste en `planning` jusqu'à ce que le plan que le backend
    va **réellement exécuter** soit accepté par `verifyExecutionPlan()` (même
    comparaison sémantique que P-008, contre le plan accepté) ;
  - plan compatible : `planning → running`. `running` signifie donc toujours
    que le backend peut modifier le système ;
  - plan qui exige une nouvelle confirmation : `planning →
    awaiting-confirmation`, raison `plan-changed` ; rien n'est exécuté ;
  - la transition `running → awaiting-confirmation` n'existe pas ;
  - une transaction ne peut pas réussir sans cette vérification ;
  - transitions validées en plus du diagramme initial : `planning →
    cancelled`, `queued → interrupted`, `planning → interrupted`.
- **Flatpak** : la vérification a lieu dans le signal `ready` de la
  transaction exécutée (seconde passe) ; `ready` est émis avant toute
  opération, avec toutes les opérations résolues, et retourner `FALSE`
  arrête la transaction sans rien télécharger ni déployer [MESURÉ en
  installation utilisateur, prototype 1 ; à confirmer en installation
  système, en VM]. La comparaison est une fonction pure du cœur, appelable
  depuis le thread Flatpak.
- **Limite PackageKit, maintenue explicitement** : la vérification ne peut
  porter que sur une **nouvelle simulation**, transaction distincte de
  l'exécution. La vraie transaction recalcule tout, et rien ne lie les deux
  appels [CODE et MESURÉ, prototype 4]. La simulation **ne garantit pas** à
  elle seule que le plan exécuté sera identique : la fenêtre est réduite, pas
  fermée. Garde-fous proposés, à tester en VM : nouvelle simulation juste
  avant l'exécution dans la file PackageKit unique, nouvelle simulation si
  l'état de dpkg ou des listes APT a changé depuis, annulation si
  `DownloadSizeRemaining` diffère du plan tant que l'annulation est
  possible, comparaison après coup des paquets traités avec le plan.
- **Détails** : ARCHITECTURE §3.5, SERVICE.md §5.1, CORE.md.

### D-018 — Conservation du journal des transactions (question Q-8a)

- **Statut** : Validée (propriétaire, phase 0.4).
- **Décision** : les deux limites s'appliquent, la plus stricte l'emporte :
  - au plus **200 transactions terminées** ;
  - au plus **90 jours** après leur fin ;
  - une transaction **non terminée** n'est jamais supprimée ;
  - les plans JSON (plan accepté, dernier plan) sont conservés avec la
    transaction pendant toute la durée de rétention.
- **Mise en œuvre** : `state.sqlite` (SERVICE.md §8.2), étape B.

### D-019 — Délai de confirmation (question Q-8b)

- **Statut** : Validée (propriétaire, phase 0.4).
- **Décision** : une transaction en `awaiting-confirmation` attend au plus
  **10 minutes**, puis passe en `cancelled`. Le délai est géré par le
  service (minuterie de la boucle principale), sans thread ni verrou
  conservé pendant l'attente. Le cœur n'a pas d'horloge : le service appelle
  `Transaction::cancel()` à l'échéance.

### D-020 — Sources du service v0.1 : fichier de configuration

- **Statut** : Validée (propriétaire, étape E).
- **Décision** : tant que les backends ne fournissent pas
  `Backend::catalogSources()` (étapes F et G), le service lit les sources à
  indexer dans un fichier déclaratif,
  `$XDG_CONFIG_HOME/cutefish-store/sources.conf` : une source par ligne, dans
  la syntaxe de l'option `--source` de l'indexeur. Aucune logique propre à
  une distribution ou à Flatpak dans le service. Sans fichier, aucune
  source : le service est `degraded` tant qu'aucun catalogue n'existe.
- **Écartés** : la découverte intégrée (`/var/lib/swcatalog`,
  `/var/lib/flatpak/appstream`), qui dupliquerait la logique des backends ;
  l'absence de rafraîchissement.
- **Complétée** (étape F, D-028) : le fichier reste, et s'ajoute aux sources
  que le backend PackageKit fournit par `catalogSources()` ; il l'emporte sur
  un même identifiant ou un même fichier.

### D-021 — `Refresh` du service v0.1 : objet de transaction non journalisé

- **Statut** : Validée (propriétaire, étape E).
- **Décision** : `Refresh` garde la signature d'ARCHITECTURE §5.2 et rend le
  chemin d'un objet `org.cutefish.Store1.Transaction` de type `refresh`
  (états `running`, puis `succeeded`, `failed` ou `cancelled`), publié
  5 minutes après sa fin. Il n'est **pas** écrit dans le journal, dont les
  transactions sont `install`, `remove`, `update` (D-018). Sans backend, le
  rafraîchissement relit les fichiers de métadonnées déjà sur le disque ;
  avec un backend, voir D-028.

### D-022 — Service v0.1 : aucune méthode qui ne peut pas fonctionner

- **Statut** : Validée (propriétaire, étape E).
- **Décision** : `Install`, `Remove`, `Update`, `UpdateAll`, `EnableSource`,
  `DisableSource`, `GetUpdates`, `GetPermissions`, ainsi que la propriété
  `UpdateCount` et le signal `UpdatesChanged`, ne sont pas exportés tant que
  les backends n'existent pas. L'introspection ne montre que ce qui
  fonctionne ; un appel reçoit `UnknownMethod`.
- **Modifiée** (étape F, D-025) : avec un backend configuré, `GetUpdates`,
  `UpdateCount` et `UpdatesChanged` sont exportés.
- **Modifiée** (étape F2) : avec un backend configuré, `Install`, `Remove`,
  `Update` et `UpdateAll` sont exportés. `EnableSource`, `DisableSource` et
  `GetPermissions` restent absentes.

### D-023 — Filtrage des mises à jour par l'hôte (D-010)

- **Statut** : Validée (propriétaire, étape F).
- **Décision** : un backend rapporte les mises à jour **telles que son
  gestionnaire les voit** (pour PackageKit : tous les paquets, sans les
  versions identiques à l'installée). L'hôte garde celles des applications du
  catalogue et les runtimes avec une fonction pure du cœur,
  `applicationUpdates()` (`applicationlinks.h`) ; `linkInstalled()` relie de
  même l'inventaire installé aux applications. La signature de
  `Backend::queryUpdates` ne change pas ; seul son commentaire est précisé.
- **Raison** : les backends ne connaissent pas le catalogue (P-001) ; le
  filtrage est testable sans D-Bus.
- **Écarté** : passer le catalogue aux backends.

### D-024 — Classement des dépôts APT par hôte

- **Statut** : Validée (propriétaire, étape F).
- **Décision** : dans le profil Debian, un dépôt dont l'URL a pour hôte
  `debian.org` ou un de ses sous-domaines (`deb.debian.org`,
  `security.debian.org`) est `distribution` ; tout autre est `third-party`
  (y compris le dépôt Cutefish). Les sources DEP-11 de `/var/lib/swcatalog`
  sont classées de même d'après l'hôte de leur nom de fichier.
- **Limite** : un miroir Debian sous un autre nom d'hôte est classé
  `third-party` ; une liste de miroirs connus pourra venir plus tard.

### D-025 — Portée de l'étape F : backend complet, intégration en lecture

- **Statut** : Validée (propriétaire, étape F).
- **Décision** : le backend PackageKit implémente tout le contrat
  (`plan`, `execute`, `reattach`, `refresh`, `cancel` compris) et est testé
  seul contre un PackageKit simulé. Le service ne l'utilise qu'en lecture :
  propriété `Backends`, inventaire installé dans `state.sqlite`,
  `GetUpdates`. `Install`, `Remove`, `Update`, `UpdateAll` et le
  gestionnaire de transactions forment une étape ultérieure, validée
  séparément. (Le rafraîchissement des dépôts dans `Refresh` ne faisait pas
  partie de cette réponse : il relève de D-028.)
- **Conséquence** : un service prêt dont le backend configuré est
  inutilisable est `degraded` (ARCHITECTURE §5.2) ; le catalogue reste
  lisible.

### D-026 — Tests contre le vrai packagekitd

- **Statut** : Validée (propriétaire, étape F).
- **Décision** : des tests à part (`store-pkbackend-real-tests`), hors du
  `ctest` par défaut, lancés seulement avec
  `CUTEFISH_STORE_REAL_PACKAGEKIT=1`. Requêtes et transactions `SIMULATE`
  uniquement : jamais `execute`, `reattach`, `refresh` (`RefreshCache`), ni
  `interactive=true`. Tous les autres tests utilisent un PackageKit simulé
  sur un bus privé ; les tests D-Bus existants du service passent
  `--packagekit off`.

### D-030 — File par backend : libérée pendant l'attente de confirmation

- **Statut** : Validée (propriétaire, étape F2).
- **Décision** : une transaction planifie et s'exécute seule sur son
  backend (P-009), dans l'ordre d'arrivée ; en `awaiting-confirmation`, elle
  libère la file ; après `Confirm`, elle attend son tour en restant en
  `planning`. `execute()` refait la simulation (D-017) : un plan devenu
  ancien entre-temps est repéré. Un rafraîchissement des dépôts (D-028)
  passe dans la même file.

### D-031 — `UpdateAll` : transaction parente et enfants

- **Statut** : Validée (propriétaire, étape F2 ; proposition de SERVICE
  §4.4).
- **Décision** : `UpdateAll` crée une transaction parente `update-all`
  (`Backend` = `multiple`, non journalisée) et une transaction enfant
  `update` par backend et installation, chacune avec la propriété `Parent`.
  `Confirm` et `Cancel` de la parente s'appliquent aux enfants qui le
  permettent ; son état résume les leurs. Sans mise à jour : `NotFound`.

### D-032 — Paquet protégé : refus pendant la planification

- **Statut** : Validée (propriétaire, étape F2).
- **Décision** : le contrat `Backend` ne change pas ; `Remove` rend une
  transaction, qui échoue en `planning` avec le code `not-allowed`
  (`ErrorCode::NotAllowed`, nouveau dans le cœur) quand le plan supprimerait
  un paquet protégé, demandé ou en cascade. Écart assumé avec SERVICE §4.4
  (`NotAllowed` immédiat).

### D-028 — Actualisation locale et actualisation des dépôts séparées

- **Statut** : Validée (propriétaire, clôture de l'étape F ; option B de
  l'audit, qui remplace la proposition initiale).
- **Décision** : deux opérations distinctes.
  - **Actualisation locale** : relit les métadonnées déjà sur le disque
    (`Backend::catalogSources()` et `sources.conf`) et réindexe ; sans réseau,
    sans `RefreshCache`. C'est l'import initial, et `Refresh` avec l'option
    `repositories` à faux.
  - **Actualisation complète** (`Refresh`, `repositories` vrai par défaut) :
    tente `Backend::refresh()` (PackageKit : `RefreshCache(false)`,
    `interactive`, sans indice `background`), **puis réindexe les métadonnées
    locales même si `RefreshCache` échoue**. Dans ce cas, le catalogue peut
    être remplacé (`CatalogChanged`), mais la transaction se termine en
    `failed` avec l'erreur de PackageKit (détails : « repositories not
    refreshed (…) ; the local metadata was reindexed ») et `LastRefresh`
    n'est pas écrite. Une annulation arrête tout (`cancelled`, rien n'est
    réindexé) ; elle passe à PackageKit tant que `AllowCancel` est vrai.
  - **Sources** : celles du backend et celles de `sources.conf` sont
    réunies ; sur un même identifiant, le fichier l'emporte. Une seconde
    référence à un **même fichier de métadonnées** sous un autre identifiant
    (un `sources.conf` écrit avant que le backend ne liste le fichier,
    D-020) est ignorée, avec un avertissement dans le journal ; celle du
    fichier est gardée.
- **Raison** : le catalogue local ne dépend jamais du réseau ; un
  `apt update` fait ailleurs est pris en compte même hors ligne ; l'échec des
  dépôts reste visible.
- **Limite** : si la réindexation locale échoue aussi, la transaction porte
  l'erreur locale ; celle des dépôts n'est que dans le journal.
- **Écartés** : l'association stricte (un échec de `RefreshCache` empêchait
  toute réindexation, régression par rapport à la v0.1) ; indexer deux fois
  un même fichier (la seconde source n'avait que des offres refusées).

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
  Précisée par D-017 (vérification du plan d'exécution, modèle d'états).
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

### P-002 — Indexation AppStream dans un sous-processus éphémère

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée, justification corrigée après la matrice de
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
- **Mesures** (statut de la décision inchangé) :
  - prototype 3 : `AsMetadata` retenu, `AsPool` perdant les branches
    Flatpak ; lecture en bloc des deux catalogues 471 MiB de pic ;
  - Core v0.2, indexeur réel, lecture en flux par lots de 20 composants,
    Debian `main` + Flathub : 4,4 à 4,7 s, pic 37 à 39 MiB, instantané de
    25 Mo [MESURÉ sur la machine d'audit, une seule machine]. L'objectif
    initial (< 10 s, < 300 Mo) est tenu sur cette machine ;
  - détails : [CATALOG.md](CATALOG.md) §3 et §8.

### P-003 — Deux bases SQLite : catalogue (instantané) et état

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée.
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

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée, corrigée après la matrice de validation. Écart par
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

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée, précisée après la matrice de validation.
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

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée.
- **Décision** : la v1 ne livre aucun helper root. Les opérations
  privilégiées sont entièrement déléguées :
  - à PackageKit (APT) ;
  - à libflatpak et `flatpak-system-helper` pour l'installation Flatpak
    système (D-011).
- **Justification** : les dépôts éditeurs, qui nécessitent un helper
  privilégié (PackageKit ne sait pas ajouter un dépôt avec sa clé [CODE]),
  sont hors périmètre v1 (D-003). Sans eux, aucun code privilégié n'est
  nécessaire.

### P-010 — Cycle de vie du service : activation D-Bus, timer, arrêt sur inactivité

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée, règle d'inactivité précisée après la matrice de
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
       client, quand plus rien n'est en cours : **5 minutes** (validé en
       phase 0.4). Un démarrage par le timer, sans client, suit la même
       règle une fois le travail terminé ;
    5. les téléchargements d'images en file sont abandonnés à l'arrêt ; un
       fichier en cours d'écriture est supprimé (§5.8).
- **Justification** :
  - zéro mémoire consommée quand la boutique ne sert pas ;
  - modèle identique aux services de cutefish-core et de l'installeur
    (activation D-Bus et arrêt sur inactivité [CODE]) ;
  - une fenêtre ouverte garde le service actif : elle continue de recevoir
    les signaux de progression et de catalogue.

### P-012 — API D-Bus versionnée par le nom d'interface

- **Statut** : Validée (propriétaire, phase 0.4, question Q-6 de SERVICE.md).
  Statut précédent : proposée.
- **Décision** :
  - les interfaces portent un numéro de version (`…Store1`,
    `…Store1.Transaction`) ;
  - les ajouts compatibles (nouvelles clés `a{sv}`, nouvelles méthodes) ne
    changent pas ce numéro ;
  - une rupture crée `…Store2`, et les deux versions coexistent le temps de
    la migration.
- **Justification** : les dictionnaires `a{sv}` permettent d'étendre l'API
  sans casser les clients existants.

---

## Décisions proposées (à valider)

### D-027 — Mises à jour gardées en mémoire

- **Statut** : Proposée (étape F), à valider.
- **Décision** : la liste filtrée des mises à jour vit dans la mémoire du
  service ; elle est recalculée au démarrage, aux signaux du démon, à chaque
  nouveau catalogue et après chaque `Refresh`. La table `updates` de
  SERVICE §8.2 n'est pas encore créée.
- **Raison** : aucune lecture ne la demande encore hors de `GetUpdates`
  (`has-update` dans les résultats de recherche viendra avec elle) ; le
  calcul coûte deux requêtes PackageKit.
- **Conséquence** : juste après un démarrage, `GetUpdates` est vide le
  temps de ces requêtes ; `UpdatesChanged` n'est émis que si la liste
  change (donc pas quand il n'y a aucune mise à jour). La liste est vidée
  quand le backend devient inutilisable. Pour PackageKit, `download-size`
  vaut toujours 0 : `GetUpdates` ne donne pas de taille.

### D-029 — Taille de téléchargement totale d'un plan

- **Statut** : Proposée (étape F), à valider.
- **Décision** : `PlanSpec::downloadSize` porte le total donné par un
  backend qui ne le répartit pas par paquet (PackageKit :
  `DownloadSizeRemaining` après la simulation) ; `Plan::downloadSize()` le
  rend, sinon la somme des éléments. Une différence de total entre deux
  plans est un `SizeChanged` informatif, sans nouvelle confirmation. Le
  total est gardé dans le journal (`state.sqlite`, clé facultative
  `downloadSize` du plan, format inchangé en version 1 : un ancien
  enregistrement se lit comme la somme des éléments).
- **Raison** : trouvé par les tests réels (D-026) : sans cela, tout plan
  PackageKit annonçait 0 octet.

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
| O-009 | Tranchée → D-013 | **Langues indexées** : langue de session et `C` seulement, ou toutes les langues configurées ? | Taille de l'index, temps d'indexation. Coûts mesurés en Core v0.2 (descriptions `C` : 25 Mo ; + français : 28 Mo ; toutes : 58 Mo ; durée 4,4 à 4,9 s) : [CATALOG.md](CATALOG.md) §5 | Avant le service (l'indexeur reçoit déjà les langues en argument) |
| O-010 | Ouverte | **Serveur de notifications** : quel serveur `org.freedesktop.Notifications` est disponible sous Cutefish ? Non vérifié en 0.1. | Notifications de mises à jour en arrière-plan | Phase 0.3 |
| O-011 | Ouverte | **Contenu éditorial** (mises en avant, sélections) : source et format. | Écran d'accueil | Avant le design de l'accueil |
| O-012 | Tranchée → D-015 | **Fréquence du rafraîchissement en arrière-plan** et comportement sur connexion limitée (NetworkManager `Metered`). | Consommation réseau | Phase 0.3 |
| O-013 | Ouverte (mises à jour avec nouveau runtime : tranché → D-014) | **Nouvelles installations Flatpak en système** : la boutique doit-elle permettre de choisir l'installation système pour une nouvelle application, et pour qui ? | Effet sur tous les utilisateurs ; autorisations différentes selon le groupe `sudo` (D-011) | Après la v1, ou avant si le propriétaire le souhaite |
| O-014 | Ouverte | **Formatage automatique** : faut-il un `.clang-format` propre au projet, et lequel ? Cutefish n'en fournit aucun (P-013). | Homogénéité du code, vérification en CI | Avant le premier fichier source |
