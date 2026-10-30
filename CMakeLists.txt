cmake_minimum_required(VERSION 3.20)
project(Minecraft2D LANGUAGES CXX)

set(CMAKE_TOOLCHAIN_FILE "C:/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools/VC/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")

find_package(nlohmann_json CONFIG REQUIRED)

add_subdirectory("raylib" raylib_build)

set(MC2D_COMMON
    src/common/blocks.cpp
    src/common/world.cpp
    src/common/physics.cpp
    src/common/net.cpp
)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
foreach(CFG Debug Release RelWithDebInfo MinSizeRel)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

add_executable(mc2d_server
    src/server/main.cpp
    ${MC2D_COMMON}
)
target_include_directories(mc2d_server PRIVATE src)
target_link_libraries(mc2d_server PRIVATE nlohmann_json::nlohmann_json ws2_32)

add_executable(mc2d_client
    src/client/main.cpp
    src/client/assets.cpp
    src/client/platform_win32.cpp
    ${MC2D_COMMON}
)
target_include_directories(mc2d_client PRIVATE src)
target_link_libraries(mc2d_client PRIVATE
    raylib
    nlohmann_json::nlohmann_json
    ws2_32
)

if (MSVC)
    target_compile_options(mc2d_server PRIVATE /W3)
    target_compile_options(mc2d_client PRIVATE /W3)
endif()

add_custom_target(mc2d DEPENDS mc2d_server mc2d_client)
