#ifndef RAYTILES_LIBRARY_H
#define RAYTILES_LIBRARY_H
#include <memory>
#include <string>

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
  double upload_budget_sec = 0.002;
  int max_uploads_per_frame = 8;
  int anchor_x_tile = 1223;
  int anchor_z_tile = 828;
  double near_plane = 1;
  double far_plane = 100000;
  bool use_mipmap = true;
  bool allow_insecure_tls = false;

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

class manager;

class streamer {
 public:
  explicit streamer(config conf, provider maps_provider);
  ~streamer();

  streamer(const streamer &) = delete;
  streamer &operator=(const streamer &) = delete;
  streamer(streamer &&) noexcept;
  streamer &operator=(streamer &&) noexcept;

  void update(const Camera3D &camera) const;
  void draw(const Camera3D &camera) const;
  void debug(const Camera3D &camera) const;
  void set_ambient_light(Color color) const;
  void set_fog_color(Color color) const;
  [[nodiscard]] float ground_height(Vector3 position) const;

 private:
  std::unique_ptr<manager> impl;
};

}  // namespace raytiles

#endif  // RAYTILES_LIBRARY_H
