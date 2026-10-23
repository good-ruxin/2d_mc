#include "client/assets.h"
#include "client/platform_win.h"
#include "common/blocks.h"
#include "common/net.h"
#include "common/physics.h"
#include "common/world.h"

#include <raylib.h>
#include <raymath.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

using namespace mc;
using json = nlohmann::json;

namespace {

constexpr int INV_SIZE = 36;
constexpr int HOTBAR_SIZE = 9;
constexpr float FIXED_DT = 0.05f;
constexpr int PROTOCOL_VERSION = 1;

const Color COLOR_TEXT = Color{255, 255, 255, 255};
const Color COLOR_TEXT_SHADOW = Color{40, 40, 40, 255};
const Color COLOR_GOLD = Color{255, 220, 80, 255};
const Color COLOR_GREEN = Color{110, 220, 110, 255};

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

Color lerpColor(Color a, Color b, float t) {
    t = clamp01(t);
    return Color{
        static_cast<unsigned char>(a.r + (b.r - a.r) * t),
        static_cast<unsigned char>(a.g + (b.g - a.g) * t),
        static_cast<unsigned char>(a.b + (b.b - a.b) * t),
        static_cast<unsigned char>(a.a + (b.a - a.a) * t),
    };
}

Color withAlpha(Color color, unsigned char alpha) {
    color.a = alpha;
    return color;
}

Color scaleColor(Color color, float brightness) {
    brightness = std::clamp(brightness, 0.0f, 1.0f);
    return Color{
        static_cast<unsigned char>(std::clamp(color.r * brightness, 0.0f, 255.0f)),
        static_cast<unsigned char>(std::clamp(color.g * brightness, 0.0f, 255.0f)),
        static_cast<unsigned char>(std::clamp(color.b * brightness, 0.0f, 255.0f)),
        color.a,
    };
}

std::string utf8FromCodepoint(int codepoint) {
    std::string out;
    if (codepoint <= 0x7F) {
        out.push_back(static_cast<char>(codepoint));
    } else if (codepoint <= 0x7FF) {
        out.push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0xFFFF) {
        out.push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    } else if (codepoint <= 0x10FFFF) {
        out.push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
    }
    return out;
}

void popLastUtf8(std::string& text) {
    if (text.empty()) return;
    size_t index = text.size() - 1;
    while (index > 0 && (static_cast<unsigned char>(text[index]) & 0xC0) == 0x80) --index;
    text.erase(index);
}

bool looksLikeGameRoot(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path / "assets" / "gui" / "title" / "minecraft.png", ec);
}

std::filesystem::path findGameRoot() {
    std::vector<std::filesystem::path> starts;
    starts.push_back(std::filesystem::current_path());
    starts.push_back(executableDirectory());
    for (const auto& start : starts) {
        std::filesystem::path path = start;
        for (int i = 0; i < 6; ++i) {
            if (looksLikeGameRoot(path)) return path;
            if (!path.has_parent_path() || path.parent_path() == path) break;
            path = path.parent_path();
        }
    }
    return std::filesystem::current_path();
}

bool parseAddress(const std::string& input, std::string& host, int& port) {
    std::string text = input;
    while (!text.empty() && text.front() == ' ') text.erase(text.begin());
    while (!text.empty() && text.back() == ' ') text.pop_back();
    if (text.empty()) return false;
    size_t colon = text.rfind(':');
    if (colon != std::string::npos && colon != 0 && colon + 1 < text.size()) {
        host = text.substr(0, colon);
        try {
            port = std::stoi(text.substr(colon + 1));
        } catch (...) {
            return false;
        }
    } else {
        host = text;
        port = 25565;
    }
    return !host.empty() && port > 0 && port < 65536;
}

struct NetClient {
    NetSocket socket;
    std::thread reader;
    std::atomic<bool> connected{false};
    std::atomic<bool> running{false};
    std::mutex sendMutex;
    std::mutex inboxMutex;
    std::mutex reasonMutex;
    std::deque<json> inbox;
    std::string disconnectReason;

    ~NetClient() { close(); }

    bool connectTo(const std::string& host, int port, std::string& error) {
        close();
        if (!socket.connectTo(host, port, 3000, error)) return false;
        connected = true;
        running = true;
        {
            std::lock_guard<std::mutex> lock(reasonMutex);
            disconnectReason.clear();
        }
        reader = std::thread([this] {
            while (running) {
                json message;
                std::string error;
                if (!recvJson(socket, message, error)) {
                    if (running) {
                        connected = false;
                        {
                            std::lock_guard<std::mutex> lock(reasonMutex);
                            disconnectReason = error;
                        }
                        json disconnect = {{"type", "__disconnect"}, {"reason", error}};
                        std::lock_guard<std::mutex> lock(inboxMutex);
                        inbox.push_back(disconnect);
                    }
                    break;
                }
                if (!message.is_discarded()) {
                    std::lock_guard<std::mutex> lock(inboxMutex);
                    inbox.push_back(std::move(message));
                    if (inbox.size() > 512) inbox.pop_front();
                }
            }
        });
        return true;
    }

    bool send(const json& message) {
        if (!connected) return false;
        std::string error;
        if (!sendJson(socket, sendMutex, message, error)) {
            connected = false;
            return false;
        }
        return true;
    }

    bool poll(json& message) {
        std::lock_guard<std::mutex> lock(inboxMutex);
        if (inbox.empty()) return false;
        message = std::move(inbox.front());
        inbox.pop_front();
        return true;
    }

    std::string reason() {
        std::lock_guard<std::mutex> lock(reasonMutex);
        return disconnectReason;
    }

    void close() {
        running = false;
        connected = false;
        socket.shutdownBoth();
        if (reader.joinable()) reader.join();
        socket.close();
        std::lock_guard<std::mutex> lock(inboxMutex);
        inbox.clear();
    }
};

class LightMap {
public:
    void rebuild(const World& world) {
        width_ = world.width();
        height_ = world.height();
        const int count = width_ * height_;
        sky_.assign(count, 0);
        block_.assign(count, 0);
        std::deque<int> queue;

        for (int x = 0; x < width_; ++x) {
            for (int y = 0; y < height_; ++y) {
                if (world.opaque(x, y)) break;
                int index = y * width_ + x;
                sky_[index] = 15;
                queue.push_back(index);
            }
        }
        propagate(world, sky_, queue, false);

        queue.clear();
        for (int y = 0; y < height_; ++y) {
            for (int x = 0; x < width_; ++x) {
                int light = blockDef(world.fg(x, y)).light;
                if (light > 0) {
                    int index = y * width_ + x;
                    block_[index] = static_cast<uint8_t>(std::min(15, light));
                    queue.push_back(index);
                }
            }
        }
        propagate(world, block_, queue, true);
    }

    float score(const World& world, int x, int y, float daylight) const {
        int bestSky = 0;
        int bestBlock = 0;
        auto sample = [&](int sx, int sy) {
            if (sx < 0 || sy < 0 || sx >= width_ || sy >= height_) return;
            if (world.opaque(sx, sy)) return;
            int index = sy * width_ + sx;
            bestSky = std::max(bestSky, static_cast<int>(sky_[index]));
            bestBlock = std::max(bestBlock, static_cast<int>(block_[index]));
        };
        sample(x, y);
        sample(x + 1, y);
        sample(x - 1, y);
        sample(x, y + 1);
        sample(x, y - 1);
        float sky = (static_cast<float>(bestSky) / 15.0f) * daylight;
        float block = static_cast<float>(bestBlock) / 15.0f;
        return std::max(sky, block);
    }

    uint8_t skyAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
        return sky_[y * width_ + x];
    }

    uint8_t blockAt(int x, int y) const {
        if (x < 0 || y < 0 || x >= width_ || y >= height_) return 0;
        return block_[y * width_ + x];
    }

private:
    void propagate(const World& world, std::vector<uint8_t>& data, std::deque<int>& queue, bool isBlock) {
        const int width = width_;
        const int height = height_;
        while (!queue.empty()) {
            int index = queue.front();
            queue.pop_front();
            int level = data[index];
            if (level <= 1) continue;
            int x = index % width;
            int y = index / width;
            const int nx[4] = {x + 1, x - 1, x, x};
            const int ny[4] = {y, y, y + 1, y - 1};
            for (int i = 0; i < 4; ++i) {
                if (nx[i] < 0 || ny[i] < 0 || nx[i] >= width || ny[i] >= height) continue;
                if (world.opaque(nx[i], ny[i])) continue;
                int next = ny[i] * width + nx[i];
                int nextLevel = level - 1;
                if (isBlock && world.fg(nx[i], ny[i]) == WATER) nextLevel -= 1;
                if (nextLevel > data[next]) {
                    data[next] = static_cast<uint8_t>(std::max(0, nextLevel));
                    queue.push_back(next);
                }
            }
        }
    }

    int width_ = 0;
    int height_ = 0;
    std::vector<uint8_t> sky_;
    std::vector<uint8_t> block_;
};

struct PendingInput {
    MoveInput input;
};

struct RemotePlayer {
    int id = 0;
    std::string name;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    int facing = 1;
    bool sneak = false;
    bool sprint = false;
    bool onGround = true;
    bool dead = false;
    bool creative = false;
    int health = 20;
    float walkPhase = 0.0f;
    float swing = 0.0f;
};

struct EntityView {
    int id = 0;
    std::string type;
    float targetX = 0.0f;
    float targetY = 0.0f;
    float x = 0.0f;
    float y = 0.0f;
    int facing = 1;
    int health = 10;
    int maxHealth = 10;
    int hurt = 0;
    std::string item;
    int count = 0;
    bool seen = false;
    float walkPhase = 0.0f;
};

struct Particle {
    float x = 0.0f;
    float y = 0.0f;
    float vx = 0.0f;
    float vy = 0.0f;
    float life = 0.0f;
    float maxLife = 1.0f;
    float size = 0.18f;
    std::string texture;
    Rectangle source{0, 0, 4, 4};
    Color tint{255, 255, 255, 255};
    bool gravity = true;
};

struct Toast {
    std::string text;
    std::string sub;
    float life = 4.0f;
    float maxLife = 4.0f;
};

struct ChatLine {
    std::string from;
    std::string text;
    float age = 0.0f;
};

struct WorldRenderable {
    enum Kind { Player, Mob, Item } kind;
    int id;
    float y;
    int entityIndex;
};

class Game {
public:
    struct Args {
        std::string connect;
        std::string name = "Steve";
        std::string screenshot;
        float screenshotDelay = 0.0f;
        std::string screenshotUi;
        bool quitAfterScreenshot = false;
        bool singleplayer = false;
        int width = 1280;
        int height = 720;
    };

    int run(const Args& args);

private:
    enum class Screen { Title, Multiplayer, Settings, Connecting, Playing, Disconnected };
    enum class Overlay { None, Pause, Inventory, Death };

    bool initialize(const Args& args);
    void shutdown();
    void frame(float dt);
    void pollNetwork();
    void updateGlobalInput();
    void updatePlaying(float dt);
    void updateDesiredInput();
    void fixedTick();
    void updateRendererSmoothing(float dt);
    void updateParticles(float dt);
    void updateCamera(float dt);
    void handleSelfState(const json& self, uint64_t tick, uint64_t time);
    void handleSnapshot(const json& message);
    void handleInventoryMessage(const json& message);
    void handleStats(const json& message);
    void handleChatMessage(const json& message);
    void handleEvent(const json& message);
    void handleBlockMessage(const json& message);
    void onWorldLoaded();

    void render();
    void renderTitle();
    void renderMultiplayer();
    void renderSettings();
    void renderConnecting();
    void renderDisconnected();
    void renderPlaying();
    void renderSky(float daylight);
    void renderParallax(float daylight);
    void renderWorldTiles();
    void renderTile(int x, int y, bool background, float daylight);
    void renderTorchGlow();
    void renderRenderables(float daylight);
    void renderPig(const EntityView& entity, float daylight);
    void renderZombie(const EntityView& entity, float daylight);
    void renderItemEntity(const EntityView& entity, float daylight);
    void renderPlayerFigure(const std::string& skin, float worldX, float worldY, int facing,
                            bool sneaking, bool sprinting, bool onGround, float walkPhase,
                            float swing, Color tint, const std::string& heldItem, float modelHeight,
                            bool showHeldItem);
    void renderParticles();
    void renderHud();
    void renderChat();
    void renderToasts();
    void renderDebug();
    void renderPlayerList();
    void renderPause();
    void renderInventory();
    void renderDeath();
    void renderTargetHighlight();
    void renderBackgroundWall();
    void drawInventorySlot(Rectangle rect, const ItemStack& stack, bool hovered, bool selected);
    void drawItemStack(const ItemStack& stack, Rectangle rect, float scale);
    void drawItemIcon(const std::string& id, Rectangle rect);
    void drawMenuDirt();
    void drawPanel(Rectangle rect, float alpha = 0.82f);
    bool drawButton(Rectangle rect, const std::string& label, bool enabled = true);
    void drawText(const std::string& text, float x, float y, float size, Color color, bool shadow = true);
    void drawTextCentered(const std::string& text, float centerX, float y, float size, Color color,
                          bool shadow = true);
    float measureText(const std::string& text, float size);
    void drawTooltip(const std::string& text, Vector2 mouse);

    Rectangle inventorySlotRect(int slot) const;
    void buildInventoryLayout();
    void handleInventoryClick();
    void handlePauseClick();
    void handleDeathClick();
    void openChat(bool slash);
    void sendChat();
    void closeInventoryIfOpen();
    void toggleInventory();
    void setScreen(Screen screen);
    void disconnectTo(Screen target, const std::string& reason = "");
    bool connectToAddress(const std::string& address, bool launchServerIfNeeded, bool singleplayer);
    bool launchLocalServer(int port, std::string& error);
    void stopOwnedServer();
    void takeScreenshot(const std::string& explicitPath = "");
    void applyScreenshotUi();
    std::string assetPath(const std::string& path) const { return assets_.pathOf(path); }
    float blockPixels() const { return 48.0f * zoom_; }
    Vector2 worldToScreen(float wx, float wy) const;
    Vector2 screenToWorld(float sx, float sy) const;
    bool isNight() const;
    float daylightFactor() const;
    void spawnBlockParticles(int bx, int by, BlockId block);
    void spawnParticle(float x, float y, const std::string& texture, Rectangle source, Color tint,
                       float vx, float vy, float life, float size, bool gravity = true);
    void addToast(const std::string& text, const std::string& sub = "");
    void addChat(const std::string& from, const std::string& text);
    std::string currentHeldItem() const;
    int slotForHotbar(int index) const { return index; }
    int findEntityAtScreen(Vector2 mouse);
    void startMining(float dt);
    void handleGameplayMouse(float dt);

    Args args_;
    Assets assets_;
    std::filesystem::path root_;
    Screen screen_ = Screen::Title;
    Screen settingsReturn_ = Screen::Title;
    Overlay overlay_ = Overlay::None;
    bool running_ = true;
    bool cursorVisible_ = true;
    float uiScale_ = 3.0f;

    NetClient net_;
    std::string addressInput_ = "127.0.0.1:25565";
    std::string lastAddress_;
    std::string connectingLabel_;
    std::string disconnectReason_;
    bool connecting_ = false;
    bool startingLocalServer_ = false;
    float connectingTimer_ = 0.0f;
    bool ownsServer_ = false;
    void* ownedServerProcess_ = nullptr;
    unsigned long ownedServerPid_ = 0;
    bool quitAfterScreenshot_ = false;
    bool screenshotTaken_ = false;
    float autoScreenshotTimer_ = 0.0f;
    bool screenshotUiApplied_ = false;
    std::string screenshotPath_;
    std::string screenshotUi_;
    bool autoQuit_ = false;

    bool hasWorld_ = false;
    World world_;
    LightMap lightMap_;
    int myId_ = -1;
    std::string myName_ = "Steve";
    uint64_t worldTime_ = 1000;
    uint64_t worldSeed_ = 0;
    uint64_t serverTick_ = 0;
    PlayerBody body_;
    std::vector<ItemStack> inventory_ = std::vector<ItemStack>(INV_SIZE);
    ItemStack cursorStack_;
    int selected_ = 0;
    int health_ = 20;
    int food_ = 20;
    int air_ = 300;
    bool dead_ = false;
    bool creative_ = false;
    bool flying_ = false;
    uint32_t inputSeq_ = 0;
    uint32_t ackSeq_ = 0;
    std::deque<PendingInput> pendingInputs_;
    float tickAccumulator_ = 0.0f;
    float renderX_ = 0.0f;
    float renderY_ = 0.0f;
    float cameraX_ = 0.0f;
    float cameraY_ = 0.0f;
    float zoom_ = 1.0f;
    int zoomIndex_ = 0;
    float walkPhase_ = 0.0f;
    float swing_ = 0.0f;
    float attackCooldown_ = 0.0f;
    float selectedNameTimer_ = 0.0f;
    float playTime_ = 0.0f;
    float redFlash_ = 0.0f;
    float shake_ = 0.0f;
    float blockBreakFlash_ = 0.0f;
    float lastHurtTime_ = -100.0f;
    bool showHud_ = true;
    bool showDebug_ = false;
    bool showPlayerList_ = false;
    bool flyingToggleReady_ = true;
    float lastSpaceTap_ = -100.0f;
    float lastATap_ = -100.0f;
    float lastDTap_ = -100.0f;
    int lastTapDirection_ = 0;
    bool doubleTapSprint_ = false;
    int facing_ = 1;

    std::map<int, RemotePlayer> remotePlayers_;
    std::map<int, EntityView> entities_;
    std::vector<Particle> particles_;
    std::deque<Toast> toasts_;
    std::deque<ChatLine> chatLines_;
    bool chatOpen_ = false;
    std::string chatInput_;
    float chatBlink_ = 0.0f;
    std::vector<std::string> chatHistory_;
    int chatHistoryIndex_ = -1;

