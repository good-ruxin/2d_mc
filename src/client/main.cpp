#include "client/assets.h"
#include "common/blocks.h"
#include "common/net.h"
#include "common/physics.h"
#include "common/world.h"
#include "client/platform_win32.h"

#include <raylib.h>
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
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <string>
#include <thread>
#include <vector>


using namespace mc;
using json = nlohmann::json;

namespace {

constexpr int INV_SIZE = 36;
constexpr int HOTBAR_SIZE = 9;
constexpr int PROTOCOL_VERSION = 1;
constexpr float FIXED_DT = 0.05f;
constexpr float PI_F = 3.14159265358979323846f;

Color scaleColor(Color c, float b) {
    b = std::clamp(b, 0.0f, 1.0f);
    return Color{
        static_cast<unsigned char>(c.r * b),
        static_cast<unsigned char>(c.g * b),
        static_cast<unsigned char>(c.b * b),
        c.a
    };
}

std::filesystem::path exeDirectory() {
    return mc::getExecutableDirectory();
}

bool looksLikeRoot(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p / "assets" / "gui" / "title" / "minecraft.png", ec);
}

std::filesystem::path findRoot() {
    std::vector<std::filesystem::path> starts;
    starts.push_back(std::filesystem::current_path());
    starts.push_back(exeDirectory());

    for (const auto& start : starts) {
        std::filesystem::path p = start;
        for (int i = 0; i < 6; ++i) {
            if (looksLikeRoot(p)) return p;
            if (!p.has_parent_path() || p.parent_path() == p) break;
            p = p.parent_path();
        }
    }
    return std::filesystem::current_path();
}

bool parseAddress(const std::string& input, std::string& host, int& port) {
    std::string s = input;
    while (!s.empty() && s.front() == ' ') s.erase(s.begin());
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.empty()) return false;

    const size_t colon = s.rfind(':');
    if (colon != std::string::npos && colon != 0 && colon + 1 < s.size()) {
        host = s.substr(0, colon);
        try { port = std::stoi(s.substr(colon + 1)); }
        catch (...) { return false; }
    } else {
        host = s;
        port = 25565;
    }
    return !host.empty() && port > 0 && port < 65536;
}

