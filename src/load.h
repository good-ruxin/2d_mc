#include <iostream>
#include <fstream>
#include <string>
#include <unordered_map>
#include <raylib.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

class AssetManager {
private:
    // 保存所有纹理：Key = "分组/名字" (如 "gui/title")
    std::unordered_map<std::string, Texture2D> textures;

public:
    // 从 JSON 加载所有资源
    void LoadFromJson(const std::string& jsonPath) {
        std::ifstream file(jsonPath);
        if (!file.is_open()) {
            std::cout << "[错误] 找不到配置文件: " << jsonPath << std::endl;
            return;
        }

        json j;
        file >> j;

        if (!j.contains("textures")) return;

        // 遍历 JSON
        for (auto& [groupName, groupObj] : j["textures"].items()) {
            for (auto& [assetName, pathValue] : groupObj.items()) {
                std::string path = pathValue.get<std::string>();
                std::string key = groupName + "/" + assetName;

                Texture2D tex = LoadTexture(path.c_str());
                if (tex.id != 0) {
                    textures[key] = tex;
                    std::cout << "[加载成功] " << key << " -> " << path << std::endl;
                } else {
                    std::cout << "[加载失败] " << key << " -> " << path << std::endl;
                }
            }
        }
    }

    // 获取纹理
    Texture2D Get(const std::string& group, const std::string& name) {
        std::string key = group + "/" + name;
        if (textures.find(key) != textures.end()) {
            return textures[key];
        }
        std::cout << "[警告] 找不到纹理: " << key << std::endl;
        return {0}; // 返回空纹理
    }

    // 统一卸载
    void UnloadAll() {
        for (auto& pair : textures) {
            UnloadTexture(pair.second);
        }
        textures.clear();
    }
};

// 全局实例
AssetManager Assets;