    MoveInput desiredInput_{};
    bool hoverValid_ = false;
    int hoverX_ = -1;
    int hoverY_ = -1;
    bool mineActive_ = false;
    int mineX_ = -1;
    int mineY_ = -1;
    float mineProgress_ = 0.0f;
    float mineCooldown_ = 0.0f;
    float mineParticleTimer_ = 0.0f;
    int hoveredEntity_ = -1;

    std::array<Rectangle, INV_SIZE> invSlotRects_{};
    std::vector<Rectangle> recipeRects_;
    std::vector<int> recipeIndices_;
    Rectangle invPanel_{};
    Rectangle invPlayerArea_{};
    Rectangle pausePanel_{};
    std::array<Rectangle, 3> pauseButtons_{};
    Rectangle deathPanel_{};
    std::array<Rectangle, 2> deathButtons_{};
    std::array<Rectangle, 2> disconnectedButtons_{};
    bool inventoryLayoutBuilt_ = false;

    Texture2D glowTexture_{};
    Texture2D whiteTexture_{};
    Texture2D sunTexture_{};
    Texture2D moonTexture_{};
    bool screenshotRequested_ = false;
    std::string screenshotRequestPath_;
    std::mt19937 random_{std::random_device{}()};
    std::string splashText_;
    float splashScale_ = 1.0f;
    float titleAnimation_ = 0.0f;
    std::string pendingScreenshotUi_;
};

// ---------------------------------------------------------------------------
// Setup and lifecycle
// ---------------------------------------------------------------------------

int Game::run(const Args& args) {
    if (!initialize(args)) return 1;
    while (!WindowShouldClose() && running_) {
        frame(GetFrameTime());
    }
    shutdown();
    return 0;
}

bool Game::initialize(const Args& args) {
    args_ = args;
    root_ = findGameRoot();
    std::error_code ec;
    std::filesystem::current_path(root_, ec);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT | FLAG_WINDOW_HIGHDPI);
    InitWindow(args_.width, args_.height, "Minecraft 2D");
    SetWindowMinSize(960, 540);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetRandomSeed(static_cast<unsigned int>(std::time(nullptr)));

    std::string assetError;
    if (!assets_.init(root_, assetError)) {
        TraceLog(LOG_ERROR, "ASSETS: %s", assetError.c_str());
        CloseWindow();
        return false;
    }
    if (!assetError.empty()) TraceLog(LOG_WARNING, "ASSETS: %s", assetError.c_str());

    std::string netError;
    if (!netInit(netError)) {
        TraceLog(LOG_ERROR, "NET: %s", netError.c_str());
    }

    Image glow = GenImageGradientRadial(128, 128, 0.0f, WHITE, BLANK);
    glowTexture_ = LoadTextureFromImage(glow);
    UnloadImage(glow);
    SetTextureFilter(glowTexture_, TEXTURE_FILTER_BILINEAR);
    Image white = GenImageColor(2, 2, WHITE);
    whiteTexture_ = LoadTextureFromImage(white);
    UnloadImage(white);

    auto loadKeyedTexture = [&](const std::string& path) -> Texture2D {
        Image image = LoadImage(assetPath(path).c_str());
        if (image.data == nullptr) return Texture2D{};
        ImageFormat(&image, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        auto* pixels = static_cast<unsigned char*>(image.data);
        for (int i = 0; i < image.width * image.height; ++i) {
            unsigned char r = pixels[i * 4 + 0];
            unsigned char g = pixels[i * 4 + 1];
            unsigned char b = pixels[i * 4 + 2];
            unsigned char alpha = std::max({r, g, b});
            if (r < 8 && g < 8 && b < 8) alpha = 0;
            else alpha = static_cast<unsigned char>(std::min(255, alpha + 18));
            pixels[i * 4 + 0] = 255;
            pixels[i * 4 + 1] = 255;
            pixels[i * 4 + 2] = 255;
            pixels[i * 4 + 3] = alpha;
        }
        Texture2D texture = LoadTextureFromImage(image);
        UnloadImage(image);
        SetTextureFilter(texture, TEXTURE_FILTER_POINT);
        return texture;
    };
    sunTexture_ = loadKeyedTexture("environment/sun.png");
    moonTexture_ = loadKeyedTexture("environment/moon_phases.png");

    static const char* splashes[] = {
        "也是 2D 的！",
        "由 C++ 与 raylib 驱动！",
        "现在有背景板了！",
        "客户端 / 服务端分离！",
        "原版键位！",
        "试试按 F3！",
        "Workbench 也在！",
        "挖掘、合成、生存！",
        "20 TPS！",
        "100% 方块！",
    };
    splashText_ = splashes[random_() % (sizeof(splashes) / sizeof(splashes[0]))];

    myName_ = args_.name.empty() ? "Steve" : args_.name;
    addressInput_ = "127.0.0.1:25565";
    ShowCursor();

    if (!args_.screenshot.empty()) {
        screenshotPath_ = args_.screenshot;
        autoScreenshotTimer_ = 0.0f;
        autoQuit_ = args_.quitAfterScreenshot;
        screenshotUi_ = args_.screenshotUi;
        screenshotTaken_ = false;
        screenshotUiApplied_ = false;
    }
    if (!args_.connect.empty()) {
        if (!connectToAddress(args_.connect, false, args_.connect == "127.0.0.1:25565")) {
            // The error screen is already set up.
        }
    } else if (args_.singleplayer) {
        connectToAddress("127.0.0.1:25565", true, true);
    }
    return true;
}

void Game::shutdown() {
    if (net_.connected) {
        if (ownsServer_) {
            net_.send(json{{"type", "chat"}, {"text", "/stop"}});
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
        }
    }
    net_.close();
    stopOwnedServer();
    if (glowTexture_.id != 0) UnloadTexture(glowTexture_);
    if (whiteTexture_.id != 0) UnloadTexture(whiteTexture_);
    if (sunTexture_.id != 0) UnloadTexture(sunTexture_);
    if (moonTexture_.id != 0) UnloadTexture(moonTexture_);
    assets_.unload();
    if (IsWindowReady()) CloseWindow();
    netShutdown();
}

bool Game::launchLocalServer(int port, std::string& error) {
    std::filesystem::path serverPath = executableDirectory() / "mc2d_server.exe";
    if (!std::filesystem::exists(serverPath)) serverPath = root_ / "bin" / "mc2d_server.exe";
    if (!std::filesystem::exists(serverPath)) serverPath = root_ / "mc2d_server.exe";
    if (!std::filesystem::exists(serverPath)) {
        error = "找不到 mc2d_server.exe";
        return false;
    }
    std::string arguments = "--port " + std::to_string(port);
    std::string errorText;
    if (!launchProcessHidden(serverPath, arguments, root_, &ownedServerProcess_, ownedServerPid_, errorText)) {
        error = errorText;
        ownsServer_ = false;
        return false;
    }
    ownsServer_ = true;
    startingLocalServer_ = true;
    TraceLog(LOG_INFO, "已启动本地服务器 PID %lu", static_cast<unsigned long>(ownedServerPid_));
    return true;
}

void Game::stopOwnedServer() {
    if (!ownsServer_ || !ownedServerProcess_) {
        ownsServer_ = false;
        return;
    }
    if (!waitForProcess(ownedServerProcess_, 1600)) {
        terminateProcess(ownedServerProcess_);
        waitForProcess(ownedServerProcess_, 400);
    }
    closeProcessHandle(ownedServerProcess_);
    ownedServerProcess_ = nullptr;
    ownedServerPid_ = 0;
    ownsServer_ = false;
}

bool Game::connectToAddress(const std::string& address, bool launchServerIfNeeded, bool singleplayer) {
    lastAddress_ = address;
    addressInput_ = address;
    std::string host;
    int port = 25565;
    if (!parseAddress(address, host, port)) {
        disconnectReason_ = "服务器地址格式无效";
        setScreen(Screen::Disconnected);
        return false;
    }
    setScreen(Screen::Connecting);
    ShowCursor();
    connecting_ = true;
    connectingTimer_ = 0.0f;
    startingLocalServer_ = false;
    connectingLabel_ = "正在连接 " + address + " ...";
    net_.close();
    std::string error;
    bool connected = net_.connectTo(host, port, error);
    if (!connected && launchServerIfNeeded) {
        startingLocalServer_ = true;
        connectingLabel_ = "正在启动本地服务器...";
        if (!launchLocalServer(port, error)) {
            disconnectReason_ = error;
            setScreen(Screen::Disconnected);
            return false;
        }
        for (int attempt = 0; attempt < 30 && !connected; ++attempt) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            connected = net_.connectTo(host, port, error);
        }
        startingLocalServer_ = false;
        if (!connected) {
            disconnectReason_ = "本地服务器启动失败: " + error;
            setScreen(Screen::Disconnected);
            return false;
        }
    }
    if (!connected) {
        disconnectReason_ = error.empty() ? "无法连接到服务器" : error;
        setScreen(Screen::Disconnected);
        return false;
    }
    connectingLabel_ = "正在加入世界...";
    json hello = {{"type", "hello"}, {"name", myName_}, {"version", PROTOCOL_VERSION}};
    if (!net_.send(hello)) {
        disconnectReason_ = "发送握手失败";
        net_.close();
        setScreen(Screen::Disconnected);
        return false;
    }
    hasWorld_ = false;
    myId_ = -1;
    remotePlayers_.clear();
    entities_.clear();
    pendingInputs_.clear();
    ackSeq_ = 0;
    inputSeq_ = 0;
    dead_ = false;
    creative_ = false;
    inventory_.assign(INV_SIZE, ItemStack{});
    cursorStack_.clear();
    (void)singleplayer;
    return true;
}

void Game::disconnectTo(Screen target, const std::string& reason) {
    if (!reason.empty()) disconnectReason_ = reason;
    net_.close();
    if (ownsServer_) {
        stopOwnedServer();
    }
    hasWorld_ = false;
    myId_ = -1;
    connecting_ = false;
    remotePlayers_.clear();
    entities_.clear();
    particles_.clear();
    if (target == Screen::Title) {
        overlay_ = Overlay::None;
        chatOpen_ = false;
        ShowCursor();
    }
    setScreen(target);
}

void Game::setScreen(Screen screen) {
    screen_ = screen;
    if (screen != Screen::Playing) {
        ShowCursor();
        cursorVisible_ = true;
    }
}

// ---------------------------------------------------------------------------
// Input / simulation
// ---------------------------------------------------------------------------

void Game::updateGlobalInput() {
    if (IsKeyPressed(KEY_F11)) {
        ToggleFullscreen();
    }
    if (IsKeyPressed(KEY_F2)) takeScreenshot();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    uiScale_ = std::clamp(static_cast<float>(height) / 240.0f, 2.0f, 4.0f);
    splashScale_ = std::clamp(static_cast<float>(width) / 1280.0f, 0.75f, 1.6f);
    (void)width;
}

void Game::openChat(bool slash) {
    if (screen_ != Screen::Playing || overlay_ != Overlay::None || dead_) return;
    chatOpen_ = true;
    chatInput_ = slash ? "/" : "";
    chatHistoryIndex_ = -1;
    ShowCursor();
    cursorVisible_ = true;
}

void Game::sendChat() {
    std::string text = chatInput_;
    chatOpen_ = false;
    chatInput_.clear();
    ShowCursor();
    cursorVisible_ = true;
    if (text.empty()) return;
    chatHistory_.push_back(text);
    if (chatHistory_.size() > 30) chatHistory_.erase(chatHistory_.begin());
    net_.send(json{{"type", "chat"}, {"text", text}});
    if (!net_.connected) {
        screen_ = Screen::Disconnected;
        if (disconnectReason_.empty()) disconnectReason_ = "与服务器断开连接";
    }
}

void Game::closeInventoryIfOpen() {
    if (overlay_ == Overlay::Inventory) {
        net_.send(json{{"type", "close_inventory"}});
        cursorStack_.clear();
        overlay_ = Overlay::None;
        ShowCursor();
        cursorVisible_ = true;
    }
}

void Game::toggleInventory() {
    if (overlay_ == Overlay::Inventory) {
        closeInventoryIfOpen();
    } else if (overlay_ == Overlay::None && !dead_) {
        overlay_ = Overlay::Inventory;
        ShowCursor();
        cursorVisible_ = true;
        inventoryLayoutBuilt_ = false;
    }
}

void Game::updateDesiredInput() {
    desiredInput_ = MoveInput{};
    if (screen_ != Screen::Playing || dead_ || !hasWorld_) return;
    if (overlay_ != Overlay::None || chatOpen_) {
        desiredInput_.facing = static_cast<float>(facing_);
        return;
    }
    int move = 0;
    if (IsKeyDown(KEY_A)) --move;
    if (IsKeyDown(KEY_D)) ++move;
    desiredInput_.move = move;
    desiredInput_.jump = IsKeyDown(KEY_SPACE);
    desiredInput_.sneak = IsKeyDown(KEY_LEFT_SHIFT);
    bool ctrlSprint = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);
    desiredInput_.sprint = ctrlSprint || doubleTapSprint_;
    if (move != 0) facing_ = move;
    else if (!chatOpen_) {
        Vector2 mouse = GetMousePosition();
        Vector2 world = screenToWorld(mouse.x, mouse.y);
        if (std::abs(world.x - body_.x) > 0.4f) facing_ = world.x > body_.x ? 1 : -1;
    }
    desiredInput_.facing = static_cast<float>(facing_);
    if (move == 0) doubleTapSprint_ = false;
    if (IsKeyPressed(KEY_A)) {
        float now = static_cast<float>(GetTime());
        if (lastTapDirection_ == -1 && now - lastATap_ < 0.32f) doubleTapSprint_ = true;
        lastATap_ = now;
        lastTapDirection_ = -1;
    }
    if (IsKeyPressed(KEY_D)) {
        float now = static_cast<float>(GetTime());
        if (lastTapDirection_ == 1 && now - lastDTap_ < 0.32f) doubleTapSprint_ = true;
        lastDTap_ = now;
        lastTapDirection_ = 1;
    }
    if (IsKeyPressed(KEY_SPACE)) {
        float now = static_cast<float>(GetTime());
        if (creative_ && now - lastSpaceTap_ < 0.32f) {
            net_.send(json{{"type", "toggle_fly"}});
            lastSpaceTap_ = -100.0f;
        } else {
            lastSpaceTap_ = now;
        }
    }
}

void Game::fixedTick() {
    if (!hasWorld_ || screen_ != Screen::Playing || dead_) return;
    MoveInput input = desiredInput_;
    input.seq = ++inputSeq_;
    pendingInputs_.push_back({input});
    if (pendingInputs_.size() > 120) pendingInputs_.pop_front();
    json message = {
        {"type", "input"},
        {"seq", input.seq},
        {"move", input.move},
        {"jump", input.jump},
        {"sneak", input.sneak},
        {"sprint", input.sprint},
        {"facing", input.facing},
    };
    net_.send(message);
    tickPlayer(world_, body_, input, creative_);
    playTime_ += FIXED_DT;
    if (body_.onGround && std::abs(body_.vx) > 0.2f) {
        walkPhase_ += std::abs(body_.vx) * FIXED_DT * 2.7f;
        if (std::fmod(playTime_, 0.35f) < FIXED_DT && (random_() % 100) < 35) {
            Vector2 screen = worldToScreen(body_.x, body_.y);
            (void)screen;
            spawnParticle(body_.x + (facing_ > 0 ? -0.18f : 0.18f), body_.y, "block/dirt",
                          Rectangle{0, 0, 3, 3}, Color{160, 140, 110, 255},
                          -facing_ * 0.35f, -0.4f, 0.35f, 0.06f, true);
        }
    }
}

void Game::handleSelfState(const json& self, uint64_t tick, uint64_t time) {
    (void)tick;
    if (time > 0) {
        // Network time is authoritative, but the client advances locally for
        // a smooth sun. Snap only when the accumulated drift is visible.
        float incoming = static_cast<float>(time);
        float current = static_cast<float>(worldTime_ % 24000);
        float diff = incoming - current;
        if (std::abs(diff) > 15.0f) worldTime_ = time;
    }
    serverTick_ = tick;
    health_ = self.value("health", health_);
    food_ = self.value("food", food_);
    air_ = self.value("air", air_);
    dead_ = self.value("dead", dead_);
    creative_ = self.value("creative", creative_);
    flying_ = self.value("flying", flying_);
    selected_ = std::clamp(self.value("selected", selected_), 0, HOTBAR_SIZE - 1);
    if (dead_) {
        if (overlay_ != Overlay::Death) {
            overlay_ = Overlay::Death;
            ShowCursor();
            cursorVisible_ = true;
        }
        return;
    }
    if (overlay_ == Overlay::Death) overlay_ = Overlay::None;
    uint32_t ack = self.value("ack", 0u);
    ackSeq_ = std::max(ackSeq_, ack);
    PlayerBody serverBody{};
    serverBody.x = self.value("x", body_.x);
    serverBody.y = self.value("y", body_.y);
    serverBody.vx = self.value("vx", 0.0f);
    serverBody.vy = self.value("vy", 0.0f);
    serverBody.onGround = self.value("onGround", false);
    serverBody.sneaking = self.value("sneaking", false);
    serverBody.sprinting = self.value("sprinting", false);
    serverBody.flying = flying_;
    serverBody.facing = self.value("facing", 1.0f) < 0.0f ? -1 : 1;
    while (!pendingInputs_.empty() && pendingInputs_.front().input.seq <= ack) {
        pendingInputs_.pop_front();
    }
    float distSq = (serverBody.x - body_.x) * (serverBody.x - body_.x) +
                   (serverBody.y - body_.y) * (serverBody.y - body_.y);
    if (distSq > 36.0f) {
        body_ = serverBody;
        pendingInputs_.clear();
    } else {
        body_ = serverBody;
        for (const auto& pending : pendingInputs_) {
            tickPlayer(world_, body_, pending.input, creative_);
        }
    }
    if (!hasWorld_) return;
    if (std::abs(body_.x - renderX_) > 8.0f || std::abs(body_.y - renderY_) > 8.0f) {
        renderX_ = body_.x;
        renderY_ = body_.y;
    }
}

