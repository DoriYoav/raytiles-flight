/// @file providers.hpp
/// Concepts that describe a tile provider. A provider is any value type that
/// can answer:
///   - which host should HTTP requests go to (`get_host`)
///   - what URL path serves a given tile, per kind
///     (`get_texture_url`, `get_heightmap_url`)
///   - what is the deepest zoom served per kind
///     (`get_texture_max_zoom`, `get_heightmap_max_zoom`)
///
/// The API is split per-kind on purpose: callers always know which kind they
/// want (the manager always requests both for every tile), so a runtime
/// `tile_kind` tag would only push a redundant switch into every implementor.
///
/// The pool is templated on `IsProvider`, so a misnamed or wrong-signature
/// method surfaces as a `static_assert` at the provider's own definition site
/// instead of as a template error inside the pool.
#pragma once

#include <concepts>
#include <string>

namespace raytiles::providers {

template <typename T>
concept HasGetHost = requires(const T t) {
  { t.get_host() } -> std::convertible_to<std::string>;
};

template <typename T>
concept HasGetTextureUrl = requires(const T t, int zoom, int x, int z) {
  { t.get_texture_url(zoom, x, z) } -> std::convertible_to<std::string>;
};

template <typename T>
concept HasGetHeightmapUrl = requires(const T t, int zoom, int x, int z) {
  { t.get_heightmap_url(zoom, x, z) } -> std::convertible_to<std::string>;
};

template <typename T>
concept HasGetTextureMaxZoom = requires(const T t) {
  { t.get_texture_max_zoom() } -> std::convertible_to<int>;
};

template <typename T>
concept HasGetHeightmapMaxZoom = requires(const T t) {
  { t.get_heightmap_max_zoom() } -> std::convertible_to<int>;
};

template <typename T>
concept IsProvider = HasGetHost<T> && HasGetTextureUrl<T> && HasGetHeightmapUrl<T> && HasGetTextureMaxZoom<T> && HasGetHeightmapMaxZoom<T>;

}  // namespace raytiles::providers
