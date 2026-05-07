# Raytiles

<img src="raytiles.png" alt="logo" width="256" align="right"/>

3D world streaming engine for [raylib](https://www.raylib.com/). Pulls satellite imagery and heightmap tiles from a
map provider (Mapbox by default), turns them into displaced 3D meshes, and renders the world around a moving camera.
Built for small-scale flight simulators, mission planners, and similar geospatial visualizations.

Allow streaming any location on Earth up to zoom level 15.

Example video with base zoom level 11 up to max zoom level 14:

https://github.com/user-attachments/assets/0422ffea-654f-4299-8860-23f99d7d98ec

## Features

- Background tile downloading (HTTP + persistent on-disk cache).
- Adaptive level-of-detail: more detail near the camera, less far away.
- GPU-side displacement via a heightmap-driven vertex shader.
- Per-frame upload budgeting — no GPU stalls on bursty load.
- Ground-truth altitude queries (`ground_height`) for collision / spawning.
- RAII everywhere: zero manual `Unload*` calls, zero leaks on error paths.

## Quick Start

`MAPBOX_TOKEN` environment variable must be set to a valid Mapbox API token before running the example below.

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

## TODOs

- [x] support raylib 6.0
- [ ] support web builds (emscripten)
- [ ] support more providers (TomTom, OpenStreetMap, etc.)
- [ ] support more than level 15 zoom