void Game::handleSnapshot(const json& message) {
    if (!message.contains("players") || !message["players"].is_array()) return;
    uint64_t tick = message.value("tick", 0ULL);
    uint64_t time = message.value("time", 0ULL);
    serverTick_ = tick;
    if (time > 0) {
        float incoming = static_cast<float>(time);
        float current = static_cast<float>(worldTime_ % 24000);
        float diff = incoming - current;
        if (std::abs(diff) > 15.0f) worldTime_ = time;
    }
    for (auto& remote : remotePlayers_) remote.second.name.clear();
    std::map<int, bool> seenRemote;
    for (const auto& p : message["players"]) {
        int id = p.value("id", -1);
        if (id <= 0 || id == myId_) continue;
        seenRemote[id] = true;
        auto& remote = remotePlayers_[id];
        if (remote.id == 0) {
            remote.id = id;
            remote.x = p.value("x", 0.0f);
            remote.y = p.value("y", 0.0f);
            remote.targetX = remote.x;
            remote.targetY = remote.y;
        }
        remote.name = p.value("name", "Steve");
        remote.targetX = p.value("x", remote.targetX);
        remote.targetY = p.value("y", remote.targetY);
        remote.vx = p.value("vx", 0.0f);
        remote.vy = p.value("vy", 0.0f);
        remote.facing = p.value("facing", 1.0f) < 0.0f ? -1 : 1;
        remote.sneak = p.value("sneak", false);
        remote.sprint = p.value("sprint", false);
        remote.onGround = p.value("onGround", true);
        remote.dead = p.value("dead", false);
        remote.creative = p.value("creative", false);
        remote.health = p.value("health", 20);
    }
    for (auto it = remotePlayers_.begin(); it != remotePlayers_.end();) {
        if (!seenRemote[it->first]) it = remotePlayers_.erase(it);
        else ++it;
    }

    for (auto& entity : entities_) entity.second.seen = false;
    if (message.contains("entities") && message["entities"].is_array()) {
        for (const auto& e : message["entities"]) {
            int id = e.value("id", -1);
            if (id <= 0) continue;
            auto& view = entities_[id];
            if (view.id == 0) {
                view.id = id;
                view.x = e.value("x", 0.0f);
                view.y = e.value("y", 0.0f);
                view.targetX = view.x;
                view.targetY = view.y;
            }
            view.type = e.value("type", "item");
            view.targetX = e.value("x", view.targetX);
            view.targetY = e.value("y", view.targetY);
            view.facing = e.value("facing", 1.0f) < 0.0f ? -1 : 1;
            view.health = e.value("health", 1);
            view.maxHealth = e.value("maxHealth", 1);
            view.hurt = e.value("hurt", 0);
            view.item = e.value("item", "");
            view.count = e.value("count", 1);
            view.seen = true;
        }
    }
    for (auto it = entities_.begin(); it != entities_.end();) {
        if (!it->second.seen) it = entities_.erase(it);
        else ++it;
    }
}

void Game::handleInventoryMessage(const json& message) {
    if (!message.contains("slots") || !message["slots"].is_array()) return;
    for (int i = 0; i < INV_SIZE; ++i) inventory_[i].clear();
    int index = 0;
    for (const auto& item : message["slots"]) {
        if (index >= INV_SIZE) break;
        if (item.is_object()) {
            inventory_[index].id = item.value("id", "");
            inventory_[index].count = item.value("count", 0);
            if (inventory_[index].count <= 0) inventory_[index].clear();
        }
        ++index;
    }
    if (message.contains("cursor") && message["cursor"].is_object()) {
        cursorStack_.id = message["cursor"].value("id", "");
        cursorStack_.count = message["cursor"].value("count", 0);
    } else {
        cursorStack_.clear();
    }
    selected_ = std::clamp(message.value("selected", selected_), 0, HOTBAR_SIZE - 1);
}

void Game::handleStats(const json& message) {
    health_ = message.value("health", health_);
    food_ = message.value("food", food_);
    air_ = message.value("air", air_);
    dead_ = message.value("dead", dead_);
    creative_ = message.value("creative", creative_);
    flying_ = message.value("flying", flying_);
    selected_ = std::clamp(message.value("selected", selected_), 0, HOTBAR_SIZE - 1);
    if (dead_ && overlay_ != Overlay::Death) {
        overlay_ = Overlay::Death;
        ShowCursor();
        cursorVisible_ = true;
    }
}

void Game::handleChatMessage(const json& message) {
    addChat(message.value("from", ""), message.value("text", ""));
}

void Game::handleEvent(const json& message) {
    std::string kind = message.value("kind", "");
    if (kind == "block_break") {
        int x = message.value("x", -1);
        int y = message.value("y", -1);
        int block = message.value("block", 0);
        spawnBlockParticles(x, y, static_cast<BlockId>(block));
        blockBreakFlash_ = 1.0f;
    } else if (kind == "player_hurt") {
        redFlash_ = 1.0f;
        shake_ = std::max(shake_, 0.6f);
        lastHurtTime_ = static_cast<float>(GetTime());
    } else if (kind == "mob_hurt") {
        float x = message.value("x", 0.0f);
        float y = message.value("y", 0.0f);
        for (int i = 0; i < 6; ++i) {
            spawnParticle(x, y - 0.4f, "", Rectangle{0, 0, 2, 2}, Color{200, 50, 50, 255},
                          (static_cast<float>(random_() % 100) / 100.0f - 0.5f) * 3.0f,
                          -1.0f - static_cast<float>(random_() % 100) / 100.0f,
                          0.5f, 0.08f, true);
        }
    } else if (kind == "mob_death") {
        float x = message.value("x", 0.0f);
        float y = message.value("y", 0.0f);
        for (int i = 0; i < 14; ++i) {
            spawnParticle(x, y - 0.4f, "", Rectangle{0, 0, 2, 2}, Color{130, 170, 100, 255},
                          (static_cast<float>(random_() % 100) / 100.0f - 0.5f) * 4.0f,
                          -1.0f - static_cast<float>(random_() % 100) / 80.0f,
                          0.7f, 0.1f, true);
        }
    } else if (kind == "eat") {
        redFlash_ = 0.08f;
    } else if (kind == "player_death") {
        if (message.value("id", -1) == myId_) redFlash_ = 1.5f;
    }
}

void Game::handleBlockMessage(const json& message) {
    int x = message.value("x", -1);
    int y = message.value("y", -1);
    int fg = message.value("fg", -1);
    if (x < 0 || y < 0 || fg < 0) return;
    world_.setFg(x, y, static_cast<BlockId>(fg));
    lightMap_.rebuild(world_);
    if (mineX_ == x && mineY_ == y) {
        mineProgress_ = 0.0f;
        mineActive_ = false;
    }
}

void Game::onWorldLoaded() {
    hasWorld_ = true;
    dead_ = false;
    overlay_ = Overlay::None;
    renderX_ = body_.x;
    renderY_ = body_.y;
    cameraX_ = body_.x;
    cameraY_ = body_.y - 1.2f;
    tickAccumulator_ = 0.0f;
    chatOpen_ = false;
    setScreen(Screen::Playing);
    HideCursor();
    cursorVisible_ = false;
    inventoryLayoutBuilt_ = false;
    for (int i = 0; i < HOTBAR_SIZE; ++i) {
        if (!inventory_[i].empty()) {
            selected_ = i;
            break;
        }
    }
    addToast("世界已加载", "按 E 打开背包 · 按 T 聊天");
}

void Game::pollNetwork() {
    if (!net_.connected && screen_ == Screen::Connecting) {
        // The reader thread reports the reason through the inbox.
    }
    json message;
    while (net_.poll(message)) {
        std::string type = message.value("type", "");
        if (type == "welcome") {
            myId_ = message.value("id", -1);
            myName_ = message.value("name", myName_);
            worldTime_ = message.value("time", worldTime_);
            worldSeed_ = message.value("seed", 0ULL);
            json spawn = message.contains("spawn") ? message["spawn"] : json::object();
            body_ = PlayerBody{};
            body_.x = spawn.value("x", 0.0f);
            body_.y = spawn.value("y", 0.0f);
            renderX_ = body_.x;
            renderY_ = body_.y;
        } else if (type == "world") {
            int width = message.value("width", 512);
            int height = message.value("height", 128);
            std::vector<int> fgRuns = message.value("fg", std::vector<int>{});
            std::vector<int> bgRuns = message.value("bg", std::vector<int>{});
            world_ = World(width, height);
            auto fg = World::decodeRuns(fgRuns, width * height);
            auto bg = World::decodeRuns(bgRuns, width * height);
            std::copy(fg.begin(), fg.end(), world_.fgData().begin());
            std::copy(bg.begin(), bg.end(), world_.bgData().begin());
            lightMap_.rebuild(world_);
            onWorldLoaded();
        } else if (type == "self") {
            json self = message.contains("self") ? message["self"] : json::object();
            handleSelfState(self, message.value("tick", 0ULL), message.value("time", 0ULL));
        } else if (type == "snapshot") {
            handleSnapshot(message);
        } else if (type == "inventory") {
            handleInventoryMessage(message);
        } else if (type == "stats") {
            handleStats(message);
        } else if (type == "chat") {
            handleChatMessage(message);
        } else if (type == "toast") {
            addToast(message.value("text", ""), message.value("sub", ""));
        } else if (type == "event") {
            handleEvent(message);
        } else if (type == "block") {
            handleBlockMessage(message);
        } else if (type == "denied") {
            disconnectReason_ = message.value("reason", "服务器拒绝了连接");
            net_.close();
            setScreen(Screen::Disconnected);
        } else if (type == "__disconnect") {
            std::string reason = message.value("reason", "与服务器断开连接");
            if (screen_ == Screen::Playing || screen_ == Screen::Connecting) {
                disconnectReason_ = reason;
                net_.close();
                setScreen(Screen::Disconnected);
            }
        } else if (type == "respawned") {
            body_ = PlayerBody{};
            body_.x = message.value("x", body_.x);
            body_.y = message.value("y", body_.y);
            renderX_ = body_.x;
            renderY_ = body_.y;
            pendingInputs_.clear();
            ackSeq_ = message.value("ack", 0u);
            overlay_ = Overlay::None;
            dead_ = false;
            HideCursor();
            cursorVisible_ = false;
            addChat("系统", "你已重生");
        }
    }
    if (screen_ == Screen::Playing && !net_.connected) {
        disconnectReason_ = net_.reason().empty() ? "与服务器断开连接" : net_.reason();
        net_.close();
        setScreen(Screen::Disconnected);
    }
    if (screen_ == Screen::Connecting && !net_.connected) {
        disconnectReason_ = net_.reason().empty() ? "无法连接到服务器" : net_.reason();
        net_.close();
        setScreen(Screen::Disconnected);
    }
}

void Game::updateCamera(float dt) {
    if (!hasWorld_) return;
    float p = blockPixels();
    float halfW = static_cast<float>(GetScreenWidth()) * 0.5f / p;
    float halfH = static_cast<float>(GetScreenHeight()) * 0.5f / p;
    float targetX = body_.x + facing_ * 0.8f;
    float targetY = body_.y - 1.15f;
    float minX = halfW;
    float maxX = static_cast<float>(world_.width()) - halfW;
    if (maxX < minX) targetX = static_cast<float>(world_.width()) * 0.5f;
    else targetX = std::clamp(targetX, minX, maxX);
    targetY = std::clamp(targetY, halfH - 8.0f, static_cast<float>(world_.height()) - halfH + 8.0f);
    float smooth = std::min(1.0f, dt * 9.0f);
    cameraX_ += (targetX - cameraX_) * smooth;
    cameraY_ += (targetY - cameraY_) * smooth;
}

void Game::updateRendererSmoothing(float dt) {
    if (!hasWorld_) return;
    float smooth = std::min(1.0f, dt * 18.0f);
    renderX_ += (body_.x - renderX_) * smooth;
    renderY_ += (body_.y - renderY_) * smooth;
    for (auto& [id, remote] : remotePlayers_) {
        (void)id;
        remote.x += (remote.targetX - remote.x) * std::min(1.0f, dt * 14.0f);
        remote.y += (remote.targetY - remote.y) * std::min(1.0f, dt * 14.0f);
        remote.walkPhase += std::abs(remote.vx) * dt * 2.2f;
        if (remote.health <= 0) remote.dead = true;
    }
    for (auto& [id, entity] : entities_) {
        (void)id;
        entity.x += (entity.targetX - entity.x) * std::min(1.0f, dt * 14.0f);
        entity.y += (entity.targetY - entity.y) * std::min(1.0f, dt * 14.0f);
        entity.walkPhase += std::abs(entity.targetX - entity.x) * dt * 8.0f;
    }
    worldTime_ += static_cast<uint64_t>(std::round(dt * 20.0f));
    playTime_ += dt;
    walkPhase_ += std::abs(body_.vx) * dt * 2.7f;
    swing_ = std::max(0.0f, swing_ - dt * 4.0f);
    attackCooldown_ = std::max(0.0f, attackCooldown_ - dt);
    selectedNameTimer_ = std::max(0.0f, selectedNameTimer_ - dt);
    redFlash_ = std::max(0.0f, redFlash_ - dt * 2.2f);
    shake_ = std::max(0.0f, shake_ - dt * 2.0f);
    blockBreakFlash_ = std::max(0.0f, blockBreakFlash_ - dt * 6.0f);
    mineCooldown_ = std::max(0.0f, mineCooldown_ - dt);
    chatBlink_ += dt;
}

void Game::updateParticles(float dt) {
    for (auto& particle : particles_) {
        particle.life -= dt;
        particle.x += particle.vx * dt;
        particle.y += particle.vy * dt;
        if (particle.gravity) particle.vy += 14.0f * dt;
        particle.vx *= 0.985f;
    }
    particles_.erase(std::remove_if(particles_.begin(), particles_.end(),
                                    [](const Particle& p) { return p.life <= 0.0f; }),
                     particles_.end());
    for (auto& toast : toasts_) toast.life -= dt;
    while (!toasts_.empty() && toasts_.front().life <= 0.0f) toasts_.pop_front();
    for (auto& chat : chatLines_) chat.age += dt;
    while (!chatLines_.empty() && chatLines_.size() > 100) chatLines_.pop_front();
}

void Game::updatePlaying(float dt) {
    if (screen_ != Screen::Playing) return;
    connecting_ = false;
    updateDesiredInput();

    if (chatOpen_) {
        int codepoint = GetCharPressed();
        while (codepoint > 0) {
            chatInput_ += utf8FromCodepoint(codepoint);
            codepoint = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE)) {
            if (!chatInput_.empty()) popLastUtf8(chatInput_);
        }
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
            sendChat();
        }
        if (IsKeyPressed(KEY_ESCAPE)) {
            chatOpen_ = false;
            chatInput_.clear();
            ShowCursor();
            cursorVisible_ = true;
        }
        if (IsKeyPressed(KEY_UP) && !chatHistory_.empty()) {
            if (chatHistoryIndex_ < 0) chatHistoryIndex_ = static_cast<int>(chatHistory_.size()) - 1;
            else chatHistoryIndex_ = std::max(0, chatHistoryIndex_ - 1);
            chatInput_ = chatHistory_[chatHistoryIndex_];
        }
        if (IsKeyPressed(KEY_DOWN) && !chatHistory_.empty()) {
            if (chatHistoryIndex_ >= 0) ++chatHistoryIndex_;
            if (chatHistoryIndex_ >= static_cast<int>(chatHistory_.size())) {
                chatHistoryIndex_ = -1;
                chatInput_.clear();
            } else {
                chatInput_ = chatHistory_[chatHistoryIndex_];
            }
        }
        return;
    }

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (overlay_ == Overlay::Inventory) {
            closeInventoryIfOpen();
        } else if (overlay_ == Overlay::Pause) {
            overlay_ = Overlay::None;
            HideCursor();
            cursorVisible_ = false;
        } else if (overlay_ == Overlay::None) {
            overlay_ = Overlay::Pause;
            ShowCursor();
            cursorVisible_ = true;
        }
    }
    if (overlay_ == Overlay::None && !dead_) {
        if (IsKeyPressed(KEY_E)) toggleInventory();
        if (IsKeyPressed(KEY_T)) openChat(false);
        if (IsKeyPressed(KEY_SLASH)) openChat(true);
        if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) openChat(false);
        if (IsKeyPressed(KEY_F3)) showDebug_ = !showDebug_;
        if (IsKeyPressed(KEY_F1)) showHud_ = !showHud_;
        if (IsKeyPressed(KEY_F5)) {
            zoomIndex_ = (zoomIndex_ + 1) % 3;
            static const float levels[3] = {1.0f, 1.35f, 0.78f};
            zoom_ = levels[zoomIndex_];
        }
        if (IsKeyPressed(KEY_Q)) {
            net_.send(json{{"type", "drop"}, {"slot", selected_}});
            swing_ = 1.0f;
        }
        for (int i = 0; i < HOTBAR_SIZE; ++i) {
            int key = KEY_ONE + i;
            if (IsKeyPressed(key)) {
                selected_ = i;
                selectedNameTimer_ = 2.2f;
                net_.send(json{{"type", "select"}, {"slot", i}});
            }
        }
        float wheel = GetMouseWheelMove();
        if (wheel != 0.0f) {
            int direction = wheel > 0.0f ? -1 : 1;
            selected_ = (selected_ + direction + HOTBAR_SIZE) % HOTBAR_SIZE;
            selectedNameTimer_ = 2.2f;
            net_.send(json{{"type", "select"}, {"slot", selected_}});
        }
        showPlayerList_ = IsKeyDown(KEY_TAB);
        handleGameplayMouse(dt);
    } else {
        showPlayerList_ = false;
        if (overlay_ == Overlay::Inventory) {
            buildInventoryLayout();
            handleInventoryClick();
            if (IsKeyPressed(KEY_E)) closeInventoryIfOpen();
        } else if (overlay_ == Overlay::Pause) {
            handlePauseClick();
            if (IsKeyPressed(KEY_ESCAPE)) {
                overlay_ = Overlay::None;
                HideCursor();
                cursorVisible_ = false;
            }
        } else if (overlay_ == Overlay::Death) {
            handleDeathClick();
        }
    }
    if (overlay_ == Overlay::None && !dead_) {
        HideCursor();
        cursorVisible_ = false;
    } else {
        ShowCursor();
        cursorVisible_ = true;
    }

    // Advance aim highlighting even while idle.
    if (overlay_ == Overlay::None && !dead_) {
        Vector2 mouse = GetMousePosition();
        Vector2 world = screenToWorld(mouse.x, mouse.y);
        int bx = static_cast<int>(std::floor(world.x));
        int by = static_cast<int>(std::floor(world.y));
        hoverX_ = bx;
        hoverY_ = by;
        hoverValid_ = world_.inBounds(bx, by);
        if (hoverValid_) {
            float dx = (static_cast<float>(bx) + 0.5f) - body_.x;
            float dy = (static_cast<float>(by) + 0.5f) - body_.eyeY();
            hoverValid_ = dx * dx + dy * dy <= 5.6f * 5.6f;
        }
        hoveredEntity_ = findEntityAtScreen(mouse);
    } else {
        hoverValid_ = false;
        hoveredEntity_ = -1;
    }
}

