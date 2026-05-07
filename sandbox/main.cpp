#include <cstdlib>
#include <format>
#include <stdexcept>
#include <string>
#include <string_view>

#include "../raytiles.h"
#include "rlgl.h"

static std::string required_env(const char *name, std::string_view label) {
  if (const char *value = std::getenv(name); value && *value) {
    return value;
  }
  throw std::runtime_error(std::format("missing {} token in options or environment variables", label));
}

int main() {
  SetTraceLogLevel(LOG_DEBUG);

  InitWindow(800, 600, "raytiles");

  raytiles::config conf;
  conf.anchor_x_tile = 1179.0f;
  conf.anchor_z_tile = 797.0f;

  const raytiles::provider provider(required_env("MAPBOX_TOKEN", "mapbox token"));
  raytiles::streamer streamer(conf, provider);

  Camera3D camera;
  camera.position = Vector3{5000.0f, 3000.0f, 5000.0f};
  camera.target = Vector3{0.0f, 0.0f, 0.0f};
  camera.up = Vector3{0.0f, 1.0f, 0.0f};
  camera.fovy = 45.0f;
  camera.projection = CAMERA_PERSPECTIVE;

  rlSetClipPlanes(1, 100000);

  streamer.set_fog_color(SKYBLUE);

  while (!WindowShouldClose()) {
    streamer.update(camera);
    BeginDrawing();
    ClearBackground(SKYBLUE);

    BeginMode3D(camera);
    streamer.draw(camera);
    EndMode3D();
    // streamer.debug(camera);
    EndDrawing();

    const auto dt = GetFrameTime();
    if (IsKeyDown(KEY_W)) camera.position.z -= 1500.0f * dt;
    if (IsKeyDown(KEY_S)) camera.position.z += 1500.0f * dt;
    if (IsKeyDown(KEY_A)) camera.position.x -= 1500.0f * dt;
    if (IsKeyDown(KEY_D)) camera.position.x += 1500.0f * dt;
  }

  CloseWindow();
  return 0;
}
