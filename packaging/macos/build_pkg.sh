#!/usr/bin/env bash
# Builds the MLiveMIX macOS installer (.pkg) with AU and VST3 as selectable components.
#
# Usage: packaging/macos/build_pkg.sh <artefacts/Release dir> <version> <output.pkg>
#   e.g. packaging/macos/build_pkg.sh build/MLiveMIX_artefacts/Release 0.1.0 dist/MLiveMIX.pkg
#
# Optional: set INSTALLER_SIGN_IDENTITY="Developer ID Installer: ..." to sign the .pkg.

set -euo pipefail

ART="$1"
VERSION="$2"
OUT="$3"

HERE="$(cd "$(dirname "$0")" && pwd)"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

ID_PREFIX="com.mlivemix.mlivemix"

build_component () {
    local name="$1" bundle="$2" location="$3"
    local root="$WORK/root-$name"
    local plist="$WORK/$name.plist"

    if [ ! -d "$bundle" ]; then
        echo "error: $bundle not found" >&2
        exit 1
    fi

    mkdir -p "$root"
    ditto "$bundle" "$root/$(basename "$bundle")"

    # Install exactly where we say, never "relocate" to another copy of the bundle found on disk.
    pkgbuild --analyze --root "$root" "$plist"
    local i=0
    while /usr/libexec/PlistBuddy -c "Print :$i" "$plist" > /dev/null 2>&1; do
        # plutil -replace also creates the key when pkgbuild left it out
        plutil -replace "$i.BundleIsRelocatable" -bool NO "$plist"
        i=$((i + 1))
    done

    local args=(--root "$root"
                --component-plist "$plist"
                --identifier "$ID_PREFIX.$name"
                --version "$VERSION"
                --install-location "$location")

    if [ -d "$HERE/scripts-$name" ]; then
        args+=(--scripts "$HERE/scripts-$name")
    fi

    pkgbuild "${args[@]}" "$WORK/MLiveMIX-$name.pkg"
}

build_component au   "$ART/AU/MLiveMIX.component" "/Library/Audio/Plug-Ins/Components"
build_component vst3 "$ART/VST3/MLiveMIX.vst3"   "/Library/Audio/Plug-Ins/VST3"

sed "s/@VERSION@/$VERSION/g" "$HERE/distribution.xml" > "$WORK/distribution.xml"

mkdir -p "$(dirname "$OUT")"

sign_args=()
if [ -n "${INSTALLER_SIGN_IDENTITY:-}" ]; then
    sign_args=(--sign "$INSTALLER_SIGN_IDENTITY")
fi

productbuild --distribution "$WORK/distribution.xml" \
             --package-path "$WORK" \
             --resources "$HERE/resources" \
             "${sign_args[@]+"${sign_args[@]}"}" \
             "$OUT"

echo "Installer written to $OUT"