void Game::handleGameplayMouse(float dt) {
    if (chatOpen_ || overlay_ != Overlay::None || dead_) return;
    Vector2 mouse = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        int entityId = findEntityAtScreen(mouse);
        if (entityId >= 0 && attackCooldown_ <= 0.0f) {
            auto it = entities_.find(entityId);
            if (it != entities_.end()) {
                float dx = it->second.x - body_.x;
                float dy = (it->second.y - 0.5f) - body_.centerY();
                if (dx * dx + dy * dy <= 4.2f * 4.2f) {
                    net_.send(json{{"type", "attack"}, {"id", entityId}});
                    attackCooldown_ = 0.5f;
                    swing_ = 1.0f;
                    facing_ = dx >= 0 ? 1 : -1;
                }
            }
        }
    }

    if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && hoveredEntity_ < 0 && mineCooldown_ <= 0.0f) {
        startMining(dt);
    } else {
        mineActive_ = false;
        if (!IsMouseButtonDown(MOUSE_BUTTON_LEFT)) mineProgress_ = 0.0f;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        swing_ = 1.0f;
        std::string held = currentHeldItem();
        const ItemDef* item = itemDef(held);
        if (item && item->food > 0 && food_ < 20) {
            net_.send(json{{"type", "eat"}, {"slot", selected_}});
            return;
        }
        if (hoverValid_) {
            BlockId target = world_.fg(hoverX_, hoverY_);
            if (target == CRAFTING_TABLE && !IsKeyDown(KEY_LEFT_SHIFT)) {
                toggleInventory();
                return;
            }
            BlockId block = AIR;
            if (isBlockItem(held, &block)) {
                net_.send(json{{"type", "place"}, {"x", hoverX_}, {"y", hoverY_}});
            }
        }
    }
}

void Game::startMining(float dt) {
    if (!hoverValid_) {
        mineActive_ = false;
        mineProgress_ = 0.0f;
        return;
    }
    BlockId block = world_.fg(hoverX_, hoverY_);
    if (block == AIR || block == WATER || block == LAVA || block == BEDROCK) {
        if (!(creative_ && block == BEDROCK)) {
            mineActive_ = false;
            mineProgress_ = 0.0f;
            return;
        }
    }
    if (hoverX_ != mineX_ || hoverY_ != mineY_) {
        mineX_ = hoverX_;
        mineY_ = hoverY_;
        mineProgress_ = 0.0f;
        mineActive_ = true;
        facing_ = (static_cast<float>(hoverX_) + 0.5f) >= body_.x ? 1 : -1;
    }
    float seconds = breakSeconds(block, currentHeldItem());
    if (!std::isfinite(seconds)) {
        mineActive_ = false;
        return;
    }
    if (seconds <= 0.0f) seconds = 0.05f;
    mineProgress_ += dt / seconds;
    mineParticleTimer_ += dt;
    if (mineParticleTimer_ > 0.12f) {
        mineParticleTimer_ = 0.0f;
        spawnBlockParticles(hoverX_, hoverY_, block);
    }
    swing_ = std::max(swing_, 0.45f);
    if (mineProgress_ >= 1.0f) {
        net_.send(json{{"type", "break"}, {"x", mineX_}, {"y", mineY_}});
        mineProgress_ = 0.0f;
        mineCooldown_ = 0.22f;
    }
}

void Game::spawnBlockParticles(int bx, int by, BlockId block) {
    const BlockDef& def = blockDef(block);
    if (!def.tex || !*def.tex) return;
    for (int i = 0; i < 4; ++i) {
        float sx = static_cast<float>(random_() % 12);
        float sy = static_cast<float>(random_() % 12);
        Color tint = scaleColor(WHITE, 0.85f + static_cast<float>(random_() % 30) / 100.0f);
        spawnParticle(static_cast<float>(bx) + 0.2f + static_cast<float>(random_() % 60) / 100.0f,
                      static_cast<float>(by) + 0.2f + static_cast<float>(random_() % 60) / 100.0f,
                      def.tex, Rectangle{sx, sy, 4, 4}, tint,
                      (static_cast<float>(random_() % 100) / 100.0f - 0.5f) * 3.2f,
                      -1.0f - static_cast<float>(random_() % 100) / 100.0f,
                      0.5f + static_cast<float>(random_() % 40) / 100.0f, 0.14f, true);
    }
}

void Game::spawnParticle(float x, float y, const std::string& texture, Rectangle source, Color tint,
                         float vx, float vy, float life, float size, bool gravity) {
    Particle particle;
    particle.x = x;
    particle.y = y;
    particle.texture = texture;
    particle.source = source;
    particle.tint = tint;
    particle.vx = vx;
    particle.vy = vy;
    particle.life = life;
    particle.maxLife = life;
    particle.size = size;
    particle.gravity = gravity;
    particles_.push_back(particle);
}

void Game::addToast(const std::string& text, const std::string& sub) {
    Toast toast;
    toast.text = text;
    toast.sub = sub;
    toast.life = toast.maxLife = 4.0f;
    toasts_.push_back(toast);
    while (toasts_.size() > 4) toasts_.pop_front();
}

void Game::addChat(const std::string& from, const std::string& text) {
    ChatLine line;
    line.from = from;
    line.text = text;
    line.age = 0.0f;
    chatLines_.push_back(line);
    while (chatLines_.size() > 100) chatLines_.pop_front();
}

std::string Game::currentHeldItem() const {
    if (selected_ < 0 || selected_ >= INV_SIZE) return "";
    return inventory_[selected_].id;
}

int Game::findEntityAtScreen(Vector2 mouse) {
    int best = -1;
    float p = blockPixels();
    for (const auto& [id, entity] : entities_) {
        if (entity.type == "item") continue;
        Vector2 screen = worldToScreen(entity.x, entity.y);
        float width = (entity.type == "pig" ? 1.1f : 0.7f) * p;
        float height = (entity.type == "pig" ? 0.9f : 1.95f) * p;
        Rectangle rect{screen.x - width * 0.5f, screen.y - height, width, height};
        if (CheckCollisionPointRec(mouse, rect)) {
            float dx = entity.x - body_.x;
            float dy = (entity.y - 0.8f) - body_.centerY();
            if (dx * dx + dy * dy <= 4.5f * 4.5f) best = id;
        }
    }
    return best;
}

// ---------------------------------------------------------------------------
// Frame / network tick
// ---------------------------------------------------------------------------

void Game::frame(float dt) {
    dt = std::min(dt, 0.1f);
    updateGlobalInput();
    if (screen_ == Screen::Title || screen_ == Screen::Multiplayer || screen_ == Screen::Settings) {
        titleAnimation_ += dt;
        if (IsKeyPressed(KEY_F11)) ToggleFullscreen();
        if (screen_ == Screen::Multiplayer) {
            int codepoint = GetCharPressed();
            while (codepoint > 0) {
                if (codepoint >= 32) addressInput_ += utf8FromCodepoint(codepoint);
                codepoint = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE)) {
                if (!addressInput_.empty()) popLastUtf8(addressInput_);
            }
            if (IsKeyPressed(KEY_ESCAPE)) setScreen(Screen::Title);
            if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
                connectToAddress(addressInput_, false, false);
            }
        }
    }
    if (screen_ == Screen::Connecting) {
        connectingTimer_ += dt;
        if (IsKeyPressed(KEY_ESCAPE)) {
            net_.close();
            if (ownsServer_) stopOwnedServer();
            disconnectReason_ = "连接已取消";
            setScreen(Screen::Title);
        }
        if (connectingTimer_ > 12.0f && net_.connected && !hasWorld_) {
            net_.close();
            disconnectReason_ = "连接超时：没有收到世界数据";
            setScreen(Screen::Disconnected);
        }
    }
    pollNetwork();
    if (screen_ == Screen::Playing) {
        updatePlaying(dt);
        updateCamera(dt);
        updateRendererSmoothing(dt);
        updateParticles(dt);
        tickAccumulator_ += dt;
        if (tickAccumulator_ > 0.25f) tickAccumulator_ = 0.25f;
        while (tickAccumulator_ >= FIXED_DT) {
            fixedTick();
            tickAccumulator_ -= FIXED_DT;
        }
    } else {
        updateParticles(dt);
    }

    if (screen_ == Screen::Playing && dead_ && overlay_ != Overlay::Death) {
        overlay_ = Overlay::Death;
        ShowCursor();
    }

    if (!screenshotPath_.empty() && !screenshotTaken_) {
        autoScreenshotTimer_ += dt;
        if (autoScreenshotTimer_ >= args_.screenshotDelay) {
            bool wantsUi = !screenshotUi_.empty() && screenshotUi_ != "none";
            if (wantsUi && !screenshotUiApplied_) {
                applyScreenshotUi();
                screenshotUiApplied_ = true;
                // Let the requested overlay be rendered for a few frames so
                // the captured back/front buffer definitely contains it.
                autoScreenshotTimer_ = args_.screenshotDelay - 0.18f;
            } else {
                takeScreenshot(screenshotPath_);
                screenshotTaken_ = true;
                if (autoQuit_ || quitAfterScreenshot_) running_ = false;
            }
        }
    }
    render();
    if (screenshotRequested_) {
        // TakeScreenshot is raylib's tested capture path. On high-DPI Windows
        // the raw framebuffer can contain unused black area, so crop back to
        // the logical render size before writing the requested file.
        std::filesystem::path finalPath = screenshotRequestPath_;
        std::filesystem::path tempPath = root_ / ".mc2d_capture.png";
        std::error_code ec;
        std::filesystem::remove(tempPath, ec);
        TakeScreenshot(".mc2d_capture.png");
        Image image = LoadImage(tempPath.string().c_str());
        if (image.data != nullptr) {
            int contentWidth = GetRenderWidth();
            int contentHeight = GetRenderHeight();
            TraceLog(LOG_INFO, "Capture geometry: raw %dx%d / render %dx%d", image.width, image.height,
                     contentWidth, contentHeight);
            if (image.width > contentWidth || image.height > contentHeight) {
                Rectangle crop{0.0f, static_cast<float>(std::max(0, image.height - contentHeight)),
                               static_cast<float>(std::min(contentWidth, image.width)),
                               static_cast<float>(std::min(contentHeight, image.height))};
                ImageCrop(&image, crop);
            }
            if (finalPath.has_parent_path()) {
                std::filesystem::create_directories(finalPath.parent_path(), ec);
            }
            std::filesystem::remove(finalPath, ec);
            ExportImage(image, finalPath.string().c_str());
            UnloadImage(image);
        }
        std::filesystem::remove(tempPath, ec);
        screenshotRequested_ = false;
    }
}

void Game::applyScreenshotUi() {
    if (screen_ != Screen::Playing) return;
    if (screenshotUi_ == "inventory") {
        overlay_ = Overlay::Inventory;
        inventoryLayoutBuilt_ = false;
        ShowCursor();
    } else if (screenshotUi_ == "pause") {
        overlay_ = Overlay::Pause;
        ShowCursor();
    } else if (screenshotUi_ == "death") {
        overlay_ = Overlay::Death;
        ShowCursor();
    } else if (screenshotUi_ == "debug") {
        showDebug_ = true;
    }
}

void Game::takeScreenshot(const std::string& explicitPath) {
    std::filesystem::path path;
    if (!explicitPath.empty()) {
        path = std::filesystem::path(explicitPath);
        if (path.is_relative()) path = root_ / path;
    } else {
        std::time_t now = std::time(nullptr);
        std::tm local{};
        localtime_s(&local, &now);
        std::ostringstream name;
        name << "screenshots/screenshot_" << std::put_time(&local, "%Y%m%d_%H%M%S") << ".png";
        path = root_ / name.str();
    }
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path());
    screenshotRequestPath_ = path.string();
    screenshotRequested_ = true;
    addToast("截图已保存", path.filename().string());
    TraceLog(LOG_INFO, "Screenshot requested: %s", screenshotRequestPath_.c_str());
}

// ---------------------------------------------------------------------------
// Rendering helpers
// ---------------------------------------------------------------------------

void Game::render() {
    BeginDrawing();
    switch (screen_) {
    case Screen::Title: renderTitle(); break;
    case Screen::Multiplayer: renderMultiplayer(); break;
    case Screen::Settings: renderSettings(); break;
    case Screen::Connecting: renderConnecting(); break;
    case Screen::Playing: renderPlaying(); break;
    case Screen::Disconnected: renderDisconnected(); break;
    }
    EndDrawing();
}

void Game::drawText(const std::string& text, float x, float y, float size, Color color, bool shadow) {
    Font& font = assets_.font();
    float spacing = std::max(1.0f, size * 0.025f);
    if (shadow) {
        DrawTextEx(font, text.c_str(), Vector2{std::round(x) + std::max(1.0f, size * 0.07f),
                                               std::round(y) + std::max(1.0f, size * 0.07f)},
                   size, spacing, COLOR_TEXT_SHADOW);
    }
    DrawTextEx(font, text.c_str(), Vector2{std::round(x), std::round(y)}, size, spacing, color);
}

void Game::drawTextCentered(const std::string& text, float centerX, float y, float size, Color color,
                            bool shadow) {
    float width = measureText(text, size);
    drawText(text, centerX - width * 0.5f, y, size, color, shadow);
}

float Game::measureText(const std::string& text, float size) {
    float spacing = std::max(1.0f, size * 0.025f);
    return MeasureTextEx(assets_.font(), text.c_str(), size, spacing).x;
}

void Game::drawMenuDirt() {
    Texture2D& dirt = assets_.tex("block/dirt");
    float tile = 64.0f * std::max(1.0f, std::floor(uiScale_ / 2.0f));
    for (float y = 0; y < GetScreenHeight(); y += tile) {
        for (float x = 0; x < GetScreenWidth(); x += tile) {
            DrawTexturePro(dirt, Rectangle{0, 0, 16, 16}, Rectangle{x, y, tile, tile},
                           Vector2{0, 0}, 0.0f, Color{95, 95, 95, 255});
        }
    }
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{10, 10, 16, 130});
}

void Game::drawPanel(Rectangle rect, float alpha) {
    DrawRectangleRec(rect, Color{12, 12, 18, static_cast<unsigned char>(220 * alpha)});
    DrawRectangleLinesEx(rect, 3.0f, Color{100, 100, 112, static_cast<unsigned char>(240 * alpha)});
    DrawRectangleLinesEx(Rectangle{rect.x + 5, rect.y + 5, rect.width - 10, rect.height - 10}, 1.0f,
                         Color{200, 200, 210, static_cast<unsigned char>(80 * alpha)});
}

bool Game::drawButton(Rectangle rect, const std::string& label, bool enabled) {
    bool hovered = enabled && CheckCollisionPointRec(GetMousePosition(), rect);
    Texture2D& texture = assets_.tex(hovered ? "gui/sprites/widget/button_highlighted"
                                             : "gui/sprites/widget/button");
    // Nine-slice the vanilla 200x20 button.
    float scale = rect.height / 20.0f;
    float left = 4.0f * scale;
    float right = 4.0f * scale;
    DrawTexturePro(texture, Rectangle{0, 0, 4, 20}, Rectangle{rect.x, rect.y, left, rect.height},
                   Vector2{0, 0}, 0.0f, enabled ? WHITE : Color{160, 160, 160, 255});
    DrawTexturePro(texture, Rectangle{4, 0, 192, 20},
                   Rectangle{rect.x + left, rect.y, rect.width - left - right, rect.height},
                   Vector2{0, 0}, 0.0f, enabled ? WHITE : Color{160, 160, 160, 255});
    DrawTexturePro(texture, Rectangle{196, 0, 4, 20},
                   Rectangle{rect.x + rect.width - right, rect.y, right, rect.height},
                   Vector2{0, 0}, 0.0f, enabled ? WHITE : Color{160, 160, 160, 255});
    Color textColor = enabled ? (hovered ? Color{255, 255, 170, 255} : WHITE) : Color{130, 130, 130, 255};
    float textSize = 8.0f * uiScale_;
    drawTextCentered(label, rect.x + rect.width * 0.5f, rect.y + (rect.height - textSize) * 0.5f - 2.0f,
                     textSize, textColor);
    return hovered;
}

