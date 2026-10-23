#include "common/net.h"

#include <algorithm>
#include <cstring>
#include <vector>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace mc {
namespace {

constexpr uint32_t MAX_MESSAGE_SIZE = 32u * 1024u * 1024u;
bool g_wsaReady = false;

SOCKET toSocket(std::uintptr_t value) { return static_cast<SOCKET>(value); }
std::uintptr_t fromSocket(SOCKET value) { return static_cast<std::uintptr_t>(value); }

void putU32LE(char* dst, uint32_t value) {
    dst[0] = static_cast<char>(value & 0xFF);
    dst[1] = static_cast<char>((value >> 8) & 0xFF);
    dst[2] = static_cast<char>((value >> 16) & 0xFF);
    dst[3] = static_cast<char>((value >> 24) & 0xFF);
}

uint32_t getU32LE(const char* src) {
    return static_cast<uint32_t>(static_cast<unsigned char>(src[0])) |
           (static_cast<uint32_t>(static_cast<unsigned char>(src[1])) << 8) |
           (static_cast<uint32_t>(static_cast<unsigned char>(src[2])) << 16) |
           (static_cast<uint32_t>(static_cast<unsigned char>(src[3])) << 24);
}

void configureSocket(SOCKET s) {
    BOOL noDelay = TRUE;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char*>(&noDelay), sizeof(noDelay));
    BOOL keepAlive = TRUE;
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, reinterpret_cast<const char*>(&keepAlive), sizeof(keepAlive));
    DWORD timeout = 3000;
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
}

} // namespace

std::string netErrorText(int code) {
    if (code == 0) code = WSAGetLastError();
    char* buffer = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, static_cast<DWORD>(code), 0,
                   reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string text = buffer ? buffer : "未知网络错误";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

bool netInit(std::string& error) {
    if (g_wsaReady) return true;
    WSADATA data{};
    int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        error = "Winsock 初始化失败: " + netErrorText(rc);
        return false;
    }
    g_wsaReady = true;
    return true;
}

void netShutdown() {
    if (g_wsaReady) {
        WSACleanup();
        g_wsaReady = false;
    }
}

NetSocket::NetSocket() = default;
NetSocket::NetSocket(std::uintptr_t socket) : sock_(socket) {}

NetSocket::~NetSocket() { close(); }

void NetSocket::close() {
    if (sock_ != kInvalid) {
        closesocket(toSocket(sock_));
        sock_ = kInvalid;
    }
}

void NetSocket::shutdownBoth() {
    if (sock_ != kInvalid) shutdown(toSocket(sock_), SD_BOTH);
}

bool NetSocket::connectTo(const std::string& host, int port, int timeoutMs, std::string& error) {
    close();
    addrinfo hints{};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    addrinfo* result = nullptr;
    std::string service = std::to_string(port);
    int rc = getaddrinfo(host.c_str(), service.c_str(), &hints, &result);
    if (rc != 0 || !result) {
        error = "无法解析服务器地址: " + host;
        if (result) freeaddrinfo(result);
        return false;
    }
    SOCKET s = socket(result->ai_family, result->ai_socktype, result->ai_protocol);
    if (s == INVALID_SOCKET) {
        error = "无法创建套接字: " + netErrorText();
        freeaddrinfo(result);
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    int conn = connect(s, result->ai_addr, static_cast<int>(result->ai_addrlen));
    freeaddrinfo(result);
    if (conn == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            error = "连接失败: " + netErrorText(err);
            closesocket(s);
            return false;
        }
        fd_set writeSet;
        FD_ZERO(&writeSet);
        FD_SET(s, &writeSet);
        fd_set errSet;
        FD_ZERO(&errSet);
        FD_SET(s, &errSet);
        timeval tv{};
        tv.tv_sec = timeoutMs / 1000;
        tv.tv_usec = (timeoutMs % 1000) * 1000;
        int selected = select(0, nullptr, &writeSet, &errSet, &tv);
        if (selected <= 0) {
            error = selected == 0 ? "连接超时" : ("连接失败: " + netErrorText());
            closesocket(s);
            return false;
        }
        int soError = 0;
        int len = sizeof(soError);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (soError != 0) {
            error = "连接失败: " + netErrorText(soError);
            closesocket(s);
            return false;
        }
    }
    u_long blocking = 0;
    ioctlsocket(s, FIONBIO, &blocking);
    configureSocket(s);
    sock_ = fromSocket(s);
    return true;
}

