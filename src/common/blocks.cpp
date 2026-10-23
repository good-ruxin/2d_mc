#include "common/blocks.h"

#include <algorithm>
#include <array>
#include <limits>
#include <unordered_map>

namespace mc {
namespace {

const BlockDef kBlocks[] = {
    // id, display, texture, solid, opaque, replaceable, cutout, liquid, hardness, tool, tier, light, drop, min, max
    {"air", "空气", "", false, false, true, false, false, 0.0f, ToolType::None, 0, 0, "", 0, 0},
    {"grass_block", "草方块", "block/grass_block_side", true, true, false, false, false, 0.6f, ToolType::Shovel, 0, 0, "dirt", 1, 1},
    {"dirt", "泥土", "block/dirt", true, true, false, false, false, 0.5f, ToolType::Shovel, 0, 0, "dirt", 1, 1},
    {"stone", "石头", "block/stone", true, true, false, false, false, 1.5f, ToolType::Pickaxe, 1, 0, "cobblestone", 1, 1},
    {"cobblestone", "圆石", "block/cobblestone", true, true, false, false, false, 2.0f, ToolType::Pickaxe, 1, 0, "cobblestone", 1, 1},
    {"sand", "沙子", "block/sand", true, true, false, false, false, 0.5f, ToolType::Shovel, 0, 0, "sand", 1, 1},
    {"gravel", "沙砾", "block/gravel", true, true, false, false, false, 0.6f, ToolType::Shovel, 0, 0, "gravel", 1, 1},
    {"oak_log", "橡木原木", "block/oak_log", true, true, false, false, false, 2.0f, ToolType::Axe, 0, 0, "oak_log", 1, 1},
    {"oak_leaves", "橡树树叶", "block/oak_leaves", true, false, false, true, false, 0.2f, ToolType::Axe, 0, 0, "oak_sapling", 1, 1},
    {"oak_planks", "橡木木板", "block/oak_planks", true, true, false, false, false, 2.0f, ToolType::Axe, 0, 0, "oak_planks", 1, 1},
    {"coal_ore", "煤矿石", "block/coal_ore", true, true, false, false, false, 3.0f, ToolType::Pickaxe, 1, 0, "coal", 1, 2},
    {"iron_ore", "铁矿石", "block/iron_ore", true, true, false, false, false, 3.0f, ToolType::Pickaxe, 2, 0, "iron_ingot", 1, 2},
    {"gold_ore", "金矿石", "block/gold_ore", true, true, false, false, false, 3.0f, ToolType::Pickaxe, 3, 0, "gold_ingot", 1, 1},
    {"diamond_ore", "钻石矿石", "block/diamond_ore", true, true, false, false, false, 3.0f, ToolType::Pickaxe, 4, 0, "diamond", 1, 2},
    {"bedrock", "基岩", "block/bedrock", true, true, false, false, false, -1.0f, ToolType::None, 0, 0, "", 0, 0},
    {"water", "水", "block/water_still", false, false, true, false, true, -1.0f, ToolType::None, 0, 0, "", 0, 0},
    {"lava", "熔岩", "block/lava_still", false, false, true, false, true, -1.0f, ToolType::None, 0, 15, "", 0, 0},
    {"torch", "火把", "block/torch", false, false, false, true, false, 0.0f, ToolType::None, 0, 14, "torch", 1, 1},
    {"crafting_table", "工作台", "block/crafting_table_front", true, true, false, false, false, 2.5f, ToolType::Axe, 0, 0, "crafting_table", 1, 1},
    {"glass", "玻璃", "block/glass", true, false, false, true, false, 0.3f, ToolType::None, 0, 0, "", 0, 0},
    {"stone_bricks", "石砖", "block/stone_bricks", true, true, false, false, false, 1.5f, ToolType::Pickaxe, 1, 0, "stone_bricks", 1, 1},
    {"short_grass", "矮草", "block/short_grass", false, false, true, true, false, 0.0f, ToolType::None, 0, 0, "", 0, 0},
    {"tall_grass", "高草", "block/short_grass", false, false, true, true, false, 0.0f, ToolType::None, 0, 0, "", 0, 0},
    {"poppy", "虞美人", "block/poppy", false, false, true, true, false, 0.0f, ToolType::None, 0, 0, "poppy", 1, 1},
    {"dandelion", "蒲公英", "block/dandelion", false, false, true, true, false, 0.0f, ToolType::None, 0, 0, "dandelion", 1, 1},
    {"oak_sapling", "橡树树苗", "block/oak_sapling", false, false, true, true, false, 0.0f, ToolType::None, 0, 0, "oak_sapling", 1, 1},
};

constexpr int kBlockCount = static_cast<int>(sizeof(kBlocks) / sizeof(kBlocks[0]));
static_assert(kBlockCount == BLOCK_COUNT, "Block table and enum are out of sync");

std::vector<ItemDef> buildItems() {
    std::vector<ItemDef> items;
    for (int i = 1; i < kBlockCount; ++i) {
        if (i == BEDROCK || i == WATER || i == LAVA) continue;
        const BlockDef& b = kBlocks[i];
        ItemDef item;
        item.id = b.id;
        item.name = b.name;
        item.tex = b.tex;
        item.maxStack = 64;
        item.isBlock = true;
        item.block = static_cast<BlockId>(i);
        item.tool = ToolType::None;
        item.tier = 0;
        item.toolSpeed = 1.0f;
        item.attack = 1;
        item.food = 0;
        items.push_back(item);
    }

    auto add = [&](const char* id, const char* name, const char* tex, int stack,
                   ToolType tool, int tier, float speed, int attack, int food) {
        ItemDef item;
        item.id = id;
        item.name = name;
        item.tex = tex;
        item.maxStack = stack;
        item.isBlock = false;
        item.block = AIR;
        item.tool = tool;
        item.tier = tier;
        item.toolSpeed = speed;
        item.attack = attack;
        item.food = food;
        items.push_back(item);
    };

    add("stick", "木棍", "item/stick", 64, ToolType::None, 0, 1.0f, 1, 0);
    add("coal", "煤炭", "item/coal", 64, ToolType::None, 0, 1.0f, 1, 0);
    add("charcoal", "木炭", "item/charcoal", 64, ToolType::None, 0, 1.0f, 1, 0);
    add("iron_ingot", "铁锭", "item/iron_ingot", 64, ToolType::None, 0, 1.0f, 1, 0);
    add("gold_ingot", "金锭", "item/gold_ingot", 64, ToolType::None, 0, 1.0f, 1, 0);
    add("diamond", "钻石", "item/diamond", 64, ToolType::None, 0, 1.0f, 1, 0);
    add("bread", "面包", "item/bread", 64, ToolType::None, 0, 1.0f, 1, 6);
    add("apple", "苹果", "item/apple", 64, ToolType::None, 0, 1.0f, 1, 4);
    add("porkchop", "生猪排", "item/porkchop", 64, ToolType::None, 0, 1.0f, 1, 3);
    add("cooked_porkchop", "熟猪排", "item/cooked_porkchop", 64, ToolType::None, 0, 1.0f, 1, 8);

    add("wooden_pickaxe", "木镐", "item/wooden_pickaxe", 1, ToolType::Pickaxe, 1, 2.0f, 2, 0);
    add("stone_pickaxe", "石镐", "item/stone_pickaxe", 1, ToolType::Pickaxe, 2, 4.0f, 3, 0);
    add("iron_pickaxe", "铁镐", "item/iron_pickaxe", 1, ToolType::Pickaxe, 3, 6.0f, 4, 0);
    add("diamond_pickaxe", "钻石镐", "item/diamond_pickaxe", 1, ToolType::Pickaxe, 4, 8.0f, 5, 0);
    add("wooden_axe", "木斧", "item/wooden_axe", 1, ToolType::Axe, 1, 2.0f, 3, 0);
    add("stone_axe", "石斧", "item/stone_axe", 1, ToolType::Axe, 2, 4.0f, 4, 0);
    add("iron_axe", "铁斧", "item/iron_axe", 1, ToolType::Axe, 3, 6.0f, 5, 0);
    add("diamond_axe", "钻石斧", "item/diamond_axe", 1, ToolType::Axe, 4, 8.0f, 6, 0);
    add("wooden_shovel", "木锹", "item/wooden_shovel", 1, ToolType::Shovel, 1, 2.0f, 2, 0);
    add("stone_shovel", "石锹", "item/stone_shovel", 1, ToolType::Shovel, 2, 4.0f, 2, 0);
    add("iron_shovel", "铁锹", "item/iron_shovel", 1, ToolType::Shovel, 3, 6.0f, 3, 0);
    add("wooden_sword", "木剑", "item/wooden_sword", 1, ToolType::Sword, 1, 1.0f, 4, 0);
    add("stone_sword", "石剑", "item/stone_sword", 1, ToolType::Sword, 2, 1.0f, 5, 0);
    add("iron_sword", "铁剑", "item/iron_sword", 1, ToolType::Sword, 3, 1.0f, 6, 0);
    add("diamond_sword", "钻石剑", "item/diamond_sword", 1, ToolType::Sword, 4, 1.0f, 7, 0);
    return items;
}

std::vector<ItemDef> buildItemList() {
    return buildItems();
}

std::vector<Recipe> buildRecipes() {
    return {
        {"planks", "橡木木板", "oak_planks", 4, {{"oak_log", 1}}},
        {"stick", "木棍", "stick", 4, {{"oak_planks", 2}}},
        {"crafting_table", "工作台", "crafting_table", 1, {{"oak_planks", 4}}},
        {"torch", "火把", "torch", 4, {{"coal", 1}, {"stick", 1}}},
        {"torch_charcoal", "火把（木炭）", "torch", 4, {{"charcoal", 1}, {"stick", 1}}},
        {"stone_bricks", "石砖", "stone_bricks", 4, {{"stone", 4}}},
        {"wooden_pickaxe", "木镐", "wooden_pickaxe", 1, {{"oak_planks", 3}, {"stick", 2}}},
        {"stone_pickaxe", "石镐", "stone_pickaxe", 1, {{"cobblestone", 3}, {"stick", 2}}},
        {"iron_pickaxe", "铁镐", "iron_pickaxe", 1, {{"iron_ingot", 3}, {"stick", 2}}},
        {"diamond_pickaxe", "钻石镐", "diamond_pickaxe", 1, {{"diamond", 3}, {"stick", 2}}},
        {"wooden_axe", "木斧", "wooden_axe", 1, {{"oak_planks", 3}, {"stick", 2}}},
        {"stone_axe", "石斧", "stone_axe", 1, {{"cobblestone", 3}, {"stick", 2}}},
        {"iron_axe", "铁斧", "iron_axe", 1, {{"iron_ingot", 3}, {"stick", 2}}},
        {"wooden_sword", "木剑", "wooden_sword", 1, {{"oak_planks", 2}, {"stick", 1}}},
        {"stone_sword", "石剑", "stone_sword", 1, {{"cobblestone", 2}, {"stick", 1}}},
        {"iron_sword", "铁剑", "iron_sword", 1, {{"iron_ingot", 2}, {"stick", 1}}},
        {"diamond_sword", "钻石剑", "diamond_sword", 1, {{"diamond", 2}, {"stick", 1}}},
    };
}

} // namespace

const BlockDef& blockDef(BlockId id) {
    int i = static_cast<int>(id);
    if (i < 0 || i >= kBlockCount) i = 0;
    return kBlocks[i];
}

const BlockDef& blockDef(int id) {
    return blockDef(static_cast<BlockId>(id));
}

const std::vector<ItemDef>& allItems() {
    static const std::vector<ItemDef> items = buildItemList();
    return items;
}

const ItemDef* itemDef(const std::string& id) {
    static const std::unordered_map<std::string, const ItemDef*> map = [] {
        std::unordered_map<std::string, const ItemDef*> result;
        for (const auto& item : allItems()) result[item.id] = &item;
        return result;
    }();
    auto it = map.find(id);
    return it == map.end() ? nullptr : it->second;
}

const std::vector<Recipe>& allRecipes() {
    static const std::vector<Recipe> recipes = buildRecipes();
    return recipes;
}

bool isBlockItem(const std::string& item, BlockId* out) {
    const ItemDef* def = itemDef(item);
    if (!def || !def->isBlock) return false;
    if (out) *out = def->block;
    return true;
}

bool isSolidBlock(BlockId id) { return blockDef(id).solid; }
bool isOpaqueBlock(BlockId id) { return blockDef(id).opaque; }
bool isReplaceableBlock(BlockId id) { return blockDef(id).replaceable; }

std::string blockName(BlockId id) { return blockDef(id).name; }

std::string itemName(const std::string& id) {
    const ItemDef* def = itemDef(id);
    return def ? def->name : id;
}

int toolTier(const std::string& heldItem) {
    const ItemDef* def = itemDef(heldItem);
    return def ? def->tier : 0;
}

int attackDamage(const std::string& heldItem) {
    const ItemDef* def = itemDef(heldItem);
    return def ? def->attack : 1;
}

float breakSeconds(BlockId block, const std::string& heldItem) {
    const BlockDef& def = blockDef(block);
    if (def.hardness < 0.0f) return std::numeric_limits<float>::infinity();
    if (def.hardness <= 0.0f) return 0.0f;
    float speed = 1.0f;
    const ItemDef* item = itemDef(heldItem);
    if (item && def.tool != ToolType::None && item->tool == def.tool) {
        speed = item->toolSpeed;
    }
    return (def.hardness * 1.5f) / std::max(0.25f, speed);
}

ItemStack makeStack(const std::string& id, int count) {
    ItemStack stack;
    stack.id = id;
    stack.count = std::max(0, count);
    if (stack.count == 0) stack.id.clear();
    return stack;
}

int stackLimit(const std::string& id) {
    const ItemDef* def = itemDef(id);
    return def ? def->maxStack : 64;
}

bool canAddItem(const std::vector<ItemStack>& inv, const std::string& id, int count) {
    if (count <= 0) return true;
    const int limit = stackLimit(id);
    int room = 0;
    for (const auto& slot : inv) {
        if (slot.empty()) room += limit;
        else if (slot.id == id) room += std::max(0, limit - slot.count);
        if (room >= count) return true;
    }
    return room >= count;
}

int addItem(std::vector<ItemStack>& inv, const std::string& id, int count) {
    if (count <= 0 || id.empty()) return count;
    const int limit = stackLimit(id);
    int remaining = count;
    for (auto& slot : inv) {
        if (remaining <= 0) break;
        if (!slot.empty() && slot.id == id && slot.count < limit) {
            int moved = std::min(remaining, limit - slot.count);
            slot.count += moved;
            remaining -= moved;
        }
    }
    for (auto& slot : inv) {
        if (remaining <= 0) break;
        if (slot.empty()) {
            int moved = std::min(remaining, limit);
            slot.id = id;
            slot.count = moved;
            remaining -= moved;
        }
    }
    return remaining;
}

int countItem(const std::vector<ItemStack>& inv, const std::string& id) {
    int total = 0;
    for (const auto& slot : inv) if (!slot.empty() && slot.id == id) total += slot.count;
    return total;
}

bool removeItem(std::vector<ItemStack>& inv, const std::string& id, int count) {
    if (count <= 0) return true;
    if (countItem(inv, id) < count) return false;
    int remaining = count;
    for (auto& slot : inv) {
        if (remaining <= 0) break;
        if (!slot.empty() && slot.id == id) {
            int taken = std::min(remaining, slot.count);
            slot.count -= taken;
            remaining -= taken;
            if (slot.count <= 0) slot.clear();
        }
    }
    return true;
}

} // namespace mc
