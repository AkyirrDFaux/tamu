#!/bin/bash
# Launches the Tamu Flutter app (debug Linux build).
set -e
cd "$(dirname "$0")/app"
exec build/linux/x64/debug/bundle/tamuapp "$@"