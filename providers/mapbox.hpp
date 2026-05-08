/// @file mapbox.hpp
/// Reference `IsProvider` implementation backed by the Mapbox tile API.
/// Holds the API token and builds URL paths for satellite imagery and
/// terrain-rgb heightmaps.
#pragma once

#include <format>
#include <string>
#include <utility>

#include "providers.hpp"

namespace raytiles::providers {

class mapbox {
  std::string token;

 public:
  /// @param token Mapbox API token. Must be non-empty; not validated here.
  explicit mapbox(std::string token) : token(std::move(token)) {}

  [[nodiscard]] std::string get_host() const { return "https://api.mapbox.com"; }

  [[nodiscard]] std::string get_texture_url(const int zoom, const int x, const int z) const {
    return std::format("/v4/mapbox.satellite/{}/{}/{}.png?access_token={}", zoom, x, z, token);
  }

  [[nodiscard]] std::string get_heightmap_url(const int zoom, const int x, const int z) const {
    return std::format("/v4/mapbox.terrain-rgb/{}/{}/{}.pngraw?access_token={}", zoom, x, z, token);
  }

  /// Documented Mapbox limits: satellite goes up to z22, terrain-rgb caps at z15.
  /// The streamer's `config::max_zoom` must not exceed either of these.
  [[nodiscard]] int get_texture_max_zoom() const { return 22; }
  [[nodiscard]] int get_heightmap_max_zoom() const { return 15; }
};

static_assert(IsProvider<mapbox>, "raytiles::providers::mapbox must satisfy IsProvider");

}  // namespace raytiles::providers
