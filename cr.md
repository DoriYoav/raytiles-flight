# Code Review — raytiles

Scope: full library (`include/raytiles.h`, `src/raytiles.cpp`, `src/raii.hpp`, `src/tilekey.hpp`, `src/downloader.hpp`).

Focus: **memory**, **correctness**, **performance**.

Severity legend: **[H]** high (bug, leak, crash) · **[M]** medium (incorrect behavior, real perf hit) · **[L]** low (
smell, micro-perf, hygiene).

---

## Correctness

### [H] SSL certificate verification disabled on Apple

`downloader.hpp:78-80` unconditionally disables verification on `__APPLE__`. This is a security regression and should
not ship. Make it opt-in via `config` (e.g., `config.allow_insecure_tls = false`) so users can enable it explicitly
during local debugging only.

### [M] `ground_height` returns `0.0f` for "tile not loaded" - BY DESIGN, BUT MUST BE DOCUMENTED

`raytiles.cpp:306` returns `0.0f` as both "no tile" and "ocean at sea level". A flight-sim caller can't tell apart.
Change to `std::optional<float>` or document the sentinel value at minimum. (`-10000.0f` is the lowest valid Mapbox
value; pick a sentinel below it if you must keep `float`.)

### [M] `get_height_from_image` silently returns 0 on unsupported pixel formats - BY DESIGN, BUT MUST BE DOCUMENTED

`raytiles.cpp:43-44`. Currently raylib decodes Mapbox PNGs as R8G8B8 or R8G8B8A8, but a future raylib upgrade or
cache-loaded format change can break this silently — terrain becomes flat at sea level. Either log a warning once (with
format value), or convert at load time:

```cpp
if (img.format != PIXELFORMAT_UNCOMPRESSED_R8G8B8) ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8);
```

in `process_loaded_tiles` before storing.

### [L] `is_tile_covered` relies on signed shift semantics

`raytiles.cpp:432, 437-438` use `key.x >> 1`, `key.z >> 1`, `key.x << 1`, `key.z << 1` for negative tile indices. C++20
made arithmetic right-shift on signed well-defined (floor toward -∞) and left-shift well-defined on negative is **still
UB**. `int x = -1; x << 1;` is UB. Use `static_cast<unsigned>(x) << 1`, or `x * 2` (defined and the compiler folds it
identically). Fix the children-coordinate computation:

```cpp
const int child_x = key.x * 2;
const int child_z = key.z * 2;
```

### [L] `loaded_tile` / `loading_tile` duplicate the TileKey fields

Both structs carry `int x, z, zoom` — but they are stored as values in `std::unordered_map<TileKey, ...>` and the key
already has these. Wastes memory and risks drift if anyone updates one but not the other. Drop the duplicates and pull
them from the key during iteration.

---

## Memory

### [M] `httplib::Client` constructed per `fetch` call

`downloader.hpp:73`. A new TLS handshake (and DNS) for every single tile. With cold-start of ~200 tiles and 4 workers,
that's ~50 handshakes per worker before keep-alive can possibly help — but `Client` is local, so keep-alive applies only
to subsequent `Get` calls on **the same instance**, of which there are none. Move the client to a `thread_local` (or
per-worker member) so each worker keeps its own connection alive across many tiles.

### [M] No bound on `loading_tiles` size

When the camera teleports, `process_current_location` enqueues *every* missing tile in the new desired set in one
frame (potentially hundreds). All of them pile into the pool's queue and `loading_tiles`. With slow connections, memory
grows until the wave settles. Consider capping how many spawns per frame (already in TODO list).

### [M] Heightmap CPU image kept per loaded tile (~192KB / tile)

Now retained for `ground_height`. With ~200 tiles in the steady state that's roughly 38MB. Acceptable for a desktop app,
but worth documenting and optionally exposing a config flag (`keep_heightmap = true`) to disable for clients that don't
need ground queries.

### [L] `loaded_tile.done` adds 1 byte + alignment padding for nothing

See correctness section — also a small struct-size waste across hundreds of entries.

### [L] `models`, `tile_sizes`, `tile_distances` are `std::map<int, X>`

These are **dense, small, integer-keyed** containers (4-5 entries). `std::map` is a red-black tree → 3 allocations +
pointer chasing per lookup. Replace with `std::array<X, max_zooms>` indexed by `zoom - base_zoom`. See performance
section for the same point.

### [L] Possible `Image` leak on streamer destruction with futures in flight

If the streamer is destroyed while a worker has just decoded an `Image` and called `set_value`, the
`shared_future<Image>` inside `loading_tiles` is destroyed (no consumer). The `Image` struct is destroyed but its `data`
pointer is **not** unloaded — raylib's `Image` is a POD that doesn't own its allocation; only `UnloadImage` frees the
buffer.

