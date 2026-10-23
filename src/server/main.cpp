#include "common/blocks.h"
#include "common/net.h"
#include "common/physics.h"
#include "common/world.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <conio.h>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace mc;
using json = nlohmann::json;

namespace {

std::atomic<bool> g_running{true};
std::mutex g_logMutex;

void logLine(const std::string& text) {
    std::lock_guard<std::mutex> lock(g_logMutex);
    std::cout << text << std::endl;
}

BOOL WINAPI consoleHandler(DWORD) {
    g_running = false;
    return TRUE;
}

constexpr int INV_SIZE = 36;
constexpr int HOTBAR_SIZE = 9;
constexpr int SNAPSHOT_INTERVAL = 2;

struct Entity {
    int id = 0;
    std::string type;      // pig, zombie, item
    float x = 0.0f, y = 0.0f;
    float vx = 0.0f, vy = 0.0f;
    bool onGround = false;
    int facing = 1;
    int health = 10;
    int maxHealth = 10;
    int hurt = 0;
    int age = 0;
    int wander = 0;
    int dir = 1;
    int attackCooldown = 0;
    std::string item;
    int count = 0;
};

struct Player {
    int id = 0;
    std::string name;
    PlayerBody body;
    std::vector<ItemStack> inv;
    ItemStack cursor;
    int selected = 0;
    int health = 20;
    int food = 20;
    int air = 300;
    bool dead = false;
    bool creative = false;
    bool flying = false;
    uint32_t ackSeq = 0;
    MoveInput pendingInput;
    bool hasInput = false;
    int attackCooldown = 0;
    int useCooldown = 0;
    int hurtCooldown = 0;
    int foodTick = 0;
    int regenTick = 0;
    int starveTick = 0;
    int drownTick = 0;
    int lavaTick = 0;
    int lastHurtTick = -1000;
    float spawnX = 0.0f;
    float spawnY = 0.0f;
    bool joined = false;
    int joinedTick = 0;
};

struct Client {
    int id = 0;
    std::shared_ptr<NetSocket> socket;
    std::thread reader;
    std::mutex sendMutex;
    std::atomic<bool> alive{true};
    bool joined = false;
};

struct Message {
    int clientId;
    json payload;
};

class Server {
public:
    Server(int port, std::filesystem::path worldPath, uint64_t requestedSeed, bool fresh)
        : port_(port), worldPath_(std::move(worldPath)), requestedSeed_(requestedSeed), fresh_(fresh) {}

    ~Server() { shutdown(); }

    bool init(std::string& error) {
        if (!netInit(error)) return false;
        if (!listener_.start(port_, error)) return false;

        bool loaded = false;
        if (!fresh_ && std::filesystem::exists(worldPath_)) {
            loaded = world_.load(worldPath_.string(), time_);
            if (loaded) logLine("[World] 已载入存档 " + worldPath_.string());
        }
        if (!loaded) {
            uint64_t seed = requestedSeed_;
            if (seed == 0) {
                std::random_device rd;
                seed = (static_cast<uint64_t>(rd()) << 32) ^ rd() ^
                       static_cast<uint64_t>(std::chrono::high_resolution_clock::now().time_since_epoch().count());
            }
            world_.generate(seed);
            time_ = 1000;
            logLine("[World] 已生成新世界，种子: " + std::to_string(seed));
        }

        findSpawn();
        seedPigs();
        playersPath_ = worldPath_.parent_path() / (worldPath_.stem().string() + ".players.json");
        if (!std::filesystem::exists(playersPath_) && worldPath_.stem() == "world") {
            std::filesystem::path legacy = worldPath_.parent_path() / "players.json";
            if (std::filesystem::exists(legacy)) playersPath_ = legacy;
        }
        loadPlayerData();
        logLine("[Server] Minecraft 2D 专用服务器已启动");
        logLine("[Server] 地址: 0.0.0.0:" + std::to_string(port_) +
                "  世界: " + std::to_string(world_.width()) + "x" + std::to_string(world_.height()));
        logLine("[Server] 输入 stop 保存并关闭服务器，输入 help 查看控制台指令");
        return true;
    }

    void run() {
        SetConsoleCtrlHandler(consoleHandler, TRUE);
        using clock = std::chrono::steady_clock;
        auto nextTick = clock::now();
        while (g_running) {
            nextTick += std::chrono::milliseconds(50);
            acceptClients();
            pollConsole();
            drainMessages();
            tick();
            cleanupClients();

            auto now = clock::now();
            if (nextTick < now) nextTick = now;
            std::this_thread::sleep_until(nextTick);
        }
        saveWorld();
        savePlayerData();
        logLine("[Server] 正在关闭...");
    }

    void shutdown() {
        if (shutdownDone_) return;
        shutdownDone_ = true;
        listener_.close();
        for (auto& [id, client] : clients_) {
            (void)id;
            client->alive = false;
            if (client->socket) {
                client->socket->shutdownBoth();
                client->socket->close();
            }
            if (client->reader.joinable()) client->reader.join();
        }
        clients_.clear();
    }

private:
    int port_;
    std::filesystem::path worldPath_;
    uint64_t requestedSeed_;
    bool fresh_;
    World world_;
    TcpListener listener_;
    std::filesystem::path playersPath_;
    std::map<std::string, json> savedPlayers_;
    uint64_t time_ = 1000;
    uint64_t tick_ = 0;
    int nextClientId_ = 1;
    int nextEntityId_ = 1;
    std::map<int, std::unique_ptr<Client>> clients_;
    std::map<int, Player> players_;
    std::vector<Entity> entities_;
    std::mutex inboxMutex_;
    std::deque<Message> inbox_;
    std::string consoleLine_;
    std::mt19937 rng_{std::random_device{}()};
    float spawnX_ = 0.0f;
    float spawnY_ = 0.0f;
    uint64_t lastSaveTick_ = 0;
    bool shutdownDone_ = false;
    bool worldChangeDirty_ = false;

    void pollConsole() {
        // Non-blocking console input. This also works when the server was
        // started without a console window (for example by the client).
        while (_kbhit()) {
            int ch = _getch();
            if (ch == '\r' || ch == '\n') {
                std::cout << std::endl;
                if (!consoleLine_.empty()) handleConsoleCommand(consoleLine_);
                consoleLine_.clear();
            } else if (ch == '\b') {
                if (!consoleLine_.empty()) {
                    consoleLine_.pop_back();
                    std::cout << "\b \b";
                }
            } else if (ch >= 32 && ch < 127) {
                consoleLine_.push_back(static_cast<char>(ch));
                std::cout << static_cast<char>(ch) << std::flush;
            }
        }
    }

