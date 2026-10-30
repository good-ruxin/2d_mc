#pragma once

#include <filesystem>
#include <string>

namespace mc {

std::filesystem::path getExecutableDirectory();

bool launchServerProcess(const std::filesystem::path& exe,
                         const std::filesystem::path& cwd,
                         int port,
                         void*& processHandle,
                         std::string& error);

void stopServerProcess(void* processHandle);

} // namespace mc
