#!/bin/bash
# Builds and launches the Tamu Flutter app (debug Linux build).
set -e
cd "$(dirname "$0")/app"
flutter build linux --debug
exec build/linux/x64/debug/bundle/tamuapp "$@"