std::string utf8Codepoint(int cp) {
    std::string s;
    if (cp <= 0x7F) {
        s.push_back(static_cast<char>(cp));
    } else if (cp <= 0x7FF) {
        s.push_back(static_cast<char>(0xC0 | (cp >> 6)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        s.push_back(static_cast<char>(0xE0 | (cp >> 12)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    } else {
        s.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        s.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }
    return s;
}

void popUtf8(std::string& s) {
    if (s.empty()) return;
    size_t i = s.size() - 1;
    while (i > 0 && (static_cast<unsigned char>(s[i]) & 0xC0) == 0x80) --i;
    s.erase(i);
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
    std::string reason;

    ~NetClient() { close(); }

    bool connectTo(const std::string& host, int port, std::string& error) {
        close();
        if (!socket.connectTo(host, port, 3000, error)) return false;
        connected = true;
        running = true;
        reader = std::thread([this] {
            while (running) {
                json msg;
                std::string err;
                if (!recvJson(socket, msg, err)) {
                    if (running) {
                        connected = false;
                        {
                            std::lock_guard<std::mutex> l(reasonMutex);
                            reason = err;
                        }
                        json d = {{"type", "__disconnect"}, {"reason", err}};
                        std::lock_guard<std::mutex> l(inboxMutex);
                        inbox.push_back(std::move(d));
                    }
                    break;
                }
                if (msg.is_object()) {
                    std::lock_guard<std::mutex> l(inboxMutex);
                    inbox.push_back(std::move(msg));
                    if (inbox.size() > 512) inbox.pop_front();
                }
            }
        });
        return true;
    }

    bool send(const json& msg) {
        if (!connected) return false;
        std::string err;
        if (!sendJson(socket, sendMutex, msg, err)) {
            connected = false;
            return false;
        }
        return true;
    }

    bool poll(json& out) {
        std::lock_guard<std::mutex> l(inboxMutex);
        if (inbox.empty()) return false;
        out = std::move(inbox.front());
        inbox.pop_front();
        return true;
    }

    std::string getReason() {
        std::lock_guard<std::mutex> l(reasonMutex);
        return reason;
    }

    void close() {
        running = false;
        connected = false;
        socket.shutdownBoth();
        if (reader.joinable()) reader.join();
        socket.close();
        std::lock_guard<std::mutex> l(inboxMutex);
        inbox.clear();
    }
};

struct RemotePlayer {
    int id = 0;
    std::string name;
    float tx = 0, ty = 0;
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    int facing = 1;
    bool sneak = false;
    bool sprint = false;
    bool onGround = true;
    bool dead = false;
    bool creative = false;
    int health = 20;
    float walkPhase = 0;
};

struct EntityView {
    int id = 0;
    std::string type;
    float tx = 0, ty = 0;
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    int facing = 1;
    int health = 10;
    int maxHealth = 10;
    int hurt = 0;
    std::string item;
    int count = 0;
    float walkPhase = 0;
    bool seen = false;
};

struct Particle {
    float x = 0, y = 0;
    float vx = 0, vy = 0;
    float life = 1;
    float maxLife = 1;
    float size = 0.15f;
    std::string tex;
    Rectangle src{0, 0, 4, 4};
    Color tint = WHITE;
    bool gravity = true;
};

struct Toast {
    std::string text;
    std::string sub;
    float life = 4;
    float maxLife = 4;
};

struct ChatLine {
    std::string from;
    std::string text;
    float age = 0;
};

struct PendingInput {
    MoveInput input;
};

struct ContainerSlotView {
    int id = -1;          // Shared server slot number used by slot_click/swap_hotbar messages.
    Rectangle bounds{};   // Screen-space hitbox derived from the corresponding GUI texture.
};

enum class Screen {
    Title,
    Settings,
    Connecting,
    Playing,
    Disconnected
};

enum class Overlay {
    None,
    Pause,
    Inventory,
    Death
};

struct Options {
    std::string connect;
    std::string name = "Steve";
    int width = 1280;
    int height = 720;
};

class Game {
public:
    bool init(const Options& opt);
    void run();
    void shutdown();

private:
    Options opt_;
    Assets assets_;
    NetClient net_;
    std::filesystem::path root_;
    Screen screen_ = Screen::Title;
    Overlay overlay_ = Overlay::None;
    bool quit_ = false;

    // Authoritative world/player state and the local rendering prediction.
    bool hasWorld_ = false;
    World world_;
    int myId_ = -1;
    std::string myName_ = "Steve";
    uint64_t worldTime_ = 1000;
    PlayerBody body_;
    std::vector<ItemStack> inventory_ = std::vector<ItemStack>(INV_SIZE);
    ItemStack cursorStack_;
    // Server snapshots for the active container; these never own gameplay state.
    std::vector<ItemStack> craftingGrid_ = std::vector<ItemStack>(9);
    std::vector<ItemStack> chestSlots_ = std::vector<ItemStack>(54);
    std::array<ItemStack, 3> furnaceSlots_{};
    ItemStack craftResult_;
    std::string containerGui_;
    int containerViewers_ = 0;
    int furnaceBurnTime_ = 0;
    int furnaceBurnTotal_ = 0;
    int furnaceCookTime_ = 0;
    int furnaceCookTotal_ = 200;
    float containerScale_ = 3.0f;
    int selected_ = 0;
    int health_ = 20;
    int food_ = 20;
    int air_ = 300;
    bool dead_ = false;
    bool creative_ = false;
    bool flying_ = false;

    // Input sequence numbers and unacknowledged inputs used by client prediction.
    uint32_t seq_ = 0;
    uint32_t ack_ = 0;
    std::deque<PendingInput> pending_;
    float tickAcc_ = 0;

    float renderX_ = 0;
    float renderY_ = 0;
    float cameraX_ = 0;
    float cameraY_ = 0;
    float zoom_ = 1.0f;
    int zoomIndex_ = 0;
    float walkPhase_ = 0;
    float swing_ = 0;
    float attackCooldown_ = 0;
    float selectedNameTimer_ = 0;
    float redFlash_ = 0;
    float playTime_ = 0;

    int hoverX_ = -1;
    int hoverY_ = -1;
    bool hoverValid_ = false;
    int hoveredEntity_ = -1;

    int mineX_ = -1;
    int mineY_ = -1;
    bool mineActive_ = false;
    float mineProgress_ = 0;
    float mineCooldown_ = 0;
    float mineParticleTimer_ = 0;

    int facing_ = 1;
    MoveInput desiredInput_{};
    float uiScale_ = 3;

    // Interpolated remote players/entities and transient visual effects.
    std::map<int, RemotePlayer> remotes_;
    std::map<int, EntityView> entities_;
    std::vector<Particle> particles_;
    std::deque<Toast> toasts_;
    std::deque<ChatLine> chats_;

    bool chatOpen_ = false;
    std::string chatInput_;
    float chatBlink_ = 0;

    bool showHud_ = true;
    bool showDebug_ = false;
    bool showPlayerList_ = false;

    // Connection/retry state and an optional locally launched server process.
    std::string address_ = "127.0.0.1:25565";
    std::string lastAddress_;
    bool lastWasLocal_ = true;
    std::string disconnectReason_;
    float connectingTimer_ = 0;

    bool ownsServer_ = false;
    void* ownedServerProcess_ = nullptr;

    float spawnX_ = 0;
    float spawnY_ = 0;

    // Recomputed screen-space controls and inventory slot hitboxes.
    std::array<Rectangle, 3> titleButtons_{};
    Rectangle settingsBack_{};
    Rectangle discRetry_{};
    Rectangle discBack_{};
    Rectangle pauseResume_{};
    Rectangle pauseQuit_{};
    Rectangle deathRespawn_{};
    Rectangle deathTitle_{};
    Rectangle invPanel_{};
    std::vector<ContainerSlotView> containerSlots_;

    std::mt19937 rng_{std::random_device{}()};

    void update(float dt);
    void draw();

    void pollNet();
    void processMessage(const json& m);

    void connectTo(const std::string& addr, bool launchLocal);
    bool launchLocalServer(int port, std::string& err);
    void stopOwnedServer();
    void disconnectToTitle(const std::string& reason = "");

    void updateTitle();
    void updateSettings();
    void updateConnecting(float dt);
    void updateDisconnected();
    void updatePlaying(float dt);

    void updateDesiredInput();
    void fixedUpdate();
    void updateCamera(float dt);
    void updateSmoothing(float dt);
    void updateHover();

    void handleGameplayInput(float dt);
    void handleInventoryInput();
    void handlePauseInput();
    void handleDeathInput();
    void handleChatInput();

    void startMining(float dt);
    int findEntityAtMouse(Vector2 mouse) const;
    std::string currentHeldItem() const;

    void handleSelf(const json& self);
    void handleSnapshot(const json& m);
    void handleInventory(const json& m);
    void handleContainer(const json& m);
    void handleStats(const json& m);
    void handleBlock(const json& m);

    void onWorldLoaded();

    void addChat(const std::string& from, const std::string& text);
    void addToast(const std::string& text, const std::string& sub = "");
    void spawnBlockParticles(int x, int y, BlockId block);
    void spawnParticle(float x, float y, const std::string& tex, Rectangle src, Color tint,
                       float vx, float vy, float life, float size, bool gravity = true);
    void updateParticles(float dt);
    void takeScreenshot();

    float lightAt(int x, int y) const;
    float daylightFactor() const;
    Vector2 worldToScreen(float wx, float wy) const;
    Vector2 screenToWorld(float sx, float sy) const;
    float blockPixels() const { return 48.0f * zoom_; }

    void drawTitle();
    void drawSettings();
    void drawConnecting();
    void drawDisconnected();
    void drawPlaying();
    void drawSky();
    void drawTiles(bool background);
    void drawTile(int x, int y, bool background);
    void drawTarget();
    void drawRenderables();
    void drawHumanoid(const std::string& skin, float wx, float wy, int facing, bool moving,
                      float walkPhase, float swing, Color tint, const std::string& held,
                      float heightBlocks, bool drawHeld);
    void drawPig(const EntityView& e);
    void drawZombie(const EntityView& e);
    void drawItemEntity(const EntityView& e);
    void drawParticles();
    void drawHud();
    void drawInventory();
    void drawPause();
    void drawDeath();
    void drawChat();
    void drawToasts();
    void drawDebug();
    void drawPlayerList();

    void drawMenuDirt();
    void drawPanel(Rectangle r, float alpha = 0.82f);
    bool drawButton(Rectangle r, const std::string& text);
    void drawText(const std::string& text, float x, float y, float size, Color color, bool shadow = true);
    void drawTextCentered(const std::string& text, float cx, float y, float size, Color color, bool shadow = true);
    float measureText(const std::string& text, float size);
    void drawItemIcon(const std::string& id, Rectangle r);
    void drawItemStack(const ItemStack& stack, Rectangle r, float scale);
    void buildInventoryLayout();
    void drawTooltip(const std::string& text, Vector2 mouse);
};

// -----------------------------------------------------------------------------
// init / run / shutdown
// -----------------------------------------------------------------------------

bool Game::init(const Options& opt) {
    opt_ = opt;
    root_ = findRoot();
    std::error_code ec;
    std::filesystem::current_path(root_, ec);

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_VSYNC_HINT);
    InitWindow(opt_.width, opt_.height, "Minecraft 2D");
    SetWindowMinSize(1280, 720);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetRandomSeed(static_cast<unsigned int>(std::time(nullptr)));

    std::string err;
    if (!assets_.init(root_, err)) {
        TraceLog(LOG_ERROR, "ASSETS: %s", err.c_str());
        CloseWindow();
        return false;
    }
    if (!err.empty()) TraceLog(LOG_WARNING, "ASSETS: %s", err.c_str());

    std::string netErr;
    netInit(netErr);
    if (!netErr.empty()) TraceLog(LOG_WARNING, "NET: %s", netErr.c_str());

    myName_ = opt_.name.empty() ? "Steve" : opt_.name;
    screen_ = Screen::Title;
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);

    if (!opt_.connect.empty()) {
        connectTo(opt_.connect, false);
    }
    return true;
}

void Game::run() {
    while (!WindowShouldClose() && !quit_) {
        float dt = GetFrameTime();
        if (dt > 0.1f) dt = 0.1f;
        update(dt);
        BeginDrawing();
        ClearBackground(BLACK);
        draw();
        EndDrawing();
    }
}

void Game::shutdown() {
    if (ownsServer_ && net_.connected) {
        net_.send(json{{"type", "chat"}, {"text", "/stop"}});
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    net_.close();
    stopOwnedServer();
    assets_.unload();
    if (IsWindowReady()) CloseWindow();
    netShutdown();
}

// -----------------------------------------------------------------------------
// Network
// -----------------------------------------------------------------------------

void Game::connectTo(const std::string& addr, bool launchLocal) {
    lastAddress_ = addr;
    lastWasLocal_ = launchLocal;
    address_ = addr;

    std::string host;
    int port = 25565;
    if (!parseAddress(addr, host, port)) {
        screen_ = Screen::Disconnected;
        disconnectReason_ = "服务器地址格式无效";
        return;
    }

    net_.close();
    screen_ = Screen::Connecting;
    connectingTimer_ = 0;
    disconnectReason_.clear();

    std::string err;
    bool ok = net_.connectTo(host, port, err);

    if (!ok && launchLocal) {
        if (!launchLocalServer(port, err)) {
            screen_ = Screen::Disconnected;
            disconnectReason_ = err;
            return;
        }
        for (int i = 0; i < 40 && !ok; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            ok = net_.connectTo(host, port, err);
        }
    }

    if (!ok) {
        screen_ = Screen::Disconnected;
        disconnectReason_ = err.empty() ? "无法连接到服务器" : err;
        return;
    }

    json hello = {
        {"type", "hello"},
        {"name", myName_},
        {"version", PROTOCOL_VERSION}
    };
    net_.send(hello);
    pending_.clear();
    seq_ = 0;
    ack_ = 0;
}

bool Game::launchLocalServer(int port, std::string& err) {
    std::filesystem::path serverExe = exeDirectory() / "mc2d_server.exe";
    if (!std::filesystem::exists(serverExe)) serverExe = root_ / "bin" / "mc2d_server.exe";
    if (!std::filesystem::exists(serverExe)) serverExe = root_ / "mc2d_server.exe";

    if (!std::filesystem::exists(serverExe)) {
        err = "找不到 mc2d_server.exe，请先编译服务端";
        return false;
    }
    if (!mc::launchServerProcess(serverExe, root_, port, ownedServerProcess_, err)) {
        return false;
    }
    ownsServer_ = true;
    return true;
}

void Game::stopOwnedServer() {
    if (!ownsServer_) return;
    if (ownedServerProcess_) {
        mc::stopServerProcess(ownedServerProcess_);
        ownedServerProcess_ = nullptr;
    }
    ownsServer_ = false;
}

void Game::disconnectToTitle(const std::string& reason) {
    if (ownsServer_ && net_.connected) {
        net_.send(json{{"type", "chat"}, {"text", "/stop"}});
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    net_.close();
    stopOwnedServer();

    hasWorld_ = false;
    myId_ = -1;
    dead_ = false;
    containerGui_.clear();
    overlay_ = Overlay::None;
    chatOpen_ = false;
    pending_.clear();
    remotes_.clear();
    entities_.clear();
    particles_.clear();
    screen_ = Screen::Title;
    disconnectReason_ = reason;
    SetMouseCursor(MOUSE_CURSOR_DEFAULT);
}

void Game::pollNet() {
    json msg;
    while (net_.poll(msg)) processMessage(msg);

    if (screen_ == Screen::Playing && !net_.connected) {
        disconnectReason_ = net_.getReason().empty() ? "与服务器断开连接" : net_.getReason();
        net_.close();
        stopOwnedServer();
        screen_ = Screen::Disconnected;
    }

    if (screen_ == Screen::Connecting && !net_.connected) {
        disconnectReason_ = net_.getReason().empty() ? "无法连接到服务器" : net_.getReason();
        net_.close();
        stopOwnedServer();
        screen_ = Screen::Disconnected;
    }
}

void Game::processMessage(const json& m) {
    const std::string type = m.value("type", "");

    if (type == "welcome") {
        myId_ = m.value("id", -1);
        myName_ = m.value("name", myName_);
        worldTime_ = m.value("time", worldTime_);
        if (m.contains("spawn")) {
            spawnX_ = m["spawn"].value("x", 0.0f);
            spawnY_ = m["spawn"].value("y", 0.0f);
            body_.x = spawnX_;
            body_.y = spawnY_;
            renderX_ = body_.x;
            renderY_ = body_.y;
        }
    } else if (type == "world") {
        int w = m.value("width", 512);
        int h = m.value("height", 128);
        std::vector<int> fgRuns = m.value("fg", std::vector<int>{});
        std::vector<int> bgRuns = m.value("bg", std::vector<int>{});

        world_ = World(w, h);
        auto fg = World::decodeRuns(fgRuns, w * h);
        auto bg = World::decodeRuns(bgRuns, w * h);
        std::copy(fg.begin(), fg.end(), world_.fgData().begin());
        std::copy(bg.begin(), bg.end(), world_.bgData().begin());

        if (body_.x == 0 && body_.y == 0) {
            body_.x = spawnX_;
            body_.y = spawnY_;
        }
        onWorldLoaded();
    } else if (type == "self") {
        if (m.contains("self") && m["self"].is_object()) {
            handleSelf(m["self"]);
        }
        if (m.contains("time")) worldTime_ = m.value("time", worldTime_);
    } else if (type == "snapshot") {
        handleSnapshot(m);
        if (m.contains("time")) worldTime_ = m.value("time", worldTime_);
    } else if (type == "inventory") {
        handleInventory(m);
    } else if (type == "container") {
        handleContainer(m);
    } else if (type == "stats") {
        handleStats(m);
    } else if (type == "block") {
        handleBlock(m);
    } else if (type == "chat") {
        addChat(m.value("from", ""), m.value("text", ""));
    } else if (type == "toast") {
        addToast(m.value("text", ""), m.value("sub", ""));
    } else if (type == "event") {
        const std::string kind = m.value("kind", "");
        if (kind == "block_break") {
            spawnBlockParticles(m.value("x", 0), m.value("y", 0), static_cast<BlockId>(m.value("block", 0)));
        } else if (kind == "player_hurt") {
            redFlash_ = 1.0f;
        } else if (kind == "mob_hurt") {
            redFlash_ = std::max(redFlash_, 0.15f);
        }
    } else if (type == "respawned") {
        body_.x = m.value("x", spawnX_);
        body_.y = m.value("y", spawnY_);
        renderX_ = body_.x;
        renderY_ = body_.y;
        ack_ = m.value("ack", ack_);
        pending_.clear();
        dead_ = false;
        overlay_ = Overlay::None;
        addChat("系统", "你已重生");
    } else if (type == "denied") {
        disconnectReason_ = m.value("reason", "服务器拒绝连接");
        net_.close();
        stopOwnedServer();
        screen_ = Screen::Disconnected;
    } else if (type == "__disconnect") {
        disconnectReason_ = m.value("reason", "与服务器断开连接");
        net_.close();
        stopOwnedServer();
        screen_ = Screen::Disconnected;
    }
}

// -----------------------------------------------------------------------------
// Message handlers
// -----------------------------------------------------------------------------

void Game::handleSelf(const json& self) {
    health_ = self.value("health", health_);
    food_ = self.value("food", food_);
    air_ = self.value("air", air_);
    dead_ = self.value("dead", dead_);
    creative_ = self.value("creative", creative_);
    flying_ = self.value("flying", flying_);
    selected_ = std::clamp(self.value("selected", selected_), 0, HOTBAR_SIZE - 1);

    if (dead_) {
        overlay_ = Overlay::Death;
        return;
    }
    if (overlay_ == Overlay::Death) overlay_ = Overlay::None;

    const uint32_t ack = self.value("ack", 0u);
    ack_ = std::max(ack_, ack);
    while (!pending_.empty() && pending_.front().input.seq <= ack_) {
        pending_.pop_front();
    }

    PlayerBody sb{};
    sb.x = self.value("x", body_.x);
    sb.y = self.value("y", body_.y);
    sb.vx = self.value("vx", 0.0f);
    sb.vy = self.value("vy", 0.0f);
    sb.onGround = self.value("onGround", false);
    sb.sneaking = self.value("sneaking", false);
    sb.sprinting = self.value("sprinting", false);
    sb.flying = flying_;
    sb.facing = self.value("facing", 1.0f) < 0.0f ? -1 : 1;

    const float dx = sb.x - body_.x;
    const float dy = sb.y - body_.y;
    if (dx * dx + dy * dy > 64.0f) pending_.clear();

    body_ = sb;
    for (const auto& p : pending_) tickPlayer(world_, body_, p.input, creative_);

    if (std::abs(body_.x - renderX_) > 8.0f || std::abs(body_.y - renderY_) > 8.0f) {
        renderX_ = body_.x;
        renderY_ = body_.y;
    }
}

void Game::handleSnapshot(const json& m) {
    if (m.contains("players") && m["players"].is_array()) {
        std::map<int, bool> seen;
        for (const auto& p : m["players"]) {
            int id = p.value("id", -1);
            if (id <= 0 || id == myId_) continue;
            seen[id] = true;
            RemotePlayer& r = remotes_[id];
            if (r.id == 0) {
                r.id = id;
                r.x = p.value("x", 0.0f);
                r.y = p.value("y", 0.0f);
                r.tx = r.x;
                r.ty = r.y;
            }
            r.name = p.value("name", "Steve");
            r.tx = p.value("x", r.tx);
            r.ty = p.value("y", r.ty);
            r.vx = p.value("vx", 0.0f);
            r.vy = p.value("vy", 0.0f);
            r.facing = p.value("facing", 1.0f) < 0.0f ? -1 : 1;
            r.sneak = p.value("sneak", false);
            r.sprint = p.value("sprint", false);
            r.onGround = p.value("onGround", true);
            r.dead = p.value("dead", false);
            r.creative = p.value("creative", false);
            r.health = p.value("health", 20);
        }
        for (auto it = remotes_.begin(); it != remotes_.end();) {
            if (!seen[it->first]) it = remotes_.erase(it);
            else ++it;
        }
    }

    if (m.contains("entities") && m["entities"].is_array()) {
        for (auto& [id, e] : entities_) { (void)id; e.seen = false; }
        for (const auto& e : m["entities"]) {
            int id = e.value("id", -1);
            if (id <= 0) continue;
            EntityView& v = entities_[id];
            if (v.id == 0) {
                v.id = id;
                v.x = e.value("x", 0.0f);
                v.y = e.value("y", 0.0f);
                v.tx = v.x;
                v.ty = v.y;
            }
            v.type = e.value("type", "item");
            v.tx = e.value("x", v.tx);
            v.ty = e.value("y", v.ty);
            v.vx = e.value("vx", 0.0f);
            v.vy = e.value("vy", 0.0f);
            v.facing = e.value("facing", 1.0f) < 0.0f ? -1 : 1;
            v.health = e.value("health", 1);
            v.maxHealth = e.value("maxHealth", 1);
            v.hurt = e.value("hurt", 0);
            v.item = e.value("item", "");
            v.count = e.value("count", 1);
            v.seen = true;
        }
        for (auto it = entities_.begin(); it != entities_.end();) {
            if (!it->second.seen) it = entities_.erase(it);
            else ++it;
        }
    }
}

void Game::handleInventory(const json& m) {
    if (!m.contains("slots") || !m["slots"].is_array()) return;
    for (auto& s : inventory_) s.clear();
    int i = 0;
    for (const auto& item : m["slots"]) {
        if (i >= INV_SIZE) break;
        if (item.is_object()) {
            inventory_[i].id = item.value("id", "");
            inventory_[i].count = item.value("count", 0);
            if (inventory_[i].count <= 0) inventory_[i].clear();
        }
        ++i;
    }
    cursorStack_.clear();
    if (m.contains("cursor") && m["cursor"].is_object()) {
        cursorStack_.id = m["cursor"].value("id", "");
        cursorStack_.count = m["cursor"].value("count", 0);
        if (cursorStack_.count <= 0) cursorStack_.clear();
    }
    selected_ = std::clamp(m.value("selected", selected_), 0, HOTBAR_SIZE - 1);
}

void Game::handleContainer(const json& m) {
    // Container packets are snapshots: the server remains the source of truth
    // for every slot and the client only derives screen-space hitboxes.
    handleInventory(m);
    containerGui_ = m.value("gui", "");
    if (containerGui_.empty()) {
        overlay_ = Overlay::None;
        craftResult_.clear();
        return;
    }

    auto readStacks = [](const json& value, std::vector<ItemStack>& output) {
        for (auto& stack : output) stack.clear();
        if (!value.is_array()) return;
        for (size_t i = 0; i < value.size() && i < output.size(); ++i) {
            const auto& item = value[i];
            if (!item.is_object()) continue;
            output[i].id = item.value("id", "");
            output[i].count = item.value("count", 0);
            if (output[i].count <= 0) output[i].clear();
        }
    };

    if (m.contains("grid")) readStacks(m["grid"], craftingGrid_);
    if (m.contains("chest")) readStacks(m["chest"], chestSlots_);
    if (m.contains("furnace") && m["furnace"].is_object()) {
        const auto& furnace = m["furnace"];
        const std::array<std::string, 3> names{"input", "fuel", "output"};
        for (size_t i = 0; i < names.size(); ++i) {
            furnaceSlots_[i].clear();
            const auto& item = furnace.value(names[i], json(nullptr));
            if (!item.is_object()) continue;
            furnaceSlots_[i].id = item.value("id", "");
            furnaceSlots_[i].count = item.value("count", 0);
            if (furnaceSlots_[i].count <= 0) furnaceSlots_[i].clear();
        }
        furnaceBurnTime_ = furnace.value("burnTime", 0);
        furnaceBurnTotal_ = furnace.value("burnTotal", 0);
        furnaceCookTime_ = furnace.value("cookTime", 0);
        furnaceCookTotal_ = std::max(1, furnace.value("cookTotal", 200));
    }
    craftResult_.clear();
    if (m.contains("craftResult") && m["craftResult"].is_object()) {
        craftResult_.id = m["craftResult"].value("id", "");
        craftResult_.count = m["craftResult"].value("count", 0);
        if (craftResult_.count <= 0) craftResult_.clear();
    }
    containerViewers_ = m.value("viewers", 0);
    overlay_ = Overlay::Inventory;
    buildInventoryLayout();
}

void Game::handleStats(const json& m) {
    health_ = m.value("health", health_);
    food_ = m.value("food", food_);
    air_ = m.value("air", air_);
    dead_ = m.value("dead", dead_);
    creative_ = m.value("creative", creative_);
    flying_ = m.value("flying", flying_);
    selected_ = std::clamp(m.value("selected", selected_), 0, HOTBAR_SIZE - 1);
    if (dead_) overlay_ = Overlay::Death;
    else if (overlay_ == Overlay::Death) overlay_ = Overlay::None;
}

void Game::handleBlock(const json& m) {
    int x = m.value("x", -1);
    int y = m.value("y", -1);
    int fg = m.value("fg", -1);
    if (x < 0 || y < 0 || fg < 0) return;
    world_.setFg(x, y, static_cast<BlockId>(fg));
    if (mineX_ == x && mineY_ == y) {
        mineProgress_ = 0;
        mineActive_ = false;
    }
}

void Game::onWorldLoaded() {
    hasWorld_ = true;
    screen_ = Screen::Playing;
    overlay_ = Overlay::None;
    dead_ = false;
    renderX_ = body_.x;
    renderY_ = body_.y;
    cameraX_ = body_.x;
    cameraY_ = body_.y - 1.2f;
    tickAcc_ = 0;
    chatOpen_ = false;
    SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
    addToast("世界已加载", "按 E 打开背包 · 按 T 聊天");
}

// -----------------------------------------------------------------------------
// Update
// -----------------------------------------------------------------------------

void Game::update(float dt) {
    uiScale_ = std::clamp(GetScreenHeight() / 240.0f, 2.0f, 4.0f);
    playTime_ += dt;

    if (IsKeyPressed(KEY_F11)) ToggleFullscreen();

    pollNet();

    if (screen_ == Screen::Title) updateTitle();
    else if (screen_ == Screen::Settings) updateSettings();
    else if (screen_ == Screen::Connecting) updateConnecting(dt);
    else if (screen_ == Screen::Disconnected) updateDisconnected();
    else if (screen_ == Screen::Playing) {
        if (IsKeyPressed(KEY_F1)) showHud_ = !showHud_;
        if (IsKeyPressed(KEY_F3)) showDebug_ = !showDebug_;
        if (IsKeyPressed(KEY_F5)) {
            zoomIndex_ = (zoomIndex_ + 1) % 3;
            const float levels[3] = {1.0f, 1.35f, 0.78f};
            zoom_ = levels[zoomIndex_];
        }
        if (IsKeyPressed(KEY_F2)) takeScreenshot();
        updatePlaying(dt);
    }

    updateParticles(dt);
    chatBlink_ += dt;

    if (screen_ == Screen::Playing && overlay_ == Overlay::None && !chatOpen_ && !dead_) {
        SetMouseCursor(MOUSE_CURSOR_CROSSHAIR);
    } else {
        SetMouseCursor(MOUSE_CURSOR_DEFAULT);
    }
}

void Game::updateTitle() {
    float w = static_cast<float>(GetScreenWidth());
    float h = static_cast<float>(GetScreenHeight());
    float bw = std::min(420.0f, w * 0.36f);
    float bh = 52.0f;
    float x = (w - bw) * 0.5f;
    float y = h * 0.50f;
    titleButtons_[0] = {x, y, bw, bh};
    titleButtons_[1] = {x, y + 64, bw, bh};
    titleButtons_[2] = {x, y + 128, bw, bh};

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 mouse = GetMousePosition();

    if (CheckCollisionPointRec(mouse, titleButtons_[0])) {
        connectTo("127.0.0.1:25565", true);
    } else if (CheckCollisionPointRec(mouse, titleButtons_[1])) {
        screen_ = Screen::Settings;
    } else if (CheckCollisionPointRec(mouse, titleButtons_[2])) {
        quit_ = true;
    }
}

void Game::updateSettings() {
    float w = static_cast<float>(GetScreenWidth());
    float h = static_cast<float>(GetScreenHeight());
    settingsBack_ = {w * 0.5f - 160, h - 100, 320, 48};

    if (IsKeyPressed(KEY_ESCAPE)) {
        screen_ = Screen::Title;
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) &&
        CheckCollisionPointRec(GetMousePosition(), settingsBack_)) {
        screen_ = Screen::Title;
    }
}

void Game::updateConnecting(float dt) {
    connectingTimer_ += dt;
    if (IsKeyPressed(KEY_ESCAPE)) {
        net_.close();
        stopOwnedServer();
        screen_ = Screen::Title;
        disconnectReason_ = "已取消连接";
        return;
    }
    if (connectingTimer_ > 15.0f && !hasWorld_) {
        net_.close();
        stopOwnedServer();
        screen_ = Screen::Disconnected;
        disconnectReason_ = "连接超时，没有收到世界数据";
    }
}

void Game::updateDisconnected() {
    float w = static_cast<float>(GetScreenWidth());
    float h = static_cast<float>(GetScreenHeight());
    discRetry_ = {w * 0.5f - 220, h * 0.68f, 200, 48};
    discBack_ = {w * 0.5f + 20, h * 0.68f, 200, 48};

    if (IsKeyPressed(KEY_ESCAPE)) {
        screen_ = Screen::Title;
        return;
    }
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        Vector2 m = GetMousePosition();
        if (CheckCollisionPointRec(m, discRetry_)) {
            connectTo(lastAddress_.empty() ? address_ : lastAddress_, lastWasLocal_);
        } else if (CheckCollisionPointRec(m, discBack_)) {
            screen_ = Screen::Title;
        }
    }
}

void Game::updatePlaying(float dt) {
    if (!hasWorld_) return;

    worldTime_ += static_cast<uint64_t>(std::llround(dt * 20.0));
    updateCamera(dt);

    if (IsKeyPressed(KEY_ESCAPE)) {
        if (chatOpen_) {
            chatOpen_ = false;
        } else if (overlay_ == Overlay::Inventory) {
            net_.send(json{{"type", "close_inventory"}});
            containerGui_.clear();
            overlay_ = Overlay::None;
        } else if (overlay_ == Overlay::Pause) {
            overlay_ = Overlay::None;
        } else if (overlay_ == Overlay::None && !dead_) {
            overlay_ = Overlay::Pause;
        }
    }

    if (IsKeyPressed(KEY_E) && !dead_) {
        if (overlay_ == Overlay::Inventory) {
            net_.send(json{{"type", "close_inventory"}});
            containerGui_.clear();
            overlay_ = Overlay::None;
        } else if (overlay_ == Overlay::None) {
            containerGui_ = "inventory";
            overlay_ = Overlay::Inventory;
            net_.send(json{{"type", "open_container"}, {"gui", "inventory"}, {"x", 0}, {"y", 0}});
            buildInventoryLayout();
        }
    }

    if (!chatOpen_ && overlay_ == Overlay::None && !dead_) {
        if (IsKeyPressed(KEY_T) || IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_SLASH)) {
            chatOpen_ = true;
            chatInput_ = IsKeyPressed(KEY_SLASH) ? "/" : "";
        }
    }

    if (chatOpen_) {
        handleChatInput();
    } else if (overlay_ == Overlay::Inventory) {
        buildInventoryLayout();
        handleInventoryInput();
    } else if (overlay_ == Overlay::Pause) {
        handlePauseInput();
    } else if (overlay_ == Overlay::Death) {
        handleDeathInput();
    } else if (!dead_) {
        updateHover();
        handleGameplayInput(dt);
    } else {
        overlay_ = Overlay::Death;
    }

    updateDesiredInput();
    tickAcc_ += dt;
    if (tickAcc_ > 0.25f) tickAcc_ = 0.25f;
    while (tickAcc_ >= FIXED_DT) {
        fixedUpdate();
        tickAcc_ -= FIXED_DT;
    }

    updateSmoothing(dt);
    attackCooldown_ = std::max(0.0f, attackCooldown_ - dt);
    mineCooldown_ = std::max(0.0f, mineCooldown_ - dt);
    selectedNameTimer_ = std::max(0.0f, selectedNameTimer_ - dt);
    swing_ = std::max(0.0f, swing_ - dt * 5.0f);
    redFlash_ = std::max(0.0f, redFlash_ - dt * 2.5f);
}

void Game::updateHover() {
    hoverValid_ = false;
    hoveredEntity_ = -1;
    hoverX_ = -1;
    hoverY_ = -1;

    if (!hasWorld_ || overlay_ != Overlay::None || dead_ || chatOpen_) return;

    Vector2 m = GetMousePosition();
    Vector2 wpos = screenToWorld(m.x, m.y);
    hoverX_ = static_cast<int>(std::floor(wpos.x));
    hoverY_ = static_cast<int>(std::floor(wpos.y));

    if (world_.inBounds(hoverX_, hoverY_)) {
        const float dx = (hoverX_ + 0.5f) - body_.x;
        const float dy = (hoverY_ + 0.5f) - body_.eyeY();
        hoverValid_ = dx * dx + dy * dy <= 5.6f * 5.6f;
    }

    hoveredEntity_ = findEntityAtMouse(m);
}

void Game::updateDesiredInput() {
    desiredInput_ = MoveInput{};
    if (!hasWorld_ || screen_ != Screen::Playing || dead_) return;

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
    desiredInput_.sprint = IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL);

    if (move != 0) {
        facing_ = move;
    } else {
        Vector2 m = GetMousePosition();
        Vector2 wpos = screenToWorld(m.x, m.y);
        if (std::abs(wpos.x - body_.x) > 0.5f) facing_ = wpos.x > body_.x ? 1 : -1;
    }
    desiredInput_.facing = static_cast<float>(facing_);
}

