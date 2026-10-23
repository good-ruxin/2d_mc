#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>

namespace mc {

using json = nlohmann::json;

bool netInit(std::string& error);
void netShutdown();
std::string netErrorText(int code = 0);

// The platform socket handle is kept opaque here so client code can include
// this header without pulling in <winsock2.h>/<windows.h> (which conflict with
// raylib's Rectangle/ShowCursor/CloseWindow names).
class NetSocket {
public:
    NetSocket();
    explicit NetSocket(std::uintptr_t socket);
    ~NetSocket();

    NetSocket(const NetSocket&) = delete;
    NetSocket& operator=(const NetSocket&) = delete;

    bool connectTo(const std::string& host, int port, int timeoutMs, std::string& error);
    void close();
    void shutdownBoth();
    bool valid() const { return sock_ != kInvalid; }
    std::uintptr_t raw() const { return sock_; }

    bool sendBytes(const void* data, size_t size, std::string& error);
    bool recvExact(void* data, size_t size, std::string& error);

    static constexpr std::uintptr_t kInvalid = static_cast<std::uintptr_t>(~0ull);

private:
    std::uintptr_t sock_ = kInvalid;
};

class TcpListener {
public:
    TcpListener();
    ~TcpListener();
    bool start(int port, std::string& error);
    std::unique_ptr<NetSocket> acceptOne(std::string& error);
    void close();
    bool valid() const { return sock_ != NetSocket::kInvalid; }

private:
    std::uintptr_t sock_ = NetSocket::kInvalid;
};

bool sendJson(NetSocket& socket, std::mutex& mutex, const json& message, std::string& error);
bool recvJson(NetSocket& socket, json& message, std::string& error);

} // namespace mc
