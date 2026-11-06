# Cutefish Store — Architecture technique

Statut : **référence de conception, phase 0.2**. Aucune ligne de code n'existe
encore. Ce document décrit ce qui sera construit. Les décisions qu'il applique,
et celles qui restent ouvertes, sont tenues à jour dans
[DECISIONS.md](DECISIONS.md) (identifiants `D-`, `P-`, `O-`).

Niveau de preuve, comme dans l'audit de la phase 0.1 :

- **[CODE]** : vérifié dans le code des dépôts CutefishOS ;
- **[DOC]** : documentation officielle ;
- **[MESURÉ]** : mesuré sur la machine d'audit (Debian 13, AppStream 1.0.5,
  PackageKit 1.3.1, libflatpak 1.16.6, SQLite 3.46.1) ;
- **[SYMBOLE]** : symbole présent dans la bibliothèque installée ; la
  signature et la sémantique restent à confirmer dans la documentation de
  l'API ;
- **[HYPOTHÈSE]** : non vérifié, à valider ;
- **[À VALIDER]** : point de conception à confirmer par un prototype ou une
  mesure avant de s'y engager.

## Sommaire

1. [Vision et principes](#1-vision-et-principes)
2. [Architecture globale](#2-architecture-globale)
3. [Diagrammes et flux de données](#3-diagrammes-et-flux-de-données)
4. [Responsabilités des composants](#4-responsabilités-des-composants)
5. Spécification préliminaire de l'API D-Bus (à rédiger)
6. Catalogue et index (à rédiger)
7. Backends APT et Flatpak (à rédiger)
8. Threads et GLib (à rédiger)
9. Modèle de sécurité (à rédiger)
10. Performances et caches (à rédiger)
11. Arborescence du projet (à rédiger)
12. Dépendances et compilation (à rédiger)
13. Stratégie de tests (à rédiger)
14. Limites, risques et évolutions (à rédiger)

---

## 1. Vision et principes

Cutefish Store est une boutique logicielle pour Linux. Elle est native dans
Cutefish en v1, et son cœur est conçu pour être réutilisé par d'autres
interfaces et sur d'autres environnements (D-001). La v1 gère APT/DEB et
Flatpak (D-003).

### Principes

1. **Le thread graphique ne fait que de l'interface.**
   - Aucun réseau, aucun parsing important, aucune requête SQL et aucun appel
     D-Bus synchrone sur ce thread.
   - L'interface ne parle qu'au service, de façon asynchrone.
2. **Une seule frontière : l'API D-Bus.**
   - L'interface ne connaît ni PackageKit, ni libflatpak, ni AppStream, ni le
     schéma SQLite (D-002, P-004).
   - Toute autre interface future utilise le même contrat.
3. **GLib reste là où elle est indispensable.**
   - libflatpak est confinée au service, sur des threads dédiés.
   - libappstream est confinée à l'indexeur, un processus éphémère (D-004,
     P-002).
   - L'interface ne charge jamais GLib.
4. **Aucun privilège propre.**
   - Les opérations système passent par PackageKit, qui applique polkit.
   - Flatpak s'installe dans l'espace utilisateur (D-005, P-007).
5. **Les différences entre backends restent visibles.**
   - L'abstraction commune couvre le cycle de vie (rechercher, planifier,
     exécuter, suivre, annuler).
   - Les détails propres à chaque format (paquets supprimés par APT,
     permissions Flatpak) restent dans le plan, sans être aplatis.
6. **Mesurer avant d'optimiser, mais concevoir pour la vitesse.** Les
   structures (index FTS5, vues virtualisées, caches) sont choisies d'après les
   mesures de la phase 0.1. Les objectifs chiffrés (§10) sont vérifiés en CI.
7. **Simplicité.**
   - Pas de plugins dynamiques, pas d'abstraction pour des formats
     hypothétiques (P-001).
   - Une seule couche entre le service et chaque gestionnaire.
8. **Conventions Cutefish.** C++20, CMake, conventions de nommage, GoogleTest,
   gettext, activation D-Bus et polkit comme dans cutefish-core (P-013).

### Hors périmètre v1

- Snap, AppImage, RPM, dépôts éditeurs.
- Notes et avis, comptes utilisateurs, paiements.
- Mises à jour hors ligne du système (O-005).
- Installation Flatpak système (D-005, O-006).
- Sessions X11.

---

## 2. Architecture globale

### Processus

| Processus | Rôle | Durée de vie | Privilèges | Dépendances notables |
|---|---|---|---|---|
| `cutefish-store` | Interface graphique | Tant que la fenêtre est ouverte | Utilisateur | `Cutefish::Toolkit` ; **pas** de GLib |
| `cutefish-store-service` | Cœur : catalogue, recherche, état, transactions, images, sources | Activé par D-Bus ou par timer ; s'arrête sur inactivité (P-010) | Utilisateur | `Cutefish::ToolkitBase`, SQLite, libcurl, libflatpak (GLib) |
| `cutefish-store-indexer` | Lit les catalogues AppStream et produit un instantané du catalogue | Lancé par le service, se termine après l'écriture | Utilisateur | libappstream (GLib), SQLite |

Composants extérieurs utilisés (aucun n'est modifié) :

| Composant | Utilisé pour | Bus ou mécanisme |
|---|---|---|
| PackageKit (`packagekitd`, root) + backend `apt` | Tout ce qui touche APT : état installé, plan, installation, suppression, mises à jour, rafraîchissement | Bus système, `org.freedesktop.PackageKit` |
| polkit et agent polkit de Cutefish (`cutefish-core/polkit-agent`) | Authentifications demandées par PackageKit | Bus système |
| libflatpak (dans le service) | Installation utilisateur : remotes, refs, transactions, métadonnées AppStream de Flathub | Bibliothèque ; aucun démon pour l'installation utilisateur |
| `flatpak-system-helper` | Seulement si l'installation système est activée plus tard (O-006) | Bus système, via libflatpak |
| `org.cutefish.AppRuntime` (cutefish-core) | Lancer une application installée | Bus de session [CODE] |
| `org.freedesktop.Notifications` | Notifier les mises à jour et la fin des transactions en arrière-plan | Bus de session ; serveur Cutefish non vérifié (O-010) |
| systemd utilisateur | Unité du service et timer de rafraîchissement | — |

### Emplacements des données

Tous les emplacements sont relatifs aux répertoires XDG de l'utilisateur. Rien
n'est écrit dans `/etc` ou `/var` par la boutique elle-même.

| Donnée | Emplacement | Nature |
|---|---|---|
| Instantané du catalogue | `$XDG_CACHE_HOME/cutefish-store/catalog.sqlite` | Régénérable (P-003) |
| Images téléchargées | `$XDG_CACHE_HOME/cutefish-store/images/` | Régénérable, borné en taille |
| État et journal des transactions | `$XDG_STATE_HOME/cutefish-store/state.sqlite` | Mutable, propriété du service |
| Préférences | `$XDG_CONFIG_HOME/cutefish-store/settings.ini` | INI au format de `base::Settings` [CODE] |
| Installation Flatpak utilisateur | `~/.local/share/flatpak` | Gérée par libflatpak |

### Pourquoi un service, plusieurs bibliothèques et un indexeur séparé

On aurait pu tout regrouper dans le service, ou découper en plusieurs
services. Le choix retenu (P-001, P-002) :

- **un seul service résident** : un seul nom de bus, un seul cycle de vie, une
  seule file de transactions ;
- **des bibliothèques statiques internes** (`common`, `core`, `backend-apt`,
  `backend-flatpak`, `service`), testables séparément, sans ABI de plugin ;
- **un seul processus annexe, l'indexeur**, justifié par deux faits :
  1. [MESURÉ] parser tout Flathub avec libappstream monte à 459 Mo RSS de
     pic ; cette mémoire n'a rien à faire dans un processus résident ;
  2. le XML vient du réseau : un plantage de parseur ne doit pas emporter les
     transactions en cours.

  Les transactions et les backends restent dans le service : ils ont besoin
  d'un état partagé (file, verrous, journal) et d'une durée de vie longue.

---

## 3. Diagrammes et flux de données

### 3.1 Composants

```mermaid
flowchart LR
    subgraph UI["cutefish-store (interface, sans GLib)"]
        Pages["Pages et vues virtualisées"]
        Client["StoreClient<br/>(proxy D-Bus asynchrone)"]
        Pages --> Client
    end

    subgraph SVC["cutefish-store-service (bus de session)"]
        Api["service : adaptateur D-Bus<br/>org.cutefish.Store1"]
        Core["core : catalogue, recherche,<br/>état, transactions, politique"]
        Img["core : cache d'images<br/>(libcurl)"]
        Apt["backend-apt<br/>(PackageKit via sd-bus)"]
        Fp["backend-flatpak<br/>(libflatpak, threads GLib)"]
        Api --> Core
        Core --> Img
        Core --> Apt
        Core --> Fp
    end

    IDX["cutefish-store-indexer<br/>(libappstream, éphémère)"]

    Client <-- "D-Bus session" --> Api
    Core -- "lance, puis valide<br/>l'instantané" --> IDX

    PK["packagekitd (root)"]
    POL["polkit + agent Cutefish"]
    FH[("Flathub (réseau)")]
    DEB[("Catalogues DEP-11<br/>/var/lib/swcatalog")]
    FPS[("~/.local/share/flatpak")]

    Apt <-- "D-Bus système" --> PK
    PK --> POL
    Fp --> FPS
    Fp <--> FH
    IDX --> DEB
    IDX --> FPS
    Img <--> FH
```

### 3.2 Démarrage de l'interface et recherche

```mermaid
sequenceDiagram
    participant U as Interface
    participant B as Bus de session
    participant S as Service
    participant R as Thread de lecture SQLite
    U->>B: GetStatus() (asynchrone)
    B-->>S: activation D-Bus si le service est arrêté
    S-->>U: {state: ready, catalog-generation: 42}
    Note over U: première frame affichée sans attendre la réponse<br/>(squelettes de chargement)
    U->>S: Search("fire", {limit: 60})
    S->>R: requête FTS5 (index en lecture seule)
    R-->>S: identifiants et résumés
    S-->>U: aa{sv} (résultats)
    U->>U: ListView/grille : bindItem des lignes visibles
    U->>S: FetchImages([urls visibles])
    S-->>U: ImagesReady({url: chemin local})
```

### 3.3 Rafraîchissement du catalogue

```mermaid
sequenceDiagram
    participant T as Timer systemd ou interface
    participant S as Service
    participant F as Thread Flatpak
    participant P as PackageKit
    participant I as Indexeur
    T->>S: Refresh()
    par sources APT
        S->>P: RefreshCache(force=false), hint background=true
        P-->>S: Finished
    and Flathub
        S->>F: mise à jour de l'appstream de la remote utilisateur
        F-->>S: terminé (commit OSTree signé)
    end
    S->>S: empreintes des fichiers sources (taille, mtime, hash)
    alt une source a changé
        S->>I: lancement à basse priorité, chemin temporaire
        I-->>S: code de sortie 0, instantané écrit
        S->>S: validation (schéma, integrity_check, volumes)
        S->>S: rename() atomique, génération + 1
        S-->>S: signal CatalogChanged(génération)
    end
    S->>P: Resolve(installed) des paquets du catalogue
    S->>F: liste des refs installées et des mises à jour
    S-->>S: signaux InstalledChanged, UpdatesChanged
```

### 3.4 Installation d'un paquet APT

```mermaid
sequenceDiagram
    participant U as Interface
    participant S as Service
    participant P as PackageKit
    participant A as Agent polkit
    U->>S: Install(appId, offerId, {})
    S-->>U: chemin de l'objet Transaction (état: planning)
    S->>P: Resolve(nom du paquet), puis InstallPackages(SIMULATE | ONLY_TRUSTED)
    P-->>S: Package(...) × n, Finished
    S->>S: politique : suppressions ? → confirmation requise
    S-->>U: PropertiesChanged(State=awaiting-confirmation, Plan)
    U->>S: Transaction.Confirm()
    S->>P: InstallPackages(ONLY_TRUSTED), hints interactive=true
    P->>A: demande d'authentification (polkit)
    A-->>P: autorisé
    P-->>S: Status, Percentage, ItemProgress ...
    S-->>U: PropertiesChanged(Progress, Phase), au plus 10 par seconde
    P-->>S: Finished(success)
    S->>S: journal, état installé mis à jour
    S-->>U: Finished("success", "", "")
```

### 3.5 États d'une transaction

```mermaid
stateDiagram-v2
    [*] --> queued
    queued --> planning
    planning --> awaiting_confirmation: le plan exige un accord
    planning --> running: plan sans effet notable
    awaiting_confirmation --> running: Confirm()
    awaiting_confirmation --> cancelled: Cancel()
    running --> succeeded
    running --> failed
    running --> cancelled: Cancel() accepté
    planning --> failed
    queued --> cancelled: Cancel()
    running --> interrupted: le service s'est arrêté
    succeeded --> [*]
    failed --> [*]
    cancelled --> [*]
    interrupted --> [*]
```

Pendant `running`, une **phase** plus fine est publiée : `waiting-for-auth`,
`waiting-for-lock`, `downloading`, `installing`, `removing`, `finishing`. Elle
est dérivée du `Status` de PackageKit ou des opérations de `FlatpakTransaction`.

---

## 4. Responsabilités des composants

### 4.1 `cutefish-store` (interface)

**Fait** :

- pages Accueil, Catégories, Recherche, Détails, Installées, Mises à jour,
  Sources, Préférences ;
- vues virtualisées : `ListView` du toolkit [CODE], grille virtualisée à
  écrire (§10) ;
- thème, mode sombre, couleur d'accent via `AppearanceWatcher` [CODE] ;
- affichage de la progression des transactions à partir des propriétés D-Bus ;
- confirmation des plans (suppressions, permissions) ;
- lancement d'une application installée via `org.cutefish.AppRuntime` [CODE].

**Ne fait pas** :

- accès réseau ;
- lecture de fichiers du service ;
- appels à PackageKit ou Flatpak ;
- décisions de sécurité. L'interface affiche les plans, mais la règle « cette
  transaction exige une confirmation » vient du service (P-008).

### 4.2 `common` (bibliothèque partagée par l'interface et le service)

- Noms D-Bus, noms d'erreurs, clés des dictionnaires `a{sv}`, valeurs des
  énumérations sérialisées (états, phases, types de backend).
- Conversions entre ces dictionnaires et des structures C++ simples
  (`AppSummary`, `TransactionInfo`, …).
- Aucune logique métier et aucune dépendance au-delà de `Cutefish::ToolkitBase`
  (pour `dbus::Variant`).

### 4.3 `core`

- **Modèle** : `App` (composant AppStream fusionné), `Offer` (une façon de
  l'installer : un paquet APT ou une ref Flatpak), `InstalledItem`,
  `UpdateItem`, `Plan`, `Transaction`.
- **Catalogue** : ouverture de l'instantané en lecture seule, requêtes de
  navigation et de détail, génération courante, remplacement atomique.
- **Recherche** : traduction de la saisie en requête FTS5, classement, filtres.
- **Dédoublonnage** : règles de fusion et de choix de l'offre par défaut. Elles
  sont appliquées par l'indexeur, mais définies dans `core` pour être testables
  et partagées.
- **État** : `state.sqlite` (installé, mises à jour, journal), réconciliation
  avec les backends.
- **Transactions** : file par backend, machine d'états (§3.5), politique de
  confirmation (P-008), journal, reprise après redémarrage.
- **Images** : planification des téléchargements (priorité, déduplication,
  annulation), cache disque borné.
- **Sources** : liste unifiée des sources (dépôts APT en lecture, remotes
  Flatpak), activation de Flathub (P-011).
- **Erreurs** : taxonomie commune (§5.6) et traduction des erreurs des
  backends.
- **Interface `Backend`** (§7.1), avec deux implémentations.

### 4.4 `backend-apt`

- Client asynchrone de `org.freedesktop.PackageKit` sur le bus système, via
  `base::dbus` [CODE].
- Correspondance entre noms de paquets et `package_id`, état installé,
  simulation, installation, suppression, mises à jour, rafraîchissement,
  liste des dépôts.
- Traduction de `Status`, `Percentage`, `ItemProgress` et `ErrorCode` vers le
  modèle commun.
- Aucun thread propre : tout passe par la boucle principale du service.

### 4.5 `backend-flatpak`

- Deux threads GLib dédiés (requêtes et transactions), chacun avec son propre
  `GMainContext` et son propre `FlatpakInstallation` (§8).
- Remotes de l'installation utilisateur, refs installées, mises à jour,
  métadonnées de permissions, transactions, mise à jour de l'appstream de
  Flathub.
- Fournit à l'indexeur les chemins des catalogues AppStream des remotes.

### 4.6 `service` (`cutefish-store-service`)

- Exporte l'API `org.cutefish.Store1` (§5) avec `base::dbus::Object` [CODE].
- Valide toutes les entrées D-Bus avant de les transmettre à `core`.
- Gère le cycle de vie : activation, arrêt sur inactivité, rafraîchissement
  par timer, suivi des clients.
- Lance l'indexeur et met en place ses instantanés.
- Envoie les notifications de bureau.

### 4.7 `cutefish-store-indexer`

- Entrée : la liste des sources (chemins des catalogues, origine, priorité,
  langue) passée par le service en arguments ou sur l'entrée standard.
- Lecture avec libappstream (DEP-11 YAML, XML Flathub, métadonnées locales des
  applications installées).
- Normalisation, dédoublonnage, écriture de l'instantané SQLite avec l'index
  FTS5, puis sortie.
- N'accède pas au réseau et n'a aucune interface D-Bus.
