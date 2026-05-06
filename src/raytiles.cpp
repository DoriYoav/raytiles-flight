#include "raytiles.h"
#include "tilekey.hpp"
#include <format>
#include <future>
#include <string>
#include <unordered_map>
#include <algorithm>
#include <ranges>
#include <utility>
#include <vector>
#include <raylib.h>
#include <chrono>

using namespace std::chrono_literals;

namespace raytiles {
    namespace {
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
    }

    streamer::streamer(config conf, provider maps_provider) : conf(std::move(conf)),
                                                              maps_provider(std::move(maps_provider)),
                                                              displacement_shader(LoadShader("terrain.vs", "terrain.fs")),
                                                              tile_downloader(4) {
        const auto distance = static_cast<float>(conf.rendering_radius) * conf.base_zoom_tile_size;

        // prepare tiles size and distance for each zoom
        for (int zoom = conf.base_zoom; zoom <= conf.max_zoom; ++zoom) {
            const int ratio = 1 << (zoom - conf.base_zoom);
            tile_sizes[zoom] = conf.base_zoom_tile_size / static_cast<float>(ratio);
            tile_distances[zoom] = distance * distance / static_cast<float>(ratio * ratio * ratio);
        }

        // creating model for each zoom level, avoiding the need to stretch the model
        for (int zoom = conf.base_zoom; zoom <= conf.max_zoom; ++zoom) {
            const int res = 16 * (1 << (zoom - conf.base_zoom));
            const float size = tile_sizes.at(zoom);
            const float skirt_size = zoom == conf.base_zoom ? size * conf.skirt_size * 3.0f : size * conf.skirt_size;

            models[zoom] = LoadModelFromMesh(GenMeshPlane(size + skirt_size, size + skirt_size, res, res));
            models[zoom].materials[0].shader = displacement_shader;
        }

        // cache shader locations
        cam_pos_loc = GetShaderLocation(displacement_shader, "cameraPosition");
        ambient_loc = GetShaderLocation(displacement_shader, "ambientLight");
        fog_color_log = GetShaderLocation(displacement_shader, "fogColor");

        // configure the slot the heightmap data use - MATERIAL_MAP_ROUGHNESS slot
        constexpr int heightmapSlotIndex = MATERIAL_MAP_ROUGHNESS;
        const int shaderLocation = GetShaderLocation(displacement_shader, "heightMap");
        SetShaderValue(displacement_shader, shaderLocation, &heightmapSlotIndex, SHADER_UNIFORM_INT);

        constexpr float heightScale = 5.0;
        const int scaleLoc = GetShaderLocation(displacement_shader, "heightScale");
        SetShaderValue(displacement_shader, scaleLoc, &heightScale, SHADER_UNIFORM_FLOAT);
        TraceLog(LOG_INFO, "raytiles streamer initialized");
    }

    streamer::~streamer() {
        for (const auto &val: models | std::views::values) UnloadModel(val);
        UnloadShader(displacement_shader);
    }

