# Raytiles

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

## Public API

Everything you need is in [`include/raytiles.h`](raytiles.h):

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
