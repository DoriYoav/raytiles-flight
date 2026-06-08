#!/usr/bin/env bash
# Launch the raytiles flight demo (TIE / X-Wing, C to switch ships).
# Uses free Esri ArcGIS imagery; MAPBOX_TOKEN just needs to be non-empty.
set -e

# Run from this script's folder, wherever it's invoked from.
cd "$(dirname "$0")"

# Build the demo if it isn't there yet (first run after a fresh clone).
if [ ! -x build/demo ]; then
    echo "demo binary not found — building it first..."
    cmake --build build --target demo -j8
fi

MAPBOX_TOKEN=dummy ./build/demo