void Game::fixedUpdate() {
    if (!hasWorld_ || screen_ != Screen::Playing || dead_) return;

    MoveInput input = desiredInput_;
    input.seq = ++seq_;
    pending_.push_back({input});
    if (pending_.size() > 120) pending_.pop_front();

    json m = {
        {"type", "input"},
        {"seq", input.seq},
        {"move", input.move},
        {"jump", input.jump},
        {"sneak", input.sneak},
        {"sprint", input.sprint},
        {"facing", input.facing}
    };
    net_.send(m);

    tickPlayer(world_, body_, input, creative_);
    if (body_.onGround && std::abs(body_.vx) > 0.1f) {
        walkPhase_ += std::abs(body_.vx) * FIXED_DT * 2.5f;
    }
}

void Game::updateCamera(float dt) {
    if (!hasWorld_) return;
    const float p = blockPixels();
    const float halfW = GetScreenWidth() * 0.5f / p;
    const float halfH = GetScreenHeight() * 0.5f / p;

    float tx = renderX_ + facing_ * 0.6f;
    float ty = renderY_ - 1.15f;

    if (world_.width() > halfW * 2) tx = std::clamp(tx, halfW, world_.width() - halfW);
    else tx = world_.width() * 0.5f;

    if (world_.height() > halfH * 2) ty = std::clamp(ty, halfH, world_.height() - halfH);
    else ty = world_.height() * 0.5f;

    const float k = std::min(1.0f, dt * 9.0f);
    cameraX_ += (tx - cameraX_) * k;
    cameraY_ += (ty - cameraY_) * k;
}

