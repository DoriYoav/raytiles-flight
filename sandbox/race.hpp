#pragma once

// Timed racing course for the raytiles flight demo.
//
// Fly through a chain of ring checkpoints, in order, against the clock. The course
// is randomly generated each race but with spacing rules (bounded leg distance and
// turn angle) so it stays flyable. Gates are placed a fixed height ABOVE the real
// terrain (resolved lazily from streamer.ground_height as tiles stream in).
//
// Coordinate convention: gates are stored in ABSOLUTE world coordinates. Detection
// runs in absolute space (caller passes the ship's absolute position = user - world_offset),
// which is invariant under the demo's large-world rebase. Drawing converts to user
// space via (absolute + world_offset).

#include "raylib.h"
#include "raymath.h"
#include <cmath>
#include <optional>
#include <random>
#include <vector>

namespace race {

// Map a unit vector `from` onto `to`, expressed as an axis + angle in DEGREES
// (for DrawCircle3D's rlRotatef). Returns false only for the antiparallel
// degenerate case where the caller should skip the optional accent ring.
inline bool axis_angle_between(Vector3 from, Vector3 to, Vector3 &axis, float &angle_deg) {
    from = Vector3Normalize(from);
    to = Vector3Normalize(to);
    const float d = Vector3DotProduct(from, to);
    if (d > 0.9999f) { axis = {1.0f, 0.0f, 0.0f}; angle_deg = 0.0f; return true; }
    if (d < -0.9999f) { axis = Vector3Normalize(Vector3Perpendicular(from)); angle_deg = 180.0f; return true; }
    axis = Vector3Normalize(Vector3CrossProduct(from, to));
    angle_deg = acosf(d) * RAD2DEG;
    return true;
}

// Draw a ring "hoop" centred at `center`, with its plane perpendicular to `n`
// (so you fly through it along n). The tube primitive auto-orients its rings
// perpendicular to (end-start), so it is degeneracy-free.
inline void draw_ring(Vector3 center, Vector3 n, float radius, Color color) {
    n = Vector3Normalize(n);
    const float half_len = radius * 0.18f;
    const Vector3 a = Vector3Subtract(center, Vector3Scale(n, half_len));
    const Vector3 b = Vector3Add(center, Vector3Scale(n, half_len));
    DrawCylinderWiresEx(a, b, radius, radius, 24, color);

    Vector3 axis;
    float angle_deg;
    if (axis_angle_between({0.0f, 0.0f, 1.0f}, n, axis, angle_deg)) {
        DrawCircle3D(center, radius * 0.72f, axis, angle_deg, color);
    }
}

// True on the frame the segment prev->cur crosses the gate plane within `radius`.
inline bool passed_gate(Vector3 prev, Vector3 cur, Vector3 center, Vector3 n, float radius) {
    n = Vector3Normalize(n);
    const float d_prev = Vector3DotProduct(Vector3Subtract(prev, center), n);
    const float d_cur = Vector3DotProduct(Vector3Subtract(cur, center), n);
    if ((d_prev > 0.0f) == (d_cur > 0.0f)) return false; // no plane crossing this frame
    const float denom = d_prev - d_cur;
    if (denom == 0.0f) return false;
    const float t = d_prev / denom; // in [0,1]
    const Vector3 hit = Vector3Add(prev, Vector3Scale(Vector3Subtract(cur, prev), t));
    return Vector3Length(Vector3Subtract(hit, center)) <= radius;
}

struct Gate {
    float x, z;              // absolute horizontal position
    float agl;               // this gate's target height above terrain
    std::optional<float> y;  // absolute altitude once resolved (ground + agl)
    Vector3 dir;             // ring normal = normalized XZ direction prev -> this gate
};

enum class State { Idle, Racing, Finished };
enum class Mode { Competition, Infinite, Target };

class Race {
public:
    // Expert tuning (all tunable).
    static constexpr int N = 7;                 // number of gates
    static constexpr float MIN_GAP = 700.0f;    // metres between consecutive gates
    static constexpr float MAX_GAP = 1400.0f;
    static constexpr float MAX_TURN = 80.0f * DEG2RAD; // max heading change per leg
    static constexpr float GATE_RADIUS = 40.0f; // ring radius (tight = expert)
    static constexpr float CORE_RADIUS = 30.0f; // Target mode: solid + hittable filled core
    static constexpr float AGL_MIN = 100.0f;    // gate height above terrain (randomised per gate)
    static constexpr float AGL_MAX = 300.0f;
    static constexpr float PROVISIONAL_Y = 4500.0f; // shown until terrain height resolves

    [[nodiscard]] State state() const { return state_; }
    [[nodiscard]] float elapsed() const { return elapsed_; }
    [[nodiscard]] int gate_index() const { return active_; }
    [[nodiscard]] int gate_count() const { return static_cast<int>(gates_.size()); }
    [[nodiscard]] std::optional<float> best_time() const { return best_; }
    [[nodiscard]] const std::vector<Gate> &gates() const { return gates_; }

    [[nodiscard]] Mode mode() const { return mode_; }

    // Generate a fresh random course from `start_abs` along `heading_rad`; begin racing.
    void start(Vector3 start_abs, float heading_rad, Mode mode) {
        rng_.seed(std::random_device{}());
        gates_.clear();
        mode_ = mode;
        gen_heading_ = heading_rad;
        gen_x_ = start_abs.x;
        gen_z_ = start_abs.z;
        for (int i = 0; i < N; ++i) append_gate(); // append_gate keeps each gate facing the next
        active_ = 0;
        elapsed_ = 0.0f;
        state_ = State::Racing;
    }

