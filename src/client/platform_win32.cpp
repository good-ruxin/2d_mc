#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <vector>

#include "client/platform_win32.h"

namespace mc {

std::filesystem::path getExecutableDirectory() {
    wchar_t buf[MAX_PATH * 4]{};
    DWORD n = GetModuleFileNameW(nullptr, buf, static_cast<DWORD>(sizeof(buf) / sizeof(buf[0])));
    if (n == 0) return std::filesystem::current_path();
    return std::filesystem::path(std::wstring(buf, n)).parent_path();
}

bool launchServerProcess(const std::filesystem::path& exe,
                         const std::filesystem::path& cwd,
                         int port,
                         void*& processHandle,
                         std::string& error) {
    std::wstring cmd = L"\"" + exe.wstring() + L"\" --port " + std::to_wstring(port);
    std::wstring cwdW = cwd.wstring();

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> cmdBuf(cmd.begin(), cmd.end());
    cmdBuf.push_back(L'\0');

    BOOL ok = CreateProcessW(
        nullptr,
        cmdBuf.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        cwdW.c_str(),
        &si,
        &pi
    );

    if (!ok) {
        error = "无法启动本地服务端，错误码 " + std::to_string(GetLastError());
        return false;
    }

    CloseHandle(pi.hThread);
    processHandle = pi.hProcess;
    return true;
}

void stopServerProcess(void* processHandle) {
    if (!processHandle) return;
    HANDLE h = static_cast<HANDLE>(processHandle);
    DWORD r = WaitForSingleObject(h, 1500);
    if (r == WAIT_TIMEOUT) TerminateProcess(h, 0);
    CloseHandle(h);
}

} // namespace mc
