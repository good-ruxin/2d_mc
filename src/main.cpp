//
// Created by win on 2026/10/17.
//

#include <iostream>

//json

#include <nlohmann/json.hpp>
using json = nlohmann::json;

//管理所有实体与组件 EnTT：实体组件系统（ECS）
#include <entt/entt.hpp>
//游戏框架与图形渲染
#include <raylib.h>
//Dear ImGui：即时模式 GUI
#include "imgui.h"

int main() {
    InitWindow(800, 450, "raylib window");
    SetTargetFPS(60);
    while (!WindowShouldClose()) {
        BeginDrawing();
        ClearBackground(RAYWHITE);
        DrawText("Hello raylib", 190, 200, 20, LIGHTGRAY);
        EndDrawing();
    }
    CloseWindow();
    return 0;
}