void Game::drawTooltip(const std::string& text, Vector2 mouse) {
    if (text.empty()) return;
    float size = 7.0f * uiScale_;
    float width = measureText(text, size) + 24;
    float height = size + 16;
    float x = std::min(mouse.x + 18, GetScreenWidth() - width - 8);
    float y = std::min(mouse.y + 12, GetScreenHeight() - height - 8);
    DrawRectangleRounded(Rectangle{x, y, width, height}, 0.25f, 4, Color{20, 12, 35, 240});
    DrawRectangleLinesEx(Rectangle{x, y, width, height}, 1.5f, Color{120, 80, 190, 255});
    drawText(text, x + 10, y + 6, size, WHITE);
}

void Game::drawItemIcon(const std::string& id, Rectangle rect) {
    const ItemDef* def = itemDef(id);
    std::string texture = def ? def->tex : "block/dirt";
    Texture2D& tex = assets_.tex(texture);
    DrawTexturePro(tex, Rectangle{0, 0, (float)tex.width, (float)tex.height}, rect, Vector2{0, 0}, 0.0f,
                   WHITE);
}

void Game::drawItemStack(const ItemStack& stack, Rectangle rect, float scale) {
    if (stack.empty()) return;
    float inset = rect.width * 0.09f;
    drawItemIcon(stack.id, Rectangle{rect.x + inset, rect.y + inset, rect.width - inset * 2,
                                     rect.height - inset * 2});
    if (stack.count > 1) {
        float size = 8.0f * scale;
        std::string count = std::to_string(stack.count);
        float width = measureText(count, size);
        drawText(count, rect.x + rect.width - width - 3, rect.y + rect.height - size - 2, size, WHITE);
    }
}

void Game::drawInventorySlot(Rectangle rect, const ItemStack& stack, bool hovered, bool selected) {
    Texture2D& slot = assets_.tex("gui/sprites/container/slot");
    DrawTexturePro(slot, Rectangle{0, 0, 18, 18}, rect, Vector2{0, 0}, 0.0f,
                   hovered ? Color{180, 200, 180, 255} : WHITE);
    if (selected) DrawRectangleLinesEx(rect, 3.0f, COLOR_GOLD);
    drawItemStack(stack, rect, uiScale_ / 3.0f);
}

// ---------------------------------------------------------------------------
// Menus
// ---------------------------------------------------------------------------

void Game::renderTitle() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    Texture2D& background = assets_.tex("gui/title/background/Background.jpg");
    float coverScale = std::max(static_cast<float>(width) / background.width,
                                static_cast<float>(height) / background.height) * 1.06f;
    float bw = background.width * coverScale;
    float bh = background.height * coverScale;
    float drift = std::sin(titleAnimation_ * 0.05f) * 18.0f;
    DrawTexturePro(background, Rectangle{0, 0, (float)background.width, (float)background.height},
                   Rectangle{(width - bw) * 0.5f + drift, (height - bh) * 0.5f, bw, bh},
                   Vector2{0, 0}, 0.0f, WHITE);
    DrawRectangleGradientV(0, 0, width, height, Color{10, 15, 30, 110}, Color{5, 6, 12, 180});

    Texture2D& logo = assets_.tex("gui/title/minecraft.png");
    float logoWidth = std::min(static_cast<float>(width) * 0.62f, 760.0f);
    float logoHeight = logoWidth * 0.25f;
    float logoX = (width - logoWidth) * 0.5f;
    float logoY = height * 0.11f;
    DrawTexturePro(logo, Rectangle{0, 0, (float)logo.width, (float)logo.height},
                   Rectangle{logoX + 8, logoY + 12, logoWidth, logoHeight}, Vector2{0, 0}, 0.0f,
                   Color{0, 0, 0, 100});
    DrawTexturePro(logo, Rectangle{0, 0, (float)logo.width, (float)logo.height},
                   Rectangle{logoX, logoY, logoWidth, logoHeight}, Vector2{0, 0}, 0.0f, WHITE);

    float splashSize = 26.0f * splashScale_;
    float splashX = logoX + logoWidth * 0.82f;
    float splashY = logoY + logoHeight * 0.72f;
    DrawTextPro(assets_.font(), splashText_.c_str(), Vector2{splashX, splashY}, Vector2{0, 0}, -12.0f,
                splashSize, 1.0f, COLOR_GOLD);
    float wobble = 1.0f + std::sin(titleAnimation_ * 4.0f) * 0.035f;
    (void)wobble;

    float buttonWidth = std::min(static_cast<float>(width) * 0.32f, 420.0f);
    float buttonHeight = 52.0f * uiScale_ / 3.0f;
    float buttonX = (width - buttonWidth) * 0.5f;
    float firstY = height * 0.52f;
    float gap = buttonHeight + 13;
    Rectangle single{buttonX, firstY, buttonWidth, buttonHeight};
    Rectangle multi{buttonX, firstY + gap, buttonWidth, buttonHeight};
    Rectangle settings{buttonX, firstY + gap * 2, buttonWidth, buttonHeight};
    Rectangle quit{buttonX, firstY + gap * 3, buttonWidth, buttonHeight};
    drawButton(single, "单人游戏");
    drawButton(multi, "多人游戏");
    drawButton(settings, "设置...");
    drawButton(quit, "退出游戏");

    if (CheckCollisionPointRec(GetMousePosition(), single) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        myName_ = args_.name.empty() ? "Steve" : args_.name;
        connectToAddress("127.0.0.1:25565", true, true);
    } else if (CheckCollisionPointRec(GetMousePosition(), multi) &&
               IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        setScreen(Screen::Multiplayer);
    } else if (CheckCollisionPointRec(GetMousePosition(), settings) &&
               IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        settingsReturn_ = Screen::Title;
        setScreen(Screen::Settings);
    } else if (CheckCollisionPointRec(GetMousePosition(), quit) &&
               IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        running_ = false;
    }

    std::string version = "Minecraft 2D 1.0  ·  C++ / raylib  ·  客户端 + 服务端";
    float footerSize = 7.0f * uiScale_;
    drawText(version, 18, height - footerSize - 14, footerSize, Color{235, 235, 235, 220});
    std::string credit = "非官方项目 · 资源来自 assets";
    drawText(credit, width - measureText(credit, footerSize) - 18,
             height - footerSize - 14, footerSize, Color{235, 235, 235, 190});
}

void Game::renderMultiplayer() {
    drawMenuDirt();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float panelWidth = std::min(720.0f, width * 0.72f);
    Rectangle panel{(width - panelWidth) * 0.5f, height * 0.2f, panelWidth, height * 0.56f};
    drawPanel(panel);
    drawTextCentered("多人游戏", width * 0.5f, panel.y + 26, 14.0f * uiScale_, WHITE);
    float labelSize = 8.0f * uiScale_;
    drawText("服务器地址", panel.x + 48, panel.y + 112, labelSize, Color{220, 220, 220, 255});
    Rectangle field{panel.x + 48, panel.y + 150, panel.width - 96, 58};
    Texture2D& fieldTex = assets_.tex("gui/sprites/widget/text_field");
    DrawTexturePro(fieldTex, Rectangle{0, 0, 200, 20}, field, Vector2{0, 0}, 0.0f, WHITE);
    drawText(addressInput_, field.x + 18, field.y + 15, 8.0f * uiScale_, WHITE);
    drawText("支持 host:port，默认端口 25565", panel.x + 50, panel.y + 220, 6.0f * uiScale_,
             Color{180, 190, 200, 255});
    Rectangle join{panel.x + 48, panel.y + panel.height - 130, panel.width - 96, 54};
    drawButton(join, "加入服务器");
    Rectangle back{panel.x + 48, panel.y + panel.height - 64, panel.width - 96, 46};
    drawButton(back, "返回");
    if (CheckCollisionPointRec(GetMousePosition(), join) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        connectToAddress(addressInput_, false, false);
    }
    if (CheckCollisionPointRec(GetMousePosition(), back) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        setScreen(Screen::Title);
    }
}

void Game::renderSettings() {
    drawMenuDirt();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float panelWidth = std::min(1080.0f, width * 0.9f);
    float panelHeight = std::min(620.0f, height * 0.84f);
    Rectangle panel{(width - panelWidth) * 0.5f, (height - panelHeight) * 0.5f, panelWidth, panelHeight};
    drawPanel(panel);
    drawTextCentered("设置与操作", width * 0.5f, panel.y + 20, 14.0f * uiScale_, WHITE);
    drawText("控制方案与原版一致。", panel.x + 48, panel.y + 84, 8.0f * uiScale_,
             Color{230, 230, 230, 255});
    struct Row { const char* key; const char* action; };
    Row left[] = {
        {"A / D", "向左 / 向右移动"},
        {"空格", "跳跃 / 游泳"},
        {"左 Shift", "潜行（防止跌落）"},
        {"左 Ctrl", "疾跑"},
        {"双击 A / D", "疾跑"},
        {"鼠标左键", "攻击 / 挖掘"},
        {"鼠标右键", "放置 / 使用 / 进食"},
        {"1 - 9 / 滚轮", "选择快捷栏"},
    };
    Row right[] = {
        {"E", "打开或关闭背包"},
        {"Q", "丢弃选中的物品"},
        {"T / Enter", "打开聊天"},
        {"/", "输入指令"},
        {"Tab", "在线玩家列表"},
        {"F1", "隐藏 / 显示 HUD"},
        {"F2", "截图"},
        {"F3", "调试信息"},
        {"F5", "切换镜头远近"},
        {"F11", "全屏"},
        {"Esc", "暂停 / 返回"},
    };
    float size = 6.5f * uiScale_;
    float columnY = panel.y + 140;
    float leftX = panel.x + 56;
    float rightX = panel.x + panelWidth * 0.53f;
    drawText("移动与交互", leftX, columnY - 38, size * 1.2f, COLOR_GOLD);
    for (const auto& row : left) {
        drawText(row.key, leftX, columnY, size, Color{120, 220, 255, 255});
        drawText(row.action, leftX + 175, columnY, size, WHITE);
        columnY += 48;
    }
    columnY = panel.y + 140;
    drawText("界面与视角", rightX, columnY - 38, size * 1.2f, COLOR_GOLD);
    for (const auto& row : right) {
        drawText(row.key, rightX, columnY, size, Color{120, 220, 255, 255});
        drawText(row.action, rightX + 190, columnY, size, WHITE);
        columnY += 37;
    }
    Rectangle back{panel.x + panelWidth * 0.5f - 170, panel.y + panelHeight - 72, 340, 48};
    drawButton(back, "返回");
    if (CheckCollisionPointRec(GetMousePosition(), back) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        setScreen(settingsReturn_);
    }
}

void Game::renderConnecting() {
    drawMenuDirt();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    drawTextCentered("Minecraft 2D", width * 0.5f, height * 0.30f, 18.0f * uiScale_, WHITE);
    int dots = static_cast<int>(connectingTimer_ * 2.0f) % 4;
    std::string label = connectingLabel_ + std::string(dots, '.');
    drawTextCentered(label, width * 0.5f, height * 0.45f, 8.0f * uiScale_,
                     Color{235, 235, 235, 255});
    if (startingLocalServer_) {
        drawTextCentered("首次启动需要生成世界，请稍候", width * 0.5f, height * 0.55f,
                         6.0f * uiScale_, Color{200, 200, 160, 255});
    }
    drawTextCentered("按 Esc 取消", width * 0.5f, height * 0.66f, 6.0f * uiScale_,
                     Color{190, 190, 190, 255});
}

void Game::renderDisconnected() {
    drawMenuDirt();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float panelWidth = std::min(760.0f, width * 0.76f);
    Rectangle panel{(width - panelWidth) * 0.5f, height * 0.24f, panelWidth, height * 0.5f};
    drawPanel(panel);
    drawTextCentered("连接中断", width * 0.5f, panel.y + 24, 14.0f * uiScale_, WHITE);
    drawTextCentered(disconnectReason_, width * 0.5f, panel.y + 112, 7.0f * uiScale_,
                     Color{255, 190, 150, 255});
    drawTextCentered("请确认服务器进程正在运行。也可以直接点击“单人游戏”自动启动本地服务器。",
                     width * 0.5f, panel.y + 160, 6.0f * uiScale_, Color{210, 210, 210, 255});
    Rectangle again{panel.x + 80, panel.y + panel.height - 125, panel.width * 0.5f - 100, 52};
    Rectangle back{panel.x + panel.width * 0.5f + 20, panel.y + panel.height - 125,
                   panel.width * 0.5f - 100, 52};
    drawButton(again, "重新连接");
    drawButton(back, "返回标题");
    if (CheckCollisionPointRec(GetMousePosition(), again) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        bool local = lastAddress_.empty() || lastAddress_ == "127.0.0.1:25565";
        connectToAddress(lastAddress_.empty() ? addressInput_ : lastAddress_, local, local);
    }
    if (CheckCollisionPointRec(GetMousePosition(), back) && IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        stopOwnedServer();
        setScreen(Screen::Title);
    }
}

// ---------------------------------------------------------------------------
// World rendering
// ---------------------------------------------------------------------------

Vector2 Game::worldToScreen(float wx, float wy) const {
    float p = blockPixels();
    return Vector2{
        (wx - cameraX_) * p + GetScreenWidth() * 0.5f,
        (wy - cameraY_) * p + GetScreenHeight() * 0.5f,
    };
}

Vector2 Game::screenToWorld(float sx, float sy) const {
    float p = blockPixels();
    return Vector2{
        (sx - GetScreenWidth() * 0.5f) / p + cameraX_,
        (sy - GetScreenHeight() * 0.5f) / p + cameraY_,
    };
}

bool Game::isNight() const {
    uint64_t t = worldTime_ % 24000;
    return t >= 13000 && t < 23000;
}

float Game::daylightFactor() const {
    float progress = static_cast<float>(worldTime_ % 24000) / 24000.0f;
    float elevation = std::sin(progress * 2.0f * PI);
    return std::clamp(0.16f + 0.9f * std::max(0.0f, elevation), 0.16f, 1.0f);
}

void Game::renderSky(float daylight) {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float progress = static_cast<float>(worldTime_ % 24000) / 24000.0f;
    float angle = progress * 2.0f * PI;
    float elevation = std::sin(angle);
    float twilight = std::clamp(1.0f - std::abs(elevation) * 4.0f, 0.0f, 1.0f);

    Color nightTop{5, 8, 20, 255};
    Color nightBottom{16, 24, 48, 255};
    Color dayTop{58, 120, 226, 255};
    Color dayBottom{175, 214, 255, 255};
    float day = clamp01((elevation + 0.08f) * 2.5f);
    Color top = lerpColor(nightTop, dayTop, day);
    Color bottom = lerpColor(nightBottom, dayBottom, day);
    DrawRectangleGradientV(0, 0, width, height, top, bottom);

    // Stars.
    if (daylight < 0.6f) {
        float alpha = (1.0f - daylight / 0.6f) * 200.0f;
        for (int i = 0; i < 110; ++i) {
            uint32_t h = static_cast<uint32_t>(i * 2654435761u);
            float sx = static_cast<float>(h % 1000) / 1000.0f * width;
            float sy = static_cast<float>((h >> 10) % 560) / 1000.0f * height * 0.72f;
            float twinkle = 0.55f + 0.45f * std::sin(titleAnimation_ * 1.5f + i * 2.3f);
            DrawRectangle(static_cast<int>(sx), static_cast<int>(sy), (i % 9 == 0) ? 2 : 1, 2,
                          Color{235, 240, 255, static_cast<unsigned char>(alpha * twinkle)});
        }
    }

    // Celestial bodies.
    float size = std::max(56.0f, static_cast<float>(height) * 0.105f);
    float horizon = height * 0.62f;
    if (progress < 0.5f) {
        float t = progress * 2.0f;
        float sx = width * (0.07f + t * 0.86f);
        float sy = horizon - std::sin(t * PI) * height * 0.48f;
        DrawCircleV(Vector2{sx, sy}, size * 0.95f, Color{255, 245, 190, static_cast<unsigned char>(35 * day)});
        if (sunTexture_.id != 0) {
            DrawTexturePro(sunTexture_, Rectangle{0, 0, 32, 32},
                           Rectangle{sx - size * 0.5f, sy - size * 0.5f, size, size},
                           Vector2{0, 0}, 0.0f, Color{255, 250, 225, 255});
        }
    } else {
        float t = (progress - 0.5f) * 2.0f;
        float sx = width * (0.07f + t * 0.86f);
        float sy = horizon - std::sin(t * PI) * height * 0.48f;
        DrawCircleV(Vector2{sx, sy}, size * 0.85f, Color{190, 205, 255, 28});
        if (moonTexture_.id != 0) {
            int phase = static_cast<int>((worldTime_ / 24000) % 8);
            Rectangle source{static_cast<float>((phase % 4) * 32), static_cast<float>((phase / 4) * 32), 32, 32};
            DrawTexturePro(moonTexture_, source,
                           Rectangle{sx - size * 0.45f, sy - size * 0.45f, size * 0.9f, size * 0.9f},
                           Vector2{0, 0}, 0.0f, WHITE);
        }
    }

    // Sunset glow near the horizon.
    if (twilight > 0.01f) {
        DrawRectangleGradientV(0, static_cast<int>(height * 0.30f), width,
                               static_cast<int>(height * 0.55f),
                               Color{255, 135, 75, 0},
                               Color{255, 125, 65, static_cast<unsigned char>(80 * twilight)});
    }

    // Pixel clouds with two parallax layers.
    float cloudUnit = std::max(1.0f, static_cast<float>(width) / 1280.0f);
    unsigned char cloudAlpha = static_cast<unsigned char>((28 + 55 * daylight) * 0.8f);
    for (int layer = 0; layer < 2; ++layer) {
        float scroll = cameraX_ * (layer == 0 ? 6.0f : 13.0f) + static_cast<float>(worldTime_) * 0.10f;
        float spacing = 430.0f * cloudUnit;
        float baseY = height * (0.08f + layer * 0.085f);
        for (int i = -1; i < 6; ++i) {
            float cx = std::fmod(-scroll + i * spacing + width * 3.0f, spacing * 5.0f) - spacing;
            uint32_t seed = static_cast<uint32_t>(i + layer * 17);
            float wobble = std::sin(titleAnimation_ * 0.2f + seed) * 5.0f;
            float cy = baseY + wobble + (seed % 3) * 18.0f * cloudUnit;
            Color cloud = Color{255, 255, 255, cloudAlpha};
            Color shadow = Color{195, 210, 230, static_cast<unsigned char>(cloudAlpha * 0.45f)};
            float u = cloudUnit;
            DrawRectangle(static_cast<int>(cx), static_cast<int>(cy + 10 * u), static_cast<int>(180 * u),
                          static_cast<int>(20 * u), shadow);
            DrawRectangle(static_cast<int>(cx + 24 * u), static_cast<int>(cy + 6 * u),
                          static_cast<int>(150 * u), static_cast<int>(18 * u), cloud);
            DrawRectangle(static_cast<int>(cx + 55 * u), static_cast<int>(cy), static_cast<int>(88 * u),
                          static_cast<int>(20 * u), cloud);
            DrawRectangle(static_cast<int>(cx + 86 * u), static_cast<int>(cy - 12 * u),
                          static_cast<int>(42 * u), static_cast<int>(16 * u), cloud);
        }
    }
    renderParallax(daylight);
}