void Game::updateSmoothing(float dt) {
    if (!hasWorld_) return;

    const float k1 = std::min(1.0f, dt * 18.0f);
    renderX_ += (body_.x - renderX_) * k1;
    renderY_ += (body_.y - renderY_) * k1;

    for (auto& [id, r] : remotes_) {
        (void)id;
        const float k = std::min(1.0f, dt * 14.0f);
        r.x += (r.tx - r.x) * k;
        r.y += (r.ty - r.y) * k;
        r.walkPhase += std::abs(r.vx) * dt * 2.5f;
    }

    for (auto& [id, e] : entities_) {
        (void)id;
        const float k = std::min(1.0f, dt * 14.0f);
        e.x += (e.tx - e.x) * k;
        e.y += (e.ty - e.y) * k;
        e.walkPhase += std::abs(e.vx) * dt * 2.5f;
        if (e.hurt > 0) e.hurt = std::max(0, e.hurt - 1);
    }
}

void Game::updateParticles(float dt) {
    for (auto& p : particles_) {
        p.life -= dt;
        p.x += p.vx * dt;
        p.y += p.vy * dt;
        if (p.gravity) p.vy += 14.0f * dt;
        p.vx *= 0.985f;
    }
    particles_.erase(
        std::remove_if(particles_.begin(), particles_.end(),
                       [](const Particle& p) { return p.life <= 0; }),
        particles_.end());

    for (auto& t : toasts_) t.life -= dt;
    while (!toasts_.empty() && toasts_.front().life <= 0) toasts_.pop_front();

    for (auto& c : chats_) c.age += dt;
    while (chats_.size() > 100) chats_.pop_front();
}

// -----------------------------------------------------------------------------
// Actions
// -----------------------------------------------------------------------------

void Game::handleGameplayInput(float dt) {
    if (dead_ || overlay_ != Overlay::None || chatOpen_) return;

    if (IsKeyPressed(KEY_Q)) {
        net_.send(json{{"type", "drop"}, {"slot", selected_}});
    }

    for (int i = 0; i < HOTBAR_SIZE; ++i) {
        if (IsKeyPressed(KEY_ONE + i)) {
            selected_ = i;
            selectedNameTimer_ = 2;
            net_.send(json{{"type", "select"}, {"slot", i}});
        }
    }

    const float wheel = GetMouseWheelMove();
    if (wheel != 0) {
        int dir = wheel > 0 ? -1 : 1;
        selected_ = (selected_ + dir + HOTBAR_SIZE) % HOTBAR_SIZE;
        selectedNameTimer_ = 2;
        net_.send(json{{"type", "select"}, {"slot", selected_}});
    }

    const bool leftDown = IsMouseButtonDown(MOUSE_BUTTON_LEFT);
    const bool leftPressed = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);

    if (leftPressed && hoveredEntity_ >= 0 && attackCooldown_ <= 0) {
        auto it = entities_.find(hoveredEntity_);
        if (it != entities_.end()) {
            const float dx = it->second.x - body_.x;
            const float dy = (it->second.y - 0.5f) - body_.centerY();
            if (dx * dx + dy * dy <= 4.5f * 4.5f) {
                net_.send(json{{"type", "attack"}, {"id", hoveredEntity_}});
                attackCooldown_ = 0.5f;
                swing_ = 1;
                facing_ = dx >= 0 ? 1 : -1;
            }
        }
    }

    if (leftDown && hoveredEntity_ < 0 && mineCooldown_ <= 0) {
        startMining(dt);
    } else {
        mineActive_ = false;
        if (!leftDown) mineProgress_ = 0;
    }

    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) {
        swing_ = 1;
        const std::string held = currentHeldItem();
        const ItemDef* item = itemDef(held);
        if (item && item->food > 0 && food_ < 20) {
            net_.send(json{{"type", "eat"}, {"slot", selected_}});
            return;
        }

        if (hoverValid_) {
            if (world_.fg(hoverX_, hoverY_) == CRAFTING_TABLE && !IsKeyDown(KEY_LEFT_SHIFT)) {
                containerGui_ = "crafting_table";
                overlay_ = Overlay::Inventory;
                net_.send(json{{"type", "open_container"}, {"gui", "crafting_table"},
                               {"x", hoverX_}, {"y", hoverY_}});
                buildInventoryLayout();
                return;
            }
            if (world_.fg(hoverX_, hoverY_) == FURNACE) {
                containerGui_ = "furnace";
                overlay_ = Overlay::Inventory;
                net_.send(json{{"type", "open_container"}, {"gui", "furnace"},
                               {"x", hoverX_}, {"y", hoverY_}});
                buildInventoryLayout();
                return;
            }
            if (world_.fg(hoverX_, hoverY_) == CHEST) {
                containerGui_ = "chest";
                overlay_ = Overlay::Inventory;
                net_.send(json{{"type", "open_container"}, {"gui", "chest"},
                               {"x", hoverX_}, {"y", hoverY_}});
                buildInventoryLayout();
                return;
            }
            BlockId b = AIR;
            if (isBlockItem(held, &b)) {
                net_.send(json{{"type", "place"}, {"x", hoverX_}, {"y", hoverY_}});
            }
        }
    }
}

void Game::startMining(float dt) {
    if (!hoverValid_) {
        mineActive_ = false;
        mineProgress_ = 0;
        return;
    }

    BlockId b = world_.fg(hoverX_, hoverY_);
    if (b == AIR || b == WATER || b == LAVA || b == BEDROCK) {
        mineActive_ = false;
        mineProgress_ = 0;
        return;
    }

    if (hoverX_ != mineX_ || hoverY_ != mineY_) {
        mineX_ = hoverX_;
        mineY_ = hoverY_;
        mineProgress_ = 0;
        mineActive_ = true;
    }

    const float seconds = breakSeconds(b, currentHeldItem());
    if (!std::isfinite(seconds) || seconds < 0) return;
    if (seconds <= 0.001f) mineProgress_ = 1;
    else mineProgress_ += dt / seconds;

    mineParticleTimer_ += dt;
    if (mineParticleTimer_ > 0.12f) {
        mineParticleTimer_ = 0;
        spawnBlockParticles(hoverX_, hoverY_, b);
    }

    swing_ = std::max(swing_, 0.35f);

    if (mineProgress_ >= 1) {
        net_.send(json{{"type", "break"}, {"x", mineX_}, {"y", mineY_}});
        mineProgress_ = 0;
        mineCooldown_ = 0.2f;
    }
}

int Game::findEntityAtMouse(Vector2 mouse) const {
    int best = -1;
    const float p = blockPixels();
    for (const auto& [id, e] : entities_) {
        if (e.type == "item") continue;
        Vector2 s = worldToScreen(e.x, e.y);
        const float w = (e.type == "pig" ? 1.1f : 0.7f) * p;
        const float h = (e.type == "pig" ? 0.9f : 1.95f) * p;
        Rectangle r{s.x - w * 0.5f, s.y - h, w, h};
        if (CheckCollisionPointRec(mouse, r)) {
            const float dx = e.x - body_.x;
            const float dy = (e.y - 0.8f) - body_.centerY();
            if (dx * dx + dy * dy <= 4.5f * 4.5f) best = id;
        }
    }
    return best;
}

std::string Game::currentHeldItem() const {
    if (selected_ < 0 || selected_ >= INV_SIZE) return "";
    return inventory_[selected_].id;
}

void Game::handleInventoryInput() {
    // Keep slot decisions server-authoritative; the client sends intent only.
    Vector2 mouse = GetMousePosition();
    for (int hotbar = 0; hotbar < HOTBAR_SIZE; ++hotbar) {
        if (!IsKeyPressed(KEY_ONE + hotbar)) continue;
        for (const auto& slot : containerSlots_) {
            if (CheckCollisionPointRec(mouse, slot.bounds)) {
                net_.send(json{{"type", "swap_hotbar"}, {"slot", slot.id}, {"hotbar", hotbar}});
                return;
            }
        }
    }

    const bool left = IsMouseButtonPressed(MOUSE_BUTTON_LEFT);
    const bool right = IsMouseButtonPressed(MOUSE_BUTTON_RIGHT);
    if (!left && !right) return;
    for (const auto& slot : containerSlots_) {
        if (CheckCollisionPointRec(mouse, slot.bounds)) {
            net_.send(json{
                {"type", "slot_click"},
                {"slot", slot.id},
                {"button", right ? 1 : 0},
                {"shift", IsKeyDown(KEY_LEFT_SHIFT)}
            });
            return;
        }
    }
}

