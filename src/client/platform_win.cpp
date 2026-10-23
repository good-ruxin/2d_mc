#include "client/platform_win.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mc {
namespace {

std::wstring utf8ToWide(const std::string& text) {
    if (text.empty()) return {};
    int length = MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, nullptr, 0);
    std::wstring result(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.c_str(), -1, result.data(), length);
    if (!result.empty() && result.back() == L'\0') result.pop_back();
    return result;
}

std::string windowsErrorText(DWORD code) {
    char* buffer = nullptr;
    FormatMessageA(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, code, 0, reinterpret_cast<LPSTR>(&buffer), 0, nullptr);
    std::string text = buffer ? buffer : "未知 Windows 错误";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ')) text.pop_back();
    return text;
}

} // namespace

std::filesystem::path executableDirectory() {
    wchar_t buffer[MAX_PATH * 4]{};
    DWORD size = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    if (size == 0) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(buffer, size)).parent_path();
}

bool launchProcessHidden(const std::filesystem::path& executable,
                         const std::string& arguments,
                         const std::filesystem::path& workingDirectory,
                         void** processHandle,
                         unsigned long& processId,
                         std::string& error) {
    if (!processHandle) {
        error = "无效的进程句柄指针";
        return false;
    }
    *processHandle = nullptr;
    std::wstring command = L"\"" + executable.wstring() + L"\" " + utf8ToWide(arguments);
    std::wstring cwd = workingDirectory.wstring();
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    std::vector<wchar_t> commandBuffer(command.begin(), command.end());
    commandBuffer.push_back(L'\0');
    BOOL ok = CreateProcessW(nullptr, commandBuffer.data(), nullptr, nullptr, FALSE,
                             CREATE_NO_WINDOW, nullptr, cwd.c_str(), &startup, &process);
    if (!ok) {
        DWORD code = GetLastError();
        error = "无法启动进程: " + windowsErrorText(code);
        return false;
    }
    CloseHandle(process.hThread);
    *processHandle = static_cast<void*>(process.hProcess);
    processId = static_cast<unsigned long>(process.dwProcessId);
    return true;
}

bool waitForProcess(void* processHandle, int timeoutMs) {
    if (!processHandle) return true;
    DWORD result = WaitForSingleObject(static_cast<HANDLE>(processHandle), static_cast<DWORD>(timeoutMs));
    return result == WAIT_OBJECT_0;
}

void terminateProcess(void* processHandle) {
    if (processHandle) TerminateProcess(static_cast<HANDLE>(processHandle), 0);
}

void closeProcessHandle(void* processHandle) {
    if (processHandle) CloseHandle(static_cast<HANDLE>(processHandle));
}

} // namespace mc