void Game::renderParallax(float daylight) {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float p = blockPixels();
    float horizon = (69.0f - cameraY_) * p + height * 0.5f;
    // Anchor the skyline to the local terrain rather than a fixed world height.
    if (hasWorld_) {
        int feetX = std::clamp(static_cast<int>(std::floor(cameraX_)), 0, world_.width() - 1);
        int surface = -1;
        for (int y = 2; y < world_.height(); ++y) {
            BlockId ground = world_.fg(feetX, y);
            if (ground == GRASS_BLOCK || ground == SAND || ground == DIRT) {
                surface = y;
                break;
            }
        }
        if (surface < 0) surface = world_.surfaceY(feetX);
        if (surface >= 0 && surface < world_.height()) {
            horizon = (static_cast<float>(surface) + 0.35f - cameraY_) * p + height * 0.5f;
        }
    }
    if (horizon < -100 || horizon > height + 100) return;

    float day = clamp01(daylight * 1.2f);
    Color farColor = lerpColor(Color{17, 28, 38, 255}, Color{92, 130, 140, 255}, day);
    Color nearColor = lerpColor(Color{11, 22, 26, 255}, Color{48, 92, 72, 255}, day);
    auto ridge = [&](Color color, float parallax, float baseHeight, float step, float phase,
                     float amplitude) {
        float offset = cameraX_ * p * parallax;
        for (float x = -step; x < width + step; x += step) {
            float n = std::sin((x + offset) * 0.006f + phase) * 0.55f +
                      std::sin((x + offset) * 0.015f + phase * 2.0f) * 0.3f +
                      std::sin((x + offset) * 0.04f + phase * 4.0f) * 0.15f;
            float y = horizon - baseHeight - n * amplitude;
            DrawRectangle(static_cast<int>(x), static_cast<int>(y), static_cast<int>(step) + 1,
                          static_cast<int>(height - y + 200), color);
        }
    };
    ridge(farColor, 0.04f, 32.0f, 18.0f, 1.7f, 34.0f);
    ridge(nearColor, 0.10f, 6.0f, 14.0f, 4.3f, 22.0f);

    // Distant forest silhouettes.
    Color treeColor = lerpColor(Color{8, 22, 20, 200}, Color{52, 100, 72, 190}, day);
    float treeOffset = cameraX_ * p * 0.18f;
    for (int i = 0; i < 22; ++i) {
        uint32_t h = static_cast<uint32_t>(i * 1664525u + 1013904223u);
        float sx = std::fmod(static_cast<float>(h % 2000) * 0.7f - treeOffset + width * 4.0f,
                             static_cast<float>(width) + 240.0f) - 120.0f;
        float th = 26.0f + static_cast<float>((h >> 8) % 60) * 0.5f;
        DrawRectangle(static_cast<int>(sx), static_cast<int>(horizon - th),
                      static_cast<int>(3.0f * p / 48.0f * 1.3f), static_cast<int>(th), treeColor);
        for (int layer = 0; layer < 3; ++layer) {
            float cy = horizon - th - layer * 9.0f;
            float cw = 21.0f - layer * 4.0f;
            DrawRectangle(static_cast<int>(sx - cw * 0.5f + 2), static_cast<int>(cy),
                          static_cast<int>(cw), 15, treeColor);
        }
    }
}

void Game::renderTile(int x, int y, bool background, float daylight) {
    BlockId id = background ? world_.bg(x, y) : world_.fg(x, y);
    if (id == AIR) return;
    const BlockDef& def = blockDef(id);
    if (!def.tex || !*def.tex) return;
    float p = blockPixels();
    Vector2 screen = worldToScreen(static_cast<float>(x), static_cast<float>(y));
    Rectangle dest{screen.x - 0.5f, screen.y - 0.5f, p + 1.0f, p + 1.0f};
    Texture2D& texture = assets_.tex(def.tex);
    Rectangle source{0, 0, (float)texture.width, (float)texture.height};
    if ((id == WATER || id == LAVA) && texture.height >= 16) {
        int frames = std::max(1, texture.height / 16);
        int frame = static_cast<int>((worldTime_ / 5) % static_cast<uint64_t>(frames));
        source = Rectangle{0, static_cast<float>(frame * 16), 16, 16};
    }
    float light = lightMap_.score(world_, x, y, daylight);
    float brightness = 0.16f + 0.84f * light;
    if (background) brightness *= 0.56f;
    Color baseTint = WHITE;
    if (!background) {
        if (id == OAK_LEAVES) baseTint = Color{104, 174, 76, 255};
        else if (id == SHORT_GRASS || id == TALL_GRASS) baseTint = Color{119, 184, 84, 255};
        else if (id == OAK_SAPLING) baseTint = Color{124, 190, 90, 255};
    }
    Color tint = scaleColor(baseTint, brightness);
    if (def.cutout) tint.a = static_cast<unsigned char>(230 * std::clamp(brightness + 0.25f, 0.0f, 1.0f));
    if (id == WATER) {
        tint = Color{90, 150, 255, static_cast<unsigned char>(160 + 50 * light)};
    } else if (id == LAVA) {
        tint = Color{255, 170, 80, 255};
    }
    DrawTexturePro(texture, source, dest, Vector2{0, 0}, 0.0f, tint);
    if (background) {
        DrawRectangle(static_cast<int>(dest.x), static_cast<int>(dest.y), static_cast<int>(dest.width),
                      static_cast<int>(dest.height), Color{5, 8, 12, 45});
    }
}

void Game::renderWorldTiles() {
    float p = blockPixels();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    int x0 = static_cast<int>(std::floor(cameraX_ - width * 0.5f / p)) - 1;
    int x1 = static_cast<int>(std::ceil(cameraX_ + width * 0.5f / p)) + 1;
    int y0 = static_cast<int>(std::floor(cameraY_ - height * 0.5f / p)) - 1;
    int y1 = static_cast<int>(std::ceil(cameraY_ + height * 0.5f / p)) + 1;
    x0 = std::max(x0, 0);
    y0 = std::max(y0, 0);
    x1 = std::min(x1, world_.width() - 1);
    y1 = std::min(y1, world_.height() - 1);
    float daylight = daylightFactor();

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (world_.bg(x, y) != AIR) renderTile(x, y, true, daylight);
        }
    }
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (world_.fg(x, y) != AIR) renderTile(x, y, false, daylight);
        }
    }
    renderTargetHighlight();
}

void Game::renderTorchGlow() {
    float p = blockPixels();
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    int x0 = std::max(0, static_cast<int>(cameraX_ - width * 0.5f / p) - 1);
    int x1 = std::min(world_.width() - 1, static_cast<int>(cameraX_ + width * 0.5f / p) + 1);
    int y0 = std::max(0, static_cast<int>(cameraY_ - height * 0.5f / p) - 1);
    int y1 = std::min(world_.height() - 1, static_cast<int>(cameraY_ + height * 0.5f / p) + 1);
    BeginBlendMode(BLEND_ALPHA);
    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            BlockId id = world_.fg(x, y);
            int light = blockDef(id).light;
            if (light <= 0) continue;
            Vector2 screen = worldToScreen(x + 0.5f, y + 0.5f);
            float radius = (id == LAVA ? 6.5f : 5.0f) * p;
            float flicker = 0.85f + 0.15f * std::sin(titleAnimation_ * 9.0f + x * 1.7f + y);
            Color tint = id == LAVA ? Color{255, 140, 60, 58} : Color{255, 175, 70, 48};
            tint.a = static_cast<unsigned char>(tint.a * flicker);
            DrawTexturePro(glowTexture_, Rectangle{0, 0, 128, 128},
                           Rectangle{screen.x, screen.y, radius, radius}, Vector2{radius * 0.5f, radius * 0.5f},
                           0.0f, tint);
        }
    }
    EndBlendMode();
}

void Game::renderTargetHighlight() {
    if (!hoverValid_ || overlay_ != Overlay::None || dead_) return;
    float p = blockPixels();
    Vector2 screen = worldToScreen(static_cast<float>(hoverX_), static_cast<float>(hoverY_));
    Rectangle rect{screen.x, screen.y, p, p};
    DrawRectangleLinesEx(rect, 2.0f, Color{15, 15, 15, 170});
    DrawRectangleLinesEx(Rectangle{rect.x + 1, rect.y + 1, rect.width - 2, rect.height - 2}, 1.0f,
                         Color{255, 255, 255, 90});
    if (mineActive_ && mineX_ == hoverX_ && mineY_ == hoverY_ && mineProgress_ > 0.0f) {
        int stage = std::clamp(static_cast<int>(mineProgress_ * 10.0f), 0, 9);
        Texture2D& cracks = assets_.tex("block/destroy_stage_" + std::to_string(stage));
        DrawTexturePro(cracks, Rectangle{0, 0, 16, 16}, Rectangle{rect.x, rect.y, p, p}, Vector2{0, 0},
                       0.0f, Color{255, 255, 255, 210});
    }
}

void Game::renderRenderables(float daylight) {
    std::vector<WorldRenderable> renderables;
    renderables.push_back({WorldRenderable::Player, myId_, body_.y, -1});
    for (const auto& [id, remote] : remotePlayers_) {
        renderables.push_back({WorldRenderable::Player, id, remote.y, -1});
    }
    for (const auto& [id, entity] : entities_) {
        renderables.push_back({entity.type == "item" ? WorldRenderable::Item : WorldRenderable::Mob,
                               id, entity.y, -1});
    }
    std::sort(renderables.begin(), renderables.end(), [](const WorldRenderable& a, const WorldRenderable& b) {
        if (a.kind != b.kind) {
            if (a.kind == WorldRenderable::Item) return true;
            if (b.kind == WorldRenderable::Item) return false;
        }
        return a.y < b.y;
    });

    float p = blockPixels();
    for (const auto& item : renderables) {
        if (item.kind == WorldRenderable::Mob) {
            auto it = entities_.find(item.id);
            if (it == entities_.end()) continue;
            if (it->second.type == "pig") renderPig(it->second, daylight);
            else if (it->second.type == "zombie") renderZombie(it->second, daylight);
        } else if (item.kind == WorldRenderable::Item) {
            auto it = entities_.find(item.id);
            if (it == entities_.end()) continue;
            renderItemEntity(it->second, daylight);
        } else {
            if (item.id == myId_) {
                float brightness = std::max(0.45f, 0.25f + lightMap_.score(
                    world_, static_cast<int>(std::floor(body_.x)), static_cast<int>(std::floor(body_.y - 0.3f)), daylight));
                Color tint = scaleColor(WHITE, brightness);
                if (redFlash_ > 0.25f) tint = lerpColor(tint, Color{255, 80, 80, 255}, 0.45f);
                Vector2 screen = worldToScreen(renderX_, renderY_);
                DrawEllipse(static_cast<int>(screen.x), static_cast<int>(screen.y), p * 0.33f, p * 0.12f,
                            Color{0, 0, 0, 80});
                renderPlayerFigure("entity/player/wide/steve.png", renderX_, renderY_, facing_,
                                   body_.sneaking, body_.sprinting, body_.onGround, walkPhase_,
                                   swing_, tint, currentHeldItem(), 1.8f, true);
            } else {
                auto remoteIt = remotePlayers_.find(item.id);
                if (remoteIt == remotePlayers_.end()) continue;
                RemotePlayer& remote = remoteIt->second;
                static const char* skins[] = {
                    "entity/player/wide/steve.png", "entity/player/wide/alex.png",
                    "entity/player/wide/ari.png", "entity/player/wide/efe.png",
                    "entity/player/wide/kai.png", "entity/player/wide/makena.png",
                    "entity/player/wide/noor.png", "entity/player/wide/sunny.png",
                    "entity/player/wide/zuri.png",
                };
                std::string skin = skins[std::abs(remote.id) % 9];
                Vector2 screen = worldToScreen(remote.x, remote.y);
                DrawEllipse(static_cast<int>(screen.x), static_cast<int>(screen.y), p * 0.33f, p * 0.12f,
                            Color{0, 0, 0, 80});
                float brightness = std::max(0.45f, 0.25f + lightMap_.score(world_, static_cast<int>(remote.x),
                    static_cast<int>(remote.y - 0.3f), daylight));
                Color tint = scaleColor(WHITE, brightness);
                if (remote.dead) tint = Color{180, 180, 180, 150};
                renderPlayerFigure(skin, remote.x, remote.y, remote.facing, remote.sneak, remote.sprint,
                                   remote.onGround, remote.walkPhase, remote.swing, tint, "", 1.8f, false);
                // Name tag.
                float tagY = screen.y - 1.95f * p;
                float nameWidth = measureText(remote.name, 7.0f * uiScale_ * 0.7f);
                DrawRectangleRounded(Rectangle{screen.x - nameWidth * 0.5f - 8, tagY - 26, nameWidth + 16, 24},
                                     0.35f, 5, Color{0, 0, 0, 120});
                drawTextCentered(remote.name, screen.x, tagY - 24, 7.0f * uiScale_ * 0.7f, WHITE);
            }
        }
    }
}

void Game::renderPlayerFigure(const std::string& skin, float worldX, float worldY, int facing,
                              bool sneaking, bool sprinting, bool onGround, float walkPhase,
                              float swing, Color tint, const std::string& heldItem, float modelHeight,
                              bool showHeldItem) {
    Texture2D& texture = assets_.tex(skin);
    float p = blockPixels();
    Vector2 screen = worldToScreen(worldX, worldY);
    float unit = (modelHeight * p) / 32.0f;
    if (sneaking) unit *= 0.9f;
    float cx = screen.x;
    float feet = screen.y;
    float walkAmount = (onGround && std::abs(body_.vx) > 0.15f) || sprinting ? 1.0f : 0.35f;
    float legAngle = std::sin(walkPhase) * 28.0f * walkAmount;
    float armAngle = -std::sin(walkPhase) * 24.0f * walkAmount;
    if (swing > 0.01f) armAngle -= swing * 95.0f;
    if (sneaking) {
        legAngle *= 0.2f;
        armAngle *= 0.3f;
    }
    if (!onGround) {
        legAngle = 18.0f;
        armAngle = -22.0f;
    }

    auto drawPart = [&](Rectangle source, float localX, float localY, float w, float h, float rotation,
                        float originX, float originY) {
        float x = localX;
        if (facing < 0) x = -(localX + w);
        Rectangle src = source;
        if (facing < 0) src.width = -src.width;
        Rectangle dest{cx + x, feet + localY, w, h};
        DrawTexturePro(texture, src, dest, Vector2{originX, originY}, facing < 0 ? -rotation : rotation,
                       tint);
    };

    float headBob = onGround ? std::sin(walkPhase * 2.0f) * unit * 0.35f : 0.0f;
    float baseY = -32.0f * unit + headBob;
    if (sneaking) baseY += 2.0f * unit;

    // Back limbs.
    drawPart(Rectangle{20, 52, 4, 12}, -4.0f * unit, -12.0f * unit, 4.0f * unit, 12.0f * unit,
             legAngle * 0.65f, 2.0f * unit, 0.0f);
    drawPart(Rectangle{36, 52, 4, 12}, 4.0f * unit, -24.0f * unit, 4.0f * unit, 12.0f * unit,
             -armAngle * 0.6f, 2.0f * unit, 0.0f);

    // Torso.
    drawPart(Rectangle{20, 20, 8, 12}, -4.0f * unit, baseY + 8.0f * unit, 8.0f * unit, 12.0f * unit,
             0.0f, 4.0f * unit, 0.0f);

    // Front leg and arm.
    drawPart(Rectangle{4, 20, 4, 12}, -4.0f * unit, -12.0f * unit, 4.0f * unit, 12.0f * unit,
             -legAngle, 2.0f * unit, 0.0f);
    drawPart(Rectangle{44, 20, 4, 12}, 4.0f * unit, -24.0f * unit, 4.0f * unit, 12.0f * unit,
             armAngle, 2.0f * unit, 0.0f);

    // Head.
    drawPart(Rectangle{8, 8, 8, 8}, -4.0f * unit, baseY, 8.0f * unit, 8.0f * unit, 0.0f,
             4.0f * unit, 4.0f * unit);

    if (showHeldItem && !heldItem.empty()) {
        float handX = 6.5f * unit;
        float handY = -17.5f * unit;
        if (facing < 0) handX = -handX - 5.5f * unit;
        Rectangle handRect{cx + handX, feet + handY, 5.5f * unit, 5.5f * unit};
        drawItemIcon(heldItem, handRect);
    }
}