bool NetSocket::sendBytes(const void* data, size_t size, std::string& error) {
    const char* ptr = static_cast<const char*>(data);
    size_t left = size;
    while (left > 0) {
        int sent = send(toSocket(sock_), ptr, static_cast<int>(std::min<size_t>(left, 1 << 20)), 0);
        if (sent == SOCKET_ERROR) {
            error = "发送失败: " + netErrorText();
            return false;
        }
        if (sent == 0) {
            error = "连接已关闭";
            return false;
        }
        ptr += sent;
        left -= static_cast<size_t>(sent);
    }
    return true;
}

bool NetSocket::recvExact(void* data, size_t size, std::string& error) {
    char* ptr = static_cast<char*>(data);
    size_t left = size;
    while (left > 0) {
        int got = recv(toSocket(sock_), ptr, static_cast<int>(std::min<size_t>(left, 1 << 20)), 0);
        if (got == SOCKET_ERROR) {
            error = "接收失败: " + netErrorText();
            return false;
        }
        if (got == 0) {
            error = "服务器/客户端已断开连接";
            return false;
        }
        ptr += got;
        left -= static_cast<size_t>(got);
    }
    return true;
}

TcpListener::TcpListener() = default;
TcpListener::~TcpListener() { close(); }

void TcpListener::close() {
    if (sock_ != NetSocket::kInvalid) {
        closesocket(toSocket(sock_));
        sock_ = NetSocket::kInvalid;
    }
}

bool TcpListener::start(int port, std::string& error) {
    close();
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) {
        error = "无法创建监听套接字: " + netErrorText();
        return false;
    }
    BOOL reuse = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<u_short>(port));
    if (bind(s, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR) {
        error = "端口 " + std::to_string(port) + " 绑定失败: " + netErrorText();
        closesocket(s);
        return false;
    }
    if (listen(s, SOMAXCONN) == SOCKET_ERROR) {
        error = "监听失败: " + netErrorText();
        closesocket(s);
        return false;
    }
    u_long nonBlocking = 1;
    ioctlsocket(s, FIONBIO, &nonBlocking);
    sock_ = fromSocket(s);
    return true;
}

std::unique_ptr<NetSocket> TcpListener::acceptOne(std::string& error) {
    sockaddr_in clientAddress{};
    int length = sizeof(clientAddress);
    SOCKET client = accept(toSocket(sock_), reinterpret_cast<sockaddr*>(&clientAddress), &length);
    if (client == INVALID_SOCKET) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) error = "接受连接失败: " + netErrorText(err);
        return nullptr;
    }
    configureSocket(client);
    u_long blocking = 0;
    ioctlsocket(client, FIONBIO, &blocking);
    return std::make_unique<NetSocket>(fromSocket(client));
}

bool sendJson(NetSocket& socket, std::mutex& mutex, const json& message, std::string& error) {
    std::string payload = message.dump(-1, ' ', false, json::error_handler_t::replace);
    if (payload.size() > MAX_MESSAGE_SIZE) {
        error = "消息过大";
        return false;
    }
    std::vector<char> frame(payload.size() + 4);
    putU32LE(frame.data(), static_cast<uint32_t>(payload.size()));
    std::memcpy(frame.data() + 4, payload.data(), payload.size());
    std::lock_guard<std::mutex> lock(mutex);
    return socket.sendBytes(frame.data(), frame.size(), error);
}

bool recvJson(NetSocket& socket, json& message, std::string& error) {
    char header[4];
    if (!socket.recvExact(header, 4, error)) return false;
    uint32_t size = getU32LE(header);
    if (size == 0) {
        message = json::object();
        return true;
    }
    if (size > MAX_MESSAGE_SIZE) {
        error = "协议错误: 消息过大";
        return false;
    }
    std::string payload(size, '\0');
    if (!socket.recvExact(payload.data(), payload.size(), error)) return false;
    message = json::parse(payload, nullptr, false);
    if (message.is_discarded()) {
        error = "协议错误: JSON 解析失败";
        return false;
    }
    return true;
}

} // namespace mc
