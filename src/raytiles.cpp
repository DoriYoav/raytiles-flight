#include "raytiles.h"

#include <raylib.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <future>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "tilekey.hpp"

using namespace std::chrono_literals;

namespace raytiles {

namespace {
/// a better performance function instead of using
/// const auto c = GetImageColor(img, px, pz);
float get_height_from_image(const Image &img, int x, int y) {
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  if (x >= img.width) x = img.width - 1;
  if (y >= img.height) y = img.height - 1;

  const auto pixels = static_cast<const unsigned char *>(img.data);
  unsigned char r, g, b;

  if (img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8) {
    const int index = (y * img.width + x) * 3;
    r = pixels[index];
    g = pixels[index + 1];
    b = pixels[index + 2];
  } else if (img.format == PIXELFORMAT_UNCOMPRESSED_R8G8B8A8) {
    const int index = (y * img.width + x) * 4;
    r = pixels[index];
    g = pixels[index + 1];
    b = pixels[index + 2];
  } else {
    return 0.0f;
  }

  return -10000.0f + (static_cast<float>(r) * 65536.0f + static_cast<float>(g) * 256.0f + static_cast<float>(b)) * 0.1f;
}

int radius(const float height) {
  if (height < 500) return 2;
  if (height < 1000) return 3;
  if (height < 2000) return 4;
  if (height < 4000) return 5;
  if (height < 8000) return 6;
  return 7;
}

constexpr auto vertex_shader = R"(
#version 330

in vec3 vertexPosition;
in vec2 vertexTexCoord;

uniform mat4 mvp;
uniform mat4 matModel;
uniform sampler2D heightMap;
uniform float heightScale;
uniform vec3 cameraPosition;

out vec2 fragTexCoord;
out float fragCamDist;

void main()
{
    fragTexCoord = vertexTexCoord;

    vec3 color = texture(heightMap, vertexTexCoord).rgb;
    vec3 c = color * 255.0;
    float heightValue = -10000.0 + ((c.r * 65536.0 + c.g * 256.0 + c.b) * 0.1);

    vec3 displacedPosition = vertexPosition;
    displacedPosition.y += heightValue * heightScale;

    // add skirt to the edges of the terrain to hide gaps between tiles
    float epsilon = 0.001;

    bool isEdge = (vertexTexCoord.x < epsilon) ||
                  (vertexTexCoord.x > 1.0 - epsilon) ||
                  (vertexTexCoord.y < epsilon) ||
                  (vertexTexCoord.y > 1.0 - epsilon);

    if (isEdge)
    {
        displacedPosition.y -= 1000.0 * heightScale;
    }

    vec3 worldPosition = vec3(matModel * vec4(displacedPosition, 1.0));
    fragCamDist = distance(worldPosition, cameraPosition);

    gl_Position = mvp * vec4(displacedPosition, 1.0);
}
)";

constexpr auto fragment_shader = R"(
#version 330

in vec2 fragTexCoord;
in float fragCamDist;

uniform sampler2D texture0;
uniform vec4 ambientLight;
uniform vec4 fogColor;

out vec4 finalColor;

const float fogStart = 40000.0;
const float fogEnd   = 70000.0;

void main()
{
    vec4 texColor = texture(texture0, fragTexCoord);
    vec4 lit = texColor * ambientLight;
    float fogFactor = clamp((fragCamDist - fogStart) / (fogEnd - fogStart), 0.0, 1.0);
    finalColor = mix(lit, fogColor, fogFactor);
}
)";
}  // namespace

provider::provider(std::string token) : token(std::move(token)) {}

std::string provider::texture(const int zoom, const int x, const int z) {
  return std::format("/v4/mapbox.satellite/{}/{}/{}.png?access_token={}", zoom, x, z, token);
}

std::string provider::heightmap(const int zoom, const int x, const int z) {
  return std::format("/v4/mapbox.terrain-rgb/{}/{}/{}.pngraw?access_token={}", zoom, x, z, token);
}

