# Raytiles

3D tiles world streaming engine for raylib.

## Architecture

The engine streams map tiles (texture + heightmap) around a moving camera and renders them as displaced 3D meshes. It is
composed of two main parts:

- **Download pool**: a thread-safe job queue that fetches tiles in the background, decodes them into `Image` objects,
  and caches them on disk. Returns `std::shared_future<Image>` so callers can pick up the result when it's ready.
- **Tile streamer**: drives the per-frame lifecycle of tiles around the camera — deciding what should be loaded,
  promoting downloads into GPU resources, rendering, and freeing what is no longer needed.

### Data Structures

- **Desired set** — `std::unordered_set<TileKey>`: the tiles required for the current camera position, computed every
  time the camera moves enough. Recomputed wholesale, not incrementally.
- **Loading map** — `std::unordered_map<TileKey, loading_tile>`: tiles whose images are being downloaded/decoded. Each
  entry holds two `shared_future<Image>` (texture + heightmap).
- **Rendering map** — `std::unordered_map<TileKey, loaded_tile>`: tiles whose GPU textures are ready to render.
  Resources are owned via RAII wrappers (`raii::texture`), so removing an entry from the map automatically frees the GPU
  memory.

There is no separate "stale" list. Cleanup is implicit: dropping an entry from a map runs the RAII destructor.

### Life Cycle

#### 1. Update (per frame, on camera movement)

- If the camera moved enough (distance / altitude threshold), rebuild the desired set from the current camera position
  using LOD subdivision (closer tiles → higher zoom).
- For every key in the desired set that is not already in the loading or rendering map, enqueue a download job and
  insert a `loading_tile` into the loading map.

#### 2. Promote loaded tiles (per frame)

- Walk the loading map; for each entry whose two futures are ready:
    - Take ownership of the decoded `Image`s into RAII handles.
    - If either image is invalid → drop it (RAII frees both); the entry will be erased.
    - If the tile is no longer desired → drop it (RAII frees both); the entry will be erased.
    - Otherwise → upload to GPU as `raii::texture`, insert a `loaded_tile` into the rendering map.
- At most one promotion per frame to avoid GPU upload spikes.

#### 3. Render (per frame)

- Iterate the rendering map and draw every loaded tile. Do not filter by desired/covered here — rendering does not
  decide lifecycle. This keeps frames stable and avoids popping while higher-LOD replacements are still loading.

#### 4. Cleanup (per frame)

- Erase from the loading map any entry that was promoted or dropped in step 2.
- Erase from the rendering map any entry that is (a) no longer desired **and** (b) covered by an alternate-LOD tile that
  is already loaded (parent or full set of children). RAII destructors free the GPU resources.

### Ownership & Resource Safety

All raylib resources (`Shader`, `Model`, `Texture2D`, `Image`) are held through `raii::resource<T, Unload>` wrappers in
`src/raii.hpp`. This means:

- Map erase = automatic resource free.
- Early returns in error paths cannot leak (no `goto cleanup`, no manual `Unload*` calls).
- The `streamer` destructor is defaulted; member destruction order handles everything.

## TODO

Tasks to complete the architecture:

- [x] ~~Replace the per-entry `done` flag in `loading_tile` with immediate erase from the loading map after promotion (use
  iterator-safe erase or collect keys to erase after the loop).~~
- [x] ~~View-frustum culling **at render time only**: skip `DrawModel` for loaded tiles outside the camera frustum to cut
  draw calls / GPU work. The desired set and the download/upload pipeline must stay omnidirectional — clients like
  flight simulators can roll/pitch instantly and need tiles behind/around the camera already resident on the GPU to
  avoid pop-in.~~ (implemented as a cheap horizontal "behind-camera" half-space test with a one-tile buffer; skips
  `DrawModel` for tiles clearly behind the camera. Loading remains omnidirectional.)
- [ ] Keep the heightmap `Image` (CPU-side) for ground-height queries (raycast, camera collision, entity placement).
  Currently it is freed right after upload.
- [ ] Cancellation of in-flight downloads when a tile becomes stale before its future resolves, so the pool does not
  waste bandwidth / CPU on tiles we will throw away.
- [ ] Bound the number of concurrent in-flight downloads per frame and per zoom level to smooth bursts when the camera
  teleports.
- [ ] Eviction policy for the rendering map under memory pressure (LRU by last-frame-used) — today entries only leave
  when no longer desired.
- [ ] Replace `LoadModelFromMesh` per zoom with a shared mesh + per-tile transform, or instanced draw, to cut VRAM and
  draw-call overhead.
- [ ] Asynchronous GPU upload (PBO / persistent-mapped buffers) so `LoadTextureFromImage` does not stall the render
  thread; would let us promote more than one tile per frame.
- [ ] Unit tests for the LOD subdivision and `is_tile_covered` logic — these are the parts most likely to regress
  silently.