void Game::renderPig(const EntityView& entity, float daylight) {
    float p = blockPixels();
    Vector2 screen = worldToScreen(entity.x, entity.y);
    float brightness = std::max(0.35f, lightMap_.score(world_, static_cast<int>(entity.x),
                                                       static_cast<int>(entity.y - 0.3f), daylight));
    Color tint = scaleColor(WHITE, brightness);
    if (entity.hurt > 0) tint = lerpColor(tint, Color{255, 100, 100, 255}, 0.55f);
    float w = 1.25f * p;
    float h = 0.78f * p;
    bool flip = entity.facing < 0;
    DrawEllipse(static_cast<int>(screen.x), static_cast<int>(screen.y), w * 0.42f, p * 0.12f,
                Color{0, 0, 0, 70});
    float bodyY = screen.y - h;
    float bodyX = screen.x - w * 0.5f;
    if (flip) bodyX += w * 0.12f;
    DrawRectangle(static_cast<int>(bodyX), static_cast<int>(bodyY + h * 0.18f), static_cast<int>(w * 0.9f),
                  static_cast<int>(h * 0.62f), tint);
    // Legs.
    Color legTint = scaleColor(tint, 0.78f);
    for (int i = 0; i < 4; ++i) {
        float lx = bodyX + w * (0.15f + i * 0.2f);
        DrawRectangle(static_cast<int>(lx), static_cast<int>(screen.y - p * 0.17f),
                      static_cast<int>(p * 0.14f), static_cast<int>(p * 0.17f), legTint);
    }
    // Head.
    float headW = 0.55f * p;
    float headH = 0.55f * p;
    float headX = flip ? bodyX - headW * 0.55f : bodyX + w * 0.9f - headW * 0.55f;
    float headY = screen.y - h - headH * 0.38f;
    Texture2D& pig = assets_.tex("entity/pig/pig.png");
    Rectangle headSource{8, 8, 8, 8};
    if (flip) headSource.width = -8;
    DrawTexturePro(pig, headSource, Rectangle{headX, headY, headW, headH}, Vector2{0, 0}, 0.0f, tint);
    // Snout.
    Color snout = lerpColor(tint, Color{235, 130, 130, 255}, 0.45f);
    float snoutX = flip ? headX - headW * 0.18f : headX + headW * 0.74f;
    DrawRectangle(static_cast<int>(snoutX), static_cast<int>(headY + headH * 0.45f),
                  static_cast<int>(headW * 0.28f), static_cast<int>(headH * 0.32f), snout);
    // Curly tail.
    float tailX = flip ? bodyX + w * 0.9f : bodyX;
    DrawRectangle(static_cast<int>(tailX), static_cast<int>(bodyY + h * 0.3f), static_cast<int>(p * 0.08f),
                  static_cast<int>(p * 0.1f), legTint);
    if (entity.health < entity.maxHealth) {
        float barW = w * 0.8f;
        float barX = screen.x - barW * 0.5f;
        float barY = screen.y - h - headH - 18;
        DrawRectangle(static_cast<int>(barX), static_cast<int>(barY), static_cast<int>(barW), 5,
                      Color{40, 0, 0, 180});
        DrawRectangle(static_cast<int>(barX), static_cast<int>(barY),
                      static_cast<int>(barW * std::clamp(static_cast<float>(entity.health) /
                                                             std::max(1, entity.maxHealth), 0.0f, 1.0f)),
                      5, Color{220, 50, 50, 255});
    }
}

void Game::renderZombie(const EntityView& entity, float daylight) {
    float p = blockPixels();
    float brightness = std::max(0.32f, lightMap_.score(world_, static_cast<int>(entity.x),
                                                       static_cast<int>(entity.y - 0.3f), daylight));
    Color tint = scaleColor(WHITE, brightness);
    if (entity.hurt > 0) tint = lerpColor(tint, Color{255, 100, 100, 255}, 0.5f);
    Vector2 screen = worldToScreen(entity.x, entity.y);
    DrawEllipse(static_cast<int>(screen.x), static_cast<int>(screen.y), p * 0.32f, p * 0.1f,
                Color{0, 0, 0, 65});
    // A small custom zombie figure using the vanilla 64x64 skin layout.
    Texture2D& tex = assets_.tex("entity/zombie/zombie.png");
    float unit = (1.95f * p) / 32.0f;
    float walk = std::sin(entity.walkPhase) * 22.0f;
    auto part = [&](Rectangle src, float lx, float ly, float w, float h, float rot, float ox, float oy) {
        if (entity.facing < 0) {
            lx = -(lx + w);
            src.width = -src.width;
            rot = -rot;
        }
        DrawTexturePro(tex, src, Rectangle{screen.x + lx, screen.y + ly, w, h}, Vector2{ox, oy}, rot, tint);
    };
    part(Rectangle{20, 52, 4, 12}, -4 * unit, -12 * unit, 4 * unit, 12 * unit, walk * 0.6f, 2 * unit, 0);
    part(Rectangle{36, 52, 4, 12}, 4 * unit, -24 * unit, 4 * unit, 12 * unit, 70.0f, 2 * unit, 0);
    part(Rectangle{20, 20, 8, 12}, -4 * unit, -24 * unit, 8 * unit, 12 * unit, 0, 4 * unit, 0);
    part(Rectangle{4, 20, 4, 12}, -4 * unit, -12 * unit, 4 * unit, 12 * unit, -walk, 2 * unit, 0);
    part(Rectangle{44, 20, 4, 12}, 4 * unit, -24 * unit, 4 * unit, 12 * unit, 84.0f, 2 * unit, 0);
    part(Rectangle{8, 8, 8, 8}, -4 * unit, -32 * unit, 8 * unit, 8 * unit, 0, 4 * unit, 4 * unit);
    if (entity.health < entity.maxHealth) {
        float barW = p * 0.7f;
        float barY = screen.y - 1.95f * p - 16;
        DrawRectangle(static_cast<int>(screen.x - barW * 0.5f), static_cast<int>(barY), static_cast<int>(barW), 5,
                      Color{40, 0, 0, 180});
        DrawRectangle(static_cast<int>(screen.x - barW * 0.5f), static_cast<int>(barY),
                      static_cast<int>(barW * std::clamp(static_cast<float>(entity.health) /
                                                             std::max(1, entity.maxHealth), 0.0f, 1.0f)),
                      5, Color{220, 50, 50, 255});
    }
}

void Game::renderItemEntity(const EntityView& entity, float daylight) {
    float p = blockPixels();
    Vector2 screen = worldToScreen(entity.x, entity.y);
    float bob = std::sin(playTime_ * 3.0f + entity.id) * 3.0f;
    float size = p * 0.42f;
    DrawEllipse(static_cast<int>(screen.x), static_cast<int>(screen.y), p * 0.18f, p * 0.06f,
                Color{0, 0, 0, 60});
    float brightness = std::max(0.4f, lightMap_.score(world_, static_cast<int>(entity.x),
                                                      static_cast<int>(entity.y), daylight));
    const ItemDef* def = itemDef(entity.item);
    std::string tex = def ? def->tex : "block/dirt";
    Texture2D& texture = assets_.tex(tex);
    DrawTexturePro(texture, Rectangle{0, 0, (float)texture.width, (float)texture.height},
                   Rectangle{screen.x - size * 0.5f, screen.y - size - 4 + bob, size, size}, Vector2{0, 0},
                   0.0f, scaleColor(WHITE, brightness));
    if (entity.count > 1) {
        drawText(std::to_string(entity.count), screen.x + size * 0.25f, screen.y - size - 2 + bob,
                 6.0f * uiScale_, WHITE);
    }
}

void Game::renderParticles() {
    for (const auto& particle : particles_) {
        float alpha = std::clamp(particle.life / std::max(0.01f, particle.maxLife), 0.0f, 1.0f);
        Vector2 screen = worldToScreen(particle.x, particle.y);
        float size = particle.size * blockPixels();
        Color tint = particle.tint;
        tint.a = static_cast<unsigned char>(255 * alpha);
        if (particle.texture.empty()) {
            DrawRectangle(static_cast<int>(screen.x), static_cast<int>(screen.y), static_cast<int>(size),
                          static_cast<int>(size), tint);
        } else {
            Texture2D& texture = assets_.tex(particle.texture);
            DrawTexturePro(texture, particle.source,
                           Rectangle{screen.x - 1, screen.y - 1, size, size}, Vector2{0, 0}, 0.0f, tint);
        }
    }
}

// ---------------------------------------------------------------------------
// Play screen
// ---------------------------------------------------------------------------

void Game::renderPlaying() {
    if (!hasWorld_) {
        renderConnecting();
        return;
    }
    float p = blockPixels();
    float daylight = daylightFactor();
    int shakeX = 0, shakeY = 0;
    if (shake_ > 0.01f) {
        shakeX = static_cast<int>((static_cast<float>(random_() % 200) / 100.0f - 1.0f) * shake_ * 4.0f);
        shakeY = static_cast<int>((static_cast<float>(random_() % 200) / 100.0f - 1.0f) * shake_ * 4.0f);
    }

    renderSky(daylight);
    // Screen shake is applied by shifting the world transform for tile passes.
    // Simpler approach: draw tiles and renderables normally, then offset the
    // whole rendered scene is not possible without a framebuffer; use small
    // camera offsets instead.
    cameraX_ += shakeX / p;
    cameraY_ += shakeY / p;
    renderWorldTiles();
    renderTorchGlow();
    renderRenderables(daylight);
    renderParticles();
    cameraX_ -= shakeX / p;
    cameraY_ -= shakeY / p;

    if (chatOpen_) {
        Vector2 mouse = GetMousePosition();
        Vector2 world = screenToWorld(mouse.x, mouse.y);
        int bx = static_cast<int>(std::floor(world.x));
        int by = static_cast<int>(std::floor(world.y));
        if (world_.inBounds(bx, by)) {
            float dx = bx + 0.5f - body_.x;
            float dy = by + 0.5f - body_.eyeY();
            if (dx * dx + dy * dy <= 5.6f * 5.6f) {
                Vector2 screen = worldToScreen(bx, by);
                DrawRectangleLinesEx(Rectangle{screen.x, screen.y, p, p}, 2, Color{20, 20, 20, 150});
            }
        }
    }

    if (redFlash_ > 0.01f) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                      Color{180, 20, 20, static_cast<unsigned char>(std::min(120.0f, redFlash_ * 90))});
    }
    if (body_.onGround && world_.water(static_cast<int>(std::floor(body_.x)),
                                        static_cast<int>(std::floor(body_.eyeY())))) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{30, 80, 180, 75});
    }

    if (showHud_ && !dead_) renderHud();
    if (showHud_) {
        renderChat();
        renderToasts();
    }
    if (showPlayerList_) renderPlayerList();
    if (showDebug_) renderDebug();
    if (overlay_ == Overlay::Inventory) renderInventory();
    else if (overlay_ == Overlay::Pause) renderPause();
    else if (overlay_ == Overlay::Death) renderDeath();
}

void Game::renderHud() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float scale = uiScale_;
    float hotbarW = 182.0f * scale;
    float hotbarH = 22.0f * scale;
    float hotbarX = (width - hotbarW) * 0.5f;
    float hotbarY = height - hotbarH - 10.0f * scale;
    Texture2D& hotbar = assets_.tex("gui/sprites/hud/hotbar");
    DrawTexturePro(hotbar, Rectangle{0, 0, 182, 22}, Rectangle{hotbarX, hotbarY, hotbarW, hotbarH},
                   Vector2{0, 0}, 0.0f, WHITE);

    // Hearts.
    float iconScale = scale;
    float iconSize = 9.0f * iconScale;
    float heartsY = hotbarY - iconSize - 8.0f * scale;
    for (int i = 0; i < 10; ++i) {
        float x = hotbarX + i * iconSize;
        Texture2D& container = assets_.tex("gui/sprites/hud/heart/container");
        DrawTexturePro(container, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, iconSize, iconSize},
                       Vector2{0, 0}, 0.0f, WHITE);
        int value = health_ - i * 2;
        if (value >= 2) {
            Texture2D& full = assets_.tex("gui/sprites/hud/heart/full");
            DrawTexturePro(full, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, iconSize, iconSize},
                           Vector2{0, 0}, 0.0f, WHITE);
        } else if (value == 1) {
            Texture2D& half = assets_.tex("gui/sprites/hud/heart/half");
            DrawTexturePro(half, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, iconSize, iconSize},
                           Vector2{0, 0}, 0.0f, WHITE);
        }
    }
    // Food.
    for (int i = 0; i < 10; ++i) {
        float x = hotbarX + hotbarW - (i + 1) * iconSize;
        int foodValue = food_ - (9 - i) * 2;
        Texture2D& texture = assets_.tex(foodValue >= 1 ? "gui/sprites/hud/food_full"
                                                        : "gui/sprites/hud/food_empty");
        DrawTexturePro(texture, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, iconSize, iconSize},
                       Vector2{0, 0}, 0.0f, WHITE);
    }
    // Air bubbles.
    if (air_ < 300) {
        for (int i = 0; i < 10; ++i) {
            if (air_ < (i + 1) * 30) continue;
            float x = hotbarX + hotbarW - (i + 1) * iconSize;
            Texture2D& bubble = assets_.tex("gui/sprites/hud/air");
            DrawTexturePro(bubble, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY - iconSize - 4, iconSize, iconSize},
                           Vector2{0, 0}, 0.0f, WHITE);
        }
    }

    // Hotbar slots and items.
    for (int i = 0; i < HOTBAR_SIZE; ++i) {
        float slotX = hotbarX + i * 20.0f * scale + 1.0f * scale;
        float slotY = hotbarY + scale;
        Rectangle slot{slotX, slotY, 20.0f * scale, 20.0f * scale};
        if (i == selected_) {
            Texture2D& selection = assets_.tex("gui/sprites/hud/hotbar_selection");
            DrawTexturePro(selection, Rectangle{0, 0, 24, 23},
                           Rectangle{slot.x - 2.0f * scale, slot.y - 2.0f * scale, 24.0f * scale,
                                     23.0f * scale}, Vector2{0, 0}, 0.0f, WHITE);
        }
        drawItemStack(inventory_[i], Rectangle{slot.x, slot.y, 20.0f * scale, 20.0f * scale}, scale);
    }

    // Selected item name.
    if (selectedNameTimer_ > 0.0f && !inventory_[selected_].empty()) {
        std::string name = itemName(inventory_[selected_].id);
        float alpha = std::min(1.0f, selectedNameTimer_);
        float size = 7.0f * uiScale_;
        float labelWidth = measureText(name, size);
        float labelY = heartsY - size - 12;
        DrawRectangle(static_cast<int>((width - labelWidth) * 0.5f - 8), static_cast<int>(labelY - 5),
                      static_cast<int>(labelWidth + 16), static_cast<int>(size + 10),
                      Color{0, 0, 0, static_cast<unsigned char>(130 * alpha)});
        drawTextCentered(name, width * 0.5f, labelY, size, Color{255, 255, 255, static_cast<unsigned char>(255 * alpha)});
    }

    // Crosshair (mouse-targeted in this 2D adaption).
    if (overlay_ == Overlay::None && !chatOpen_) {
        Vector2 mouse = GetMousePosition();
        Texture2D& cross = assets_.tex("gui/sprites/hud/crosshair");
        float crossSize = 22.0f * scale / 2.0f;
        DrawTexturePro(cross, Rectangle{0, 0, 15, 15},
                       Rectangle{mouse.x - crossSize * 0.5f, mouse.y - crossSize * 0.5f, crossSize, crossSize},
                       Vector2{0, 0}, 0.0f, WHITE);
    }
    // Underwater overlay handled above; low health tint.
    if (health_ <= 6) {
        float pulse = 0.35f + std::sin(playTime_ * 5.0f) * 0.15f;
        DrawRectangleGradientV(0, 0, width, height, Color{120, 0, 0, 0},
                               Color{120, 0, 0, static_cast<unsigned char>(60 * pulse)});
    }
}

void Game::renderChat() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float size = 7.0f * uiScale_;
    float lineHeight = size + 7.0f;
    float bottom = height - 22.0f * uiScale_ - 24.0f;
    int visible = 10;
    float y = bottom - lineHeight * std::min<int>(visible, static_cast<int>(chatLines_.size()));
    if (chatOpen_) y = bottom - lineHeight * visible - 12;
    int skip = std::max(0, static_cast<int>(chatLines_.size()) - visible);
    for (int i = skip; i < static_cast<int>(chatLines_.size()); ++i) {
        const ChatLine& line = chatLines_[i];
        float alpha = 1.0f;
        if (!chatOpen_ && line.age > 8.0f) alpha = std::max(0.0f, 1.0f - (line.age - 8.0f) * 1.6f);
        if (alpha <= 0.0f) continue;
        std::string text = line.from.empty() ? line.text : ("<" + line.from + "> " + line.text);
        float textWidth = measureText(text, size);
        DrawRectangle(4, static_cast<int>(y - 3), static_cast<int>(textWidth + 14), static_cast<int>(lineHeight),
                      Color{0, 0, 0, static_cast<unsigned char>(110 * alpha)});
        drawText(text, 10, y, size, Color{255, 255, 255, static_cast<unsigned char>(240 * alpha)});
        y += lineHeight;
    }
    if (chatOpen_) {
        float boxY = height - 22.0f * uiScale_ - 38.0f;
        DrawRectangle(4, static_cast<int>(boxY), width - 8, 32, Color{0, 0, 0, 170});
        std::string text = "> " + chatInput_;
        drawText(text, 12, boxY + 5, size, WHITE);
        if (std::fmod(chatBlink_, 1.0f) < 0.55f) {
            float cursorX = 12 + measureText(text, size) + 2;
            DrawRectangle(static_cast<int>(cursorX), static_cast<int>(boxY + 5), 2, static_cast<int>(size + 2), WHITE);
        }
    }
}

