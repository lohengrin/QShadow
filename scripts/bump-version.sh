#!/bin/sh
# Usage: scripts/bump-version.sh X.Y.Z
# Updates the version in CMakeLists.txt and vcpkg.json (the release workflow
# checks that both match the pushed tag).
set -eu
[ $# -eq 1 ] || { echo "usage: $0 X.Y.Z" >&2; exit 1; }
case "$1" in *[!0-9.]*|"") echo "version must look like 1.2.3" >&2; exit 1;; esac
cd "$(dirname "$0")/.."
sed -i "s/^project(qshadow VERSION [0-9.]*/project(qshadow VERSION $1/" CMakeLists.txt
sed -i "s/^\(  \"version\": \)\"[0-9.]*\"/\1\"$1\"/" vcpkg.json
git --no-pager diff --stat CMakeLists.txt vcpkg.json
echo "Next: git commit -am 'release v$1' && git tag v$1 && git push origin HEAD v$1"
