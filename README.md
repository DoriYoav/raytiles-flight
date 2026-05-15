# Raytiles

<img src="res/logo.png" alt="logo" width="256" align="right"/>

**Raytiles** is a 3D geospatial engine 🌎 for [raylib](https://www.raylib.com/). Designed to stream and render the real
world in real-time, it allows
you to visualize any location on Earth directly within your raylib applications.

Built for indie developers and professionals alike, Raytiles perfectly fits UAV simulations, flight planning software,
lightweight GIS analysis, presentations, digital sandtables, and any other geospatial
visualizations ([check out the examples below!](#raytiles-examples)).

It provides precise, ground-truth altitude data, essential for accurate collision detection and spawning mechanics in
gaming, as well as topographical analysis for GIS purposes.

Originally developed to power a flight simulator, Raytiles was extracted into a lightweight, standalone library to
enable seamless embedding into any raylib project.

![GitHub Release](https://img.shields.io/github/v/release/ziv/raytiles)
![GitHub License](https://img.shields.io/github/license/ziv/raytiles)

[![macOS Build](https://github.com/ziv/raytiles/actions/workflows/macos.yml/badge.svg)](https://github.com/ziv/raytiles/actions/workflows/macos.yml)
[![Linux Build](https://github.com/ziv/raytiles/actions/workflows/linux.yml/badge.svg)](https://github.com/ziv/raytiles/actions/workflows/linux.yml)
[![Windows Build](https://github.com/ziv/raytiles/actions/workflows/windows.yml/badge.svg)](https://github.com/ziv/raytiles/actions/workflows/windows.yml)
[![Emscripten Build](https://github.com/ziv/raytiles/actions/workflows/emscripten.yml/badge.svg)](https://github.com/ziv/raytiles/actions/workflows/emscripten.yml)

## Features

- Streaming **ANY** location on Earth!
- **Background** tile downloading (HTTP + persistent on-disk cache).
- Adaptive **LOD** (level-of-detail): more detail near the camera, less far away.
- **Lights and shadows** via normal maps.
- **GPU**-side displacement via a heightmap-driven vertex shader.
- Per-frame upload **budgeting**, no GPU stalls on bursty load.
- Ground-truth **altitude queries** (`ground_height`) for collision / spawning.
- **RAII** everywhere: zero manual `Unload*` calls, zero leaks on error paths.
- Pure **C++** and **C** wrapper APIs (`raytiles.h` and `craytiles.h`).
- **Cross-platform** builds for Windows, Linux, and macOS.
- **Configurable**! fit it to your needs by tweaking `raytiles::config` fields.
- **Open-source** and permissively licensed (MIT)

## 3D Tiles

**Why not use 3D Tiles? (Cesium, Google Earth, etc.)**

3D Tiles is a powerful format for streaming and rendering large 3D geospatial datasets, but it comes with significant
complexity and overhead.

1. It designed for walkthroughs resolution and visual fidelity, not for flight simulations as Raytiles mainly targets.
2. Google 3D tiles require a token and there is limited free usage, which can be a barrier for indie developers and
   small projects.
3. Google does not allow caching 3D Tiles data, which means that every time you want to render a location, you would
   need to fetch the data from Google's servers, which can lead to latency issues and increased bandwidth usage.
4. The 3D tiles GLTF format contain numbers with high precision (double) and not fit for raylib's float based rendering
   pipeline. It requires to decode the data on CPU and upload it to GPU every frame, which can lead to performance
   issues.
5. Cesium 3D tiles is even more complex since we don't get ready models like Google, but instead we get only mesh data.
   The rest is the same as Raytiles does.

Actually, this project started with the intention to use 3D Tiles, and there is an implementation of a 3D Tiles renderer
in the `legacy/` directory, but it was eventually scrapped in favor of a simpler, lightweight approach that better fits
raytiles needs.

Example of the 3D Tiles renderer in action using Google 3D Tiles (width debug data and grid):

![3D Tiles Renderer](res/example-3dtiles.png)

## Raytiles Examples

### Rendering Area of Interest

The following example video is part of the islands of Greece, rendered with Mapbox tiles at zoom level 11 to 14:

https://github.com/user-attachments/assets/0422ffea-654f-4299-8860-23f99d7d98ec

### Lights and Shadows (Sun effect)

This example demonstrate lights and shadows:

https://github.com/user-attachments/assets/6e373cb4-a1fa-4c21-a72a-db2d0bd96a89

## Quick Start

If you are using CMake, you can add **raytiles** using `FetchContent`:

```cmake
include(FetchContent)
FetchContent_Declare(
        raytiles
        GIT_REPOSITORY https://github.com/ziv/raytiles.git
        GIT_TAG v0.7.0
        GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(raytiles)
```

The following example shows how to use the C++ API to stream and render the world in a raylib application:

```cpp
#include "raylib.h"
#include "raytiles.h"

int main() {
  InitWindow(1280, 720, "raytiles");

  // streamer confuguration with default values, tweak as needed
  raytiles::config conf;
  
  // pool configuration with default values, tweak as needed
  raytiles::pool_config pool_conf;

  // streamer instance
  raytiles::streamer streamer(conf, pool_conf);

  Camera3D camera = /* ... your camera ... */;

  while (!WindowShouldClose()) {
    // update the streamer with the current camera state
    streamer.update(camera);

    BeginDrawing();
        ClearBackground(SKYBLUE);
        BeginMode3D(camera);
            // draw the streamed world
            streamer.draw(camera);
        EndMode3D();
    EndDrawing();
  }

  CloseWindow();
}
```

See `sandbox/main.cpp` for a full runnable example with input handling.

## Anchors

**Raytiles** uses an anchor-based system to determine which part of the world to stream and render. An anchor is a point
in the world that serves as a reference for the streaming and rendering process. This point will be your raylib's
`X=0,Y=0,Z=0` position.

To define an anchor, you have to find the corresponding tile coordinates (x, y, zoom) for the location you want to
stream.

You can use the following tools to find the tile coordinates for a specific location:

- https://labs.mapbox.com/what-the-tile/
- https://tools.geofabrik.de/calc/#type=geofabrik_standard&grid=1

Find the tile in zoom 9 that closest to the area of interest and put those values in the configuration:

```c++
raytiles::config conf;

conf.anchor_x_tile = 123;
conf.anchor_y_tile = 456;
```

## Providers

**raytiles** is designed to be provider-agnostic, allowing you to use any tile server that follows the XYZ tiling
scheme (slippy maps format).

By default, it comes with a built-in providers, **Esri** for texture and **Mapzen** for heightmap (Terrarium format) and
normals, but you can easily configure any other provider using the `raytiles::pool_config ` struct.

#### Heightmap and Normals Provider

It is not recommended to replace the elevation provider since the height calculation strategy is tightly coupled with
the encoding of the heightmap, but you can replace the texture provider without any issue.

Reference to Mapzen height
calculations [can be found here](https://github.com/tilezen/joerd/blob/master/docs/formats.md).

#### Texture Provider

The default texture providers is configured as follows:

```c++
raytiles::pool_config pool_conf;

pool_conf.texture_url = "https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{zoom}/{y}/{x}";
```

Example of using **Mapbox** as texture provider (requires an access token):

```c++
raytiles::pool_config pool_conf;

pool_conf.texture_url = "https://api.mapbox.com/v4/mapbox.satellite/{zoom}/{x}/{y}.pngraw?access_token=YOUR_MAPBOX_ACCESS_TOKEN";
```

## Caching

**raytiles** includes a simple on-disk caching mechanism to store downloaded tiles, reducing bandwidth usage and
improving performance on subsequent runs. The cache is organized by provider and tile coordinates, making it easy to
manage and clear if needed. You can configure the cache directory and behavior through the `raytiles::config_pool`
struct.

Currently, the cache does not implement any eviction policy, so it will grow indefinitely as you download more tiles.
You can manually clear the cache by deleting the cache directory.

For caching purposes, the downloader pool does not cancel in-flight requests, so even if tile is currently not needed,
it will still be downloaded and cached. There
is [an open issue to add support for request cancellation](https://github.com/ziv/raytiles/issues/48).

## Memory Usage

The default configuration optimizes for relatively low memory usage, with a maximum of ~600 tiles in memory at any given
time. Note that any change in configuration may affect the memory usage, so it's important to understand how the memory
is calculated.

- Tile size: $256*256=65,536$ pixels
- RGBA texture: $65,536 \text{ pixels} \times 4 \text{ channels} = 262,144$ bytes (256 KB) per tile (uncompressed)

VRAM:

- Albedo texture + mipmap: $256 \text{ KB} \times 1.33 = \textbf{341 KB}$
- Heightmap texture $\textbf{256 KB}$
- Normal map texture $\textbf{256 KB}$
- VRAM per tile (uncompressed with mipmap): $341 + 256 + 256 = \textbf{853 KB}$

For 600 tiles, the total memory usage would be approximately:

- VRAM $853 \text{ KB} \times 600 = 511,800 \text{ KB} \approx \textbf{500 MB}$
- RAM $256 \text{ KB} \times 600 = 153,600 \text{ KB} = \textbf{150 MB}$

## How It Works

Raytiles is a conductor that orchestrates files read/download, GPU uploads, and rendering. The rest of the magic is
happened in the shaders, where we do GPU-side displacement and normal mapping to create the illusion of a detailed
terrain without the overhead of complex geometry.

![how it works](res/how-it-works0.png)

---

## TODOs

- [x] support normals for lighting (in progress)
- [ ] support more than level 15 zoom
- [ ] add sky module (atmosphere + sun + moon + stars)

---

<img src="res/raylove.png" align="left">

Made with ❤️ to the raylib community. If you find this library useful, please consider starring the repo and sharing it
with your friends! Contributions and feedback are always welcome. Happy coding!
