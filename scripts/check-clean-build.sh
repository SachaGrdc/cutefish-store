#!/bin/bash
# Checks that the project builds and passes its tests with only the Debian
# packages documented in docs/CATALOG.md ("Dépendances"), without root and
# without a virtual machine.
#
# bubblewrap runs the build in a view of this system where /usr/include and
# the pkg-config and CMake package directories contain only the files of the
# declared packages and of their dependencies (apt-cache depends --recurse),
# copied into a scratch directory. Everything else (compilers' binaries,
# runtime libraries) is the host's. It is an approximation of a clean
# Debian, not a substitute for one: undeclared runtime libraries or tools
# outside these directories would go unnoticed.
#
#   scripts/check-clean-build.sh [PACKAGE...]
#
# Without arguments, the documented list is used. Passing a shorter list
# shows what fails when a package is missing.
set -euo pipefail
export LC_ALL=C
here="$(cd "$(dirname "$0")/.." && pwd)"
command -v bwrap >/dev/null || { echo "bubblewrap (bwrap) is required" >&2; exit 1; }
packages=("$@")
[ ${#packages[@]} -gt 0 ] || packages=(build-essential cmake ninja-build pkg-config
                                       libsqlite3-dev libappstream-dev libxml2-dev libgtest-dev)
work=$(mktemp -d)
trap 'chmod -R u+w "$work"; rm -rf "$work"' EXIT
root="$work/root"
views=(/usr/include /usr/lib/x86_64-linux-gnu/pkgconfig /usr/share/pkgconfig /usr/lib/pkgconfig
       /usr/lib/x86_64-linux-gnu/cmake /usr/lib/cmake)
for v in "${views[@]}"; do mkdir -p "$root$v"; done

closure=$(apt-cache depends --recurse --no-recommends --no-suggests --no-conflicts --no-breaks \
              --no-replaces --no-enhances "${packages[@]}" 2>/dev/null | grep -v '^ ' | grep -v '^<' | sort -u)
installed=()
for p in $closure; do
    if dpkg-query -W -f='${db:Status-Status}\n' "$p" 2>/dev/null | grep -qx installed; then
        installed+=("$p")
    fi
done
echo "declared: ${packages[*]}"
echo "installed packages in their dependency closure: ${#installed[@]}"
for p in "${installed[@]}"; do
    dpkg -L "$p" 2>/dev/null || true
done | sort -u | while read -r f; do
    for v in "${views[@]}"; do
        if [[ "$f" == "$v"/* ]] && [ ! -d "$f" ] && [ -e "$f" -o -L "$f" ]; then
            cp -a --parents "$f" "$root" 2>/dev/null || true
        fi
    done
done

binds=()
for v in "${views[@]}"; do
    [ -d "$v" ] && binds+=(--bind "$root$v" "$v")
done
mkdir -p "$work/build"
bwrap --bind / / --dev /dev --proc /proc "${binds[@]}" -- bash -c "
    set -e
    cmake -S '$here' -B '$work/build' -G Ninja -DBUILD_TESTING=ON > '$work/configure.log' 2>&1 || { tail -5 '$work/configure.log'; exit 10; }
    cmake --build '$work/build' > '$work/build.log' 2>&1 || { grep -m5 -E 'error|Error' '$work/build.log'; exit 11; }
    ctest --test-dir '$work/build' 2>&1 | tail -3
"
