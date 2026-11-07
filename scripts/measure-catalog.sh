#!/bin/bash
# Builds catalogues from the real Debian and Flathub files of this machine
# and measures them. Not a unit test: results depend on the machine, the
# date of the catalogues and the page cache. Read-only for the system: the
# catalogues are only read, output goes to a temporary directory.
#
#   scripts/measure-catalog.sh [BUILD_DIR] [RUNS]
#
# DEBIAN_DEP11, DEBIAN_ICONS, FLATHUB_XML and FLATHUB_ICONS override the
# default paths below.
set -euo pipefail
export LC_ALL=C
build=${1:-build}
runs=${2:-3}
indexer="$build/src/indexer/cutefish-store-indexer"
bench="$build/tests/perf/store-catalog-bench"
debian=${DEBIAN_DEP11:-/var/lib/swcatalog/yaml/deb.debian.org_debian_dists_trixie_main_dep11_Components-amd64.yml.gz}
debianIcons=${DEBIAN_ICONS:-/var/lib/swcatalog/icons/debian-trixie-main}
flathub=${FLATHUB_XML:-/var/lib/flatpak/appstream/flathub/x86_64/active/appstream.xml.gz}
flathubIcons=${FLATHUB_ICONS:-/var/lib/flatpak/appstream/flathub/x86_64/active/icons}
for f in "$indexer" "$bench" "$debian" "$flathub"; do
    [ -e "$f" ] || { echo "missing $f" >&2; exit 1; }
done
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
DEB="id=packagekit:debian-trixie-main,backend=packagekit,scope=system,kind=distribution,origin=debian-trixie-main,priority=10,file=$debian,icons=$debianIcons"
FH="id=flatpak:system:flathub,backend=flatpak,scope=system,kind=flathub,origin=flathub,priority=20,file=$flathub,icons=$flathubIcons"

echo "date=$(date -u +%FT%TZ) debian=$(stat -L -c %s "$debian")B $(date -u -r "$(readlink -f "$debian")" +%F) flathub=$(stat -L -c %s "$flathub")B $(date -u -r "$(readlink -f "$flathub")" +%F)"
summary() { grep -E '^(source|catalogue)' | sed -E 's/^catalogue [^ ]+/catalogue/'; }
for batch in 0 20; do
    echo "## Debian alone, batch $batch"
    "$indexer" --output "$out/d.sqlite" --batch "$batch" --source "$DEB" | summary
    echo "## Flathub alone, batch $batch"
    "$indexer" --output "$out/f.sqlite" --batch "$batch" --source "$FH" | summary
done
for mode in C fr all; do
    case $mode in C) extra=();; fr) extra=(--locale fr_FR.UTF-8);; all) extra=(--all-description-locales);; esac
    for run in $(seq 1 "$runs"); do
        echo "## both sources, descriptions $mode, run $run"
        "$indexer" --output "$out/$mode.sqlite" "${extra[@]}" --source "$DEB" --source "$FH" | summary
    done
done
echo "## search and reads, catalogue with French descriptions, read in C"
"$bench" "$out/fr.sqlite"
echo "## same catalogue, read in French"
"$bench" "$out/fr.sqlite" --locale fr_FR.UTF-8
