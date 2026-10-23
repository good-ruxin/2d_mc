cmake_minimum_required(VERSION 3.20)
project(Minecraft2D LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options("$<$<CXX_COMPILER_ID:MSVC>:/utf-8>")

list(APPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vcpkg_installed/x64-windows")

find_package(nlohmann_json CONFIG REQUIRED)

# raylib is vendored in the repository.
add_subdirectory("raylib" raylib_build)

set(MC2D_COMMON
    src/common/blocks.cpp
    src/common/world.cpp
    src/common/physics.cpp
    src/common/net.cpp
)

# Both executables are placed directly in ./bin so the launcher scripts can
# start the two processes without knowing the CMake configuration name.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin")
foreach(CFG Debug Release RelWithDebInfo MinSizeRel)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${CFG} "${CMAKE_SOURCE_DIR}/bin")
endforeach()

add_executable(mc2d_server
    src/server/main.cpp
    ${MC2D_COMMON}
)
target_include_directories(mc2d_server PRIVATE src)
target_compile_definitions(mc2d_server PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mc2d_server PRIVATE nlohmann_json::nlohmann_json ws2_32)

add_executable(mc2d_client
    src/client/main.cpp
    src/client/assets.cpp
    src/client/platform_win.cpp
    ${MC2D_COMMON}
)
target_include_directories(mc2d_client PRIVATE src)
target_compile_definitions(mc2d_client PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(mc2d_client PRIVATE
    raylib
    nlohmann_json::nlohmann_json
    ws2_32
)

if (MSVC)
    target_compile_options(mc2d_server PRIVATE /W3)
    target_compile_options(mc2d_client PRIVATE /W3)
endif()

# Convenience target: `cmake --build build --target mc2d`
add_custom_target(mc2d DEPENDS mc2d_server mc2d_client)