    void acceptClients() {
        while (true) {
            std::string error;
            auto socket = listener_.acceptOne(error);
            if (!error.empty()) logLine("[Net] " + error);
            if (!socket) break;
            int id = nextClientId_++;
            if (clients_.size() >= 8) {
                json deny = {{"type", "denied"}, {"reason", "服务器人数已满"}};
                std::mutex temporary;
                std::string ignored;
                sendJson(*socket, temporary, deny, ignored);
                socket->close();
                continue;
            }
            auto client = std::make_unique<Client>();
            client->id = id;
            client->socket = std::move(socket);
            Client* raw = client.get();
            raw->reader = std::thread([this, raw] {
                while (raw->alive && g_running) {
                    json message;
                    std::string error;
                    if (!recvJson(*raw->socket, message, error)) {
                        logLine("[Net] 连接 #" + std::to_string(raw->id) + " 读取结束: " + error);
                        break;
                    }
                    {
                        std::lock_guard<std::mutex> lock(inboxMutex_);
                        inbox_.push_back({raw->id, std::move(message)});
                        if (inbox_.size() > 4096) inbox_.pop_front();
                    }
                }
                raw->alive = false;
            });
            clients_[id] = std::move(client);
            logLine("[Net] 新的连接 #" + std::to_string(id));
        }
    }

    void cleanupClients() {
        std::vector<int> dead;
        for (auto& [id, client] : clients_) {
            if (!client->alive) dead.push_back(id);
        }
        for (int id : dead) {
            auto it = clients_.find(id);
            if (it == clients_.end()) continue;
            Client& client = *it->second;
            if (players_.count(id)) {
                Player& player = players_[id];
                if (player.joined) {
                    savePlayerData();
                    broadcastChat("系统", player.name + " 离开了游戏");
                    logLine("[Player] " + player.name + " 离开了游戏");
                }
                players_.erase(id);
            }
            client.alive = false;
            client.socket->shutdownBoth();
            client.socket->close();
            if (client.reader.joinable()) client.reader.join();
            logLine("[Net] 连接 #" + std::to_string(id) + " 已关闭");
            clients_.erase(it);
        }
    }

    void drainMessages() {
        std::deque<Message> messages;
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            messages.swap(inbox_);
        }
        for (auto& message : messages) {
            auto clientIt = clients_.find(message.clientId);
            if (clientIt == clients_.end()) continue;
            Client& client = *clientIt->second;
            if (!client.alive) continue;
            if (!message.payload.is_object()) continue;
            const json& payload = message.payload;
            std::string type = payload.value("type", "");
            if (type == "hello") {
                handleHello(client, payload);
                continue;
            }
            auto playerIt = players_.find(message.clientId);
            if (playerIt == players_.end() || !playerIt->second.joined) continue;
            Player& player = playerIt->second;
            if (type == "input") {
                MoveInput input;
                input.seq = payload.value("seq", 0u);
                input.move = std::clamp(payload.value("move", 0), -1, 1);
                input.jump = payload.value("jump", false);
                input.sneak = payload.value("sneak", false);
                input.sprint = payload.value("sprint", false);
                input.facing = payload.value("facing", 1.0f) < 0.0f ? -1.0f : 1.0f;
                if (input.seq > player.ackSeq) {
                    player.pendingInput = input;
                    player.hasInput = true;
                }
            } else if (type == "break") {
                handleBreak(player, payload.value("x", -1), payload.value("y", -1));
            } else if (type == "place") {
                handlePlace(player, payload.value("x", -1), payload.value("y", -1));
            } else if (type == "attack") {
                handleAttack(player, payload.value("id", -1));
            } else if (type == "select") {
                int slot = payload.value("slot", -1);
                if (slot >= 0 && slot < HOTBAR_SIZE) {
                    player.selected = slot;
                    sendInventory(player);
                }
            } else if (type == "slot_click") {
                handleSlotClick(player, payload.value("slot", -1), payload.value("button", 0),
                                payload.value("shift", false));
            } else if (type == "close_inventory") {
                closeInventory(player);
            } else if (type == "craft") {
                handleCraft(player, payload.value("recipe", ""));
            } else if (type == "drop") {
                handleDrop(player, payload.value("slot", -1));
            } else if (type == "eat") {
                handleEat(player, payload.value("slot", -1));
            } else if (type == "respawn") {
                respawn(player);
            } else if (type == "toggle_fly") {
                if (player.creative && !player.dead) {
                    player.flying = !player.flying;
                    player.body.flying = player.flying;
                    if (player.flying) player.body.onGround = false;
                    sendStats(player);
                }
            } else if (type == "chat") {
                handleChat(player, payload.value("text", ""));
            }
        }
    }

    void loadPlayerData() {
        std::ifstream in(playersPath_);
        if (!in) return;
        json data;
        try {
            in >> data;
        } catch (...) {
            logLine("[World] 玩家数据损坏，已忽略");
            return;
        }
        if (!data.is_object()) return;
        for (auto it = data.begin(); it != data.end(); ++it) {
            if (it.value().is_object()) savedPlayers_[it.key()] = it.value();
        }
        if (!savedPlayers_.empty()) {
            logLine("[World] 已载入 " + std::to_string(savedPlayers_.size()) + " 位玩家的数据");
        }
    }

    void savePlayerData() {
        if (playersPath_.empty()) return;
        for (const auto& [id, player] : players_) {
            (void)id;
            if (!player.joined) continue;
            json slots = json::array();
            for (const auto& stack : player.inv) slots.push_back(stackJson(stack));
            savedPlayers_[player.name] = json{
                {"x", player.body.x},
                {"y", player.body.y},
                {"health", player.health},
                {"food", player.food},
                {"air", player.air},
                {"creative", player.creative},
                {"selected", player.selected},
                {"inventory", slots},
            };
        }
        std::error_code ec;
        std::filesystem::create_directories(playersPath_.parent_path(), ec);
        std::filesystem::path temp = playersPath_;
        temp += ".tmp";
        {
            std::ofstream out(temp, std::ios::trunc);
            if (!out) return;
            json root = savedPlayers_;
            out << root.dump(2);
        }
        std::filesystem::remove(playersPath_, ec);
        std::filesystem::rename(temp, playersPath_, ec);
    }

    void applySavedPlayer(Player& player) {
        auto it = savedPlayers_.find(player.name);
        if (it == savedPlayers_.end()) return;
        const json& data = it->second;
        try {
            if (data.contains("inventory") && data["inventory"].is_array()) {
                player.inv.assign(INV_SIZE, ItemStack{});
                int index = 0;
                for (const auto& value : data["inventory"]) {
                    if (index >= INV_SIZE) break;
                    if (value.is_object()) {
                        std::string id = value.value("id", "");
                        int count = std::clamp(value.value("count", 0), 0, 6400);
                        if (itemDef(id) && count > 0) player.inv[index] = makeStack(id, count);
                    }
                    ++index;
                }
            }
            float x = data.value("x", spawnX_);
            float y = data.value("y", spawnY_);
            if (x > 1.0f && y > 1.0f && x < world_.width() - 1 && y < world_.height() - 1 &&
                !world_.collidesBox(x, y, PLAYER_WIDTH, PLAYER_HEIGHT)) {
                player.body.x = x;
                player.body.y = y;
            }
            player.health = std::clamp(data.value("health", 20), 1, 20);
            player.food = std::clamp(data.value("food", 20), 0, 20);
            player.air = std::clamp(data.value("air", 300), 0, 300);
            player.creative = data.value("creative", false);
            player.selected = std::clamp(data.value("selected", 0), 0, HOTBAR_SIZE - 1);
        } catch (...) {
            // Keep the starter state if a field is malformed.
        }
    }