void Game::handlePauseInput() {
    float w = static_cast<float>(GetScreenWidth());
    float h = static_cast<float>(GetScreenHeight());
    pauseResume_ = {w * 0.5f - 200, h * 0.5f - 30, 400, 52};
    pauseQuit_ = {w * 0.5f - 200, h * 0.5f + 42, 400, 52};

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, pauseResume_)) {
        overlay_ = Overlay::None;
    } else if (CheckCollisionPointRec(m, pauseQuit_)) {
        disconnectToTitle();
    }
}

void Game::handleDeathInput() {
    float w = static_cast<float>(GetScreenWidth());
    float h = static_cast<float>(GetScreenHeight());
    deathRespawn_ = {w * 0.5f - 200, h * 0.5f + 50, 400, 52};
    deathTitle_ = {w * 0.5f - 200, h * 0.5f + 122, 400, 46};

    if (!IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) return;
    Vector2 m = GetMousePosition();
    if (CheckCollisionPointRec(m, deathRespawn_)) {
        net_.send(json{{"type", "respawn"}});
    } else if (CheckCollisionPointRec(m, deathTitle_)) {
        disconnectToTitle();
    }
}

void Game::handleChatInput() {
    int cp = GetCharPressed();
    while (cp > 0) {
        chatInput_ += utf8Codepoint(cp);
        cp = GetCharPressed();
    }

    if (IsKeyPressed(KEY_BACKSPACE)) popUtf8(chatInput_);
    if (IsKeyPressed(KEY_ESCAPE)) {
        chatOpen_ = false;
        chatInput_.clear();
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) {
        if (!chatInput_.empty()) {
            net_.send(json{{"type", "chat"}, {"text", chatInput_}});
        }
        chatOpen_ = false;
        chatInput_.clear();
    }
}

void Game::addChat(const std::string& from, const std::string& text) {
    ChatLine c;
    c.from = from;
    c.text = text;
    c.age = 0;
    chats_.push_back(c);
    if (chats_.size() > 100) chats_.pop_front();
}

void Game::addToast(const std::string& text, const std::string& sub) {
    Toast t;
    t.text = text;
    t.sub = sub;
    t.life = t.maxLife = 4;
    toasts_.push_back(t);
    while (toasts_.size() > 4) toasts_.pop_front();
}

void Game::spawnBlockParticles(int x, int y, BlockId block) {
    const BlockDef& def = blockDef(block);
    if (!def.tex || !*def.tex) return;

    for (int i = 0; i < 8; ++i) {
        float sx = static_cast<float>(rng_() % 12);
        float sy = static_cast<float>(rng_() % 12);
        float px = x + 0.2f + (rng_() % 60) / 100.0f;
        float py = y + 0.2f + (rng_() % 60) / 100.0f;
        spawnParticle(
            px, py, def.tex, Rectangle{sx, sy, 4, 4},
            Color{255, 255, 255, 255},
            ((rng_() % 100) / 100.0f - 0.5f) * 3.0f,
            -1.0f - (rng_() % 100) / 100.0f,
            0.55f, 0.14f, true);
    }
}

void Game::spawnParticle(float x, float y, const std::string& tex, Rectangle src, Color tint,
                         float vx, float vy, float life, float size, bool gravity) {
    Particle p;
    p.x = x;
    p.y = y;
    p.tex = tex;
    p.src = src;
    p.tint = tint;
    p.vx = vx;
    p.vy = vy;
    p.life = life;
    p.maxLife = life;
    p.size = size;
    p.gravity = gravity;
    particles_.push_back(p);
}

void Game::takeScreenshot() {
    std::filesystem::path dir = root_ / "screenshots";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);

    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &now);

    std::ostringstream name;
    name << "screenshot_" << std::put_time(&tm, "%Y%m%d_%H%M%S") << ".png";
    std::filesystem::path path = dir / name.str();

    TakeScreenshot(path.string().c_str());
    addToast("截图已保存", path.filename().string());
}

// -----------------------------------------------------------------------------
// Drawing helpers
// -----------------------------------------------------------------------------

float Game::lightAt(int x, int y) const {
    const int surface = world_.surfaceY(x);
    const float depth = static_cast<float>(y - surface);
    float light;
    if (depth <= 1.0f) light = 1.0f;
    else light = std::max(0.12f, 1.0f - depth * 0.025f);
    return light * daylightFactor();
}

float Game::daylightFactor() const {
    const double p = std::fmod(static_cast<double>(worldTime_), 24000.0) / 24000.0;
    const float elevation = std::sin(static_cast<float>(p) * 2.0f * PI_F);
    return std::clamp(0.20f + 0.80f * std::max(0.0f, elevation), 0.20f, 1.0f);
}

Vector2 Game::worldToScreen(float wx, float wy) const {
    const float p = blockPixels();
    return Vector2{
        (wx - cameraX_) * p + GetScreenWidth() * 0.5f,
        (wy - cameraY_) * p + GetScreenHeight() * 0.5f
    };
}

Vector2 Game::screenToWorld(float sx, float sy) const {
    const float p = blockPixels();
    return Vector2{
        (sx - GetScreenWidth() * 0.5f) / p + cameraX_,
        (sy - GetScreenHeight() * 0.5f) / p + cameraY_
    };
}

void Game::drawText(const std::string& text, float x, float y, float size, Color color, bool shadow) {
    Font& f = assets_.font();
    const float spacing = std::max(1.0f, size * 0.03f);
    if (shadow) {
        DrawTextEx(f, text.c_str(), Vector2{x + 2, y + 2}, size, spacing, Color{30, 30, 30, 220});
    }
    DrawTextEx(f, text.c_str(), Vector2{x, y}, size, spacing, color);
}

float Game::measureText(const std::string& text, float size) {
    return MeasureTextEx(assets_.font(), text.c_str(), size, std::max(1.0f, size * 0.03f)).x;
}

void Game::drawTextCentered(const std::string& text, float cx, float y, float size, Color color, bool shadow) {
    drawText(text, cx - measureText(text, size) * 0.5f, y, size, color, shadow);
}

void Game::drawPanel(Rectangle r, float alpha) {
    DrawRectangleRec(r, Color{12, 12, 18, static_cast<unsigned char>(210 * alpha)});
    DrawRectangleLinesEx(r, 2, Color{110, 110, 130, static_cast<unsigned char>(230 * alpha)});
}

bool Game::drawButton(Rectangle r, const std::string& text) {
    const bool hover = CheckCollisionPointRec(GetMousePosition(), r);
    Texture2D& t = assets_.tex(hover ? "gui/sprites/widget/button_highlighted"
                                     : "gui/sprites/widget/button");
    const float scale = r.height / 20.0f;
    const float edge = 4.0f * scale;
    DrawTexturePro(t, Rectangle{0, 0, 4, 20},
                   Rectangle{r.x, r.y, edge, r.height}, Vector2{0, 0}, 0, WHITE);
    DrawTexturePro(t, Rectangle{4, 0, 192, 20},
                   Rectangle{r.x + edge, r.y, r.width - edge * 2, r.height},
                   Vector2{0, 0}, 0, WHITE);
    DrawTexturePro(t, Rectangle{196, 0, 4, 20},
                   Rectangle{r.x + r.width - edge, r.y, edge, r.height},
                   Vector2{0, 0}, 0, WHITE);
    drawTextCentered(text, r.x + r.width * 0.5f,
                     r.y + r.height * 0.5f - r.height * 0.30f,
                     r.height * 0.52f, WHITE);
    return hover;
}

void Game::drawItemIcon(const std::string& id, Rectangle r) {
    if (id.empty()) return;
    const ItemDef* def = itemDef(id);
    const std::string tex = def ? def->tex : "block/dirt";
    Texture2D& t = assets_.tex(tex);
    DrawTexturePro(t, Rectangle{0, 0, (float)t.width, (float)t.height}, r, Vector2{0, 0}, 0, WHITE);
}

void Game::drawItemStack(const ItemStack& stack, Rectangle r, float scale) {
    if (stack.empty()) return;
    drawItemIcon(stack.id, r);
    if (stack.count > 1) {
        const float fs = 7.0f * scale;
        const std::string s = std::to_string(stack.count);
        drawText(s, r.x + r.width - measureText(s, fs) - 3, r.y + r.height - fs - 2, fs, WHITE);
    }
}

void Game::drawMenuDirt() {
    Texture2D& dirt = assets_.tex("block/dirt");
    const float tile = 64;
    for (float y = 0; y < GetScreenHeight(); y += tile) {
        for (float x = 0; x < GetScreenWidth(); x += tile) {
            DrawTexturePro(dirt, Rectangle{0, 0, 16, 16}, Rectangle{x, y, tile, tile},
                           Vector2{0, 0}, 0, Color{95, 95, 95, 255});
        }
    }
    DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), Color{10, 10, 18, 140});
}

void Game::drawTooltip(const std::string& text, Vector2 mouse) {
    if (text.empty()) return;
    const float fs = 12;
    const float w = measureText(text, fs) + 20;
    const float h = 28;
    const float x = std::min(mouse.x + 16, GetScreenWidth() - w - 8);
    const float y = std::min(mouse.y + 12, GetScreenHeight() - h - 8);
    DrawRectangleRounded(Rectangle{x, y, w, h}, 0.25f, 4, Color{15, 12, 25, 240});
    DrawRectangleLinesEx(Rectangle{x, y, w, h}, 1.5f, Color{150, 120, 220, 255});
    drawText(text, x + 10, y + 6, fs, WHITE, false);
}

// -----------------------------------------------------------------------------
// Screens
// -----------------------------------------------------------------------------

void Game::draw() {
    switch (screen_) {
        case Screen::Title: drawTitle(); break;
        case Screen::Settings: drawSettings(); break;
        case Screen::Connecting: drawConnecting(); break;
        case Screen::Playing: drawPlaying(); break;
        case Screen::Disconnected: drawDisconnected(); break;
    }
}

void Game::drawTitle() {
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());

    Texture2D& bg = assets_.tex("gui/title/background/Background.jpg");
    DrawTexturePro(bg, Rectangle{0, 0, (float)bg.width, (float)bg.height},
                   Rectangle{0, 0, w, h}, Vector2{0, 0}, 0, WHITE);
    DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h), Color{0, 0, 0, 105});

    Texture2D& logo = assets_.tex("gui/title/minecraft.png");
    const float lw = std::min(w * 0.68f, 820.0f);
    const float lh = lw * 0.25f;
    DrawTexturePro(logo, Rectangle{0, 0, (float)logo.width, (float)logo.height},
                   Rectangle{(w - lw) * 0.5f, h * 0.12f, lw, lh}, Vector2{0, 0}, 0, WHITE);

    drawTextCentered("也是 2D 的！", w * 0.5f + lw * 0.30f, h * 0.12f + lh * 0.72f, 22,
                     Color{255, 225, 80, 255});

    drawButton(titleButtons_[0], "单人游戏");
    drawButton(titleButtons_[1], "设置与操作");
    drawButton(titleButtons_[2], "退出游戏");

    drawText("Minecraft 2D 1.0  ·  C++ / raylib  ·  客户端 + 服务端", 18, h - 36, 14,
             Color{220, 220, 220, 230});
}

