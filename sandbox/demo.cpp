#include <algorithm>
#include <format>
#include <random>
#include <vector>
#include <raytiles/raytiles.h>
#include <rlgl.h>
#include "advanced-fly.hpp"
#include "fly.h"
#include "race.hpp"

static std::string required_env() {
    // The default ArcGIS imagery needs no token; fall back to a dummy so the game
    // runs on a plain double-click (no MAPBOX_TOKEN env var required).
    if (const char *value = std::getenv("MAPBOX_TOKEN"); value && *value) {
        return value;
    }
    return "dummy";
}

struct Ship {
    Model model;
    const char *name;
    Vector3 center;   // bounding-box centre, so we rotate about the visual middle
    float scale;      // uniform scale to a common reference size
    Quaternion fix;   // corrects the model's native orientation to nose +Z, up +Y
};

// Load a ship, auto-centring it and scaling so its largest dimension == target_size.
static Ship load_ship(const char *path, const char *name, float target_size, Quaternion fix) {
    Model m = LoadModel(path);
    const BoundingBox b = GetModelBoundingBox(m);
    const Vector3 center = Vector3Scale(Vector3Add(b.min, b.max), 0.5f);
    const Vector3 d = Vector3Subtract(b.max, b.min);
    const float maxdim = std::fmax(d.x, std::fmax(d.y, d.z));
    const float scale = maxdim > 0.0001f ? target_size / maxdim : 1.0f;
    return Ship{m, name, center, scale, fix};
}

enum class AppMode { Cruise, Competition, Infinite, Target };

static const char *mode_name(AppMode m) {
    switch (m) {
        case AppMode::Cruise: return "Cruise";
        case AppMode::Competition: return "Competition";
        case AppMode::Infinite: return "Infinite";
        case AppMode::Target: return "Target Course";
    }
    return "?";
}

// --- Target mode: laser bolts (Star Wars style) ---------------------------------
// Bolts/blasts live in ABSOLUTE world coords (= camera.position - world_offset), so
// they survive the large-world rebase like the gates. Render at abs + world_offset.
struct Bolt {
    Vector3 pos;   // current absolute position
    Vector3 prev;  // absolute position last frame (segment endpoint for hit test)
    Vector3 dir;   // unit travel direction (frozen at fire time)
    float dist;    // metres travelled so far
};
struct Blast {     // brief expanding explosion, absolute coords
    Vector3 pos;
    float age;     // seconds since spawn
};

static constexpr float BOLT_SPEED = 1500.0f;     // m/s
static constexpr float BOLT_MAX_RANGE = 500.0f;  // m, then despawn
static constexpr float BOLT_LENGTH = 18.0f;      // visual streak length
static constexpr float BOLT_RADIUS = 1.6f;       // visual streak radius
static constexpr float NOSE_OFFSET = 60.0f;      // spawn just ahead of the ship (drawn at +50)
static constexpr float HIT_MARGIN = 4.0f;        // hit forgiveness added to core radius
static constexpr float BLAST_DURATION = 0.35f;   // s

// Closest distance from point p to segment [a,b]; used so a fast bolt can't tunnel
// through the target between frames.
static float dist_point_to_segment(Vector3 p, Vector3 a, Vector3 b) {
    const Vector3 ab = Vector3Subtract(b, a);
    const float len2 = Vector3DotProduct(ab, ab);
    if (len2 < 1e-6f) return Vector3Distance(p, a);
    float t = Vector3DotProduct(Vector3Subtract(p, a), ab) / len2;
    t = std::clamp(t, 0.0f, 1.0f);
    const Vector3 proj = Vector3Add(a, Vector3Scale(ab, t));
    return Vector3Distance(p, proj);
}

