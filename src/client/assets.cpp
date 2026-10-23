#include "client/assets.h"

#include "client/ui_glyphs.h"

#include <algorithm>
#include <fstream>
#include <vector>

namespace mc {
namespace {

bool fileExists(const std::filesystem::path& path) {
    std::error_code ec;
    return std::filesystem::is_regular_file(path, ec);
}

} // namespace

std::string Assets::pathOf(const std::string& relativePathOrName) const {
    std::string name = relativePathOrName;
    bool hasExtension = name.size() >= 4 &&
                        (name.substr(name.size() - 4) == ".png" ||
                         name.substr(name.size() - 4) == ".jpg" ||
                         name.substr(name.size() - 4) == ".ttf" ||
                         name.substr(name.size() - 4) == ".ttc");
    if (!hasExtension) name += ".png";
    return (root_ / "assets" / name).string();
}

bool Assets::init(const std::filesystem::path& gameRoot, std::string& error) {
    root_ = gameRoot;

    Image fallbackImage = GenImageChecked(16, 16, 8, 8, MAGENTA, BLACK);
    fallback_ = LoadTextureFromImage(fallbackImage);
    UnloadImage(fallbackImage);
    SetTextureFilter(fallback_, TEXTURE_FILTER_POINT);

    std::vector<int> codepoints;
    for (int code = 32; code < 127; ++code) codepoints.push_back(code);
    for (int i = 0; i < kUiGlyphCount; ++i) codepoints.push_back(kUiGlyphs[i]);
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());

    const std::vector<std::string> fontCandidates = {
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/segoeui.ttf",
    };
    for (const auto& candidate : fontCandidates) {
        if (!fileExists(candidate)) continue;
        Font loaded = LoadFontEx(candidate.c_str(), 48, codepoints.data(), static_cast<int>(codepoints.size()));
        bool hasChinese = false;
        if (loaded.texture.id != 0 && loaded.glyphCount > 0) {
            for (int i = 0; i < loaded.glyphCount; ++i) {
                if (loaded.glyphs[i].value == 0x4E2D || loaded.glyphs[i].value == 0x6587) {
                    hasChinese = true;
                    break;
                }
            }
        }
        if (hasChinese) {
            font_ = loaded;
            hasFont_ = true;
            SetTextureFilter(font_.texture, TEXTURE_FILTER_BILINEAR);
            TraceLog(LOG_INFO, "UI font: %s (%d glyphs, atlas %dx%d)", candidate.c_str(), font_.glyphCount,
                     font_.texture.width, font_.texture.height);
            break;
        }
        if (loaded.texture.id != 0 && loaded.texture.id != GetFontDefault().texture.id) UnloadFont(loaded);
    }
    if (!hasFont_) {
        font_ = GetFontDefault();
        error = "无法加载中文字体，将使用内置英文字体";
    }
    if (fallback_.id == 0) {
        error = "无法创建占位纹理";
        return false;
    }
    return true;
}

Texture2D& Assets::tex(const std::string& relativePathOrName) {
    std::string path = pathOf(relativePathOrName);
    auto it = textures_.find(path);
    if (it != textures_.end()) return it->second;

    Texture2D texture{};
    if (fileExists(path)) {
        texture = LoadTexture(path.c_str());
        if (texture.id != 0) SetTextureFilter(texture, TEXTURE_FILTER_POINT);
    }
    if (texture.id == 0) {
        TraceLog(LOG_WARNING, "Missing texture: %s", path.c_str());
        texture = fallback_;
    }
    auto [inserted, _] = textures_.emplace(path, texture);
    return inserted->second;
}

void Assets::unload() {
    for (auto& [path, texture] : textures_) {
        if (texture.id != 0 && texture.id != fallback_.id) UnloadTexture(texture);
    }
    textures_.clear();
    if (fallback_.id != 0) {
        UnloadTexture(fallback_);
        fallback_.id = 0;
    }
    if (hasFont_) {
        UnloadFont(font_);
        hasFont_ = false;
    }
}

} // namespace mc