    // Re-run the SAME course (keep gates + resolved heights): reset progress and timer.
    void restart() {
        active_ = 0;
        elapsed_ = 0.0f;
        if (!gates_.empty()) state_ = State::Racing;
    }

    // Leave any course (cruise / free flight).
    void stop() {
        gates_.clear();
        active_ = 0;
        elapsed_ = 0.0f;
        state_ = State::Idle;
    }

    // Target mode: the active target was shot down. Advance; finish on the last one.
    void destroy_active() {
        if (state_ != State::Racing) return;
        if (active_ >= static_cast<int>(gates_.size())) return;
        ++active_;
        if (active_ >= static_cast<int>(gates_.size())) finish();
    }

    // Resolve altitude for any unresolved gate. `ground_at(absX, absZ)` returns the
    // absolute terrain Y under that point, or nullopt if its tile isn't loaded yet.
    template <class GroundFn>
    void resolve_heights(GroundFn ground_at) {
        for (auto &g : gates_) {
            if (g.y.has_value()) continue;
            if (auto h = ground_at(g.x, g.z)) g.y = *h + g.agl;
        }
    }

    // Advance the timer and test the active gate. prev/cur are the ship's ABSOLUTE
    // position last frame and this frame (= camera.position - world_offset).
    void update(float dt, Vector3 prev_abs, Vector3 cur_abs) {
        if (state_ != State::Racing) return;
        elapsed_ += dt;
        if (active_ >= static_cast<int>(gates_.size())) return;
        if (mode_ == Mode::Target) return; // Target: advance only via destroy_active()
        const Gate &g = gates_[active_];
        if (!g.y.has_value()) return; // can't pass an unresolved gate
        const Vector3 center{g.x, *g.y, g.z};
        if (passed_gate(prev_abs, cur_abs, center, g.dir, GATE_RADIUS)) {
            ++active_;
            if (mode_ == Mode::Infinite) {
                // keep a buffer of N gates ahead so the course never ends
                while (static_cast<int>(gates_.size()) - active_ < N) append_gate();
            } else if (active_ >= static_cast<int>(gates_.size())) {
                finish();
            }
        }
    }

    // Draw the gates. Call inside BeginMode3D/EndMode3D. world_offset maps abs -> user.
    void draw(Vector3 world_offset) const {
        for (int i = 0; i < static_cast<int>(gates_.size()); ++i) {
            const Gate &g = gates_[i];
            const bool resolved = g.y.has_value();
            const float y = resolved ? *g.y : PROVISIONAL_Y;
            const Vector3 center = Vector3Add(Vector3{g.x, y, g.z}, world_offset);

            Color color;
            if (!resolved) {
                color = Fade(GRAY, 0.4f);
            } else if (i < active_) {
                continue; // passed: hide to reduce clutter
            } else if (i == active_) {
                color = GOLD;
            } else {
                color = Fade(SKYBLUE, 0.35f);
            }
            draw_ring(center, g.dir, GATE_RADIUS, color);
            if (resolved && i == active_) {
                draw_ring(center, g.dir, GATE_RADIUS * 0.6f, GOLD); // inner emphasis on active gate
            }

            // Target mode: a solid filled core — the thing you shoot / can crash into.
            if (mode_ == Mode::Target && resolved) {
                if (i == active_) {
                    DrawSphere(center, CORE_RADIUS, Fade(RED, 0.85f));
                    DrawSphereWires(center, CORE_RADIUS * 1.02f, 8, 12, Fade(ORANGE, 0.9f));
                } else {
                    DrawSphere(center, CORE_RADIUS, Fade(MAROON, 0.30f)); // dim upcoming
                }
            }
        }
    }

private:
    void finish() {
        state_ = State::Finished;
        if (!best_ || elapsed_ < *best_) best_ = elapsed_;
    }

    // Append one gate ahead of the generator frontier, and make the previous last
    // gate face this new one (so every ring points at the next — course direction).
    void append_gate() {
        std::uniform_real_distribution<float> gap(MIN_GAP, MAX_GAP);
        std::uniform_real_distribution<float> turn(-MAX_TURN, MAX_TURN);
        std::uniform_real_distribution<float> agl(AGL_MIN, AGL_MAX);
        gen_heading_ += turn(rng_);
        const float g = gap(rng_);
        const float nx = gen_x_ + cosf(gen_heading_) * g;
        const float nz = gen_z_ + sinf(gen_heading_) * g;
        Gate gate;
        gate.x = nx;
        gate.z = nz;
        gate.agl = agl(rng_);
        gate.y = std::nullopt;
        gate.dir = Vector3Normalize(Vector3{nx - gen_x_, 0.0f, nz - gen_z_}); // incoming (it's the new last)
        if (!gates_.empty()) {
            Gate &prev = gates_.back();
            prev.dir = Vector3Normalize(Vector3{nx - prev.x, 0.0f, nz - prev.z}); // prev now faces this one
        }
        gates_.push_back(gate);
        gen_x_ = nx;
        gen_z_ = nz;
    }

    State state_ = State::Idle;
    Mode mode_ = Mode::Competition;
    std::vector<Gate> gates_;
    int active_ = 0;
    float elapsed_ = 0.0f;
    std::optional<float> best_;
    std::mt19937 rng_;
    float gen_heading_ = 0.0f, gen_x_ = 0.0f, gen_z_ = 0.0f; // generator frontier
};

} // namespace race