void Game::renderToasts() {
    int width = GetScreenWidth();
    float size = 7.0f * uiScale_;
    float y = 24.0f;
    for (const auto& toast : toasts_) {
        float alpha = clamp01(toast.life / 0.6f);
        if (toast.life > toast.maxLife - 0.35f) {
            alpha = clamp01((toast.maxLife - toast.life) / 0.35f);
        }
        float subSize = size * 0.82f;
        float boxWidth = std::max(measureText(toast.text, size), measureText(toast.sub, subSize)) + 40;
        float boxHeight = 52;
        Rectangle box{width - boxWidth - 20.0f, y, boxWidth, boxHeight};
        DrawRectangleRounded(box, 0.22f, 6, Color{15, 10, 20, static_cast<unsigned char>(215 * alpha)});
        DrawRectangleLinesEx(box, 2, Color{255, 220, 100, static_cast<unsigned char>(230 * alpha)});
        drawText(toast.text, box.x + 14, box.y + 8, size, Color{255, 235, 120, static_cast<unsigned char>(255 * alpha)});
        if (!toast.sub.empty()) {
            drawText(toast.sub, box.x + 14, box.y + 29, subSize,
                     Color{220, 220, 220, static_cast<unsigned char>(220 * alpha)});
        }
        y += boxHeight + 8;
    }
}

void Game::renderDebug() {
    int width = GetScreenWidth();
    const int lines = 15;
    float size = 5.6f * uiScale_;
    float lineHeight = size + 6;
    DrawRectangle(0, 0, width, static_cast<int>(lineHeight * lines + 14), Color{0, 0, 0, 150});
    std::vector<std::string> info;
    info.push_back("Minecraft 2D 1.0  ·  " + std::to_string(GetFPS()) + " FPS");
    info.push_back("XYZ: " + std::to_string(body_.x) + " / " + std::to_string(body_.y) + "   方块: " +
                   std::to_string(static_cast<int>(std::floor(body_.x))) + ", " +
                   std::to_string(static_cast<int>(std::floor(body_.y))));
    info.push_back(std::string("朝向: ") + (facing_ > 0 ? "东 (+X)" : "西 (-X)") +
                   "   在地面: " + (body_.onGround ? "是" : "否") +
                   "   疾跑: " + (body_.sprinting ? "是" : "否"));
    int day = static_cast<int>(worldTime_ / 24000) + 1;
    int tod = static_cast<int>(worldTime_ % 24000);
    int hours = static_cast<int>((tod / 1000 + 6) % 24);
    int minutes = static_cast<int>((tod % 1000) * 60 / 1000);
    std::ostringstream time;
    time << "时间: " << std::setfill('0') << std::setw(2) << hours << ":" << std::setw(2) << minutes
         << "   第 " << day << " 天   光照: " << daylightFactor();
    info.push_back(time.str());
    info.push_back("世界种子: " + std::to_string(worldSeed_) + "   尺寸: " + std::to_string(world_.width()) +
                   " x " + std::to_string(world_.height()));
    info.push_back("服务器 tick: " + std::to_string(serverTick_) + "   延迟(预测队列): " +
                   std::to_string(pendingInputs_.size() * 50) + "ms");
    info.push_back("实体: " + std::to_string(entities_.size()) + "   远程玩家: " +
                   std::to_string(remotePlayers_.size()) + "   粒子: " + std::to_string(particles_.size()));
    info.push_back("生命: " + std::to_string(health_) + "/20   饥饿: " + std::to_string(food_) +
                   "/20   氧气: " + std::to_string(air_) + "/300");
    info.push_back("模式: " + std::string(creative_ ? "创造" : "生存") + "   飞行: " +
                   (flying_ ? "是" : "否"));
    info.push_back("玩家: " + myName_ + " (#" + std::to_string(myId_) + ")   地址: " + lastAddress_);
    info.push_back("按 F3 关闭调试信息  ·  F1 隐藏 HUD  ·  F2 截图");
    (void)width;
    float y = 8;
    for (const auto& text : info) {
        drawText(text, 10, y, size, WHITE);
        y += lineHeight;
    }
    if (hoverValid_) {
        drawText("目标方块: " + blockName(world_.fg(hoverX_, hoverY_)) + " (" + std::to_string(hoverX_) + ", " +
                     std::to_string(hoverY_) + ")",
                 10, y + 4, size, COLOR_GOLD);
    }
}

void Game::renderPlayerList() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float size = 7.0f * uiScale_;
    float panelWidth = std::min(460.0f, width * 0.5f);
    float rowHeight = size + 18;
    int count = static_cast<int>(remotePlayers_.size()) + 1;
    float panelHeight = 70 + rowHeight * count;
    Rectangle panel{(width - panelWidth) * 0.5f, 60, panelWidth, panelHeight};
    drawPanel(panel, 0.9f);
    drawTextCentered("在线玩家 (" + std::to_string(count) + ")", panel.x + panel.width * 0.5f, panel.y + 12, size * 1.2f,
                     WHITE);
    float y = panel.y + 48;
    drawText(myName_ + " (你)", panel.x + 26, y, size, Color{255, 210, 120, 255});
    y += rowHeight;
    for (const auto& [id, remote] : remotePlayers_) {
        (void)id;
        drawText(remote.name, panel.x + 26, y, size, WHITE);
        y += rowHeight;
    }
}

void Game::renderPause() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    DrawRectangle(0, 0, width, height, Color{0, 0, 0, 140});
    pausePanel_ = Rectangle{width * 0.5f - 240, height * 0.5f - 180, 480, 360};
    drawPanel(pausePanel_);
    drawTextCentered("游戏已暂停", width * 0.5f, pausePanel_.y + 24, 14.0f * uiScale_, WHITE);
    pauseButtons_[0] = Rectangle{pausePanel_.x + 50, pausePanel_.y + 100, pausePanel_.width - 100, 54};
    pauseButtons_[1] = Rectangle{pausePanel_.x + 50, pausePanel_.y + 170, pausePanel_.width - 100, 54};
    pauseButtons_[2] = Rectangle{pausePanel_.x + 50, pausePanel_.y + 240, pausePanel_.width - 100, 54};
    drawButton(pauseButtons_[0], "返回游戏");
    drawButton(pauseButtons_[1], "保存并退出");
    drawButton(pauseButtons_[2], "控制说明");
    drawTextCentered("Esc 返回游戏 · 按住 Tab 查看玩家", width * 0.5f, pausePanel_.y + pausePanel_.height - 34,
                     6.0f * uiScale_, Color{190, 190, 190, 255});
}

void Game::handlePauseClick() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mouse = GetMousePosition();
    if (CheckCollisionPointRec(mouse, pauseButtons_[0])) {
        overlay_ = Overlay::None;
        HideCursor();
        cursorVisible_ = false;
    } else if (CheckCollisionPointRec(mouse, pauseButtons_[1])) {
        if (ownsServer_) {
            net_.send(json{{"type", "chat"}, {"text", "/stop"}});
        }
        disconnectTo(Screen::Title);
    } else if (CheckCollisionPointRec(mouse, pauseButtons_[2])) {
        overlay_ = Overlay::None;
        settingsReturn_ = Screen::Playing;
        // Reuse pause panel for a compact controls list.
        overlay_ = Overlay::Pause;
        addChat("系统", "控制：A/D 移动 · 空格跳跃 · Shift 潜行 · Ctrl 疾跑");
        addChat("系统", "左键挖掘/攻击 · 右键放置/使用 · E 背包 · Q 丢弃");
    }
}

void Game::renderInventory() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    buildInventoryLayout();
    DrawRectangle(0, 0, width, height, Color{0, 0, 0, 155});
    drawPanel(invPanel_);
    float titleSize = 12.0f * uiScale_;
    drawText("物品栏", invPanel_.x + 32, invPanel_.y + 20, titleSize, WHITE);
    drawText("E / Esc 关闭", invPanel_.x + invPanel_.width - 176, invPanel_.y + 28, 6.0f * uiScale_,
             Color{190, 190, 190, 255});

    DrawRectangleRec(invPlayerArea_, Color{0, 0, 0, 70});
    DrawRectangleLinesEx(invPlayerArea_, 1.5f, Color{100, 100, 120, 160});
    drawTextCentered("Steve", invPlayerArea_.x + invPlayerArea_.width * 0.5f, invPlayerArea_.y + 8, 6.0f * uiScale_,
                     Color{220, 220, 220, 255});
    float previewScale = 1.0f;
    float previewX = invPlayerArea_.x + invPlayerArea_.width * 0.5f;
    float previewY = invPlayerArea_.y + invPlayerArea_.height - 30;
    // Draw a full-size player preview by temporarily using a fake screen-space
    // figure centered in the panel.
    Texture2D& skin = assets_.tex("entity/player/wide/steve.png");
    float unit = 4.2f * uiScale_ / 2.0f * previewScale;
    auto part = [&](Rectangle src, float lx, float ly, float w, float h) {
        DrawTexturePro(skin, src, Rectangle{previewX + lx, previewY + ly, w, h}, Vector2{0, 0}, 0.0f, WHITE);
    };
    part(Rectangle{20, 52, 4, 12}, -4 * unit, -12 * unit, 4 * unit, 12 * unit);
    part(Rectangle{4, 20, 4, 12}, 0, -12 * unit, 4 * unit, 12 * unit);
    part(Rectangle{20, 20, 8, 12}, -4 * unit, -24 * unit, 8 * unit, 12 * unit);
    part(Rectangle{44, 20, 4, 12}, 4 * unit, -24 * unit, 4 * unit, 12 * unit);
    part(Rectangle{8, 8, 8, 8}, -4 * unit, -32 * unit, 8 * unit, 8 * unit);

    Vector2 mouse = GetMousePosition();
    std::string tooltip;
    for (int i = 9; i < INV_SIZE; ++i) {
        Rectangle rect = invSlotRects_[i];
        bool hovered = CheckCollisionPointRec(mouse, rect);
        drawInventorySlot(rect, inventory_[i], hovered, false);
        if (hovered && !inventory_[i].empty()) tooltip = itemName(inventory_[i].id);
    }
    for (int i = 0; i < HOTBAR_SIZE; ++i) {
        Rectangle rect = invSlotRects_[i];
        bool hovered = CheckCollisionPointRec(mouse, rect);
        drawInventorySlot(rect, inventory_[i], hovered, i == selected_);
        if (hovered && !inventory_[i].empty()) tooltip = itemName(inventory_[i].id);
    }

    // Crafting panel.
    Rectangle craftPanel{invPanel_.x + invPanel_.width * 0.63f, invPanel_.y + 70,
                         invPanel_.width * 0.34f, invPanel_.height - 100};
    DrawRectangleRounded(craftPanel, 0.02f, 4, Color{0, 0, 0, 80});
    DrawRectangleLinesEx(craftPanel, 1.5f, Color{110, 100, 140, 160});
    drawText("合成配方", craftPanel.x + 16, craftPanel.y + 10, 7.0f * uiScale_, COLOR_GOLD);
    for (size_t i = 0; i < recipeRects_.size() && i < recipeIndices_.size(); ++i) {
        const Recipe& recipe = allRecipes()[recipeIndices_[i]];
        Rectangle rect = recipeRects_[i];
        bool hovered = CheckCollisionPointRec(mouse, rect);
        DrawRectangleRounded(rect, 0.12f, 4, hovered ? Color{70, 62, 88, 230} : Color{24, 22, 32, 210});
        DrawRectangleLinesEx(rect, 1.0f, hovered ? Color{190, 170, 240, 230} : Color{70, 64, 92, 160});
        Rectangle icon{rect.x + 7, rect.y + 5, rect.height - 10, rect.height - 10};
        drawItemIcon(recipe.output, icon);
        drawText(recipe.name, rect.x + rect.height + 2, rect.y + 7, 5.0f * uiScale_,
                 hovered ? WHITE : Color{220, 220, 220, 255});
        if (hovered) {
            std::string ingredientText;
            for (const auto& [item, count] : recipe.inputs) {
                if (!ingredientText.empty()) ingredientText += " + ";
                ingredientText += std::to_string(count) + " " + itemName(item);
            }
            tooltip = ingredientText;
        }
    }

    if (!cursorStack_.empty()) {
        float size = 44.0f * uiScale_ / 3.0f;
        drawItemStack(cursorStack_, Rectangle{mouse.x - size * 0.5f, mouse.y - size * 0.5f, size, size},
                      uiScale_ / 3.0f);
    }
    if (!tooltip.empty()) drawTooltip(tooltip, mouse);
}

void Game::buildInventoryLayout() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    float panelW = std::min(static_cast<float>(width) - 80.0f, 1120.0f);
    float panelH = std::min(static_cast<float>(height) - 80.0f, 620.0f);
    invPanel_ = Rectangle{(width - panelW) * 0.5f, (height - panelH) * 0.5f, panelW, panelH};
    invPlayerArea_ = Rectangle{invPanel_.x + 30, invPanel_.y + 80, panelW * 0.15f, panelH - 130};
    float slotSize = std::min(54.0f, (panelW * 0.52f) / 9.0f);
    float gap = 1.0f;
    float gridX = invPanel_.x + panelW * 0.22f;
    float gridY = invPanel_.y + 82;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 9; ++col) {
            int slot = 9 + row * 9 + col;
            invSlotRects_[slot] = Rectangle{gridX + col * (slotSize + gap), gridY + row * (slotSize + gap),
                                             slotSize, slotSize};
        }
    }
    float hotbarY = gridY + 3 * (slotSize + gap) + 20;
    for (int col = 0; col < 9; ++col) {
        invSlotRects_[col] = Rectangle{gridX + col * (slotSize + gap), hotbarY, slotSize, slotSize};
    }
    recipeRects_.clear();
    recipeIndices_.clear();
    Rectangle craftPanel{invPanel_.x + invPanel_.width * 0.63f, invPanel_.y + 70,
                         invPanel_.width * 0.34f, invPanel_.height - 100};
    float gapX = 8;
    float buttonW = (craftPanel.width - 30) * 0.5f;
    float buttonH = 34;
    for (size_t i = 0; i < allRecipes().size(); ++i) {
        int column = static_cast<int>(i) / 9;
        int row = static_cast<int>(i) % 9;
        if (column > 1) break;
        float x = craftPanel.x + 12 + column * (buttonW + gapX);
        float y = craftPanel.y + 44 + row * (buttonH + 3);
        if (y + buttonH > craftPanel.y + craftPanel.height - 8) break;
        recipeRects_.push_back(Rectangle{x, y, buttonW, buttonH});
        recipeIndices_.push_back(static_cast<int>(i));
    }
    inventoryLayoutBuilt_ = true;
}

Rectangle Game::inventorySlotRect(int slot) const {
    if (slot < 0 || slot >= INV_SIZE) return Rectangle{0, 0, 0, 0};
    return invSlotRects_[slot];
}

void Game::handleInventoryClick() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) return;
    Vector2 mouse = GetMousePosition();
    int button = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) ? 1 : 0;
    bool shift = IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT);
    for (int i = 0; i < INV_SIZE; ++i) {
        if (CheckCollisionPointRec(mouse, invSlotRects_[i])) {
            net_.send(json{{"type", "slot_click"}, {"slot", i}, {"button", button}, {"shift", shift}});
            return;
        }
    }
    for (size_t i = 0; i < recipeRects_.size(); ++i) {
        if (CheckCollisionPointRec(mouse, recipeRects_[i])) {
            if (button != 0) return;
            const Recipe& recipe = allRecipes()[recipeIndices_[i]];
            net_.send(json{{"type", "craft"}, {"recipe", recipe.id}});
            return;
        }
    }
}

void Game::renderDeath() {
    int width = GetScreenWidth();
    int height = GetScreenHeight();
    DrawRectangle(0, 0, width, height, Color{100, 0, 0, 130});
    deathPanel_ = Rectangle{width * 0.5f - 260, height * 0.5f - 150, 520, 300};
    drawPanel(deathPanel_, 0.9f);
    drawTextCentered("你死了！", width * 0.5f, deathPanel_.y + 26, 18.0f * uiScale_, Color{255, 110, 90, 255});
    drawTextCentered("重生后生命与饥饿值恢复，背包会保留。", width * 0.5f, deathPanel_.y + 110,
                     6.5f * uiScale_, Color{225, 225, 225, 255});
    deathButtons_[0] = Rectangle{deathPanel_.x + 50, deathPanel_.y + 165, deathPanel_.width - 100, 52};
    deathButtons_[1] = Rectangle{deathPanel_.x + 50, deathPanel_.y + 227, deathPanel_.width - 100, 44};
    drawButton(deathButtons_[0], "重生");
    drawButton(deathButtons_[1], "返回标题");
}

void Game::handleDeathClick() {
    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mouse = GetMousePosition();
    if (CheckCollisionPointRec(mouse, deathButtons_[0])) {
        net_.send(json{{"type", "respawn"}});
    } else if (CheckCollisionPointRec(mouse, deathButtons_[1])) {
        disconnectTo(Screen::Title);
    }
}

} // namespace

int main(int argc, char** argv) {
    Game::Args args;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        try {
            if (arg == "--connect" && i + 1 < argc) args.connect = argv[++i];
            else if (arg == "--name" && i + 1 < argc) args.name = argv[++i];
            else if (arg == "--screenshot" && i + 1 < argc) args.screenshot = argv[++i];
            else if (arg == "--screenshot-delay" && i + 1 < argc) args.screenshotDelay = std::stof(argv[++i]);
            else if (arg == "--screenshot-ui" && i + 1 < argc) args.screenshotUi = argv[++i];
            else if (arg == "--quit-after-screenshot") args.quitAfterScreenshot = true;
            else if (arg == "--singleplayer") args.singleplayer = true;
            else if (arg == "--width" && i + 1 < argc) args.width = std::stoi(argv[++i]);
            else if (arg == "--height" && i + 1 < argc) args.height = std::stoi(argv[++i]);
        } catch (...) {
            std::cerr << "参数错误: " << arg << std::endl;
            return 1;
        }
    }
    // Game is intentionally stack allocated; initialization failure exits
    // before entering the loop.
    Game game;
    return game.run(args);
}