Mitigation: drain `loading_tiles` in `~streamer()` by calling `process_loaded_tiles()` until empty, OR have the pool
itself wrap `Image` in `raii::image` (then the `shared_future<raii::image>` would auto-unload — but `raii::image` is
move-only and `shared_future<T>` requires `T` copyable for `.get()`'s return semantics, so this needs
`shared_future<std::shared_ptr<raii::image>>`).

---

## Performance

### [L] `std::erase_if` on `rendering_tiles` re-evaluates `is_tile_covered` for every entry every frame

`raytiles.cpp:257-261`. Each call to `is_tile_covered` does up to 5 `unordered_map::contains` lookups. With 200 tiles
that's ≤1000 lookups per frame. Acceptable, but if you ever scale up: cache the "covered" boolean alongside the tile,
invalidate when neighbors change.

### [L] Worker thread count hardcoded to 4

`raytiles.cpp:144`. Configurable via `config` (e.g. `config.download_threads = 4`) and default to
`std::thread::hardware_concurrency() / 2` or similar.

### [L] `std::format` per spawn call for cache paths

`raytiles.cpp:408-409`. Negligible compared to network, but `std::vformat` allocates a string each time. Acceptable.

---

## Threading & Concurrency

### [M] `~pool()` shutdown can stall up to 5 s

`downloader.hpp:117-120` calls `request_stop()` and waits for `~jthread` (which joins). A worker mid-`fetch` won't
observe the stop token until the HTTP read returns or times out (`set_read_timeout(5)`). UX impact on app exit. Options:

- Reduce `set_read_timeout`.
- Add an httplib client-level cancellation hook.
- Accept it (users don't restart often).

### [L] `cv.wait(lock, st, predicate)` interaction with `request_stop()`

This is fine (jthread + condition_variable_any pattern), just noting it's load-bearing — don't accidentally regress to
plain `condition_variable`.

### [L] `in_flight_images` can race the producer

`enqueue_and_load` checks the map, returns existing future if present. Worker erases on completion. The window where
both holders exist is OK (multiple shared_future copies — fine), but if the same path is requested again *between*
`set_value` and `in_flight_images.erase`, the requester gets a future that's already satisfied. That's actually the
desired behavior. Just call out in a comment.

---

## API / Code Quality

### [L] `provider::texture` / `provider::heightmap` are not `const`

They don't mutate; mark `const`.

### [L] `set_ambient_light` / `set_fog_color` could be `inline` in the header

One-liners; saves a function call. Trivial.

### [L] `draw()` mutates the shared model material per tile

`raytiles.cpp:235-236`. Functionally fine because draw is single-threaded and immediate, but pattern is fragile — if
anyone else draws the model concurrently they'll see whatever textures the last tile bound. Document or refactor to use
a per-tile `Material` (cheap struct).

### [L] `DrawCubeWires` is unconditionally drawn around every tile

`raytiles.cpp:240`. It's debug visualization. Belongs in `debug()`, not `draw()`.

### [L] Magic numbers in shader and code

- skirt drop = `1000.0` (vertex shader)
- fog start/end = `40000`, `70000`
- update_distance default = `1_000_000` (m²)
- update_height default = `500`

These should be either configurable or surfaced via `config` so a flight-sim user can tune for low/high altitude regimes
without recompiling shaders.

### [L] `cmake-build-debug` and `cmake-build-release` directories are checked in

Not a code issue but a repo-hygiene one. Add to `.gitignore` if not already.

---

## Open Tasks (moved from `readme.md`)

- [x] ~~Replace the per-entry `done` flag in `loading_tile` with immediate erase from the loading map after promotion (
  use iterator-safe erase or collect keys to erase after the loop).~~
- [x] ~~View-frustum culling **at render time only**: skip `DrawModel` for loaded tiles outside the camera frustum. The
  desired set and the download/upload pipeline must stay omnidirectional — clients like flight simulators can roll/pitch
  instantly and need tiles behind/around the camera already resident on the GPU to avoid pop-in.~~ (implemented as a
  cheap horizontal "behind-camera" half-space test with a one-tile buffer; skips `DrawModel` for tiles clearly behind
  the camera. Loading remains omnidirectional.)
- [x] ~~Keep the heightmap `Image` (CPU-side) for ground-height queries (raycast, camera collision, entity
  placement).~~ (CPU-side `raii::image` retained on each `loaded_tile`; exposed via `streamer::ground_height(Vector3)`.)
- [ ] Bound the number of concurrent in-flight downloads per frame and per zoom level to smooth bursts when the camera
  teleports.
- [ ] Eviction policy for the rendering map under memory pressure (LRU by last-frame-used) — today entries only leave
  when no longer desired.
- [ ] Asynchronous GPU upload (PBO / persistent-mapped buffers) so `LoadTextureFromImage` does not stall the render
  thread; would let us promote more than one tile per frame.
- [ ] Unit tests for the LOD subdivision and `is_tile_covered` logic — these are the parts most likely to regress
  silently.
