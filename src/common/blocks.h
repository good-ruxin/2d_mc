#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

enum class ToolType { None, Pickaxe, Axe, Shovel, Sword };

enum BlockId : uint16_t {
    AIR = 0,
    GRASS_BLOCK,
    DIRT,
    STONE,
    COBBLESTONE,
    SAND,
    GRAVEL,
    OAK_LOG,
    OAK_LEAVES,
    OAK_PLANKS,
    COAL_ORE,
    IRON_ORE,
    GOLD_ORE,
    DIAMOND_ORE,
    BEDROCK,
    WATER,
    LAVA,
    TORCH,
    CRAFTING_TABLE,
    GLASS,
    STONE_BRICKS,
    SHORT_GRASS,
    TALL_GRASS,
    POPPY,
    DANDELION,
    OAK_SAPLING,
    BLOCK_COUNT
};

struct BlockDef {
    const char* id;
    const char* name;
    const char* tex;       // path below assets/, without .png
    bool solid;
    bool opaque;           // blocks sky light completely
    bool replaceable;      // can be replaced by placing a block
    bool cutout;           // alpha-tested / plant-like
    bool liquid;
    float hardness;
    ToolType tool;
    int tier;
    int light;
    const char* drop;      // empty string means no drop
    int dropMin;
    int dropMax;
};

struct ItemDef {
    std::string id;
    std::string name;
    std::string tex;
    int maxStack;
    bool isBlock;
    BlockId block;
    ToolType tool;
    int tier;
    float toolSpeed;
    int attack;
    int food;
};

struct ItemStack {
    std::string id;
    int count = 0;

    bool empty() const { return id.empty() || count <= 0; }
    void clear() { id.clear(); count = 0; }
};

struct Recipe {
    std::string id;
    std::string name;
    std::string output;
    int outputCount;
    std::vector<std::pair<std::string, int>> inputs;
};

const BlockDef& blockDef(BlockId id);
const BlockDef& blockDef(int id);
const ItemDef* itemDef(const std::string& id);
const std::vector<ItemDef>& allItems();
const std::vector<Recipe>& allRecipes();

bool isBlockItem(const std::string& item, BlockId* out = nullptr);
bool isSolidBlock(BlockId id);
bool isOpaqueBlock(BlockId id);
bool isReplaceableBlock(BlockId id);
std::string itemName(const std::string& id);
std::string blockName(BlockId id);

float breakSeconds(BlockId block, const std::string& heldItem);
int attackDamage(const std::string& heldItem);
int toolTier(const std::string& heldItem);

ItemStack makeStack(const std::string& id, int count);
bool canAddItem(const std::vector<ItemStack>& inv, const std::string& id, int count);
int addItem(std::vector<ItemStack>& inv, const std::string& id, int count);
int countItem(const std::vector<ItemStack>& inv, const std::string& id);
bool removeItem(std::vector<ItemStack>& inv, const std::string& id, int count);

} // namespace mc
