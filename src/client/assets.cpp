#include "client/assets.h"
#include "client/ui_chars.h"

#include <algorithm>
#include <set>
#include <vector>

namespace mc {
namespace {

bool fileExists(const std::filesystem::path& p) {
    std::error_code ec;
    return std::filesystem::is_regular_file(p, ec);
}

void collectUtf8(const std::string& text, std::vector<int>& out) {
    for (size_t i = 0; i < text.size();) {
        unsigned char c = static_cast<unsigned char>(text[i]);
        int cp = 0;
        int len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > text.size()) break;
        for (int j = 1; j < len; ++j) cp = (cp << 6) | (text[i + j] & 0x3F);
        out.push_back(cp);
        i += len;
    }
}


} // namespace

std::string Assets::pathOf(const std::string& relativePathOrName) const {
    std::string name = relativePathOrName;
    const bool hasExt =
        name.size() >= 4 &&
        (name.substr(name.size() - 4) == ".png" ||
         name.substr(name.size() - 4) == ".jpg" ||
         name.substr(name.size() - 4) == ".ttf" ||
         name.substr(name.size() - 4) == ".ttc");
    if (!hasExt) name += ".png";
    return (root_ / "assets" / name).string();
}

bool Assets::init(const std::filesystem::path& gameRoot, std::string& error) {
    root_ = gameRoot;

    Image fallbackImage = GenImageColor(16, 16, MAGENTA);
    fallback_ = LoadTextureFromImage(fallbackImage);
    UnloadImage(fallbackImage);
    if (fallback_.id != 0) SetTextureFilter(fallback_, TEXTURE_FILTER_POINT);

    std::vector<int> codepoints;
    for (int c = 32; c < 127; ++c) codepoints.push_back(c);
    collectUtf8(kUiGlyphText, codepoints);
    std::sort(codepoints.begin(), codepoints.end());
    codepoints.erase(std::unique(codepoints.begin(), codepoints.end()), codepoints.end());

    const std::vector<std::string> fonts = {
        "C:/Windows/Fonts/simhei.ttf",
        "C:/Windows/Fonts/msyh.ttc",
        "C:/Windows/Fonts/msyhbd.ttc",
        "C:/Windows/Fonts/segoeui.ttf",
    };

    for (const auto& f : fonts) {
        if (!fileExists(f)) continue;
        font_ = LoadFontEx(f.c_str(), 48, codepoints.data(), static_cast<int>(codepoints.size()));
        if (font_.texture.id != 0 && font_.glyphCount > 0) {
            int missing = 0;
            for (int cp : codepoints) {
                if (cp > 127 && GetGlyphIndex(font_, cp) == 0) {
                    ++missing;
                }
            }
            TraceLog(LOG_INFO, "FONT: %s glyphCount=%d codepoints=%d missingCJK=%d",
                     f.c_str(), font_.glyphCount, (int)codepoints.size(), missing);
            if (missing == 0) {
                hasFont_ = true;
                SetTextureFilter(font_.texture, TEXTURE_FILTER_BILINEAR);
                break;
            }
            UnloadFont(font_);
            font_ = Font{};
        }
    }
    if (!hasFont_) {
        font_ = GetFontDefault();
        error = "无法加载中文字体，将使用内置字体";
    }
    if (fallback_.id == 0) {
        error = "无法创建占位纹理";
        return false;
    }
    return true;
}

Texture2D& Assets::tex(const std::string& relativePathOrName) {
    const std::string path = pathOf(relativePathOrName);
    auto it = textures_.find(path);
    if (it != textures_.end()) return it->second;

    Texture2D t{};
    if (fileExists(path)) {
        t = LoadTexture(path.c_str());
        if (t.id != 0) SetTextureFilter(t, TEXTURE_FILTER_POINT);
    }
    if (t.id == 0) t = fallback_;

    auto result = textures_.emplace(path, t);
    return result.first->second;
}

void Assets::unload() {
    for (auto& [path, t] : textures_) {
        (void)path;
        if (t.id != 0 && t.id != fallback_.id) UnloadTexture(t);
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
