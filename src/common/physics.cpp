#include "common/physics.h"

#include <algorithm>
#include <cmath>

namespace mc {
namespace {

float approach(float value, float target, float delta) {
    if (value < target) return std::min(value + delta, target);
    if (value > target) return std::max(value - delta, target);
    return target;
}

bool hasGroundAt(const World& world, float x, float y) {
    // A tiny look below the feet, sampled at the four bottom corners.
    for (float dx : {-0.26f, 0.0f, 0.26f}) {
        int bx = static_cast<int>(std::floor(x + dx));
        int by = static_cast<int>(std::floor(y + 0.06f));
        if (world.solid(bx, by)) return true;
    }
    return false;
}

} // namespace

bool hasGroundSupport(const World& world, const PlayerBody& body) {
    return hasGroundAt(world, body.x, body.y);
}

void tickPlayer(const World& world, PlayerBody& body, const MoveInput& input, bool creative) {
    const float dt = TICK_SECONDS;
    const bool inWater = world.boxInWater(body.x, body.y, PLAYER_WIDTH, body.height());
    const bool inLava = world.boxInLava(body.x, body.y, PLAYER_WIDTH, body.height());

    body.justLanded = false;
    body.sneaking = input.sneak && !body.flying;
    if (body.flying && !creative) body.flying = false;

    float height = body.height();
    if (body.onGround && !hasGroundSupport(world, body)) body.onGround = false;

    float speed;
    if (body.flying) speed = 10.9f;
    else if (input.sneak) speed = 1.295f;
    else if (input.sprint) speed = 5.612f;
    else speed = 4.317f;
    if (inWater && !body.flying) speed *= 0.55f;
    if (inLava && !body.flying) speed *= 0.35f;

    float targetVx = static_cast<float>(input.move) * speed;
    float accel = body.onGround ? 38.0f : 14.0f;
    if (inWater || inLava) accel = 16.0f;
    if (body.flying) accel = 45.0f;
    body.vx = approach(body.vx, targetVx, accel * dt);

    body.sprinting = input.sprint && std::abs(input.move) > 0 && !input.sneak && !body.flying;
    body.facing = input.move != 0 ? (input.move > 0 ? 1 : -1)
                                  : (input.facing < 0.0f ? -1 : 1);

    if (body.flying) {
        body.vy = 0.0f;
        if (input.jump) body.vy = -10.9f;
        else if (input.sneak) body.vy = 10.9f;
    } else if (inWater) {
        body.vy = approach(body.vy, input.jump ? -4.0f : 2.0f, 18.0f * dt);
        if (input.jump) body.vy = -4.0f;
        body.fallDistance = 0.0f;
    } else if (inLava) {
        body.vy = approach(body.vy, input.jump ? -2.0f : 1.2f, 12.0f * dt);
        body.fallDistance = 0.0f;
    } else {
        if (input.jump && body.onGround) {
            body.vy = -9.2f;
            body.onGround = false;
        }
        body.vy += 30.0f * dt;
        body.vy = std::min(body.vy, 80.0f);
    }

    // Horizontal sweep.
    float nx = body.x + body.vx * dt;
    if (!world.collidesBox(nx, body.y, PLAYER_WIDTH, height)) {
        body.x = nx;
    } else {
        // Try a small step-up assist for the 2D tile grid.
        body.vx = 0.0f;
    }

    // Sneaking prevents walking off a ledge, like the Java edition.
    if (body.sneaking && body.onGround && !body.flying) {
        if (!hasGroundAt(world, body.x, body.y)) {
            body.x -= body.vx * dt;
            if (hasGroundAt(world, body.x, body.y)) {
                // Keep the previous safe x entirely by rejecting this tick's move.
            }
            body.vx = 0.0f;
        }
    }

    // Vertical sweep.
    float ny = body.y + body.vy * dt;
    if (!world.collidesBox(body.x, ny, PLAYER_WIDTH, height)) {
        body.y = ny;
        if (body.vy > 0.0f && !inWater && !inLava && !body.flying) {
            body.fallDistance += body.vy * dt;
        }
    } else {
        if (body.vy > 0.0f) {
            body.landingFall = body.fallDistance;
            if (!body.onGround) body.justLanded = true;
            body.fallDistance = 0.0f;
            body.onGround = true;
        }
        body.vy = 0.0f;
    }

    if (body.flying) {
        body.fallDistance = 0.0f;
        body.onGround = false;
    }

    // Keep the player inside the horizontal world bounds.
    body.x = std::clamp(body.x, PLAYER_WIDTH * 0.5f + 0.01f,
                        static_cast<float>(world.width()) - PLAYER_WIDTH * 0.5f - 0.01f);
    if (body.y > static_cast<float>(world.height()) + 10.0f) {
        body.y = static_cast<float>(world.height()) + 10.0f;
        body.vy = 0.0f;
    }
}

} // namespace mc