    void handleHello(Client& client, const json& payload) {
        if (client.joined) return;
        int version = payload.value("version", 0);
        if (version != 1) {
            sendDenied(client, "客户端版本不匹配，请使用同一版本的游戏");
            return;
        }
        std::string rawName = payload.value("name", "Steve");
        std::string name = sanitizeName(rawName);
        for (auto& [id, p] : players_) {
            if (p.name == name) {
                int suffix = 2;
                while (true) {
                    bool taken = false;
                    std::string candidate = name + std::to_string(suffix++);
                    for (auto& [otherId, other] : players_)
                        if (other.name == candidate) taken = true;
                    if (!taken) {
                        name = candidate;
                        break;
                    }
                }
            }
        }
        client.joined = true;
        Player player;
        player.id = client.id;
        player.name = name;
        player.inv.assign(INV_SIZE, ItemStack{});
        player.inv[0] = makeStack("wooden_pickaxe", 1);
        player.inv[1] = makeStack("wooden_axe", 1);
        player.inv[2] = makeStack("crafting_table", 1);
        player.inv[3] = makeStack("torch", 16);
        player.inv[4] = makeStack("oak_planks", 32);
        player.inv[5] = makeStack("dirt", 16);
        player.inv[6] = makeStack("cobblestone", 16);
        player.inv[7] = makeStack("bread", 4);
        player.inv[8] = makeStack("wooden_sword", 1);
        player.selected = 0;
        player.body.x = spawnX_;
        player.body.y = spawnY_;
        player.spawnX = spawnX_;
        player.spawnY = spawnY_;
        applySavedPlayer(player);
        player.body.flying = player.creative;
        player.joined = true;
        player.joinedTick = static_cast<int>(tick_);
        players_[client.id] = player;

        json welcome = {
            {"type", "welcome"},
            {"version", 1},
            {"id", client.id},
            {"name", name},
            {"seed", world_.seed()},
            {"width", world_.width()},
            {"height", world_.height()},
            {"time", time_},
            {"tick", tick_},
            {"spawn", {{"x", spawnX_}, {"y", spawnY_}}},
        };
        sendTo(client, welcome);

        json worldMessage = {
            {"type", "world"},
            {"width", world_.width()},
            {"height", world_.height()},
            {"fg", world_.encodeFg()},
            {"bg", world_.encodeBg()},
        };
        if (!sendTo(client, worldMessage)) return;

        sendInventory(players_[client.id]);
        sendStats(players_[client.id]);
        logLine("[Player] " + name + " 加入了游戏 (#" + std::to_string(client.id) + ")");
        broadcastChat("系统", name + " 加入了游戏");
        json toast = {{"type", "toast"}, {"text", "欢迎来到 Minecraft 2D！"}, {"sub", "按 E 打开背包，按 T 聊天"}};
        sendTo(client, toast);
    }

    void sendDenied(Client& client, const std::string& reason) {
        json message = {{"type", "denied"}, {"reason", reason}};
        sendTo(client, message);
        client.alive = false;
    }

    std::string sanitizeName(const std::string& input) {
        std::string name = input;
        if (name.size() > 24) name.resize(24);
        for (char& c : name) {
            if (c == '\n' || c == '\r' || c == '\t' || c == '"' || c == '\\') c = '_';
        }
        while (!name.empty() && name.front() == ' ') name.erase(name.begin());
        while (!name.empty() && name.back() == ' ') name.pop_back();
        if (name.empty()) name = "Steve";
        return name;
    }

    void findSpawn() {
        int center = world_.width() / 2;
        for (int offset = 0; offset < world_.width() / 2; ++offset) {
            for (int sign : {1, -1}) {
                int x = center + offset * sign;
                if (x < 2 || x >= world_.width() - 2) continue;
                int y = world_.surfaceY(x);
                if (y <= 1 || y >= world_.height() - 4) continue;
                if (world_.fg(x, y) == WATER || world_.fg(x, y) == LAVA) continue;
                bool clear = true;
                for (int dy = 1; dy <= 3; ++dy) {
                    if (world_.solid(x, y - dy)) clear = false;
                }
                if (!clear) continue;
                spawnX_ = static_cast<float>(x) + 0.5f;
                spawnY_ = static_cast<float>(y);
                return;
            }
        }
        spawnX_ = static_cast<float>(center) + 0.5f;
        spawnY_ = 60.0f;
    }

    void tick() {
        ++tick_;
        time_ = (time_ + 1) % 24000;

        for (auto& [id, player] : players_) {
            (void)id;
            if (player.dead) continue;
            MoveInput input{};
            if (player.hasInput) {
                input = player.pendingInput;
                player.hasInput = false;
                player.ackSeq = input.seq;
            } else {
                input.facing = static_cast<float>(player.body.facing);
            }
            tickPlayer(world_, player.body, input, player.creative);
            if (player.flying) player.body.flying = true;
            if (player.body.justLanded && !player.creative) {
                float fall = player.body.landingFall;
                if (fall > 3.0f) {
                    hurtPlayer(player, static_cast<int>(std::floor(fall - 3.0f)), "摔了一跤");
                }
            }
            updatePlayerEnvironment(player);
            updatePlayerCooldowns(player);
            if (tick_ % 3 == 0) updateHunger(player);
        }

        updateEntities();
        maybeSpawnMonsters();

        if (tick_ % SNAPSHOT_INTERVAL == 0) broadcastSnapshot();
        if (tick_ % 600 == 0) {
            saveWorld();
            savePlayerData();
        }
    }

    void updatePlayerCooldowns(Player& player) {
        if (player.attackCooldown > 0) --player.attackCooldown;
        if (player.useCooldown > 0) --player.useCooldown;
        if (player.hurtCooldown > 0) --player.hurtCooldown;
        if (player.body.justLanded && !player.creative) {
            player.body.landingFall = 0.0f;
        }
        if (player.health < 20 && player.food >= 18 && !player.dead) {
            if (++player.regenTick >= 80) {
                player.regenTick = 0;
                player.health = std::min(20, player.health + 1);
                player.food = std::max(0, player.food - 1);
                sendStats(player);
            }
        } else {
            player.regenTick = 0;
        }
        if (player.food <= 0 && !player.dead) {
            if (++player.starveTick >= 160) {
                player.starveTick = 0;
                hurtPlayer(player, 1, "饿死了");
            }
        } else {
            player.starveTick = 0;
        }
        if (world_.boxInLava(player.body.x, player.body.y, PLAYER_WIDTH, player.body.height())) {
            if (++player.lavaTick >= 10) {
                player.lavaTick = 0;
                hurtPlayer(player, 4, "被熔岩烧死了");
            }
        } else {
            player.lavaTick = 0;
        }
        if (player.body.y > world_.height() + 2) {
            hurtPlayer(player, 4, "掉出了世界");
            if (!player.dead) {
                player.body.x = player.spawnX;
                player.body.y = player.spawnY;
                player.body.vx = player.body.vy = 0.0f;
            }
        }
    }

