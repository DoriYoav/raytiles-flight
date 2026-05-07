# Code Review — raytiles

Scope: full library (`include/raytiles.h`, `src/raytiles.cpp`, `src/raii.hpp`, `src/tilekey.hpp`, `src/downloader.hpp`).

Focus: **memory**, **correctness**, **performance**.

Severity legend: **[H]** high (bug, leak, crash) · **[M]** medium (incorrect behavior, real perf hit) · **[L]** low (
smell, micro-perf, hygiene).


---

## API / Code Quality

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