streamer::streamer(config conf, provider maps_provider)
    : conf(std::move(conf)),
      maps_provider(std::move(maps_provider)),
      displacement_shader(raii::load_shader_from_memory(vertex_shader, fragment_shader)),
      tile_downloader(4) {
  const auto distance = static_cast<float>(conf.rendering_radius) * conf.base_zoom_tile_size;

  // prepare tiles size and distance for each zoom
  const int zoom_count = conf.max_zoom - conf.base_zoom + 1;
  tile_sizes.resize(zoom_count);
  tile_distances.resize(zoom_count);
  for (int zoom = conf.base_zoom; zoom <= conf.max_zoom; ++zoom) {
    const int ratio = 1 << (zoom - conf.base_zoom);
    const int idx = zoom - conf.base_zoom;
    tile_sizes[idx] = conf.base_zoom_tile_size / static_cast<float>(ratio);
    tile_distances[idx] = distance * distance / static_cast<float>(ratio * ratio * ratio);
  }

  // creating model for each zoom level, avoiding the need to stretch the model
  models.reserve(zoom_count);
  for (int zoom = conf.base_zoom; zoom <= conf.max_zoom; ++zoom) {
    const int idx = zoom - conf.base_zoom;
    const int res = 16 * (1 << idx);
    const float size = tile_sizes[idx];
    const float skirt_size = zoom == conf.base_zoom ? size * conf.skirt_size * 3.0f : size * conf.skirt_size;

    models.emplace_back(raii::load_model_from_mesh(GenMeshPlane(size + skirt_size, size + skirt_size, res, res)));
    models[idx]->materials[0].shader = *displacement_shader;
  }

  // cache shader locations
  cam_pos_loc = GetShaderLocation(*displacement_shader, "cameraPosition");
  ambient_loc = GetShaderLocation(*displacement_shader, "ambientLight");
  fog_color_log = GetShaderLocation(*displacement_shader, "fogColor");

  // configure the slot the heightmap data use - MATERIAL_MAP_ROUGHNESS slot
  constexpr int heightmapSlotIndex = MATERIAL_MAP_ROUGHNESS;
  SetShaderValue(*displacement_shader, GetShaderLocation(*displacement_shader, "heightMap"), &heightmapSlotIndex, SHADER_UNIFORM_INT);

  constexpr float heightScale = 1.0;
  SetShaderValue(*displacement_shader, GetShaderLocation(*displacement_shader, "heightScale"), &heightScale, SHADER_UNIFORM_FLOAT);

  TraceLog(LOG_INFO, "raytiles streamer initialized");
}

void streamer::update(const Camera3D &camera) {
  // start with clearing items we've done with
  remove_unused_tiles();

  const auto position = camera.position;

  // look for done futures in "desired_tiles" to build "rendered_tiles" map
  process_loaded_tiles();

  // do not update tiles list if didn't pass enough distance
  if (Vector3DistanceSqr(position, last_position) < conf.update_distance && std::fabsf(position.y - last_position.y) < conf.update_height) return;
  last_position = position;

  // use current location to build "desired_tiles" map
  process_current_location();
}

void streamer::draw(const Camera3D &camera) {
  // set the camera location (for distance -> fog)
  SetShaderValue(*displacement_shader, cam_pos_loc, &camera.position, SHADER_UNIFORM_VEC3);

  // set the ambient color (weather/day/night/...)
  SetShaderValue(*displacement_shader, ambient_loc, ambient_light, SHADER_UNIFORM_VEC4);

  // set the fog color (to match the sky)
  SetShaderValue(*displacement_shader, fog_color_log, fog_color, SHADER_UNIFORM_VEC4);

  // horizontal forward direction (xz plane). tiles are flat on y=0 so testing
  // against the horizontal projection of the camera forward is enough to decide
  // "is the tile in front of (or beside) the camera?". if the camera is looking
  // straight up/down, fwd_len ~ 0 and we disable culling for this frame.
  const float fwd_x = camera.target.x - camera.position.x;
  const float fwd_z = camera.target.z - camera.position.z;
  const float fwd_len = std::sqrt(fwd_x * fwd_x + fwd_z * fwd_z);
  const bool cull_enabled = fwd_len > 0.0001f;
  const float fwd_nx = cull_enabled ? fwd_x / fwd_len : 0.0f;
  const float fwd_nz = cull_enabled ? fwd_z / fwd_len : 0.0f;

  for (const auto &[key, tile] : rendering_tiles) {
    const auto size = tile_sizes[key.zoom - conf.base_zoom];

    // skip tiles that lie behind the camera by more than one tile-size buffer.
    // tiles to the side (perpendicular to forward) and anything in front pass.
    // the buffer is the tile size at this zoom so a tile straddling the camera
    // plane is still drawn.
    if (cull_enabled) {
      const float dx = tile.tx - camera.position.x;
      const float dz = tile.tz - camera.position.z;
      const float along_forward = dx * fwd_nx + dz * fwd_nz;
      if (along_forward < -size) continue;
    }

    const auto &model = models[key.zoom - conf.base_zoom];
    model->materials[0].maps[MATERIAL_MAP_ALBEDO].texture = *tile.tx_texture;
    model->materials[0].maps[MATERIAL_MAP_ROUGHNESS].texture = *tile.hm_texture;

    DrawModel(*model, {tile.tx, 0.0f, tile.tz}, 1.0f, WHITE);

    DrawCubeWires({tile.tx, 0.0f, tile.tz}, size, 200.0f, size, RED);
  }
}