    void updatePlayerEnvironment(Player& player) {
        if (player.creative || player.dead) return;
        bool headInWater = world_.water(static_cast<int>(std::floor(player.body.x)),
                                        static_cast<int>(std::floor(player.body.eyeY())));
        if (headInWater) {
            player.air = std::max(0, player.air - 1);
            if (player.air <= 0) {
                if (++player.drownTick >= 20) {
                    player.drownTick = 0;
                    hurtPlayer(player, 2, "溺水了");
                }
            }
        } else {
            player.air = std::min(300, player.air + 6);
            player.drownTick = 0;
        }
    }

    void updateHunger(Player& player) {
        if (player.creative || player.dead) return;
        player.foodTick += player.body.sprinting ? 3 : 1;
        int limit = player.body.sprinting ? 90 : 300;
        if (player.foodTick >= limit) {
            player.foodTick = 0;
            player.food = std::max(0, player.food - 1);
        }
    }

    int findGroundY(int x) {
        for (int y = 1; y < world_.height() - 3; ++y) {
            if (world_.solid(x, y) && !world_.solid(x, y - 1) && !world_.solid(x, y - 2)) return y;
        }
        return -1;
    }

    void seedPigs() {
        int count = 10;
        for (int i = 0; i < count; ++i) {
            for (int tries = 0; tries < 100; ++tries) {
                int x = 20 + static_cast<int>(rng_() % (world_.width() - 40));
                int ground = findGroundY(x);
                if (ground < 0) continue;
                if (world_.fg(x, ground) == WATER) continue;
                if (std::abs(x - static_cast<int>(spawnX_)) < 10) continue;
                Entity pig;
                pig.id = nextEntityId_++;
                pig.type = "pig";
                pig.x = static_cast<float>(x) + 0.5f;
                pig.y = static_cast<float>(ground);
                pig.health = pig.maxHealth = 10;
                pig.wander = static_cast<int>(rng_() % 100);
                pig.dir = (rng_() & 1) ? 1 : -1;
                entities_.push_back(pig);
                break;
            }
        }
    }

    void maybeSpawnMonsters() {
        if (players_.empty()) return;
        uint64_t dayTime = time_ % 24000;
        bool night = dayTime > 13000 && dayTime < 23000;
        if (!night) return;
        int zombies = 0;
        for (const auto& e : entities_) if (e.type == "zombie") ++zombies;
        if (zombies >= 10) return;
        if (rng_() % 100 > 24) return;

        std::vector<Player*> alive;
        for (auto& [id, player] : players_) {
            (void)id;
            if (!player.dead) alive.push_back(&player);
        }
        if (alive.empty()) return;
        Player* target = alive[rng_() % alive.size()];
        int side = (rng_() & 1) ? 1 : -1;
        int distance = 18 + static_cast<int>(rng_() % 25);
        int x = static_cast<int>(target->body.x) + side * distance;
        x = std::clamp(x, 4, world_.width() - 5);
        int ground = findGroundY(x);
        if (ground < 0) return;
        if (std::abs(ground - static_cast<int>(target->body.y)) > 18) return;
        Entity zombie;
        zombie.id = nextEntityId_++;
        zombie.type = "zombie";
        zombie.x = static_cast<float>(x) + 0.5f;
        zombie.y = static_cast<float>(ground);
        zombie.health = zombie.maxHealth = 20;
        zombie.facing = -side;
        zombie.dir = -side;
        zombie.wander = 200;
        entities_.push_back(zombie);
    }

    float entityWidth(const Entity& e) const {
        if (e.type == "zombie") return PLAYER_WIDTH;
        if (e.type == "pig") return 0.9f;
        return 0.25f;
    }

    float entityHeight(const Entity& e) const {
        if (e.type == "zombie") return 1.95f;
        if (e.type == "pig") return 0.9f;
        return 0.25f;
    }

    bool moveEntity(Entity& e, float dt) {
        const float w = entityWidth(e);
        const float h = entityHeight(e);
        bool blockedX = false;
        bool landed = false;

        float nx = e.x + e.vx * dt;
        if (!world_.collidesBox(nx, e.y, w, h)) e.x = nx;
        else {
            blockedX = true;
            e.vx = 0.0f;
        }

        e.vy += 28.0f * dt;
        e.vy = std::min(e.vy, 60.0f);
        float ny = e.y + e.vy * dt;
        if (!world_.collidesBox(e.x, ny, w, h)) {
            e.y = ny;
            e.onGround = false;
        } else {
            if (e.vy > 0.0f) {
                e.onGround = true;
                landed = true;
            }
            e.vy = 0.0f;
        }
        if (e.x < 1.0f) { e.x = 1.0f; e.vx = 0.0f; }
        if (e.x > world_.width() - 1.0f) { e.x = world_.width() - 1.0f; e.vx = 0.0f; }
        if (e.y > world_.height() + 4.0f) e.y = world_.height() + 4.0f;
        return blockedX && landed;
    }