int main() {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_HIGHDPI);
    InitWindow(1280, 720, "raytiles");
    InitAudioDevice();

    std::string token = required_env();

    raytiles::world_config world;
    raytiles::streaming_config streaming;
    raytiles::rendering_config rendering;
    raytiles::pool_config pool_conf;

    // pool_conf.texture_url = "https://api.mapbox.com/v4/mapbox.satellite/:zoom:/:x:/:y:.pngraw?access_token=" + token;
    pool_conf.download_threads = 8;
    // rlSetClipPlanes(1.0f, 400000.0f);

    // Mount Everest, Himalaya (27.975332, 86.936904)
    world.anchor_x_tile = 379;
    world.anchor_z_tile = 214;

    // The Grand Canyon
    // world.anchor_x_tile = 97;
    // world.anchor_z_tile = 200;

    // Adjust to fit your scene
    world.base_zoom_tile_size = 64000;
    rendering.skirt_drop = 1000.0f;
    world.skirt_overlap = {1.01f, 1.01f, 1.01f, 1.01f, 1.01f, 1.01f, 1.02f};


    raytiles::streamer streamer(world, streaming, rendering, pool_conf);

    Vector3 world_offset = {0.0f, 0.0f, 0.0f};

    auto absolute_to_user = [&](const Vector3 abs) {
        return Vector3Add(abs, world_offset);
    };

    Camera3D camera;
    camera.position = absolute_to_user({2000.0f, 5000.0f, 2000.0f});
    camera.target = absolute_to_user({3000.0f, 4750.0f, 3000.0f});
    camera.up = Vector3{0.0f, 1.0f, 0.0f};
    camera.fovy = 60.0f;
    camera.projection = CAMERA_PERSPECTIVE;

    free_camera::AdvancedFreeCamera adv_f{};

    // Flyable ships (cycle with C). The TIE sets the reference size; the others are
    // scaled to match it. `fix` rotates each model so its nose points +Z, up is +Y.
    std::vector<Ship> ships;
    {
        Model tie = LoadModel("res/tie/scene.gltf");
        const BoundingBox tb = GetModelBoundingBox(tie);
        const Vector3 tc = Vector3Scale(Vector3Add(tb.min, tb.max), 0.5f);
        const Vector3 td = Vector3Subtract(tb.max, tb.min);
        const float ref = std::fmax(td.x, std::fmax(td.y, td.z));  // reference size = TIE
        ships.push_back(Ship{tie, "TIE Fighter", tc, 1.0f, QuaternionIdentity()});
        if (FileExists("res/x-wing/scene.gltf"))
            ships.push_back(load_ship("res/x-wing/scene.gltf", "X-Wing", ref, QuaternionIdentity()));
        if (FileExists("res/snowspeeder/scene.gltf"))
            ships.push_back(load_ship("res/snowspeeder/scene.gltf", "Snowspeeder", ref, QuaternionIdentity()));
    }
    int ship_idx = 0;

    // --- audio: blaster SFX (a pool of aliases for overlap + per-voice pitch) and a
    // looping speeder engine sound while accelerating ---
    Sound blasterA = LoadSound("assets/sounds/blaster1.wav");
    Sound blasterB = LoadSound("assets/sounds/blaster2.wav");
    std::vector<Sound> shot_voices; // interleaved A,B aliases -> alternates per shot
    for (int i = 0; i < 4; ++i) {
        shot_voices.push_back(LoadSoundAlias(blasterA));
        shot_voices.push_back(LoadSoundAlias(blasterB));
    }
    int shot_voice = 0;
    int shot_combo = 0;          // rising pitch for rapid consecutive shots
    double last_shot_time = -10.0;

    Music speeder = LoadMusicStream("assets/sounds/524958__jadonthevideoguy__speedersound.mp3");
    speeder.looping = true;
    SetMusicVolume(speeder, 0.45f);
    PlayMusicStream(speeder);
    PauseMusicStream(speeder); // ready, silent until accelerating

    Sound explBase = LoadSound("assets/sounds/536548__cascoes__explosion.wav");
    std::vector<Sound> expl_voices; // pool for overlapping distant booms
    for (int i = 0; i < 6; ++i) expl_voices.push_back(LoadSoundAlias(explBase));
    int expl_voice = 0;
    // Play an explosion at an absolute impact point, attenuated by distance to the camera.
    auto play_explosion = [&](Vector3 impact_abs) {
        const Vector3 u = Vector3Add(impact_abs, world_offset);
        const float d = Vector3Distance(camera.position, u);
        const float vol = std::clamp(1.0f - d / 900.0f, 0.0f, 1.0f);
        if (vol <= 0.02f || expl_voices.empty()) return;
        Sound &v = expl_voices[expl_voice % static_cast<int>(expl_voices.size())];
        ++expl_voice;
        SetSoundVolume(v, vol * 0.8f);
        PlaySound(v);
    };

    streamer.set_fog_color(SKYBLUE);
    streamer.set_ambient_light(Color{200, 200, 200, 255});
    float sun = 1.0f;
    bool wireframe = false;
    bool labels = false;
    bool crashed = false;

    race::Race race;
    AppMode app_mode = AppMode::Cruise; // start in free flight
    std::mt19937 race_rng{std::random_device{}()};
    Vector3 race_spawn = Vector3{2000.0f, 5000.0f, 2000.0f}; // absolute; set when a run begins
    // Ship's absolute position last frame (= camera.position - world_offset), for
    // rebase-proof gate-crossing detection.
    Vector3 prev_abs = Vector3Subtract(camera.position, world_offset);

    std::vector<Bolt> bolts;   // Target mode (absolute coords)
    std::vector<Blast> blasts;

    // Begin a run for the given mode from a random spot near the map origin. Course modes
    // spawn 1000 m above the terrain under the first gate; cruise spawns above the start.
    // NOTE: terrain queries run BEFORE we zero world_offset, to stay consistent with the
    // streamer's cached offset.
    auto begin_run = [&](AppMode m) {
        std::uniform_real_distribution<float> off(-5000.0f, 5000.0f);
        const float sx = 2000.0f + off(race_rng);
        const float sz = 2000.0f + off(race_rng);
        float spawn_y;
        if (m == AppMode::Cruise) {
            race.stop();
            const float ground_start = streamer.ground_height(absolute_to_user(Vector3{sx, 0.0f, sz})).value_or(4000.0f);
            spawn_y = ground_start + 1000.0f;
        } else {
            // Ship faces +Z after reset(), so the course (and gate 1) runs straight ahead.
            race::Mode rm = race::Mode::Infinite;
            if (m == AppMode::Competition) rm = race::Mode::Competition;
            else if (m == AppMode::Target) rm = race::Mode::Target;
            race.start(Vector3{sx, 0.0f, sz}, PI / 2.0f, rm);
            const auto &g0 = race.gates().front();
            const float ground_first = streamer.ground_height(absolute_to_user(Vector3{g0.x, 0.0f, g0.z})).value_or(1500.0f);
            const float ground_start = streamer.ground_height(absolute_to_user(Vector3{sx, 0.0f, sz})).value_or(ground_first);
            spawn_y = std::fmax(ground_first + 1000.0f, ground_start + 300.0f);
        }
        adv_f.reset();
        world_offset = {0.0f, 0.0f, 0.0f};
        camera.position = Vector3{sx, spawn_y, sz};
        camera.target = Vector3{sx, spawn_y, sz + 2.0f};
        crashed = false;
        race_spawn = camera.position;
        prev_abs = camera.position; // world_offset is now zero
        bolts.clear();
        blasts.clear();
    };

    // Respawn at the current race's start (used on crash-restart of the same course).
    auto respawn_race = [&]() {
        adv_f.reset();
        world_offset = {0.0f, 0.0f, 0.0f};
        camera.position = race_spawn;
        camera.target = Vector3Add(race_spawn, Vector3{0.0f, 0.0f, 2.0f});
        crashed = false;
        race.restart();
        prev_abs = camera.position;
        bolts.clear();
        blasts.clear();
    };

    // loading loop
    for (;;) {
        streamer.update(camera, world_offset);
        if (!streamer.is_loading()) break;

        const auto loading = streamer.get_loading();
        BeginDrawing();
        ClearBackground(BLACK);
        DrawText(TextFormat("Loading... %.1f%%", loading * 100.0f), 350, 350, 50, WHITE);
        EndDrawing();
    }

    // Don't spawn inside a mountain: lift the start to >= 1000 m above local terrain
    // (matters for high anchors like Everest). world_offset is still {0,0,0} here.
    if (auto g = streamer.ground_height(camera.position)) {
        camera.position.y = std::fmax(camera.position.y, *g + 1000.0f);
        camera.target.y = camera.position.y;
    }

    // Make sure we'll see the horizon
    rlSetClipPlanes(streaming.near_plane, streaming.far_plane);

    while (!WindowShouldClose()) {
        constexpr float rebase_threshold = 4096.0f;
        const auto dt = GetFrameTime();

        // Speeder engine sound while pressing D (accelerate).
        UpdateMusicStream(speeder);
        if (IsKeyDown(KEY_D)) {
            if (!IsMusicStreamPlaying(speeder)) ResumeMusicStream(speeder);
        } else if (IsMusicStreamPlaying(speeder)) {
            PauseMusicStream(speeder);
        }

        if (!crashed) adv_f.update(camera, dt);

        // Large-world rebase: keep the user-space camera close to the origin.
        // Whenever |camera.x| or |camera.z| exceeds the threshold, slide BOTH
        // the camera (position + target) AND the world_offset by the same
        // amount, preserving the absolute camera location (user - offset).
        // In a real game you must apply the same shift to every entity that
        // lives in user space (models, lights, particles, ...).
        if (camera.position.x > rebase_threshold) {
            camera.position.x -= rebase_threshold;
            camera.target.x -= rebase_threshold;
            world_offset.x -= rebase_threshold;
        } else if (camera.position.x < -rebase_threshold) {
            camera.position.x += rebase_threshold;
            camera.target.x += rebase_threshold;
            world_offset.x += rebase_threshold;
        }
        if (camera.position.z > rebase_threshold) {
            camera.position.z -= rebase_threshold;
            camera.target.z -= rebase_threshold;
            world_offset.z -= rebase_threshold;
        } else if (camera.position.z < -rebase_threshold) {
            camera.position.z += rebase_threshold;
            camera.target.z += rebase_threshold;
            world_offset.z += rebase_threshold;
        }

        // Frame inputs are now stable for this frame -> hand them to the
        // streamer once. draw() and ground_height() will reuse these values.
        streamer.update(camera, world_offset);

        //r.set_sun_direction(Vector3{0.1f, sun, 0.0f});

        // Resolve gate altitudes (height above terrain) as tiles stream in.
        race.resolve_heights([&](float ax, float az) {
            return streamer.ground_height(absolute_to_user(Vector3{ax, 0.0f, az}));
        });

        const auto h = streamer.ground_height(camera.position).value_or(0.0f);
        if (h > camera.position.y) {
            const Vector3 ship_abs = Vector3Subtract(camera.position, world_offset);
            if (race.state() == race::State::Racing) {
                play_explosion(ship_abs);
                respawn_race(); // crash mid-race -> restart the same course at its start
            } else if (!crashed) {
                play_explosion(ship_abs); // boom once on the crash transition
                crashed = true;
            }
        }

        // Advance race timer / detect gate passes (absolute space, rebase-proof).
        const Vector3 cur_abs = Vector3Subtract(camera.position, world_offset);
        race.update(dt, prev_abs, cur_abs);
        prev_abs = cur_abs;

        // --- Laser bolts (ALL modes): fire, advance, hit ground / targets ----------
        // Bolts only affect targets (Target mode) and the ground (every mode).
        {
            const Vector3 fwd = Vector3RotateByQuaternion(Vector3{0.0f, 0.0f, 1.0f}, adv_f.orientation());

            if (IsKeyPressed(KEY_S)) {
                const Vector3 nose = Vector3Add(cur_abs, Vector3Scale(fwd, NOSE_OFFSET));
                bolts.push_back(Bolt{nose, nose, fwd, 0.0f});
                // Blaster SFX: alternate the two clips, rising pitch on rapid consecutive shots.
                const double now = GetTime();
                shot_combo = (now - last_shot_time < 0.6) ? std::min(shot_combo + 1, 3) : 0;
                last_shot_time = now;
                if (!shot_voices.empty()) {
                    Sound &v = shot_voices[shot_voice % static_cast<int>(shot_voices.size())];
                    ++shot_voice;
                    SetSoundPitch(v, 1.0f + 0.12f * shot_combo); // 1.0, 1.12, 1.24, 1.36
                    SetSoundVolume(v, 0.55f);
                    PlaySound(v);
                }
            }

            for (auto &b : bolts) {
                b.prev = b.pos;
                b.pos = Vector3Add(b.pos, Vector3Scale(b.dir, BOLT_SPEED * dt));
                b.dist += BOLT_SPEED * dt;
            }

            // Target mode: shoot the active target (its core is also a solid obstacle).
            if (app_mode == AppMode::Target && race.state() == race::State::Racing) {
                const int ai = race.gate_index();
                if (ai < race.gate_count() && race.gates()[ai].y.has_value()) {
                    const auto &g = race.gates()[ai];
                    const Vector3 target_c{g.x, *g.y, g.z};
                    const float hit_r = race::Race::CORE_RADIUS + HIT_MARGIN;
                    for (auto &b : bolts) {
                        if (b.dist > BOLT_MAX_RANGE + BOLT_LENGTH) continue;
                        if (dist_point_to_segment(target_c, b.prev, b.pos) <= hit_r) {
                            blasts.push_back(Blast{target_c, 0.0f});
                            play_explosion(target_c);
                            race.destroy_active();
                            b.dist = BOLT_MAX_RANGE + 1e6f;
                            break;
                        }
                    }
                    if (Vector3Distance(cur_abs, target_c) <= race::Race::CORE_RADIUS) {
                        play_explosion(cur_abs); // boom on flying into the target core
                        respawn_race();          // clears bolts/blasts
                    }
                }
            }

            // Ground impact (all modes): explosion where a bolt meets the terrain.
            for (auto &b : bolts) {
                if (b.dist > BOLT_MAX_RANGE) continue;
                if (auto g = streamer.ground_height(Vector3Add(b.pos, world_offset)); g && b.pos.y <= *g) {
                    const Vector3 impact{b.pos.x, *g, b.pos.z};
                    blasts.push_back(Blast{impact, 0.0f});
                    play_explosion(impact);
                    b.dist = BOLT_MAX_RANGE + 1e6f;
                }
            }

            bolts.erase(std::remove_if(bolts.begin(), bolts.end(),
                        [](const Bolt &b) { return b.dist > BOLT_MAX_RANGE; }), bolts.end());
            for (auto &x : blasts) x.age += dt;
            blasts.erase(std::remove_if(blasts.begin(), blasts.end(),
                         [](const Blast &x) { return x.age > BLAST_DURATION; }), blasts.end());
        }

        BeginDrawing();
        ClearBackground(SKYBLUE);

        BeginMode3D(camera);
        // draw the world around the camera
        streamer.draw();
        // race checkpoints (anchored in absolute world coords)
        race.draw(world_offset);

        // Laser bolts + explosions (all modes; abs -> user via +world_offset).
        {
            const bool is_tie = ships[ship_idx].name && ships[ship_idx].name[0] == 'T';
            const Color core_col = is_tie ? Color{120, 255, 120, 255} : Color{255, 70, 60, 255};
            const Color glow_col = is_tie ? Fade(GREEN, 0.35f) : Fade(RED, 0.35f);
            for (const auto &b : bolts) {
                const Vector3 head = Vector3Add(b.pos, world_offset);
                const Vector3 tail = Vector3Subtract(head, Vector3Scale(b.dir, BOLT_LENGTH));
                DrawCylinderEx(tail, head, BOLT_RADIUS * 2.2f, BOLT_RADIUS * 2.2f, 6, glow_col);
                DrawCylinderEx(tail, head, BOLT_RADIUS, BOLT_RADIUS, 6, core_col);
                DrawCapsule(tail, head, BOLT_RADIUS * 0.6f, 6, 2, WHITE);
            }
            for (const auto &x : blasts) {
                const float k = x.age / BLAST_DURATION;
                const float r = race::Race::CORE_RADIUS * (1.0f + 2.0f * k);
                DrawSphere(Vector3Add(x.pos, world_offset), r, Fade(ORANGE, 0.7f * (1.0f - k)));
            }
        }

        // --- decorative TIE: orient to flight + subtle lean into maneuvers ---
        const Quaternion ship_orient = adv_f.orientation();      // local +Z -> flight forward
        const free_camera::CameraInputs in = adv_f.inputs();     // already eased, [-1,1]

        constexpr float MAX_ROLL   = 30.0f;   // bank (most expressive)
        constexpr float MAX_PITCH  = 22.0f;   // nose lift/drop
        constexpr float MAX_YAW    = 8.0f;    // nose slew
        constexpr float COORD_TURN = 10.0f;   // extra bank while yawing (coordinated turn)

        const float pitchRad = (-in.pitch * MAX_PITCH) * DEG2RAD;                     // local X
        const float yawRad   = (in.yaw    * MAX_YAW)   * DEG2RAD;                     // local Y
        const float rollRad  = (in.roll * MAX_ROLL - in.yaw * COORD_TURN) * DEG2RAD;  // local Z

        const Quaternion lean = QuaternionFromEuler(pitchRad, yawRad, rollRad);
        const Quaternion q    = QuaternionMultiply(ship_orient, lean);  // lean in LOCAL frame

        const Vector3 forward   = Vector3RotateByQuaternion({0.0f, 0.0f, 1.0f}, ship_orient);
        const Vector3 model_pos = Vector3Add(camera.position, Vector3Scale(forward, 50.0f));

        // Compose: recentre -> native-orientation fix -> scale -> flight+lean -> place.
        Ship &ship = ships[ship_idx];
        ship.model.transform = MatrixMultiply(MatrixMultiply(MatrixMultiply(MatrixMultiply(
            MatrixTranslate(-ship.center.x, -ship.center.y, -ship.center.z),
            QuaternionToMatrix(ship.fix)),
            MatrixScale(ship.scale, ship.scale, ship.scale)),
            QuaternionToMatrix(q)),
            MatrixTranslate(model_pos.x, model_pos.y, model_pos.z));
        DrawModel(ship.model, Vector3{0.0f, 0.0f, 0.0f}, 1.0f, WHITE);
        if (wireframe) {
            streamer.draw_debug_3d();
        }
        EndMode3D();

        if (labels) {
            DrawRectangle(5, 5, 400, 100, Fade(BLACK, 0.5f));
            streamer.draw_debug_labels();
        }

        DrawRectangle(5, 550, 600, 40, Fade(BLACK, 0.5f));
        DrawText("Controls:  A/D speed,  S fire,  M mode,  G restart,  C ship,  P fullscreen,  K labels,  L wireframe", 10, 560, 10, WHITE);

        // speed bar
        {
            const float spd = adv_f.speed();
            const float maxspd = adv_f.top_speed();
            const float frac = std::clamp(maxspd > 0.0f ? spd / maxspd : 0.0f, 0.0f, 1.0f);
            const int bx = 10, by = 515, bw = 220, bh = 24;
            DrawRectangle(bx, by, bw, bh, Fade(BLACK, 0.5f));
            DrawRectangle(bx + 2, by + 2, static_cast<int>((bw - 4) * frac), bh - 4,
                          frac > 0.75f ? RED : (frac > 0.4f ? ORANGE : LIME));
            DrawText(TextFormat("SPEED  %d / %d m/s", static_cast<int>(spd), static_cast<int>(maxspd)),
                     bx + 8, by + 5, 14, WHITE);
        }

        DrawRectangle(5, 10, 280, 110, Fade(BLACK, 0.5f));
        DrawText(TextFormat("user P %d %d %d",
                            static_cast<int>(camera.position.x),
                            static_cast<int>(camera.position.y),
                            static_cast<int>(camera.position.z)
                 ), 10, 20, 20, WHITE);
        DrawText(TextFormat("offset %d %d %d",
                            static_cast<int>(world_offset.x),
                            static_cast<int>(world_offset.y),
                            static_cast<int>(world_offset.z)
                 ), 10, 50, 20, WHITE);
        DrawText(TextFormat("ship: %s", ships[ship_idx].name), 10, 85, 20, YELLOW);

        // --- mode + race HUD ---
        DrawText(TextFormat("MODE: %s    (M switch, G restart)", mode_name(app_mode)),
                 GetScreenWidth() / 2 - 200, 10, 20, WHITE);

        if (app_mode != AppMode::Cruise && race.state() != race::State::Idle) {
            const float t = race.elapsed();
            const int mm = static_cast<int>(t / 60.0f);
            const int ss = static_cast<int>(t) % 60;
            const int cs = static_cast<int>((t - static_cast<int>(t)) * 100.0f);
            const int rx = GetScreenWidth() - 260;
            DrawRectangle(rx - 10, 38, 260, 130, Fade(BLACK, 0.5f));
            DrawText(TextFormat("TIME  %02d:%02d.%02d", mm, ss, cs), rx, 48, 24, WHITE);
            if (app_mode == AppMode::Competition) {
                DrawText(TextFormat("GATE  %d / %d", race.gate_index(), race.gate_count()), rx, 78, 20, YELLOW);
            } else if (app_mode == AppMode::Target) {
                DrawText(TextFormat("TARGET  %d / %d", race.gate_index(), race.gate_count()), rx, 78, 20, RED);
            } else {
                DrawText(TextFormat("GATES  %d", race.gate_index()), rx, 78, 20, YELLOW);
            }
            if (race.gate_index() < race.gate_count()) {
                const auto &g = race.gates()[race.gate_index()];
                if (g.y.has_value()) {
                    const Vector3 c = Vector3Add(Vector3{g.x, *g.y, g.z}, world_offset);
                    DrawText(TextFormat("NEXT  %d m", static_cast<int>(Vector3Distance(camera.position, c))), rx, 103, 20, SKYBLUE);
                } else {
                    DrawText("NEXT  loading...", rx, 103, 20, GRAY);
                }
            }
            if (app_mode == AppMode::Competition || app_mode == AppMode::Target) {
                if (auto b = race.best_time()) {
                    DrawText(TextFormat("BEST  %02d:%02d.%02d", static_cast<int>(*b / 60.0f),
                                        static_cast<int>(*b) % 60,
                                        static_cast<int>((*b - static_cast<int>(*b)) * 100.0f)),
                             rx, 128, 20, GREEN);
                }
            }
        }
        // Target mode: aiming crosshair + fire hint.
        if (app_mode == AppMode::Target && race.state() == race::State::Racing) {
            const int cx = GetScreenWidth() / 2, cy = GetScreenHeight() / 2;
            DrawLine(cx - 14, cy, cx - 5, cy, Fade(RED, 0.9f));
            DrawLine(cx + 5, cy, cx + 14, cy, Fade(RED, 0.9f));
            DrawLine(cx, cy - 14, cx, cy - 5, Fade(RED, 0.9f));
            DrawLine(cx, cy + 5, cx, cy + 14, Fade(RED, 0.9f));
            DrawCircleLines(cx, cy, 18, Fade(RED, 0.6f));
            DrawText("S = fire", cx - 32, cy + 26, 18, Fade(RED, 0.9f));
        }
        if ((app_mode == AppMode::Competition || app_mode == AppMode::Target) &&
            race.state() == race::State::Finished) {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.4f));
            DrawText("FINISHED!", GetScreenWidth() / 2 - 110, GetScreenHeight() / 2 - 40, 50, GOLD);
            DrawText("Press G to race again", GetScreenWidth() / 2 - 130, GetScreenHeight() / 2 + 20, 24, WHITE);
        }

        if (crashed) {
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Fade(BLACK, 0.5f));
            DrawText("You crashed! Press R to reset.", 200, 300, 30, WHITE);
            if (IsKeyPressed(KEY_R)) {
                // Query terrain BEFORE zeroing world_offset (keeps the streamer's cache consistent).
                const float gnd = streamer.ground_height(absolute_to_user(Vector3{2000.0f, 0.0f, 2000.0f})).value_or(4000.0f);
                world_offset = {0.0f, 0.0f, 0.0f};
                const float spawn_y = std::fmax(5000.0f, gnd + 1000.0f);
                camera.position = Vector3{2000.0f, spawn_y, 2000.0f};
                camera.target = Vector3{3000.0f, spawn_y - 250.0f, 3000.0f};
                crashed = false;
            }
        }
        EndDrawing();

        if (IsKeyDown(KEY_LEFT_BRACKET)) sun -= dt * 0.5f;
        if (IsKeyDown(KEY_RIGHT_BRACKET)) sun += dt * 0.5f;
        sun = std::clamp(sun, -1.0f, 1.0f);

        if (IsKeyPressed(KEY_L)) wireframe = !wireframe;
        if (IsKeyPressed(KEY_K)) labels = !labels;
        if (IsKeyPressed(KEY_P)) ToggleBorderlessWindowed();
        if (IsKeyPressed(KEY_C)) ship_idx = (ship_idx + 1) % static_cast<int>(ships.size());
        if (IsKeyPressed(KEY_M)) {
            app_mode = static_cast<AppMode>((static_cast<int>(app_mode) + 1) % 4);
            begin_run(app_mode);
        }
        if (IsKeyPressed(KEY_G)) begin_run(app_mode);
    }

    for (auto &v : shot_voices) UnloadSoundAlias(v);
    for (auto &v : expl_voices) UnloadSoundAlias(v);
    UnloadSound(blasterA);
    UnloadSound(blasterB);
    UnloadSound(explBase);
    UnloadMusicStream(speeder);
    CloseAudioDevice();
    CloseWindow();
    return 0;
}
