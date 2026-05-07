#ifndef RAYTILES_LIBRARY_H
#define RAYTILES_LIBRARY_H
#include <string>
#include <unordered_map>
#include <vector>

#include "../src/downloader.hpp"
#include "../src/raii.hpp"
#include "../src/tilekey.hpp"
#include "raylib.h"
#include "raymath.h"

namespace raytiles {
struct config {
  int base_zoom = 11;
  int max_zoom = 14;
  float base_zoom_tile_size = 16600.0f;
  int rendering_radius = 7;
  float skirt_size = 0.05f;
  float update_distance = 1000.0f * 1000.0f;
  float update_height = 500.0f;
  int anchor_x_tile = 1223;
  int anchor_z_tile = 828;

  std::string texture_cache_path = "assets/tiles/texture/{}/{}/{}.png";
  std::string heightmap_cache_path = "assets/tiles/heightmap/{}/{}/{}.png";
};

class provider {
  std::string token;

 public:
  explicit provider(std::string token);

  std::string texture(int zoom, int x, int z);

  std::string heightmap(int zoom, int x, int z);
};

struct loading_tile {
  int x;
  int z;
  int zoom;
  float tx;
  float tz;
  std::shared_future<Image> tx_future;
  std::shared_future<Image> hm_future;
};

struct loaded_tile {
  int x;
  int z;
  int zoom;
  float tx;
  float tz;
  raii::texture tx_texture;
  raii::texture hm_texture;
  raii::image hm_image;
  bool done;
};

class streamer {
  config conf;
  provider maps_provider;
  raii::shader displacement_shader;
  pool tile_downloader;

  int cam_pos_loc = -1;
  int ambient_loc = -1;
  int fog_color_log = -1;

  float ambient_light[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  float fog_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};

  std::vector<raii::model> models = {};
  std::vector<float> tile_sizes = {};
  std::vector<float> tile_distances = {};

  Vector3 last_position = {-9999.9f, -9999.9f, -9999.9f};

  std::unordered_set<TileKey> desired_keys;
  std::unordered_map<TileKey, loading_tile> loading_tiles;
  std::unordered_map<TileKey, loaded_tile> rendering_tiles;

 public:
  explicit streamer(config conf, provider maps_provider);

  ~streamer() = default;

  void update(const Camera3D &camera);

  void draw(const Camera3D &camera);

  void debug(const Camera3D &camera);

  void set_ambient_light(Color color);

  void set_fog_color(Color color);

  [[nodiscard]] float ground_height(Vector3 position) const;

 private:
  void process_loaded_tiles();

  void process_current_location();

  void remove_unused_tiles();

  loading_tile spawn(const TileKey &tile);

  [[nodiscard]] bool is_tile_covered(const TileKey &key) const;
};
}  // namespace raytiles

#endif  // RAYTILES_LIBRARY_H