    void updateEntities() {
        std::vector<int> removeIds;
        for (auto& e : entities_) {
            if (e.hurt > 0) --e.hurt;
            if (e.age < 1000000) ++e.age;

            if (e.type == "item") {
                e.vx *= 0.88f;
                moveEntity(e, TICK_SECONDS);
                if (e.onGround) e.vx *= 0.70f;
                if (e.age > 10) {
                    for (auto& [pid, player] : players_) {
                        (void)pid;
                        if (player.dead) continue;
                        float dx = player.body.x - e.x;
                        float dy = player.body.centerY() - (e.y - entityHeight(e) * 0.5f);
                        if (dx * dx + dy * dy < 1.6f * 1.6f) {
                            if (canAddItem(player.inv, e.item, e.count)) {
                                addItem(player.inv, e.item, e.count);
                                json toast = {{"type", "toast"},
                                              {"text", "+ " + std::to_string(e.count) + " " + itemName(e.item)},
                                              {"sub", "物品已放入背包"}};
                                sendTo(player.id, toast);
                                sendInventory(player);
                                removeIds.push_back(e.id);
                                break;
                            }
                        }
                    }
                }
                if (e.age > 20 * 60 * 5) removeIds.push_back(e.id);
                continue;
            }

            if (e.type == "pig") {
                if (e.hurt > 0) {
                    // flee briefly
                    e.wander = 30;
                    e.dir = (e.vx >= 0.0f) ? 1 : -1;
                }
                if (--e.wander <= 0) {
                    e.wander = 60 + static_cast<int>(rng_() % 160);
                    int r = static_cast<int>(rng_() % 5);
                    e.dir = (r == 0) ? 0 : ((r <= 2) ? -1 : 1);
                    if (r == 0) e.dir = 0;
                }
                float targetVx = static_cast<float>(e.dir) * (e.hurt > 0 ? 2.4f : 1.1f);
                e.vx += std::clamp(targetVx - e.vx, -0.15f, 0.15f);
                bool blocked = moveEntity(e, TICK_SECONDS);
                (void)blocked;
                if (e.onGround && std::abs(e.vx) < 0.05f && e.dir != 0) {
                    e.vy = -7.3f;
                }
                if (e.y > world_.height()) removeIds.push_back(e.id);
                continue;
            }

            if (e.type == "zombie") {
                Player* closest = nullptr;
                float best = 32.0f * 32.0f;
                for (auto& [pid, player] : players_) {
                    (void)pid;
                    if (player.dead) continue;
                    float dx = player.body.x - e.x;
                    float dy = player.body.y - e.y;
                    float dist = dx * dx + dy * dy;
                    if (dist < best) {
                        best = dist;
                        closest = &player;
                    }
                }
                if (closest) {
                    float dx = closest->body.x - e.x;
                    float dy = (closest->body.y - 0.9f) - (e.y - 0.95f);
                    int dir = dx > 0.05f ? 1 : (dx < -0.05f ? -1 : 0);
                    e.dir = dir;
                    e.facing = dir == 0 ? e.facing : dir;
                    e.vx += std::clamp(static_cast<float>(dir) * 1.9f - e.vx, -0.2f, 0.2f);
                    bool blocked = moveEntity(e, TICK_SECONDS);
                    (void)blocked;
                    if (e.onGround && dir != 0 && std::abs(e.vx) < 0.08f) e.vy = -7.6f;
                    if (e.onGround && std::abs(dy) > 1.2f && dir != 0) {
                        bool wallAhead = world_.solid(static_cast<int>(std::floor(e.x + dir * 0.8f)),
                                                      static_cast<int>(std::floor(e.y - 0.5f)));
                        if (wallAhead) e.vy = -8.0f;
                    }
                    if (e.attackCooldown > 0) --e.attackCooldown;
                    if (std::abs(dx) < 1.15f && std::abs(dy) < 1.5f && e.attackCooldown <= 0) {
                        e.attackCooldown = 20;
                        hurtPlayer(*closest, 3, "被僵尸击倒了");
                        closest->body.vx += dir * 6.0f;
                        closest->body.vy = -4.0f;
                        closest->body.onGround = false;
                        json msg = {{"type", "event"}, {"kind", "player_hurt"}, {"by", "zombie"}};
                        sendTo(closest->id, msg);
                    }
                } else {
                    if (--e.wander <= 0) {
                        e.wander = 80 + static_cast<int>(rng_() % 120);
                        e.dir = (rng_() % 3 == 0) ? 0 : ((rng_() & 1) ? 1 : -1);
                    }
                    e.vx += std::clamp(static_cast<float>(e.dir) * 0.7f - e.vx, -0.1f, 0.1f);
                    moveEntity(e, TICK_SECONDS);
                }
                if (e.y > world_.height()) removeIds.push_back(e.id);
                continue;
            }
        }

        if (!removeIds.empty()) {
            entities_.erase(std::remove_if(entities_.begin(), entities_.end(), [&](const Entity& e) {
                                return std::find(removeIds.begin(), removeIds.end(), e.id) != removeIds.end();
                            }),
                            entities_.end());
        }
    }

    Entity* findEntity(int id) {
        for (auto& e : entities_) if (e.id == id) return &e;
        return nullptr;
    }

    void spawnItem(const std::string& item, int count, float x, float y, float vx, float vy) {
        if (item.empty() || count <= 0) return;
        Entity e;
        e.id = nextEntityId_++;
        e.type = "item";
        e.item = item;
        e.count = count;
        e.x = x;
        e.y = y;
        e.vx = vx;
        e.vy = vy;
        e.health = 1;
        e.age = 0;
        entities_.push_back(e);
    }

    bool withinReach(const Player& player, int x, int y, float reach = 5.6f) const {
        float bx = static_cast<float>(x) + 0.5f;
        float by = static_cast<float>(y) + 0.5f;
        float dx = bx - player.body.x;
        float dy = by - player.body.eyeY();
        return dx * dx + dy * dy <= reach * reach;
    }

    void handleBreak(Player& player, int x, int y) {
        if (player.dead) return;
        if (!withinReach(player, x, y)) return;
        BlockId block = world_.fg(x, y);
        if (block == AIR || block == WATER || block == LAVA) return;
        if (block == BEDROCK && !player.creative) return;
        const BlockDef& def = blockDef(block);
        if (def.hardness < 0 && !player.creative) return;
        world_.setFg(x, y, AIR);
        worldChangeDirty_ = true;
        broadcastBlock(x, y);
        json event = {{"type", "event"}, {"kind", "block_break"}, {"x", x}, {"y", y}, {"block", block}};
        broadcast(event);
        if (!player.creative && def.drop && *def.drop) {
            int count = def.dropMin;
            if (def.dropMax > def.dropMin) {
                count = def.dropMin + static_cast<int>(rng_() % (def.dropMax - def.dropMin + 1));
            }
            if (count > 0) {
                spawnItem(def.drop, count, static_cast<float>(x) + 0.5f, static_cast<float>(y) + 0.5f,
                          (static_cast<float>(rng_() % 100) / 100.0f - 0.5f) * 3.0f,
                          -2.0f - static_cast<float>(rng_() % 100) / 50.0f);
            }
        }
    }

    bool intersectsPlayer(const Player& player, int x, int y) const {
        float left = static_cast<float>(x);
        float right = left + 1.0f;
        float top = static_cast<float>(y);
        float bottom = top + 1.0f;
        float px0 = player.body.x - PLAYER_WIDTH * 0.5f;
        float px1 = player.body.x + PLAYER_WIDTH * 0.5f;
        float py0 = player.body.y - player.body.height();
        float py1 = player.body.y;
        return px0 < right && px1 > left && py0 < bottom && py1 > top;
    }

    void handlePlace(Player& player, int x, int y) {
        if (player.dead) return;
        if (player.selected < 0 || player.selected >= HOTBAR_SIZE) return;
        ItemStack& stack = player.inv[player.selected];
        if (stack.empty()) return;
        BlockId block = AIR;
        if (!isBlockItem(stack.id, &block)) return;
        if (!withinReach(player, x, y)) return;
        if (!world_.inBounds(x, y)) return;
        BlockId current = world_.fg(x, y);
        if (!isReplaceableBlock(current)) return;
        const BlockDef& def = blockDef(block);
        if (!def.solid && !def.cutout) return;
        bool supported = false;
        if (world_.bg(x, y) != AIR) supported = true;
        for (const auto& d : {std::pair{1, 0}, {-1, 0}, {0, 1}, {0, -1}}) {
            if (world_.solid(x + d.first, y + d.second)) supported = true;
        }
        if (def.cutout && !world_.solid(x, y + 1)) supported = false;
        if (!supported) return;
        for (auto& [id, other] : players_) {
            (void)id;
            if (!other.dead && intersectsPlayer(other, x, y)) return;
        }
        if (!player.creative) {
            --stack.count;
            if (stack.count <= 0) stack.clear();
        }
        world_.setFg(x, y, block);
        worldChangeDirty_ = true;
        broadcastBlock(x, y);
        json event = {{"type", "event"}, {"kind", "block_place"}, {"x", x}, {"y", y}, {"block", block}};
        broadcast(event);
        sendInventory(player);
    }