    void streamer::update(const Camera3D &camera) {
        DrawText(TextFormat("position: %d, %d, %d", desired_keys.size(), loading_tiles.size(), rendering_tiles.size()), 10, 10, 20, BLUE);
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
        for (const auto &[key, tile]: rendering_tiles) {
            if (tile.done) continue;

            // SetShaderValue(displacement_shader, cam_pos_loc, &camera.position, SHADER_UNIFORM_VEC3);

            // set the camera location (for distance -> fog)
            SetShaderValue(displacement_shader, cam_pos_loc, &camera.position, SHADER_UNIFORM_VEC3);

            // set the ambient color (weather/day/night/...)
            const float ambient_light[4] = {1.0f, 1.0f, 1.0f, 1.0f};
            SetShaderValue(displacement_shader, ambient_loc, ambient_light, SHADER_UNIFORM_VEC4);

            // set the fog color (to match the sky)
            const float fog_color[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            SetShaderValue(displacement_shader, fog_color_log, &fog_color, SHADER_UNIFORM_VEC4);

            const auto &model = models.at(key.zoom);
            model.materials[0].maps[MATERIAL_MAP_ALBEDO].texture = tile.tx_texture;
            model.materials[0].maps[MATERIAL_MAP_ROUGHNESS].texture = tile.hm_texture;

            DrawModel(model, {tile.tx, 0.0f, tile.tz}, 1.0f, WHITE);

            // const auto size = tile_sizes.at(key.zoom);
            // DrawCubeWires({tile.tx, 0.0f, tile.tz}, size, 10.0f, size, RED);
        }
    }

    void streamer::debug(const Camera3D &camera) {
        for (const auto &[key, tile]: rendering_tiles) {
            if (tile.done) continue;

            const auto [x, y] = GetWorldToScreen({tile.tx, 0.0f, tile.tz}, camera);
            if (x < 0 || x > 800 || y < 0 || y > 600) continue;

            DrawText(TextFormat("%d", key.zoom), static_cast<int>(x), static_cast<int>(y), 15, GREEN);
        }
    }

    void streamer::remove_unused_tiles() {
        std::erase_if(loading_tiles, [&](const auto &item) {
            return item.second.done;
        });

        std::erase_if(loading_tiles, [&](const auto &item) {
            return item.second.done;
        });
    }

    void streamer::process_current_location() {
        std::vector<TileKey> new_desired_keys;
        new_desired_keys.reserve(512);
        desired_keys.clear();
        desired_keys.reserve(512);


        auto subdivide = [&](auto &self, const int zoom, const int tx, const int tz) -> void {
            // no need for calculations, this is the last zoom
            if (zoom == conf.max_zoom) {
                new_desired_keys.push_back({zoom, tx, tz});
                desired_keys.insert({zoom, tx, tz});
                return;
            }

            // calculate distance of the tile from the camera
            const float tile_size = tile_sizes.at(zoom);
            const float world_x = (static_cast<float>(tx) + 0.5f) * tile_size;
            const float world_z = (static_cast<float>(tz) + 0.5f) * tile_size;
            const float ddx = last_position.x - world_x;
            const float ddz = last_position.z - world_z;

            // check against the next zoom distance threshold, if it's far enough,
            // add to the list, otherwise subdivide into 4 children
            if (ddx * ddx + ddz * ddz >= tile_distances.at(zoom + 1)) {
                new_desired_keys.push_back({zoom, tx, tz});
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


        // sort for searching (still cheaper than set? YES, set required the heap and O(n log n) for searching)
        // std::ranges::sort(new_desired_keys);

        // remove from desired those that not in new desired
        // std::erase_if(desired_tiles, [&](const auto &item) {
        //     const auto [key, entity] = item;
        //     return !std::ranges::binary_search(new_desired_keys, key);
        // });

        // spawn new if not in rendering list
        for (const auto &key: desired_keys) {
            if (!rendering_tiles.contains(key) && !loading_tiles.contains(key)) loading_tiles[key] = spawn(key);
        }
    }

    void streamer::process_loaded_tiles() {
        // read all desired tiles and look for those fully loaded
        for (auto &[key, tile]: loading_tiles) {
            if (tile.done) continue;

            // both should be ready
            if (tile.tx_future.wait_for(0s) != std::future_status::ready || tile.hm_future.wait_for(0s) != std::future_status::ready) continue;

            const Image tex_img = tile.tx_future.get();
            const Image height_img = tile.hm_future.get();

            // are the images valid (both!!!)
            if (!IsImageValid(tex_img) || !IsImageValid(height_img)) {
                TraceLog(LOG_WARNING, "failed to load tile %d/%d/%d - the tile will not be display in next frame", tile.zoom, tile.x, tile.z);
                UnloadImage(tex_img);
                UnloadImage(height_img);
                continue;
            }

            // todo are they still needed?
            // if (!desired_tiles.contains(key)) {
            //     TraceLog(LOG_INFO, "tile %d/%d/%d is already stale when loaded - the tile will not be display and resources will be freed", tile.zoom, tile.x,
            //              tile.z);
            //     UnloadImage(tex_img);
            //     UnloadImage(height_img);
            //     continue;
            // }

            // now it safe to do the heavy load...
            const Texture2D texture_tex = LoadTextureFromImage(tex_img);
            const Texture2D height_tex = LoadTextureFromImage(height_img);

            SetTextureWrap(texture_tex, TEXTURE_WRAP_CLAMP);
            SetTextureWrap(height_tex, TEXTURE_WRAP_CLAMP);

            // need no more todo keeping the heightmap for querying the ground height
            UnloadImage(tex_img);
            UnloadImage(height_img);

            tile.done = true; // will be cleaned next frame
            rendering_tiles[key] = loaded_tile{
                tile.x,
                tile.z,
                tile.zoom,
                tile.tx,
                tile.tz,
                texture_tex,
                height_tex,
                false
            };
            break; // only one per frame to avoid spikes, the rest will be processed in next frames
        }
    }

    loading_tile streamer::spawn(const TileKey &tile) {
        TraceLog(LOG_INFO, "spawn tile zoom=%d x=%d z=%d", tile.zoom, tile.x, tile.z);
        const auto scale = 1 << (tile.zoom - conf.base_zoom);
        const auto tx = tile.x + anchor_x_tile * scale;
        const auto tz = tile.z + anchor_z_tile * scale;

        const auto tx_path = std::vformat(conf.texture_cache_path, std::make_format_args(tile.zoom, tx, tz));
        const auto hm_path = std::vformat(conf.heightmap_cache_path, std::make_format_args(tile.zoom, tx, tz));

        const auto tile_size = tile_sizes.at(tile.zoom);
        const auto tx_url = maps_provider.texture(tile.zoom, tx, tz);
        const auto hm_url = maps_provider.heightmap(tile.zoom, tx, tz);

        auto t = loading_tile{
            tile.x,
            tile.z,
            tile.zoom,
            // world position
            (static_cast<float>(tile.x) + 0.5f) * tile_size,
            (static_cast<float>(tile.z) + 0.5f) * tile_size,
            // futures
            tile_downloader.enqueue_and_load(tx_path, tx_url),
            tile_downloader.enqueue_and_load(hm_path, hm_url),
            // done
            false
        };

        TraceLog(LOG_INFO, "tile %d,%d,%d position %f,%f", t.zoom, t.x, t.z, t.tx, t.tz);
        return t;
    }
} // namespace raytiles