void Game::drawSettings() {
    drawMenuDirt();
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    Rectangle panel{w * 0.5f - 480, h * 0.5f - 290, 960, 580};
    drawPanel(panel);
    drawTextCentered("设置与操作", w * 0.5f, panel.y + 22, 32, WHITE);

    const char* left[] = {
        "A / D        向左 / 向右移动",
        "空格         跳跃 / 游泳",
        "左 Shift     潜行（防止跌落）",
        "左 Ctrl      疾跑",
        "鼠标左键     攻击 / 挖掘",
        "鼠标右键     放置 / 使用 / 进食",
        "1 - 9 / 滚轮  选择快捷栏",
        "Q            丢弃选中物品",
    };
    const char* right[] = {
        "E            打开 / 关闭背包",
        "T / Enter    打开聊天",
        "/            输入指令",
        "Tab          在线玩家列表",
        "F1           隐藏 / 显示 HUD",
        "F2           截图",
        "F3           调试信息",
        "F5           切换镜头远近",
        "F11          全屏",
        "Esc          暂停 / 返回",
    };

    for (int i = 0; i < 8; ++i) {
        drawText(left[i], panel.x + 50, panel.y + 100 + i * 44, 16, WHITE);
    }
    for (int i = 0; i < 10; ++i) {
        drawText(right[i], panel.x + 500, panel.y + 100 + i * 40, 16, WHITE);
    }

    drawTextCentered("控制方案与原版一致；2D 世界里 A/D 对应原版横向移动。",
                     w * 0.5f, panel.y + panel.height - 70, 15, Color{210, 210, 210, 255});
    drawButton(settingsBack_, "返回");
}

void Game::drawConnecting() {
    drawMenuDirt();
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    drawTextCentered("Minecraft 2D", w * 0.5f, h * 0.28f, 40, WHITE);
    drawTextCentered("正在连接服务器...", w * 0.5f, h * 0.44f, 26, WHITE);
    drawTextCentered(address_, w * 0.5f, h * 0.52f, 18, Color{200, 200, 200, 255});
    drawTextCentered("按 Esc 取消", w * 0.5f, h * 0.64f, 15, Color{180, 180, 180, 255});
}

void Game::drawDisconnected() {
    drawMenuDirt();
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    Rectangle panel{w * 0.5f - 420, h * 0.28f, 840, 300};
    drawPanel(panel);
    drawTextCentered("连接中断", w * 0.5f, panel.y + 24, 32, WHITE);
    drawTextCentered(disconnectReason_, w * 0.5f, panel.y + 92, 18, Color{255, 200, 160, 255});
    drawTextCentered("请确认服务端进程正在运行；客户端也可以自动启动本地服务端。",
                     w * 0.5f, panel.y + 140, 15, Color{210, 210, 210, 255});
    drawButton(discRetry_, "重新连接");
    drawButton(discBack_, "返回标题");
}

// -----------------------------------------------------------------------------
// World rendering
// -----------------------------------------------------------------------------

void Game::drawSky() {
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    const float day = daylightFactor();

    Color top = Color{
        static_cast<unsigned char>(20 + 70 * day),
        static_cast<unsigned char>(30 + 110 * day),
        static_cast<unsigned char>(60 + 170 * day),
        255
    };
    Color bottom = Color{
        static_cast<unsigned char>(40 + 140 * day),
        static_cast<unsigned char>(55 + 160 * day),
        static_cast<unsigned char>(85 + 170 * day),
        255
    };
    DrawRectangleGradientV(0, 0, static_cast<int>(w), static_cast<int>(h), top, bottom);

    int sx = std::clamp(static_cast<int>(cameraX_), 0, world_.width() - 1);
    int surface = world_.surfaceY(sx);
    float horizon = worldToScreen(cameraX_, static_cast<float>(surface)).y;
    if (horizon > -200 && horizon < h + 200) {
        for (int layer = 0; layer < 2; ++layer) {
            Color c = layer == 0 ? Color{35, 60, 80, 255} : Color{22, 40, 48, 255};
            for (int x = -16; x < static_cast<int>(w) + 16; x += 16) {
                float n = std::sin((x + cameraX_ * 48 * 0.06f + layer * 80) * 0.01f) * 22.0f
                        + std::sin((x + cameraX_ * 48 * 0.02f) * 0.03f) * 10.0f;
                DrawRectangle(x, static_cast<int>(horizon - 50 - n + layer * 22), 17,
                              static_cast<int>(h - horizon + 300), c);
            }
        }
    }

    Texture2D& clouds = assets_.tex("environment/clouds");
    float scroll = std::fmod(cameraX_ * 8.0f + playTime_ * 4.0f, 512.0f);
    for (float x = -scroll - 512; x < w + 512; x += 512) {
        DrawTexturePro(clouds, Rectangle{0, 0, (float)clouds.width, (float)clouds.height},
                       Rectangle{x, 30, 512, 180}, Vector2{0, 0}, 0,
                       Color{255, 255, 255, static_cast<unsigned char>(60 + 60 * day)});
    }

    const double prog = std::fmod(static_cast<double>(worldTime_), 24000.0) / 24000.0;
    if (prog < 0.5) {
        const float t = static_cast<float>(prog / 0.5);
        const float sunX = w * 0.1f + w * 0.8f * t;
        const float sunY = h * 0.52f - std::sin(t * PI_F) * h * 0.38f;
        Texture2D& sun = assets_.tex("environment/sun");
        DrawTexturePro(sun, Rectangle{0, 0, 32, 32},
                       Rectangle{sunX - 40, sunY - 40, 80, 80}, Vector2{0, 0}, 0, WHITE);
    } else {
        const float t = static_cast<float>((prog - 0.5) / 0.5);
        const float moonX = w * 0.1f + w * 0.8f * t;
        const float moonY = h * 0.52f - std::sin(t * PI_F) * h * 0.38f;
        Texture2D& moon = assets_.tex("environment/moon_phases");
        DrawTexturePro(moon, Rectangle{0, 0, 32, 32},
                       Rectangle{moonX - 34, moonY - 34, 68, 68}, Vector2{0, 0}, 0, WHITE);
    }
}

void Game::drawTile(int x, int y, bool background) {
    const BlockId id = background ? world_.bg(x, y) : world_.fg(x, y);
    if (id == AIR) return;

    const BlockDef& def = blockDef(id);
    if (!def.tex || !*def.tex) return;

    Texture2D& t = assets_.tex(def.tex);
    Rectangle src{0, 0, 16, 16};

    if (t.height > 16) {
        const int frames = std::max(1, t.height / 16);
        const int frame = static_cast<int>((worldTime_ / 5) % frames);
        src.y = static_cast<float>(frame * 16);
    }

    const float p = blockPixels();
    Vector2 s = worldToScreen(static_cast<float>(x), static_cast<float>(y));
    Rectangle dst{s.x - 0.5f, s.y - 0.5f, p + 1.0f, p + 1.0f};

    float light = lightAt(x, y);
    if (background) light *= 0.55f;
    const float b = std::clamp(0.22f + 0.78f * light, 0.0f, 1.0f);
    Color tint = scaleColor(WHITE, b);

    if (id == WATER) tint = Color{90, 150, 255, 175};
    if (id == LAVA) tint = Color{255, 165, 70, 255};

    DrawTexturePro(t, src, dst, Vector2{0, 0}, 0, tint);
}

void Game::drawTiles(bool background) {
    const float p = blockPixels();
    const int w = GetScreenWidth();
    const int h = GetScreenHeight();

    int x0 = static_cast<int>(std::floor(cameraX_ - w * 0.5f / p)) - 1;
    int x1 = static_cast<int>(std::ceil(cameraX_ + w * 0.5f / p)) + 1;
    int y0 = static_cast<int>(std::floor(cameraY_ - h * 0.5f / p)) - 1;
    int y1 = static_cast<int>(std::ceil(cameraY_ + h * 0.5f / p)) + 1;

    x0 = std::max(0, x0);
    y0 = std::max(0, y0);
    x1 = std::min(world_.width() - 1, x1);
    y1 = std::min(world_.height() - 1, y1);

    for (int y = y0; y <= y1; ++y) {
        for (int x = x0; x <= x1; ++x) {
            if (background) {
                if (world_.bg(x, y) != AIR) drawTile(x, y, true);
            } else {
                if (world_.fg(x, y) != AIR) drawTile(x, y, false);
            }
        }
    }
}

void Game::drawTarget() {
    if (!hoverValid_ || overlay_ != Overlay::None || dead_) return;
    if (hoveredEntity_ >= 0) return;

    const float p = blockPixels();
    Vector2 s = worldToScreen(static_cast<float>(hoverX_), static_cast<float>(hoverY_));
    DrawRectangleLinesEx(Rectangle{s.x, s.y, p, p}, 2, Color{20, 20, 20, 190});

    if (mineProgress_ > 0) {
        int stage = std::clamp(static_cast<int>(mineProgress_ * 10), 0, 9);
        Texture2D& cracks = assets_.tex("block/destroy_stage_" + std::to_string(stage));
        DrawTexturePro(cracks, Rectangle{0, 0, 16, 16}, Rectangle{s.x, s.y, p, p},
                       Vector2{0, 0}, 0, Color{255, 255, 255, 210});
    }
}

void Game::drawHumanoid(const std::string& skin, float wx, float wy, int facing, bool moving,
                        float walkPhase, float swing, Color tint, const std::string& held,
                        float heightBlocks, bool drawHeld) {
    Texture2D& t = assets_.tex(skin);
    const float p = blockPixels();
    Vector2 s = worldToScreen(wx, wy);
    const float u = heightBlocks * p / 32.0f;

    DrawEllipse(static_cast<int>(s.x), static_cast<int>(s.y), p * 0.28f, p * 0.10f, Color{0, 0, 0, 70});

    const bool flip = facing < 0;
    const float walk = moving ? std::sin(walkPhase) : 0.0f;
    const float legA = walk * 28.0f;
    const float armA = -walk * 24.0f;

    auto part = [&](Rectangle src, float lx, float ly, float w, float h, float rot, float ox, float oy) {
        if (flip) {
            lx = -lx - w;
            src.width = -src.width;
            rot = -rot;
        }
        DrawTexturePro(t, src, Rectangle{s.x + lx, s.y + ly, w, h},
                       Vector2{ox, oy}, rot, tint);
    };

    // 后腿
    part(Rectangle{20, 52, 4, 12}, -4 * u, -12 * u, 4 * u, 12 * u, -legA, 2 * u, 0);
    // 身体
    part(Rectangle{20, 20, 8, 12}, -4 * u, -24 * u, 8 * u, 12 * u, 0, 4 * u, 0);
    // 前腿
    part(Rectangle{4, 20, 4, 12}, 0, -12 * u, 4 * u, 12 * u, legA, 2 * u, 0);
    // 后臂
    part(Rectangle{36, 52, 4, 12}, -8 * u, -24 * u, 4 * u, 12 * u, -armA, 2 * u, 0);
    // 前臂
    part(Rectangle{44, 20, 4, 12}, 4 * u, -24 * u, 4 * u, 12 * u, armA - swing * 100, 2 * u, 0);
    // 头
    part(Rectangle{8, 8, 8, 8}, -4 * u, -32 * u, 8 * u, 8 * u, 0, 4 * u, 4 * u);

    if (drawHeld && !held.empty()) {
        const float size = 7.0f * u;
        float hx = s.x + (facing > 0 ? 7.0f * u : -7.0f * u - size);
        float hy = s.y - 18.0f * u;
        drawItemIcon(held, Rectangle{hx, hy, size, size});
    }
}