    void handleAttack(Player& player, int entityId) {
        if (player.dead || player.attackCooldown > 0) return;
        Entity* e = findEntity(entityId);
        if (!e || e->type == "item") return;
        float dx = e->x - player.body.x;
        float dy = (e->y - entityHeight(*e) * 0.5f) - player.body.centerY();
        if (dx * dx + dy * dy > 4.0f * 4.0f) return;
        player.attackCooldown = 10;
        std::string held = (player.selected >= 0 && player.selected < INV_SIZE)
                               ? player.inv[player.selected].id
                               : "";
        int damage = attackDamage(held);
        e->health -= damage;
        e->hurt = 10;
        e->vx += (dx >= 0 ? 1.0f : -1.0f) * 5.0f;
        e->vy = -3.5f;
        e->onGround = false;
        json event = {{"type", "event"}, {"kind", "mob_hurt"}, {"id", e->id},
                      {"x", e->x}, {"y", e->y}, {"damage", damage}};
        broadcast(event);
        if (e->health <= 0) {
            int x = static_cast<int>(std::floor(e->x));
            int y = static_cast<int>(std::floor(e->y));
            (void)x;
            (void)y;
            if (e->type == "pig") {
                spawnItem("porkchop", 1 + static_cast<int>(rng_() % 2), e->x, e->y - 0.3f,
                          (static_cast<float>(rng_() % 100) / 100.0f - 0.5f) * 2.5f, -3.0f);
            }
            json death = {{"type", "event"}, {"kind", "mob_death"}, {"id", e->id},
                          {"x", e->x}, {"y", e->y}, {"mob", e->type}};
            broadcast(death);
            int deadId = e->id;
            entities_.erase(std::remove_if(entities_.begin(), entities_.end(),
                                           [&](const Entity& entity) { return entity.id == deadId; }),
                            entities_.end());
        }
    }

    void handleSlotClick(Player& player, int slot, int button, bool shift) {
        if (player.dead || slot < 0 || slot >= INV_SIZE) return;
        ItemStack& target = player.inv[slot];
        if (shift) {
            if (target.empty()) return;
            const int begin = slot < HOTBAR_SIZE ? HOTBAR_SIZE : 0;
            const int end = slot < HOTBAR_SIZE ? INV_SIZE : HOTBAR_SIZE;
            int count = target.count;
            std::string id = target.id;
            const int limit = itemDef(id) ? itemDef(id)->maxStack : 64;
            for (int i = begin; i < end && count > 0; ++i) {
                ItemStack& dst = player.inv[i];
                if (!dst.empty() && dst.id == id && dst.count < limit) {
                    int moved = std::min(count, limit - dst.count);
                    dst.count += moved;
                    count -= moved;
                }
            }
            for (int i = begin; i < end && count > 0; ++i) {
                ItemStack& dst = player.inv[i];
                if (dst.empty()) {
                    int moved = std::min(count, limit);
                    dst.id = id;
                    dst.count = moved;
                    count -= moved;
                }
            }
            if (count <= 0) target.clear();
            else target.count = count;
            sendInventory(player);
            return;
        }
        if (button == 0) {
            if (player.cursor.empty()) {
                player.cursor = target;
                target.clear();
            } else if (target.empty()) {
                target = player.cursor;
                player.cursor.clear();
            } else if (target.id == player.cursor.id) {
                const int limit = itemDef(target.id) ? itemDef(target.id)->maxStack : 64;
                int moved = std::min(player.cursor.count, limit - target.count);
                target.count += moved;
                player.cursor.count -= moved;
                if (player.cursor.count <= 0) player.cursor.clear();
            } else {
                std::swap(target, player.cursor);
            }
        } else if (button == 1) {
            if (player.cursor.empty()) {
                if (target.empty()) return;
                int take = (target.count + 1) / 2;
                player.cursor.id = target.id;
                player.cursor.count = take;
                target.count -= take;
                if (target.count <= 0) target.clear();
            } else {
                if (target.empty()) {
                    target.id = player.cursor.id;
                    target.count = 1;
                    if (--player.cursor.count <= 0) player.cursor.clear();
                } else if (target.id == player.cursor.id) {
                    const int limit = itemDef(target.id) ? itemDef(target.id)->maxStack : 64;
                    if (target.count < limit) {
                        ++target.count;
                        if (--player.cursor.count <= 0) player.cursor.clear();
                    }
                }
            }
        }
        sendInventory(player);
    }

    void closeInventory(Player& player) {
        if (!player.cursor.empty()) {
            int remaining = addItem(player.inv, player.cursor.id, player.cursor.count);
            if (remaining > 0) {
                spawnItem(player.cursor.id, remaining, player.body.x, player.body.y - 1.0f,
                          0.0f, -2.0f);
            }
            player.cursor.clear();
        }
        sendInventory(player);
    }

    void handleCraft(Player& player, const std::string& recipeId) {
        if (player.dead) return;
        for (const auto& recipe : allRecipes()) {
            if (recipe.id != recipeId) continue;
            for (const auto& [item, count] : recipe.inputs) {
                if (countItem(player.inv, item) < count) {
                    sendSystem(player, "材料不足");
                    return;
                }
            }
            if (!canAddItem(player.inv, recipe.output, recipe.outputCount)) {
                sendSystem(player, "背包已满");
                return;
            }
            for (const auto& [item, count] : recipe.inputs) removeItem(player.inv, item, count);
            addItem(player.inv, recipe.output, recipe.outputCount);
            sendInventory(player);
            json toast = {{"type", "toast"},
                          {"text", "合成 " + recipe.name + " x" + std::to_string(recipe.outputCount)},
                          {"sub", "配方解锁"}};
            sendTo(player.id, toast);
            return;
        }
    }

    void handleDrop(Player& player, int slot) {
        if (slot < 0 || slot >= INV_SIZE || player.dead) return;
        ItemStack& stack = player.inv[slot];
        if (stack.empty()) return;
        std::string id = stack.id;
        --stack.count;
        if (stack.count <= 0) stack.clear();
        spawnItem(id, 1, player.body.x + (player.body.facing > 0 ? 0.5f : -0.5f),
                  player.body.y - 1.1f, player.body.facing * 2.0f, -2.0f);
        sendInventory(player);
    }

    void handleEat(Player& player, int slot) {
        if (player.dead || slot < 0 || slot >= INV_SIZE) return;
        ItemStack& stack = player.inv[slot];
        if (stack.empty()) return;
        const ItemDef* def = itemDef(stack.id);
        if (!def || def->food <= 0) return;
        if (player.food >= 20) {
            sendSystem(player, "现在还不饿");
            return;
        }
        if (player.useCooldown > 0) return;
        player.useCooldown = 12;
        player.food = std::min(20, player.food + def->food);
        player.health = std::min(20, player.health + std::max(0, def->food - 4));
        --stack.count;
        if (stack.count <= 0) stack.clear();
        sendInventory(player);
        sendStats(player);
        json event = {{"type", "event"}, {"kind", "eat"}, {"id", player.id}};
        sendTo(player.id, event);
    }

