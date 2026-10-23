#pragma once

#include "common/blocks.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

class World {
public:
    static constexpr int SEA_LEVEL = 82;

    World(int width = 512, int height = 128);

    void generate(uint64_t seed);
    void clear(BlockId fill = AIR);

    int width() const { return width_; }
    int height() const { return height_; }
    uint64_t seed() const { return seed_; }

    bool inBounds(int x, int y) const {
        return x >= 0 && y >= 0 && x < width_ && y < height_;
    }

    BlockId fg(int x, int y) const;
    BlockId bg(int x, int y) const;
    void setFg(int x, int y, BlockId block);
    void setBg(int x, int y, BlockId block);

    bool solid(int x, int y) const;
    bool opaque(int x, int y) const;
    bool water(int x, int y) const;
    bool lava(int x, int y) const;
    bool collidesBox(float x, float y, float w, float h) const;
    bool boxInWater(float x, float y, float w, float h) const;
    bool boxInLava(float x, float y, float w, float h) const;

    int surfaceY(int x) const;
    int clearanceAbove(int x, int y) const;

    std::vector<uint16_t>& fgData() { return fg_; }
    std::vector<uint16_t>& bgData() { return bg_; }
    const std::vector<uint16_t>& fgData() const { return fg_; }
    const std::vector<uint16_t>& bgData() const { return bg_; }

    // RLE: [value, count, value, count, ...]
    std::vector<int> encodeFg() const;
    std::vector<int> encodeBg() const;
    static std::vector<uint16_t> decodeRuns(const std::vector<int>& runs, int expected);

    bool save(const std::string& path, uint64_t time) const;
    bool load(const std::string& path, uint64_t& time);

private:
    int width_;
    int height_;
    uint64_t seed_ = 0;
    std::vector<uint16_t> fg_;
    std::vector<uint16_t> bg_;
};

} // namespace mc
