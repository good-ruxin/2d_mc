#pragma once

#include "common/world.h"

#include <cstdint>

namespace mc {

constexpr float PLAYER_WIDTH = 0.6f;
constexpr float PLAYER_HEIGHT = 1.8f;
constexpr float PLAYER_SNEAK_HEIGHT = 1.5f;
constexpr float TICK_SECONDS = 0.05f;

struct MoveInput {
    int move = 0;          // -1 left, 0 idle, 1 right
    bool jump = false;
    bool sneak = false;
    bool sprint = false;
    float facing = 1.0f;
    uint32_t seq = 0;
};

struct PlayerBody {
    float x = 0.0f;
    float y = 0.0f;        // feet
    float vx = 0.0f;
    float vy = 0.0f;
    bool onGround = false;
    bool sneaking = false;
    bool sprinting = false;
    bool flying = false;
    int facing = 1;
    float fallDistance = 0.0f;
    bool justLanded = false;
    float landingFall = 0.0f;

    float height() const { return sneaking ? PLAYER_SNEAK_HEIGHT : PLAYER_HEIGHT; }
    float eyeY() const { return y - height() + 0.22f; }
    float centerX() const { return x; }
    float centerY() const { return y - height() * 0.5f; }
};

// Server and client use this exact routine. The client predicts with it and
// the server consumes the same inputs, so reconciliation is deterministic.
void tickPlayer(const World& world, PlayerBody& body, const MoveInput& input, bool creative);

bool hasGroundSupport(const World& world, const PlayerBody& body);

} // namespace mc