    void hurtPlayer(Player& player, int damage, const std::string& reason) {
        if (player.dead || player.creative || damage <= 0) return;
        if (player.hurtCooldown > 0) return;
        player.hurtCooldown = 10;
        player.lastHurtTick = static_cast<int>(tick_);
        player.health -= damage;
        if (player.health <= 0) {
            player.health = 0;
            player.dead = true;
            player.flying = false;
            player.body.flying = false;
            if (!player.cursor.empty()) {
                addItem(player.inv, player.cursor.id, player.cursor.count);
                player.cursor.clear();
            }
            sendInventory(player);
            sendStats(player);
            json death = {{"type", "event"}, {"kind", "player_death"}, {"id", player.id}, {"reason", reason}};
            broadcast(death);
            broadcastChat("系统", player.name + " " + reason);
            logLine("[Player] " + player.name + " " + reason);
        } else {
            sendStats(player);
        }
    }

    void respawn(Player& player) {
        if (!player.dead) return;
        player.dead = false;
        player.health = 20;
        player.food = 20;
        player.air = 300;
        player.flying = false;
        player.body = PlayerBody{};
        player.body.x = player.spawnX;
        player.body.y = player.spawnY;
        player.body.facing = 1;
        player.ackSeq = player.pendingInput.seq;
        sendStats(player);
        json respawned = {{"type", "respawned"},
                          {"x", player.spawnX}, {"y", player.spawnY}, {"ack", player.ackSeq}};
        sendTo(player.id, respawned);
        sendInventory(player);
    }

    void handleChat(Player& player, const std::string& raw) {
        std::string text = raw;
        if (text.size() > 500) text.resize(500);
        while (!text.empty() && (text.back() == '\n' || text.back() == '\r')) text.pop_back();
        if (text.empty()) return;
        if (text[0] == '/') {
            executeCommand(player, text);
        } else {
            broadcastChat(player.name, text);
        }
    }

    std::vector<std::string> splitWords(const std::string& text) {
        std::vector<std::string> words;
        std::istringstream stream(text);
        std::string word;
        while (stream >> word) words.push_back(word);
        return words;
    }

    void executeCommand(Player& player, const std::string& text) {
        std::vector<std::string> words = splitWords(text.substr(1));
        if (words.empty()) return;
        std::string command = words[0];
        auto arg = [&](size_t index, const std::string& fallback = "") {
            return index < words.size() ? words[index] : fallback;
        };
        if (command == "help") {
            sendSystem(player, "/time set day|night  /gamemode creative|survival");
            sendSystem(player, "/give <物品> [数量]  /tp <x> <y>  /spawn  /kill");
            sendSystem(player, "/seed  /list  /save  /clear  /stop  （聊天键 T）");
        } else if (command == "time" && arg(1) == "set") {
            std::string value = arg(2);
            if (value == "day") time_ = 1000;
            else if (value == "noon") time_ = 6000;
            else if (value == "night") time_ = 18000;
            else if (value == "midnight") time_ = 23000;
            else {
                sendSystem(player, "用法: /time set day|noon|night|midnight");
                return;
            }
            broadcastChat("系统", player.name + " 将时间设置为 " + value);
            broadcastSnapshot();
        } else if (command == "gamemode" || command == "gm") {
            std::string mode = arg(1);
            if (mode == "creative" || mode == "c" || mode == "1") {
                player.creative = true;
                player.flying = true;
                player.body.flying = true;
                sendSystem(player, "游戏模式已设置为创造模式");
            } else if (mode == "survival" || mode == "s" || mode == "0") {
                player.creative = false;
                player.flying = false;
                player.body.flying = false;
                sendSystem(player, "游戏模式已设置为生存模式");
            } else {
                sendSystem(player, "用法: /gamemode creative|survival");
                return;
            }
            sendStats(player);
        } else if (command == "give") {
            std::string id = arg(1);
            int count = 1;
            if (!arg(2).empty()) {
                try { count = std::clamp(std::stoi(arg(2)), 1, 6400); } catch (...) { count = 1; }
            }
            const ItemDef* def = itemDef(id);
            if (!def) {
                sendSystem(player, "未知物品: " + id);
                return;
            }
            int left = addItem(player.inv, id, count);
            if (left > 0) spawnItem(id, left, player.body.x, player.body.y - 1.0f, 0.0f, -1.0f);
            sendInventory(player);
            sendSystem(player, "已给予 " + std::to_string(count - left) + " 个 " + def->name);
        } else if (command == "tp") {
            float x = 0.0f, y = 0.0f;
            try {
                x = std::stof(arg(1));
                y = std::stof(arg(2));
            } catch (...) {
                sendSystem(player, "用法: /tp <x> <y>");
                return;
            }
            player.body.x = std::clamp(x, 1.0f, static_cast<float>(world_.width()) - 1.0f);
            player.body.y = std::clamp(y, 1.0f, static_cast<float>(world_.height()) - 1.0f);
            player.body.vx = player.body.vy = 0.0f;
            player.pendingInput.seq = 0;
            player.hasInput = false;
            sendSystem(player, "已传送");
        } else if (command == "spawn") {
            player.body.x = spawnX_;
            player.body.y = spawnY_;
            player.body.vx = player.body.vy = 0.0f;
            player.hasInput = false;
            sendSystem(player, "已回到出生点");
        } else if (command == "kill") {
            player.hurtCooldown = 0;
            player.creative = false;
            player.flying = false;
            player.body.flying = false;
            hurtPlayer(player, 1000, "结束了自己的生命");
        } else if (command == "seed") {
            sendSystem(player, "世界种子: " + std::to_string(world_.seed()));
        } else if (command == "save") {
            saveWorld(true);
            sendSystem(player, "世界已保存");
        } else if (command == "list") {
            std::string list;
            for (auto& [id, p] : players_) {
                (void)id;
                if (!list.empty()) list += ", ";
                list += p.name;
            }
            sendSystem(player, "在线玩家 (" + std::to_string(players_.size()) + "): " + list);
        } else if (command == "clear") {
            player.inv.assign(INV_SIZE, ItemStack{});
            player.cursor.clear();
            sendInventory(player);
            sendSystem(player, "已清空背包");
        } else if (command == "stop") {
            broadcastChat("系统", "服务器正在关闭...");
            g_running = false;
        } else if (command == "say") {
            std::string message = text.substr(text.find("say") + 3);
            while (!message.empty() && message.front() == ' ') message.erase(message.begin());
            broadcastChat("系统", message);
        } else {
            sendSystem(player, "未知指令: /" + command + "，输入 /help 查看帮助");
        }
    }

