cmake_minimum_required(VERSION 3.20)
project(2d_mc LANGUAGES CXX)

# 1. 依然挂载 vcpkg 工具链（保留它以防万一需要安装别的库）
set(CMAKE_TOOLCHAIN_FILE "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# 解决中文乱码警告
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

# ==========================================
# 【核心修改】强行将 vcpkg_installed 加入查找路径
# ==========================================
list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")

# 2. 引入 vcpkg 装好的其他库
find_package(EnTT CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# 3. 引入本地相对路径下的 raylib 源码
add_subdirectory("raylib" raylib_build)

# 4. 生成可执行文件并链接
add_executable(2d_mc src/main.cpp)

target_link_libraries(2d_mc PRIVATE
    raylib
    EnTT::EnTT
    nlohmann_json::nlohmann_json
)