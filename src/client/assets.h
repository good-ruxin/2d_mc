#pragma once

#include <raylib.h>

#include <filesystem>
#include <string>
#include <unordered_map>

namespace mc {

class Assets {
public:
    bool init(const std::filesystem::path& gameRoot, std::string& error);
    void unload();

    Texture2D& tex(const std::string& relativePathOrName);
    Font& font() { return font_; }
    bool hasFont() const { return hasFont_; }
    const std::filesystem::path& root() const { return root_; }
    std::string pathOf(const std::string& relativePathOrName) const;

private:
    std::filesystem::path root_;
    std::unordered_map<std::string, Texture2D> textures_;
    Texture2D fallback_{};
    Font font_{};
    bool hasFont_ = false;
};

} // namespace mc
