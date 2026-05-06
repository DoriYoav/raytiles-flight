#include "raytiles.h"
#include "rlgl.h"

int main() {
    InitWindow(800, 600, "raytiles");
    const raytiles::config conf;
    const raytiles::provider provider("");
    raytiles::streamer streamer(conf, provider);

    Camera3D camera;
    camera.position = Vector3{5000.0f, 3000.0f, 5000.0f};
    camera.target = Vector3{0.0f, 0.0f, 0.0f};
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 45.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    rlSetClipPlanes(1, 100000);

    while (!WindowShouldClose()) {
        streamer.update(camera);
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawText("Hello, raytiles!", 190, 200, 20, LIGHTGRAY);

        BeginMode3D(camera);
        streamer.draw(camera);
        DrawGrid(100, 10.0f);
        EndMode3D();
        streamer.debug(camera);
        EndDrawing();


        const auto dt = GetFrameTime();
        if (IsKeyDown(KEY_W)) camera.position.z -= 100.0f * dt;
        if (IsKeyDown(KEY_S)) camera.position.z += 100.0f * dt;
        if (IsKeyDown(KEY_A)) camera.position.x -= 100.0f * dt;
        if (IsKeyDown(KEY_D)) camera.position.x += 100.0f * dt;
    }

    CloseWindow();
    return 0;
}
