#include "common/world.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>
#include <unordered_map>

namespace mc {
namespace {

uint64_t hash64(uint64_t x) {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return x ^ (x >> 31);
}

float hashFloat(int64_t x, int64_t y, uint64_t seed) {
    uint64_t h = hash64(static_cast<uint64_t>(x) * 0x9e3779b97f4a7c15ULL ^
                        static_cast<uint64_t>(y) * 0xc2b2ae3d27d4eb4fULL ^ seed);
    return static_cast<float>(h & 0xFFFFFF) / static_cast<float>(0xFFFFFF);
}

float smooth(float t) { return t * t * (3.0f - 2.0f * t); }

float valueNoise1(float x, uint64_t seed) {
    int x0 = static_cast<int>(std::floor(x));
    float t = x - static_cast<float>(x0);
    float a = hashFloat(x0, 0, seed) * 2.0f - 1.0f;
    float b = hashFloat(x0 + 1, 0, seed) * 2.0f - 1.0f;
    return a + (b - a) * smooth(t);
}

float valueNoise2(float x, float y, uint64_t seed) {
    int x0 = static_cast<int>(std::floor(x));
    int y0 = static_cast<int>(std::floor(y));
    float tx = smooth(x - static_cast<float>(x0));
    float ty = smooth(y - static_cast<float>(y0));
    float a = hashFloat(x0, y0, seed);
    float b = hashFloat(x0 + 1, y0, seed);
    float c = hashFloat(x0, y0 + 1, seed);
    float d = hashFloat(x0 + 1, y0 + 1, seed);
    float ab = a + (b - a) * tx;
    float cd = c + (d - c) * tx;
    return ab + (cd - ab) * ty;
}

float fbm2(float x, float y, uint64_t seed) {
    return valueNoise2(x, y, seed) * 0.55f +
           valueNoise2(x * 2.07f, y * 2.07f, seed ^ 0xA53C9E1DULL) * 0.28f +
           valueNoise2(x * 4.11f, y * 4.11f, seed ^ 0x71D3B5A9ULL) * 0.17f;
}

constexpr uint16_t U(BlockId id) { return static_cast<uint16_t>(id); }

struct Header {
    char magic[8];
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint64_t seed;
    uint64_t time;
};

} // namespace

World::World(int width, int height)
    : width_(width), height_(height), fg_(static_cast<size_t>(width) * height, AIR),
      bg_(static_cast<size_t>(width) * height, AIR) {}

void World::clear(BlockId fill) {
    std::fill(fg_.begin(), fg_.end(), U(fill));
    std::fill(bg_.begin(), bg_.end(), U(AIR));
}

BlockId World::fg(int x, int y) const {
    if (!inBounds(x, y)) return BEDROCK;
    return static_cast<BlockId>(fg_[static_cast<size_t>(y) * width_ + x]);
}

BlockId World::bg(int x, int y) const {
    if (!inBounds(x, y)) return AIR;
    return static_cast<BlockId>(bg_[static_cast<size_t>(y) * width_ + x]);
}

void World::setFg(int x, int y, BlockId block) {
    if (inBounds(x, y)) fg_[static_cast<size_t>(y) * width_ + x] = U(block);
}

void World::setBg(int x, int y, BlockId block) {
    if (inBounds(x, y)) bg_[static_cast<size_t>(y) * width_ + x] = U(block);
}

bool World::solid(int x, int y) const {
    if (x < 0 || x >= width_) return true;
    if (y >= height_) return true;
    if (y < 0) return false;
    return blockDef(static_cast<BlockId>(fg_[static_cast<size_t>(y) * width_ + x])).solid;
}

bool World::opaque(int x, int y) const {
    if (!inBounds(x, y)) return y < 0 ? false : true;
    return blockDef(fg(x, y)).opaque;
}

bool World::water(int x, int y) const { return fg(x, y) == WATER; }
bool World::lava(int x, int y) const { return fg(x, y) == LAVA; }

bool World::collidesBox(float x, float y, float w, float h) const {
    int x0 = static_cast<int>(std::floor(x - w * 0.5f));
    int x1 = static_cast<int>(std::floor(x + w * 0.5f - 0.0001f));
    int y0 = static_cast<int>(std::floor(y - h));
    int y1 = static_cast<int>(std::floor(y - 0.0001f));
    for (int by = y0; by <= y1; ++by) {
        for (int bx = x0; bx <= x1; ++bx) {
            if (solid(bx, by)) return true;
        }
    }
    return false;
}

bool World::boxInWater(float x, float y, float w, float h) const {
    int x0 = static_cast<int>(std::floor(x - w * 0.5f));
    int x1 = static_cast<int>(std::floor(x + w * 0.5f - 0.0001f));
    int y0 = static_cast<int>(std::floor(y - h));
    int y1 = static_cast<int>(std::floor(y - 0.0001f));
    for (int by = y0; by <= y1; ++by)
        for (int bx = x0; bx <= x1; ++bx)
            if (water(bx, by)) return true;
    return false;
}

bool World::boxInLava(float x, float y, float w, float h) const {
    int x0 = static_cast<int>(std::floor(x - w * 0.5f));
    int x1 = static_cast<int>(std::floor(x + w * 0.5f - 0.0001f));
    int y0 = static_cast<int>(std::floor(y - h));
    int y1 = static_cast<int>(std::floor(y - 0.0001f));
    for (int by = y0; by <= y1; ++by)
        for (int bx = x0; bx <= x1; ++bx)
            if (lava(bx, by)) return true;
    return false;
}

int World::surfaceY(int x) const {
    if (x < 0 || x >= width_) return height_ - 1;
    for (int y = 0; y < height_; ++y) {
        if (solid(x, y)) return y;
    }
    return height_ - 1;
}

int World::clearanceAbove(int x, int y) const {
    int n = 0;
    for (int yy = y - 1; yy >= 0; --yy) {
        if (solid(x, yy)) break;
        ++n;
    }
    return n;
}

void World::generate(uint64_t seed) {
    seed_ = seed;
    clear(AIR);
    std::mt19937_64 rng(seed);
    std::vector<int> heights(width_);

    // Background wall is generated from the original terrain height even where
    // caves are carved later, so mined-out tunnels reveal a dirt/stone backdrop.
    for (int x = 0; x < width_; ++x) {
        float hills = valueNoise1(x * 0.0075f, seed ^ 0x1122ULL) * 13.0f +
                      valueNoise1(x * 0.031f, seed ^ 0x9911ULL) * 4.0f +
                      valueNoise1(x * 0.12f, seed ^ 0x2C3DULL) * 1.3f;
        int h = static_cast<int>(std::round(73.0f + hills));
        h = std::clamp(h, 56, 104);

        float biome = valueNoise1(x * 0.0045f + 400.0f, seed ^ 0xABCDEFULL);
        bool desert = biome < -0.30f;

        heights[x] = h;
        BlockId surface = desert ? SAND : GRASS_BLOCK;
        BlockId under = desert ? SAND : DIRT;
        for (int y = h; y < height_; ++y) {
            BlockId block = STONE;
            if (y == h) block = surface;
            else if (y <= h + 4) block = under;
            if (y >= height_ - 2) block = BEDROCK;
            setFg(x, y, block);
            setBg(x, y, (y <= h + 3) ? under : STONE);
        }
    }

    // Caves: two noise fields intersect to create connected, walkable caverns.
    for (int x = 2; x < width_ - 2; ++x) {
        int h = heights[x];
        for (int y = h + 6; y < height_ - 4; ++y) {
            float n1 = fbm2(x * 0.055f, y * 0.075f, seed ^ 0xCAFEULL);
            float n2 = valueNoise2(x * 0.028f + 40.0f, y * 0.050f + 17.0f, seed ^ 0xD00DULL);
            if (n1 > 0.565f && n2 > 0.42f) {
                setFg(x, y, AIR);
                if (y + 1 < height_ - 2 && n1 > 0.60f) setFg(x, y + 1, AIR);
            }
        }
    }

    // Ore veins are random walks, biased to their depth band.
    auto vein = [&](BlockId ore, int count, int minY, int maxY, int size) {
        for (int i = 0; i < count; ++i) {
            int x = 3 + static_cast<int>(rng() % (width_ - 6));
            int y = minY + static_cast<int>(rng() % std::max(1, maxY - minY));
            for (int n = 0; n < size; ++n) {
                if (inBounds(x, y) && fg(x, y) == STONE) setFg(x, y, ore);
                x += static_cast<int>(rng() % 3) - 1;
                y += static_cast<int>(rng() % 3) - 1;
                x = std::clamp(x, 1, width_ - 2);
                y = std::clamp(y, 2, height_ - 3);
            }
        }
    };
    vein(COAL_ORE, 240, 90, 124, 8);
    vein(IRON_ORE, 150, 100, 124, 6);
    vein(GOLD_ORE, 55, 110, 124, 4);
    vein(DIAMOND_ORE, 38, 116, 124, 3);

    // Lakes and ocean pockets.
    for (int x = 0; x < width_; ++x) {
        int h = heights[x];
        if (h > SEA_LEVEL) {
            for (int y = SEA_LEVEL; y < h; ++y) {
                if (fg(x, y) == AIR) setFg(x, y, WATER);
            }
            if (fg(x, h) == GRASS_BLOCK) setFg(x, h, SAND);
        }
    }

    // Oak trees. Keep a clearing at the world center for the spawn point.
    const int spawnCenter = width_ / 2;
    int treeTimer = 0;
    for (int x = 8; x < width_ - 8; ++x) {
        --treeTimer;
        if (treeTimer > 0) continue;
        treeTimer = 7 + static_cast<int>(rng() % 13);
        if (std::abs(x - spawnCenter) < 5) continue;
        int h = heights[x];
        if (fg(x, h) != GRASS_BLOCK) continue;
        if (fg(x, h - 1) != AIR || fg(x, h - 2) != AIR) continue;
        float density = valueNoise1(x * 0.012f + 900.0f, seed ^ 0x5555ULL);
        if (density < -0.2f) continue;
        int trunk = 4 + static_cast<int>(rng() % 3);
        int top = h - 1 - trunk;
        if (top < 5) continue;
        for (int y = h - 1; y >= top; --y) {
            setFg(x, y, OAK_LOG);
            setBg(x, y, AIR);
        }
        // Leaf blob.
        for (int dy = -2; dy <= 2; ++dy) {
            int radius = (dy <= 0) ? 2 : 1;
            for (int dx = -radius; dx <= radius; ++dx) {
                if (dx == 0 && dy >= 0) continue;
                int lx = x + dx;
                int ly = top + dy;
                if (!inBounds(lx, ly)) continue;
                if (fg(lx, ly) == AIR) setFg(lx, ly, OAK_LEAVES);
            }
        }
        setFg(x, top - 2, OAK_LEAVES);
        // A partly sunlit edge.
        if (rng() % 3 == 0) setFg(x + 2, top - 1, OAK_LEAVES);
    }

    // Ground cover.
    for (int x = 2; x < width_ - 2; ++x) {
        int h = heights[x];
        if (fg(x, h) != GRASS_BLOCK) continue;
        int above = h - 1;
        if (fg(x, above) != AIR) continue;
        uint64_t r = rng() % 100;
        if (r < 26) setFg(x, above, SHORT_GRASS);
        else if (r < 34) setFg(x, above, TALL_GRASS);
        else if (r < 38) setFg(x, above, POPPY);
        else if (r < 42) setFg(x, above, DANDELION);
        else if (r < 44) setFg(x, above, OAK_SAPLING);
    }

    // Guarantee a clean spawn platform at the center.
    int sx = spawnCenter;
    int sh = height_;
    for (int dx = -2; dx <= 2; ++dx) {
        int x = std::clamp(sx + dx, 0, width_ - 1);
        int y = surfaceY(x);
        sh = std::min(sh, y);
        for (int yy = std::max(0, y - 4); yy < y; ++yy) {
            if (!blockDef(fg(x, yy)).solid) setFg(x, yy, AIR);
        }
    }
    if (sh >= SEA_LEVEL) {
        // Avoid spawning inside a lake.
        for (int dx = -2; dx <= 2; ++dx) {
            int x = std::clamp(sx + dx, 0, width_ - 1);
            for (int yy = sh - 2; yy < sh; ++yy) {
                if (fg(x, yy) == WATER) setFg(x, yy, AIR);
            }
        }
    }
}

std::vector<int> World::encodeFg() const {
    std::vector<int> runs;
    runs.reserve(4096);
    uint16_t current = fg_[0];
    int count = 1;
    for (size_t i = 1; i < fg_.size(); ++i) {
        if (fg_[i] == current && count < 100000000) {
            ++count;
        } else {
            runs.push_back(current);
            runs.push_back(count);
            current = fg_[i];
            count = 1;
        }
    }
    runs.push_back(current);
    runs.push_back(count);
    return runs;
}

std::vector<int> World::encodeBg() const {
    std::vector<int> runs;
    runs.reserve(4096);
    uint16_t current = bg_[0];
    int count = 1;
    for (size_t i = 1; i < bg_.size(); ++i) {
        if (bg_[i] == current) {
            ++count;
        } else {
            runs.push_back(current);
            runs.push_back(count);
            current = bg_[i];
            count = 1;
        }
    }
    runs.push_back(current);
    runs.push_back(count);
    return runs;
}

std::vector<uint16_t> World::decodeRuns(const std::vector<int>& runs, int expected) {
    std::vector<uint16_t> result;
    result.reserve(static_cast<size_t>(expected));
    for (size_t i = 0; i + 1 < runs.size(); i += 2) {
        int value = runs[i];
        int count = runs[i + 1];
        if (count <= 0) continue;
        for (int n = 0; n < count && static_cast<int>(result.size()) < expected; ++n)
            result.push_back(static_cast<uint16_t>(value));
    }
    result.resize(static_cast<size_t>(std::max(0, expected)), 0);
    return result;
}

bool World::save(const std::string& path, uint64_t time) const {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    std::ofstream out(path, std::ios::binary);
    if (!out) return false;
    Header header{};
    std::memcpy(header.magic, "MC2DWRD1", 8);
    header.version = 1;
    header.width = static_cast<uint32_t>(width_);
    header.height = static_cast<uint32_t>(height_);
    header.seed = seed_;
    header.time = time;
    out.write(reinterpret_cast<const char*>(&header), sizeof(header));
    out.write(reinterpret_cast<const char*>(fg_.data()), static_cast<std::streamsize>(fg_.size() * sizeof(uint16_t)));
    out.write(reinterpret_cast<const char*>(bg_.data()), static_cast<std::streamsize>(bg_.size() * sizeof(uint16_t)));
    return static_cast<bool>(out);
}

bool World::load(const std::string& path, uint64_t& time) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    Header header{};
    in.read(reinterpret_cast<char*>(&header), sizeof(header));
    if (!in || std::memcmp(header.magic, "MC2DWRD1", 8) != 0 || header.version != 1) return false;
    if (header.width > 4096 || header.height > 512 || header.width < 16 || header.height < 32) return false;
    if (header.width != static_cast<uint32_t>(width_) || header.height != static_cast<uint32_t>(height_)) {
        width_ = static_cast<int>(header.width);
        height_ = static_cast<int>(header.height);
        fg_.assign(static_cast<size_t>(width_) * height_, AIR);
        bg_.assign(static_cast<size_t>(width_) * height_, AIR);
    }
    in.read(reinterpret_cast<char*>(fg_.data()), static_cast<std::streamsize>(fg_.size() * sizeof(uint16_t)));
    in.read(reinterpret_cast<char*>(bg_.data()), static_cast<std::streamsize>(bg_.size() * sizeof(uint16_t)));
    if (!in) return false;
    seed_ = header.seed;
    time = header.time;
    return true;
}

} // namespace mc