void streamer::debug(const Camera3D &camera) {
  const auto width = GetScreenWidth();
  const auto height = GetScreenHeight();
  for (const auto &[key, tile] : rendering_tiles) {
    const auto [x, y] = GetWorldToScreen({tile.tx, 0.0f, tile.tz}, camera);
    if (x < 0 || x > width || y < 0 || y > height) continue;

    Color c = key.zoom == 14 ? RED : GREEN;
    DrawText(TextFormat("%d", key.zoom), static_cast<int>(x), static_cast<int>(y), 15, c);
  }
}

void streamer::remove_unused_tiles() {
  std::erase_if(rendering_tiles, [&](const auto &item) {
    if (desired_keys.contains(item.first)) return false;
    if (!is_tile_covered(item.first)) return false;
    return true;
  });
}

void streamer::set_ambient_light(const Color color) {
  ambient_light[0] = static_cast<float>(color.r) / 255.0f;
  ambient_light[1] = static_cast<float>(color.g) / 255.0f;
  ambient_light[2] = static_cast<float>(color.b) / 255.0f;
  ambient_light[3] = static_cast<float>(color.a) / 255.0f;
}

void streamer::set_fog_color(const Color color) {
  fog_color[0] = static_cast<float>(color.r) / 255.0f;
  fog_color[1] = static_cast<float>(color.g) / 255.0f;
  fog_color[2] = static_cast<float>(color.b) / 255.0f;
  fog_color[3] = static_cast<float>(color.a) / 255.0f;
}

float streamer::ground_height(const Vector3 position) const {
  // walk from the highest available zoom down to base; whichever zoom holds the
  // tile that contains (position.x, position.z) wins. higher zoom = finer
  // sample, so we prefer it if loaded.
  for (int zoom = conf.max_zoom; zoom >= conf.base_zoom; --zoom) {
    const float size = tile_sizes[zoom - conf.base_zoom];

    const int tile_x = static_cast<int>(std::floorf(position.x / size));
    const int tile_z = static_cast<int>(std::floorf(position.z / size));

    const auto it = rendering_tiles.find(TileKey{zoom, tile_x, tile_z});
    if (it == rendering_tiles.end()) continue;

    const auto &tile = it->second;
    const Image &img = *tile.hm_image;
    if (!IsImageValid(img)) continue;

    // local uv inside the tile, [0, 1)
    const float u = (position.x - static_cast<float>(tile_x) * size) / size;
    const float v = (position.z - static_cast<float>(tile_z) * size) / size;

    const int px = static_cast<int>(u * static_cast<float>(img.width));
    const int py = static_cast<int>(v * static_cast<float>(img.height));

    return get_height_from_image(img, px, py);
  }
  return 0.0f;
}

void streamer::process_current_location() {
  desired_keys.clear();
  desired_keys.reserve(512);

  auto subdivide = [&](auto &self, const int zoom, const int tx, const int tz) -> void {
    // no need for calculations, this is the last zoom
    if (zoom == conf.max_zoom) {
      desired_keys.insert({zoom, tx, tz});
      return;
    }

    // calculate distance of the tile from the camera
    const float tile_size = tile_sizes[zoom - conf.base_zoom];
    const float world_x = (static_cast<float>(tx) + 0.5f) * tile_size;
    const float world_z = (static_cast<float>(tz) + 0.5f) * tile_size;
    const float ddx = last_position.x - world_x;
    const float ddz = last_position.z - world_z;

    // check against the next zoom distance threshold, if it's far enough,
    // add to the list, otherwise subdivide into 4 children
    if (ddx * ddx + ddz * ddz >= tile_distances[zoom + 1 - conf.base_zoom]) {
      desired_keys.insert({zoom, tx, tz});
      return;
    }

    // split into 4 children at zoom+1.
    const int child_zoom = zoom + 1;
    const int cx0 = tx * 2;
    const int cz0 = tz * 2;
    for (int ox = 0; ox < 2; ++ox)
      for (int oz = 0; oz < 2; ++oz) self(self, child_zoom, cx0 + ox, cz0 + oz);
  };

  const int current_tile_x = static_cast<int>(std::floorf(last_position.x / conf.base_zoom_tile_size));
  const int current_tile_z = static_cast<int>(std::floorf(last_position.z / conf.base_zoom_tile_size));

  const auto r = radius(last_position.y);
  const auto allowed_radius = (r - 1) * (r - 1);

  for (int dx = -r; dx <= r; ++dx)
    for (int dz = -r; dz <= r; ++dz)
      if (dz * dz + dx * dx < allowed_radius) subdivide(subdivide, conf.base_zoom, current_tile_x + dx, current_tile_z + dz);

  // spawn new if not in rendering list
  for (const auto &key : desired_keys) {
    if (!rendering_tiles.contains(key) && !loading_tiles.contains(key)) loading_tiles[key] = spawn(key);
  }
}

