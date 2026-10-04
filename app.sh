#!/bin/bash
# Builds and launches the Tamu Flutter app (debug Linux build).
set -e
ROOT="$(cd "$(dirname "$0")" && pwd)"

# Regenerate the app version stamp (scripts/gen_app_version.py hashes app/lib) before every
# build so it cannot go stale, and pass the compile date so Settings shows a real value
# instead of the 'unknown' default (Docs/App/Settings.md "App compile date").
python3 "$ROOT/scripts/gen_app_version.py"
BUILD_DATE="$(date -u +%Y-%m-%d)"

cd "$ROOT/app"
flutter build linux --debug --dart-define=APP_BUILD_DATE="$BUILD_DATE"
exec build/linux/x64/debug/bundle/tamuapp "$@"