void Game::drawPig(const EntityView& e) {
    const float p = blockPixels();
    Vector2 s = worldToScreen(e.x, e.y);
    const float light = std::max(0.35f, lightAt(static_cast<int>(e.x), static_cast<int>(e.y)));
    Color body = scaleColor(Color{240, 170, 170, 255}, light);
    Color dark = scaleColor(Color{190, 120, 120, 255}, light);

    DrawEllipse(static_cast<int>(s.x), static_cast<int>(s.y), p * 0.4f, p * 0.1f, Color{0, 0, 0, 65});
    DrawRectangle(static_cast<int>(s.x - 0.5f * p), static_cast<int>(s.y - 0.55f * p),
                  static_cast<int>(p), static_cast<int>(0.42f * p), body);

    for (int i = 0; i < 4; ++i) {
        DrawRectangle(static_cast<int>(s.x - 0.42f * p + i * 0.24f * p),
                      static_cast<int>(s.y - 0.18f * p),
                      static_cast<int>(0.13f * p), static_cast<int>(0.18f * p), dark);
    }

    Texture2D& pig = assets_.tex("entity/pig/pig.png");
    Rectangle headSrc{8, 8, 8, 8};
    if (e.facing < 0) headSrc.width = -8;
    float hx = s.x + (e.facing > 0 ? 0.2f : -0.75f) * p;
    DrawTexturePro(pig, headSrc, Rectangle{hx, s.y - 0.95f * p, 0.55f * p, 0.55f * p},
                   Vector2{0, 0}, 0, body);
    DrawRectangle(static_cast<int>(hx + (e.facing > 0 ? 0.36f : -0.1f) * p),
                  static_cast<int>(s.y - 0.78f * p),
                  static_cast<int>(0.18f * p), static_cast<int>(0.16f * p), dark);
}

void Game::drawZombie(const EntityView& e) {
    const float light = std::max(0.35f, lightAt(static_cast<int>(e.x), static_cast<int>(e.y)));
    Color tint = scaleColor(Color{175, 225, 160, 255}, light);
    drawHumanoid("entity/zombie/zombie.png", e.x, e.y, e.facing,
                 std::abs(e.vx) > 0.1f, e.walkPhase, 0, tint, "", 1.95f, false);
}

void Game::drawItemEntity(const EntityView& e) {
    const float p = blockPixels();
    Vector2 s = worldToScreen(e.x, e.y);
    const float bob = std::sin(playTime_ * 3.0f + e.id) * 3.0f;
    const float size = p * 0.45f;

    DrawEllipse(static_cast<int>(s.x), static_cast<int>(s.y), p * 0.2f, p * 0.06f, Color{0, 0, 0, 55});
    drawItemIcon(e.item.empty() ? "dirt" : e.item,
                 Rectangle{s.x - size * 0.5f, s.y - size - 4 + bob, size, size});

    if (e.count > 1) {
        drawText(std::to_string(e.count), s.x + 8, s.y - size + bob, 12, WHITE);
    }
}

void Game::drawRenderables() {
    struct R {
        int type; // 0 player, 1 entity
        int id;
        float y;
    };
    std::vector<R> list;
    list.push_back({0, -1, body_.y});

    for (const auto& [id, r] : remotes_) {
        list.push_back({0, id, r.y});
    }
    for (const auto& [id, e] : entities_) {
        list.push_back({1, id, e.y});
    }

    std::sort(list.begin(), list.end(), [](const R& a, const R& b) { return a.y < b.y; });

    for (const auto& item : list) {
        if (item.type == 0) {
            if (item.id == -1) {
                float light = std::max(0.4f, lightAt(static_cast<int>(body_.x), static_cast<int>(body_.y - 0.5f)));
                Color tint = scaleColor(WHITE, light);
                drawHumanoid("entity/player/wide/steve.png", body_.x, body_.y, facing_,
                             body_.onGround && std::abs(body_.vx) > 0.1f,
                             walkPhase_, swing_, tint, currentHeldItem(), 1.8f, true);
            } else {
                auto it = remotes_.find(item.id);
                if (it == remotes_.end()) continue;
                const RemotePlayer& r = it->second;
                static const char* skins[] = {
                    "entity/player/wide/steve.png",
                    "entity/player/wide/alex.png",
                    "entity/player/wide/ari.png",
                    "entity/player/wide/efe.png",
                    "entity/player/wide/kai.png",
                    "entity/player/wide/makena.png",
                    "entity/player/wide/noor.png",
                    "entity/player/wide/sunny.png",
                    "entity/player/wide/zuri.png"
                };
                const std::string skin = skins[std::abs(r.id) % 9];
                const float light = std::max(0.4f, lightAt(static_cast<int>(r.x), static_cast<int>(r.y - 0.5f)));
                Color tint = scaleColor(WHITE, light);
                drawHumanoid(skin, r.x, r.y, r.facing,
                             std::abs(r.vx) > 0.1f, r.walkPhase, 0, tint, "", 1.8f, false);

                Vector2 s = worldToScreen(r.x, r.y);
                const float fs = 14;
                float tw = measureText(r.name, fs);
                DrawRectangle(static_cast<int>(s.x - tw * 0.5f - 5),
                              static_cast<int>(s.y - 2.1f * blockPixels()),
                              static_cast<int>(tw + 10), 22, Color{0, 0, 0, 110});
                drawTextCentered(r.name, s.x, s.y - 2.1f * blockPixels() + 3, fs, WHITE);
            }
        } else {
            auto it = entities_.find(item.id);
            if (it == entities_.end()) continue;
            if (it->second.type == "pig") drawPig(it->second);
            else if (it->second.type == "zombie") drawZombie(it->second);
            else drawItemEntity(it->second);
        }
    }
}

void Game::drawParticles() {
    for (const auto& p : particles_) {
        Vector2 s = worldToScreen(p.x, p.y);
        const float a = std::clamp(p.life / std::max(0.001f, p.maxLife), 0.0f, 1.0f);
        const float size = p.size * blockPixels();
        Color tint = p.tint;
        tint.a = static_cast<unsigned char>(255 * a);

        if (p.tex.empty()) {
            DrawRectangle(static_cast<int>(s.x), static_cast<int>(s.y),
                          static_cast<int>(size), static_cast<int>(size), tint);
        } else {
            Texture2D& t = assets_.tex(p.tex);
            DrawTexturePro(t, p.src, Rectangle{s.x, s.y, size, size}, Vector2{0, 0}, 0, tint);
        }
    }
}

// -----------------------------------------------------------------------------
// HUD / overlays
// -----------------------------------------------------------------------------

void Game::drawHud() {
    if (!showHud_ || dead_) return;

    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    const float scale = uiScale_;

    const float hbW = 182 * scale;
    const float hbH = 22 * scale;
    const float hbX = (w - hbW) * 0.5f;
    const float hbY = h - hbH - 10 * scale;

    Texture2D& hotbar = assets_.tex("gui/sprites/hud/hotbar");
    DrawTexturePro(hotbar, Rectangle{0, 0, 182, 22}, Rectangle{hbX, hbY, hbW, hbH},
                   Vector2{0, 0}, 0, WHITE);

    for (int i = 0; i < HOTBAR_SIZE; ++i) {
        const float slotW = 20 * scale;
        const float sx = hbX + i * slotW + scale;
        const float sy = hbY + scale;

        if (i == selected_) {
            Texture2D& sel = assets_.tex("gui/sprites/hud/hotbar_selection");
            DrawTexturePro(sel, Rectangle{0, 0, 24, 23},
                           Rectangle{sx - 2 * scale, sy - 2 * scale, 24 * scale, 23 * scale},
                           Vector2{0, 0}, 0, WHITE);
        }

        drawItemStack(inventory_[i], Rectangle{sx + 2 * scale, sy + 2 * scale, 16 * scale, 16 * scale}, scale);
    }

    const float icon = 9 * scale;
    const float heartsY = hbY - 12 * scale;
    for (int i = 0; i < 10; ++i) {
        const float x = hbX + i * icon;
        Texture2D& container = assets_.tex("gui/sprites/hud/heart/container");
        DrawTexturePro(container, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, icon, icon},
                       Vector2{0, 0}, 0, WHITE);

        const int v = health_ - i * 2;
        if (v >= 2) {
            Texture2D& full = assets_.tex("gui/sprites/hud/heart/full");
            DrawTexturePro(full, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, icon, icon},
                           Vector2{0, 0}, 0, WHITE);
        } else if (v == 1) {
            Texture2D& half = assets_.tex("gui/sprites/hud/heart/half");
            DrawTexturePro(half, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, icon, icon},
                           Vector2{0, 0}, 0, WHITE);
        }
    }

    const float foodX0 = hbX + hbW - 10 * icon;
    for (int i = 0; i < 10; ++i) {
        const float x = foodX0 + i * icon;
        Texture2D& empty = assets_.tex("gui/sprites/hud/food_empty");
        DrawTexturePro(empty, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, icon, icon},
                       Vector2{0, 0}, 0, WHITE);

        const int v = food_ - i * 2;
        if (v >= 2) {
            Texture2D& full = assets_.tex("gui/sprites/hud/food_full");
            DrawTexturePro(full, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, icon, icon},
                           Vector2{0, 0}, 0, WHITE);
        } else if (v == 1) {
            Texture2D& half = assets_.tex("gui/sprites/hud/food_half");
            DrawTexturePro(half, Rectangle{0, 0, 9, 9}, Rectangle{x, heartsY, icon, icon},
                           Vector2{0, 0}, 0, WHITE);
        }
    }

    if (selectedNameTimer_ > 0 && !inventory_[selected_].empty()) {
        const float fs = 16;
        const std::string name = itemName(inventory_[selected_].id);
        drawTextCentered(name, w * 0.5f, heartsY - 26, fs, WHITE);
    }
}

void Game::buildInventoryLayout() {
    // Slot coordinates follow the 176px vanilla GUI layouts and shared slot IDs.
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    const bool chest = containerGui_ == "chest";
    const float sourceHeight = chest ? 222.0f : 166.0f;
    containerScale_ = std::min({3.0f, (w - 32.0f) / 176.0f, (h - 32.0f) / sourceHeight});
    containerScale_ = std::max(1.0f, containerScale_);
    const float panelWidth = 176.0f * containerScale_;
    const float panelHeight = sourceHeight * containerScale_;
    invPanel_ = Rectangle{(w - panelWidth) * 0.5f, (h - panelHeight) * 0.5f,
                          panelWidth, panelHeight};

    containerSlots_.clear();
    auto addSlot = [&](int id, float x, float y) {
        containerSlots_.push_back(ContainerSlotView{
            id, Rectangle{invPanel_.x + x * containerScale_, invPanel_.y + y * containerScale_,
                          18.0f * containerScale_, 18.0f * containerScale_}});
    };

    const float mainY = chest ? 140.0f : 84.0f;
    const float hotbarY = chest ? 198.0f : 142.0f;
    for (int row = 0; row < 3; ++row) {
        for (int col = 0; col < 9; ++col) addSlot(9 + row * 9 + col, 8.0f + col * 18.0f, mainY + row * 19.0f);
    }
    for (int col = 0; col < HOTBAR_SIZE; ++col) addSlot(col, 8.0f + col * 18.0f, hotbarY);

    if (containerGui_ == "inventory") {
        for (int i = 0; i < 4; ++i) addSlot(36 + i, 98.0f + (i % 2) * 18.0f, 18.0f + (i / 2) * 18.0f);
        addSlot(45, 154.0f, 28.0f);
    } else if (containerGui_ == "crafting_table") {
        for (int i = 0; i < 9; ++i) addSlot(36 + i, 30.0f + (i % 3) * 18.0f, 17.0f + (i / 3) * 18.0f);
        addSlot(45, 120.0f, 35.0f);
    } else if (containerGui_ == "furnace") {
        addSlot(46, 56.0f, 17.0f);
        addSlot(47, 56.0f, 53.0f);
        addSlot(48, 116.0f, 35.0f);
    } else if (chest) {
        for (int i = 0; i < 54; ++i) addSlot(49 + i, 8.0f + (i % 9) * 18.0f, 18.0f + (i / 9) * 18.0f);
    }
}