void streamer::process_loaded_tiles() {
  // walk loading tiles; for each entry that finished downloading, either promote
  // it to rendering_tiles or drop it (invalid / no longer desired). entries are
  // erased immediately on the iterator. uploads are bounded both by a wall-clock
  // budget (to keep the frame steady on slow GPUs) and a hard cap (to keep heavy
  // single-tile uploads from running away).
  const double frame_start = GetTime();
  int promoted = 0;

  for (auto it = loading_tiles.begin(); it != loading_tiles.end();) {
    auto &[key, tile] = *it;

    // both futures must be ready
    if (tile.tx_future.wait_for(0s) != std::future_status::ready || tile.hm_future.wait_for(0s) != std::future_status::ready) {
      ++it;
      continue;
    }

    // take ownership of the decoded images via RAII; they unload automatically
    // on every exit path below.
    raii::image tex_img{tile.tx_future.get()};
    raii::image height_img{tile.hm_future.get()};

    if (!IsImageValid(*tex_img) || !IsImageValid(*height_img)) {
      TraceLog(LOG_WARNING, "failed to load tile %d/%d/%d - dropping", tile.zoom, tile.x, tile.z);
      it = loading_tiles.erase(it);
      continue;
    }

    if (!desired_keys.contains(key)) {
      TraceLog(LOG_INFO, "tile %d/%d/%d became stale before upload - dropping", tile.zoom, tile.x, tile.z);
      it = loading_tiles.erase(it);
      continue;
    }

    // upload to GPU and move into rendering_tiles. the heightmap CPU image is
    // kept in the loaded_tile for ground_height() queries (raycasts, collision).
    raii::texture texture_tex = raii::load_texture_from_image(*tex_img);
    raii::texture height_tex = raii::load_texture_from_image(*height_img);
    SetTextureWrap(*texture_tex, TEXTURE_WRAP_CLAMP);
    SetTextureWrap(*height_tex, TEXTURE_WRAP_CLAMP);

    // mipmaps + trilinear on the albedo only — far tiles need filtering to avoid
    // moiré shimmer in flight. heightmap is sampled 1:1 in the vertex shader so
    // mipmaps would only blur the displacement.
    GenTextureMipmaps(&texture_tex.get());
    SetTextureFilter(*texture_tex, TEXTURE_FILTER_TRILINEAR);

    rendering_tiles.insert_or_assign(
        key, loaded_tile{tile.x, tile.z, tile.zoom, tile.tx, tile.tz, std::move(texture_tex), std::move(height_tex), std::move(height_img), false});

    it = loading_tiles.erase(it);
    ++promoted;

    if (promoted >= conf.max_uploads_per_frame) break;
    if (GetTime() - frame_start >= conf.upload_budget_sec) break;
  }
}

loading_tile streamer::spawn(const TileKey &tile) {
  const auto scale = 1 << (tile.zoom - conf.base_zoom);
  const auto tx = tile.x + conf.anchor_x_tile * scale;
  const auto tz = tile.z + conf.anchor_z_tile * scale;

  const auto tx_path = std::vformat(conf.texture_cache_path, std::make_format_args(tile.zoom, tx, tz));
  const auto hm_path = std::vformat(conf.heightmap_cache_path, std::make_format_args(tile.zoom, tx, tz));

  const auto tile_size = tile_sizes[tile.zoom - conf.base_zoom];
  const auto tx_url = maps_provider.texture(tile.zoom, tx, tz);
  const auto hm_url = maps_provider.heightmap(tile.zoom, tx, tz);

  auto t = loading_tile{tile.x,
                        tile.z,
                        tile.zoom,
                        (static_cast<float>(tile.x) + 0.5f) * tile_size,
                        (static_cast<float>(tile.z) + 0.5f) * tile_size,
                        tile_downloader.enqueue_and_load(tx_path, tx_url),
                        tile_downloader.enqueue_and_load(hm_path, hm_url)};

  TraceLog(LOG_DEBUG, "spawned tile %d,%d,%d position %f,%f", t.zoom, t.x, t.z, t.tx, t.tz);
  return t;
}

bool streamer::is_tile_covered(const TileKey &key) const {
  const auto contains = [&](const int zoom, const int x, const int z) { return rendering_tiles.contains(TileKey{zoom, x, z}); };

  // check parent
  if (key.zoom > conf.base_zoom) {
    if (contains(key.zoom - 1, key.x >> 1, key.z >> 1)) return true;
  }

  // check children
  if (key.zoom < conf.max_zoom) {
    const int child_x = key.x << 1;
    const int child_z = key.z << 1;
    if (contains(key.zoom + 1, child_x, child_z) && contains(key.zoom + 1, child_x + 1, child_z) && contains(key.zoom + 1, child_x, child_z + 1) &&
        contains(key.zoom + 1, child_x + 1, child_z + 1)) {
      return true;
    }
  }
  return false;
}
}  // namespace raytiles
