#pragma once

#include <filesystem>
#include <string>

namespace mc {

std::filesystem::path executableDirectory();

// Isolated Win32 process helpers. Keeping <windows.h> out of game code avoids
// collisions with raylib's Rectangle/CloseWindow/ShowCursor declarations.
bool launchProcessHidden(const std::filesystem::path& executable,
                         const std::string& arguments,
                         const std::filesystem::path& workingDirectory,
                         void** processHandle,
                         unsigned long& processId,
                         std::string& error);

bool waitForProcess(void* processHandle, int timeoutMs);
void terminateProcess(void* processHandle);
void closeProcessHandle(void* processHandle);

} // namespace mc
