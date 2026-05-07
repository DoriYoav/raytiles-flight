# Raytiles

3D world streaming engine for [raylib](https://www.raylib.com/). Pulls satellite imagery and heightmap tiles from a
map provider (Mapbox by default), turns them into displaced 3D meshes, and renders the world around a moving camera.
Built for small-scale flight simulators, mission planners, and similar geospatial visualizations.

## Features

- Background tile downloading (HTTP + persistent on-disk cache).
- Adaptive level-of-detail: more detail near the camera, less far away.
- GPU-side displacement via a heightmap-driven vertex shader.
- Per-frame upload budgeting — no GPU stalls on bursty load.
- Ground-truth altitude queries (`ground_height`) for collision / spawning.
- RAII everywhere: zero manual `Unload*` calls, zero leaks on error paths.

## Architecture

```
                     ┌──────────────┐
   camera position → │   streamer   │ → draw calls (raylib)
                     └──────┬───────┘
                            │
                ┌───────────┴───────────┐
                │                       │
        ┌───────▼─────┐         ┌───────▼──────┐
        │ desired set │         │ download pool│ ── HTTP ──▶ map provider
        │  (per-frame │         │  (N workers, │       │
        │    LOD)     │         │   bytes only)│       ▼
        └───────┬─────┘         └───────┬──────┘   on-disk
                │                       │           cache
                │       futures<bytes>  │
                │   ┌───────────────────┘
                ▼   ▼
        ┌──────────────────┐                 ┌──────────────────┐
        │  loading tiles   │ ── decode ────▶ │ rendering tiles  │
        │  (futures)       │   + upload      │ (GPU textures)   │
        └──────────────────┘   on main       └──────────────────┘
```

The streamer runs three logical phases each frame:

1. **Decide** — what tiles should be visible at the current camera position?
2. **Promote** — pick up any finished downloads, decode + upload to the GPU within a wall-clock budget.
3. **Render** — draw everything currently loaded.

Workers never touch raylib (it isn't thread-safe). Decoding and GPU uploads happen on the main thread; workers only
move bytes from the network onto disk and into a `std::shared_future<std::string>`.

## Quick Start

```cpp
#include "raytiles.h"

int main() {
  InitWindow(1280, 720, "raytiles");

  raytiles::config conf;
  conf.rendering_radius = 7;
  conf.max_zoom = 14;

  raytiles::provider provider(std::getenv("MAPBOX_TOKEN"));
  raytiles::streamer streamer(conf, provider);

  Camera3D camera = /* ... your camera ... */;

  while (!WindowShouldClose()) {
    streamer.update(camera);

    BeginDrawing();
    ClearBackground(SKYBLUE);
    BeginMode3D(camera);
    streamer.draw(camera);
    EndMode3D();
    EndDrawing();
  }

  CloseWindow();
}
```

See `sandbox/main.cpp` for a full runnable example with input handling.

## Public API

Everything you need is in [`include/raytiles.h`](include/raytiles.h):

- **`raytiles::config`** — tunables (zoom range, render radius, fog, cache paths, …). All defaults are sensible.
- **`raytiles::provider`** — wraps your map-service API token; constructs URLs.
- **`raytiles::streamer`** — the per-frame driver: `update`, `draw`, `debug`, `set_ambient_light`, `set_fog_color`,
  `ground_height`.

Each field and method is documented inline; build with Doxygen or read the header directly.

## Building

CMake + Ninja, C++23. raylib is fetched and built automatically via `FetchContent`:

```bash
cmake -B build -G Ninja
cmake --build build
```

Outputs `libraytiles.a` plus the `Sandbox` example.

### Required environment

- A Mapbox token (or compatible provider) is needed at runtime; the sandbox reads `MAPBOX_TOKEN` from the environment.

## Tile Cache

Tiles are downloaded once and cached forever under `assets/tiles/{texture,heightmap}/{zoom}/{x}/{z}.png`. The cache
is the whole point of background streaming: even tiles you fly over briefly stay on disk and load instantly next time.
Override the layout via `config::texture_cache_path` and `config::heightmap_cache_path`.
