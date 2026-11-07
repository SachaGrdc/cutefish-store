#pragma once

// A small catalogue, written with CatalogWriter, reproducing the cases seen
// in the real Debian and Flathub catalogues (appstream-indexer prototype):
// the same application from both sources, a legacy `.desktop` id, two
// Flatpak branches, two ids for one launcher, a provided id, translations.

#include "fixtures.h"

#include <cutefish/store/catalogwriter.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <unistd.h>

namespace catalogfixture {

using namespace cutefish::store;
using fixtures::unwrap;

/// A directory removed at the end of the test.
class TempDir
{
public:
    TempDir()
    {
        std::string pattern = (std::filesystem::temp_directory_path() / "cutefish-store-test-XXXXXX").string();
        if (!::mkdtemp(pattern.data()))
            std::abort();
        m_path = pattern;
    }
    ~TempDir() { std::filesystem::remove_all(m_path); }
    const std::filesystem::path& path() const { return m_path; }
    std::filesystem::path operator/(const char* name) const { return m_path / name; }

private:
    std::filesystem::path m_path;
};

inline ComponentRecord deb(const char* componentId, const char* package, const char* name, const char* summary = "")
{
    ComponentRecord r{.componentId = componentId,
                      .offer = {.format = PackageFormat::Deb, .package = package},
                      .names = {{"C", name}}};
    if (*summary)
        r.summaries.push_back({"C", summary});
    return r;
}

inline ComponentRecord flatpak(const char* componentId, const char* ref, const char* name, const char* summary = "",
                               const char* version = nullptr)
{
    ComponentRecord r{.componentId = componentId,
                      .offer = {.format = PackageFormat::Flatpak, .package = ref},
                      .names = {{"C", name}}};
    if (*summary)
        r.summaries.push_back({"C", summary});
    if (version)
        r.offer.version = unwrap(Version::upstream(version));
    return r;
}

struct Built
{
    CatalogBuildStats stats;
    std::vector<Error> rejected;
};

/// Writes the fixture catalogue to `path`.
inline Built build(const std::filesystem::path& path, std::uint64_t generation = 1, bool withFlathub = true)
{
    auto writer = unwrap(CatalogWriter::create(path, {.generation = generation, .searchLocales = {"fr_FR.UTF-8"}}));
    const Source debian = fixtures::debianSource();               // priority 10
    const Source flathub = fixtures::flathubSource(Scope::User); // priority 20
    EXPECT_TRUE(writer.addSource(debian));
    if (withFlathub) {
        EXPECT_TRUE(writer.addSource(flathub));
    }

    Built built;
    auto add = [&](const Source& source, ComponentRecord r) {
        if (auto ok = writer.addComponent(source.id(), std::move(r)); !ok)
            built.rejected.push_back(ok.error());
    };

    // GIMP in both sources, with translations and a description.
    ComponentRecord gimpDeb = deb("org.gimp.GIMP", "gimp", "GNU Image Manipulation Program", "Create images and edit photographs");
    gimpDeb.names.push_back({"fr", "Programme de manipulation d'images GNU"});
    gimpDeb.summaries.push_back({"fr", "Créer des images et retoucher des photos"});
    gimpDeb.descriptions.push_back({"C", {{DescriptionBlockType::Paragraph, "GIMP is an image editor."},
                                          {DescriptionBlockType::ListItem, "Layers"}}});
    gimpDeb.descriptions.push_back({"fr", {{DescriptionBlockType::Paragraph, "GIMP est un éditeur d'images."}}});
    gimpDeb.categories = {"Graphics", "2DGraphics"};
    gimpDeb.icon = {IconKind::LocalPath, "/var/lib/swcatalog/icons/debian/64x64/gimp.png"};
    add(debian, std::move(gimpDeb));
    if (withFlathub) {
        ComponentRecord gimpFlathub = flatpak("org.gimp.GIMP", "app/org.gimp.GIMP/x86_64/stable", "GIMP",
                                              "Create images and edit photographs", "3.0.4");
        gimpFlathub.keywords.push_back({"C", {"photoshop", "paint"}});
        gimpFlathub.categories = {"Graphics"};
        gimpFlathub.developer = "The GIMP team";
        gimpFlathub.verified = true;
        add(flathub, std::move(gimpFlathub));
    }

    // Legacy .desktop id in Debian, normalised id on Flathub: one application.
    add(debian, deb("org.gnome.gitg.desktop", "gitg", "gitg", "Graphical user interface for git"));
    if (withFlathub) {
        add(flathub, flatpak("org.gnome.gitg", "app/org.gnome.gitg/x86_64/stable", "gitg", "Git repository viewer"));
    }

    // Two branches of the same Flatpak: two offers.
    if (withFlathub) {
        add(flathub, flatpak("org.qgis.qgis", "app/org.qgis.qgis/x86_64/lts", "QGIS", "Geographic information system", "3.34"));
        add(flathub, flatpak("org.qgis.qgis", "app/org.qgis.qgis/x86_64/stable", "QGIS", "Geographic information system", "3.40"));
    }

    // Same launcher, two ids: two applications, never merged.
    add(debian, deb("org.kde.kwalletmanager5", "kwalletmanager", "KWalletManager", "Wallet management tool"));
    if (withFlathub) {
        add(flathub, flatpak("org.kde.kwalletmanager", "app/org.kde.kwalletmanager/x86_64/stable", "KWalletManager",
                             "Wallet management tool"));
    }

    // Flatpak only, keywords, a provided id that becomes an alias.
    if (withFlathub) {
        ComponentRecord inkscape = flatpak("org.inkscape.Inkscape", "app/org.inkscape.Inkscape/x86_64/stable",
                                           "Inkscape", "Vector graphics editor", "1.4");
        inkscape.keywords.push_back({"C", {"svg", "drawing"}});
        inkscape.providedIds = {"inkscape.desktop"};
        inkscape.categories = {"Graphics", "VectorGraphics"};
        add(flathub, std::move(inkscape));
    }

    // Accented name; a provided id claimed twice (ambiguous, not an alias).
    ComponentRecord editor = deb("org.example.Editeur", "editeur", "Éditeur de texte", "Édition de fichiers texte");
    editor.providedIds = {"org.example.Shared"};
    add(debian, std::move(editor));
    ComponentRecord viewer = deb("org.example.Viewer", "viewer", "Viewer", "Text viewer");
    viewer.providedIds = {"org.example.Shared"};
    add(debian, std::move(viewer));

    // Rejected components: invalid id, no untranslated name, invalid
    // package, duplicate offer. Invalid translations are dropped only.
    add(debian, deb("org..broken", "broken", "Broken"));
    ComponentRecord noName = deb("org.example.NoName", "noname", "x");
    noName.names = {{"fr", "Sans nom"}};
    add(debian, std::move(noName));
    add(debian, deb("org.example.BadPackage", "Bad_Package", "Bad"));
    add(debian, deb("org.gimp.GIMP.desktop", "gimp", "GIMP again"));
    ComponentRecord badText = deb("org.example.BadText", "badtext", "Bad text");
    badText.summaries.push_back({"de", "Kontroll\x01zeichen"});
    badText.summaries.push_back({"x y", "invalid locale"});
    add(debian, std::move(badText));

    built.stats = unwrap(writer.finish());
    return built;
}

} // namespace catalogfixture