    void handleConsoleCommand(const std::string& raw) {
        std::string line = raw;
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        if (line.empty()) return;
        std::vector<std::string> words = splitWords(line);
        std::string command = words[0];
        if (command == "stop" || command == "shutdown") {
            logLine("[Server] 收到关闭指令");
            g_running = false;
        } else if (command == "save") {
            saveWorld(true);
        } else if (command == "help") {
            logLine("控制台指令: stop | save | list | time set day|night | say <消息> | seed");
        } else if (command == "list") {
            std::string list;
            for (auto& [id, p] : players_) {
                (void)id;
                if (!list.empty()) list += ", ";
                list += p.name;
            }
            logLine("在线玩家 (" + std::to_string(players_.size()) + "): " + list);
        } else if (command == "seed") {
            logLine("世界种子: " + std::to_string(world_.seed()));
        } else if (command == "time" && words.size() >= 3 && words[1] == "set") {
            if (words[2] == "day") time_ = 1000;
            else if (words[2] == "noon") time_ = 6000;
            else if (words[2] == "night") time_ = 18000;
            else if (words[2] == "midnight") time_ = 23000;
            broadcastChat("系统", "时间已设置为 " + words[2]);
            logLine("[Server] 时间已设置为 " + words[2]);
        } else if (command == "say") {
            std::string message = line.substr(line.find("say") + 3);
            while (!message.empty() && message.front() == ' ') message.erase(message.begin());
            broadcastChat("系统", message);
        } else {
            logLine("未知控制台指令: " + command);
        }
    }

    void saveWorld(bool announce = false) {
        std::error_code ec;
        std::filesystem::create_directories(worldPath_.parent_path(), ec);
        bool ok = world_.save(worldPath_.string(), time_);
        if (ok) {
            lastSaveTick_ = tick_;
            worldChangeDirty_ = false;
            if (announce) logLine("[World] 世界已保存");
        } else {
            logLine("[World] 保存失败: " + worldPath_.string());
        }
    }

    void broadcastChat(const std::string& from, const std::string& text) {
        json message = {{"type", "chat"}, {"from", from}, {"text", text}};
        broadcast(message);
    }

    void sendSystem(Player& player, const std::string& text) {
        json message = {{"type", "chat"}, {"from", "系统"}, {"text", text}};
        sendTo(player.id, message);
    }

    json stackJson(const ItemStack& stack) const {
        if (stack.empty()) return nullptr;
        return json{{"id", stack.id}, {"count", stack.count}};
    }

    void sendInventory(Player& player) {
        json slots = json::array();
        for (const auto& stack : player.inv) slots.push_back(stackJson(stack));
        json message = {
            {"type", "inventory"},
            {"slots", slots},
            {"selected", player.selected},
            {"cursor", stackJson(player.cursor)},
        };
        sendTo(player.id, message);
    }

    json playerJson(const Player& player) const {
        return json{
            {"id", player.id},
            {"name", player.name},
            {"x", player.body.x},
            {"y", player.body.y},
            {"vx", player.body.vx},
            {"vy", player.body.vy},
            {"onGround", player.body.onGround},
            {"facing", player.body.facing},
            {"sneak", player.body.sneaking},
            {"sprint", player.body.sprinting},
            {"health", player.health},
            {"dead", player.dead},
            {"creative", player.creative},
        };
    }

    json entityJson(const Entity& e) const {
        json j{
            {"id", e.id},
            {"type", e.type},
            {"x", e.x},
            {"y", e.y},
            {"vx", e.vx},
            {"vy", e.vy},
            {"facing", e.facing},
            {"hurt", e.hurt},
            {"health", e.health},
            {"maxHealth", e.maxHealth},
        };
        if (e.type == "item") {
            j["item"] = e.item;
            j["count"] = e.count;
        }
        return j;
    }

    void sendSelfState(Player& player) {
        json self = {
            {"ack", player.ackSeq},
            {"x", player.body.x},
            {"y", player.body.y},
            {"vx", player.body.vx},
            {"vy", player.body.vy},
            {"onGround", player.body.onGround},
            {"sneaking", player.body.sneaking},
            {"sprinting", player.body.sprinting},
            {"flying", player.flying},
            {"facing", player.body.facing},
            {"health", player.health},
            {"food", player.food},
            {"air", player.air},
            {"dead", player.dead},
            {"creative", player.creative},
            {"selected", player.selected},
        };
        json message = {{"type", "self"}, {"self", self}, {"tick", tick_}, {"time", time_}};
        sendTo(player.id, message);
    }

    void broadcastSnapshot() {
        json players = json::array();
        for (auto& [id, player] : players_) {
            (void)id;
            players.push_back(playerJson(player));
        }
        json entities = json::array();
        for (const auto& e : entities_) entities.push_back(entityJson(e));
        json snapshot = {
            {"type", "snapshot"},
            {"tick", tick_},
            {"time", time_},
            {"players", players},
            {"entities", entities},
        };
        for (auto& [id, player] : players_) {
            (void)id;
            sendTo(player.id, snapshot);
            sendSelfState(player);
        }
    }

    void sendStats(Player& player) {
        json stats = {
            {"type", "stats"},
            {"health", player.health},
            {"food", player.food},
            {"air", player.air},
            {"dead", player.dead},
            {"creative", player.creative},
            {"flying", player.flying},
            {"selected", player.selected},
        };
        sendTo(player.id, stats);
    }

    void broadcastBlock(int x, int y) {
        json message = {{"type", "block"}, {"x", x}, {"y", y},
                        {"fg", static_cast<int>(world_.fg(x, y))}};
        broadcast(message);
    }

    void broadcast(const json& message) {
        for (auto& [id, client] : clients_) {
            (void)id;
            if (!client->alive) continue;
            sendTo(*client, message);
        }
    }

    bool sendTo(Client& client, const json& message) {
        if (!client.alive || !client.socket) return false;
        std::string error;
        if (!sendJson(*client.socket, client.sendMutex, message, error)) {
            client.alive = false;
            return false;
        }
        return true;
    }

    bool sendTo(int playerId, const json& message) {
        auto it = clients_.find(playerId);
        if (it == clients_.end()) return false;
        return sendTo(*it->second, message);
    }
};

} // namespace

int main(int argc, char** argv) {
    int port = 25565;
    uint64_t seed = 0;
    bool fresh = false;
    std::filesystem::path worldPath = std::filesystem::current_path() / "saves" / "world.mc2d";
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        try {
            if (arg == "--port" && i + 1 < argc) port = std::stoi(argv[++i]);
            else if (arg == "--seed" && i + 1 < argc) seed = std::stoull(argv[++i]);
            else if (arg == "--world" && i + 1 < argc) worldPath = argv[++i];
            else if (arg == "--fresh") fresh = true;
        } catch (...) {
            std::cerr << "参数错误: " << arg << std::endl;
            return 1;
        }
    }
    std::cout << "Minecraft 2D Dedicated Server" << std::endl;
    Server server(port, worldPath, seed, fresh);
    std::string error;
    if (!server.init(error)) {
        std::cerr << "[Server] 启动失败: " << error << std::endl;
        netShutdown();
        return 1;
    }
    server.run();
    server.shutdown();
    netShutdown();
    return 0;
}