void Game::drawInventory() {
    // Draw the original container texture first, then current server slot data.
    buildInventoryLayout();

    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h), Color{0, 0, 0, 150});
    const float sourceHeight = containerGui_ == "chest" ? 222.0f : 166.0f;
    const std::string texture = containerGui_ == "chest"
                                    ? "gui/container/generic_54"
                                    : "gui/container/" + (containerGui_.empty() ? "inventory" : containerGui_);
    Texture2D& background = assets_.tex(texture);
    DrawTexturePro(background, Rectangle{0, 0, 176, sourceHeight}, invPanel_,
                   Vector2{0, 0}, 0, WHITE);

    std::string title = "物品栏";
    if (containerGui_ == "crafting_table") title = "工作台";
    else if (containerGui_ == "furnace") title = "熔炉";
    else if (containerGui_ == "chest") title = "箱子";
    drawText(title, invPanel_.x + 8.0f * containerScale_, invPanel_.y + 5.0f * containerScale_,
             8.0f * containerScale_, Color{64, 64, 64, 255}, false);
    if (containerGui_ == "chest") {
        drawText("物品栏", invPanel_.x + 8.0f * containerScale_, invPanel_.y + 128.0f * containerScale_,
                 8.0f * containerScale_, Color{64, 64, 64, 255}, false);
        if (containerViewers_ > 1) {
            drawText(std::to_string(containerViewers_) + " 位玩家正在查看",
                     invPanel_.x + 8.0f * containerScale_, invPanel_.y - 24.0f, 16, WHITE);
        }
    }

    Vector2 mouse = GetMousePosition();
    std::string tooltip;
    auto stackAt = [&](int id) -> const ItemStack& {
        if (id >= 0 && id < INV_SIZE) return inventory_[id];
        if (id >= 36 && id < 45) return craftingGrid_[id - 36];
        if (id == 45) return craftResult_;
        if (id >= 46 && id <= 48) return furnaceSlots_[id - 46];
        if (id >= 49 && id < 103) return chestSlots_[id - 49];
        static const ItemStack empty;
        return empty;
    };
    for (const auto& slot : containerSlots_) {
        const Rectangle r = slot.bounds;
        const bool hover = CheckCollisionPointRec(mouse, r);
        if (slot.id == selected_ && slot.id >= 0 && slot.id < HOTBAR_SIZE) {
            DrawRectangleLinesEx(r, std::max(2.0f, containerScale_), Color{255, 220, 80, 255});
        }
        const ItemStack& stack = stackAt(slot.id);
        const float inset = containerScale_;
        drawItemStack(stack, Rectangle{r.x + inset, r.y + inset,
                                       r.width - inset * 2, r.height - inset * 2}, containerScale_);
        if (hover) {
            DrawRectangle(static_cast<int>(r.x), static_cast<int>(r.y),
                          static_cast<int>(r.width), static_cast<int>(r.height), Color{255, 255, 255, 45});
            if (!stack.empty()) tooltip = itemName(stack.id);
        }
    }

    if (containerGui_ == "furnace") {
        const float burnRatio = furnaceBurnTotal_ > 0
                                    ? std::clamp(static_cast<float>(furnaceBurnTime_) / furnaceBurnTotal_, 0.0f, 1.0f)
                                    : 0.0f;
        const float cookRatio = std::clamp(static_cast<float>(furnaceCookTime_) / furnaceCookTotal_, 0.0f, 1.0f);
        if (burnRatio > 0) {
            Texture2D& flame = assets_.tex("gui/sprites/container/furnace/lit_progress");
            const float height = flame.height * burnRatio;
            DrawTexturePro(flame,
                           Rectangle{0, flame.height - height, static_cast<float>(flame.width), height},
                           Rectangle{invPanel_.x + 56.0f * containerScale_,
                                     invPanel_.y + (51.0f - height) * containerScale_,
                                     flame.width * containerScale_, height * containerScale_},
                           Vector2{0, 0}, 0, WHITE);
        }
        if (cookRatio > 0) {
            Texture2D& arrow = assets_.tex("gui/sprites/container/furnace/burn_progress");
            DrawTexturePro(arrow, Rectangle{0, 0, arrow.width * cookRatio, static_cast<float>(arrow.height)},
                           Rectangle{invPanel_.x + 79.0f * containerScale_,
                                     invPanel_.y + 35.0f * containerScale_,
                                     arrow.width * containerScale_ * cookRatio,
                                     arrow.height * containerScale_},
                           Vector2{0, 0}, 0, WHITE);
        }
    }

    if (!cursorStack_.empty()) {
        const float size = 16.0f * containerScale_;
        drawItemStack(cursorStack_, Rectangle{mouse.x - size * 0.5f, mouse.y - size * 0.5f, size, size},
                      containerScale_);
    }
    if (!tooltip.empty()) drawTooltip(tooltip, mouse);
}

void Game::drawPause() {
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h), Color{0, 0, 0, 145});

    Rectangle panel{w * 0.5f - 280, h * 0.5f - 190, 560, 380};
    drawPanel(panel);
    drawTextCentered("游戏已暂停", w * 0.5f, panel.y + 28, 34, WHITE);
    drawButton(pauseResume_, "返回游戏");
    drawButton(pauseQuit_, "保存并退出");

    drawTextCentered("Esc 返回游戏", w * 0.5f, panel.y + panel.height - 32, 14,
                     Color{190, 190, 190, 255});
}

void Game::drawDeath() {
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    DrawRectangle(0, 0, static_cast<int>(w), static_cast<int>(h), Color{110, 0, 0, 150});

    Rectangle panel{w * 0.5f - 280, h * 0.5f - 170, 560, 360};
    drawPanel(panel);
    drawTextCentered("你死了！", w * 0.5f, panel.y + 30, 42, Color{255, 110, 90, 255});
    drawTextCentered("生命值会在重生后恢复，物品栏保留。", w * 0.5f, panel.y + 112, 16,
                     Color{220, 220, 220, 255});
    drawButton(deathRespawn_, "重生");
    drawButton(deathTitle_, "返回标题");
}

void Game::drawChat() {
    const float w = static_cast<float>(GetScreenWidth());
    const float h = static_cast<float>(GetScreenHeight());
    const float fs = 13;
    const float lineH = 20;
    const float bottom = h - 100;

    int visible = 0;
    for (auto it = chats_.rbegin(); it != chats_.rend() && visible < 10; ++it) {
        if (!chatOpen_ && it->age > 10) continue;
        ++visible;
    }

    float y = bottom - visible * lineH;
    int skipped = 0;
    for (auto it = chats_.rbegin(); it != chats_.rend() && skipped < 10; ++it) {
        if (!chatOpen_ && it->age > 10) continue;
        ++skipped;
    }

    for (auto it = chats_.begin(); it != chats_.end(); ++it) {
        if (!chatOpen_ && it->age > 10) continue;
        const std::string line = it->from.empty() ? it->text : ("<" + it->from + "> " + it->text);
        const float tw = measureText(line, fs);
        DrawRectangle(8, static_cast<int>(y - 2), static_cast<int>(tw + 14), static_cast<int>(lineH),
                      Color{0, 0, 0, 115});
        drawText(line, 14, y, fs, WHITE, false);
        y += lineH;
    }

    if (chatOpen_) {
        const float boxY = h - 82;
        DrawRectangle(8, static_cast<int>(boxY), static_cast<int>(w) - 16, 38, Color{0, 0, 0, 175});
        const std::string text = "> " + chatInput_;
        drawText(text, 16, boxY + 7, fs, WHITE, false);
        if (std::fmod(chatBlink_, 1.0f) < 0.55f) {
            float cx = 16 + measureText(text, fs) + 2;
            DrawRectangle(static_cast<int>(cx), static_cast<int>(boxY + 7), 2, 18, WHITE);
        }
    }
}

void Game::drawToasts() {
    const float w = static_cast<float>(GetScreenWidth());
    float y = 20;
    for (const auto& t : toasts_) {
        float a = std::clamp(t.life / 0.5f, 0.0f, 1.0f);
        if (t.life > t.maxLife - 0.3f) a = std::clamp((t.maxLife - t.life) / 0.3f, 0.0f, 1.0f);

        const float bw = 280;
        const float bh = 54;
        const float x = w - bw - 20;
        DrawRectangleRounded(Rectangle{x, y, bw, bh}, 0.15f, 4,
                             Color{15, 12, 25, static_cast<unsigned char>(220 * a)});
        DrawRectangleLinesEx(Rectangle{x, y, bw, bh}, 2,
                             Color{255, 220, 100, static_cast<unsigned char>(220 * a)});
        drawText(t.text, x + 12, y + 8, 14, Color{255, 235, 120, static_cast<unsigned char>(255 * a)}, false);
        if (!t.sub.empty()) {
            drawText(t.sub, x + 12, y + 30, 12, Color{210, 210, 210, static_cast<unsigned char>(220 * a)}, false);
        }
        y += bh + 8;
    }
}

void Game::drawDebug() {
    if (!showDebug_ || !hasWorld_) return;

    const float fs = 13;
    float y = 8;
    auto line = [&](const std::string& s) {
        drawText(s, 10, y, fs, WHITE, false);
        y += 17;
    };

    line("Minecraft 2D 1.0  " + std::to_string(GetFPS()) + " FPS");
    line("XYZ: " + std::to_string(body_.x) + " / " + std::to_string(body_.y));
    line("方块: " + std::to_string(static_cast<int>(std::floor(body_.x))) + ", " +
         std::to_string(static_cast<int>(std::floor(body_.y))));
    line("生命: " + std::to_string(health_) + "/20  饥饿: " + std::to_string(food_) +
         "/20  氧气: " + std::to_string(air_) + "/300");
    line(std::string("模式: ") + (creative_ ? "创造" : "生存") + "  飞行: " + (flying_ ? "是" : "否"));
    line("世界种子: " + std::to_string(world_.seed()));
    line("时间: " + std::to_string(worldTime_ % 24000) + " tick  光照: " + std::to_string(daylightFactor()));
    line("实体: " + std::to_string(entities_.size()) + "  远程玩家: " + std::to_string(remotes_.size()));
    line("待确认输入: " + std::to_string(pending_.size()));
    line("目标方块: " + (hoverValid_ ? blockName(world_.fg(hoverX_, hoverY_)) : "无"));
}

void Game::drawPlayerList() {
    if (!showPlayerList_ || !hasWorld_) return;

    const float w = static_cast<float>(GetScreenWidth());
    float pw = 420;
    float rowH = 28;
    int count = 1 + static_cast<int>(remotes_.size());
    Rectangle panel{w * 0.5f - pw * 0.5f, 50, pw, 70.0f + count * rowH};
    drawPanel(panel);
    drawTextCentered("在线玩家 (" + std::to_string(count) + ")", w * 0.5f, panel.y + 14, 20, WHITE);

    float y = panel.y + 52;
    drawText(myName_ + " (你)", panel.x + 24, y, 15, Color{255, 220, 120, 255});
    y += rowH;
    for (const auto& [id, r] : remotes_) {
        (void)id;
        drawText(r.name, panel.x + 24, y, 15, WHITE);
        y += rowH;
    }
}

void Game::drawPlaying() {
    if (!hasWorld_) {
        drawConnecting();
        return;
    }

    drawSky();
    drawTiles(true);
    drawTiles(false);
    drawTarget();
    drawRenderables();
    drawParticles();

    if (redFlash_ > 0) {
        DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                      Color{170, 20, 20, static_cast<unsigned char>(std::min(110.0f, redFlash_ * 90))});
    }

    if (showHud_) drawHud();
    drawChat();
    drawToasts();
    drawDebug();
    if (IsKeyDown(KEY_TAB)) drawPlayerList();

    if (overlay_ == Overlay::Inventory) drawInventory();
    else if (overlay_ == Overlay::Pause) drawPause();
    else if (overlay_ == Overlay::Death) drawDeath();
}

} // namespace

int main(int argc, char** argv) {
    Options opt;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        try {
            if (a == "--connect" && i + 1 < argc) opt.connect = argv[++i];
            else if (a == "--name" && i + 1 < argc) opt.name = argv[++i];
            else if (a == "--width" && i + 1 < argc) opt.width = std::stoi(argv[++i]);
            else if (a == "--height" && i + 1 < argc) opt.height = std::stoi(argv[++i]);
        } catch (...) {
            return 1;
        }
    }

    Game game;
    if (!game.init(opt)) return 1;
    game.run();
    game.shutdown();
    return 0;
}
