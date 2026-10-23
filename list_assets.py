import os
import json

# 资源根目录
ASSETS_DIR = "assets"
# 生成的文件名
OUTPUT_JSON = "assets.json"

def generate_manifest():
    manifest = {"textures": {}}

    # 遍历 assets 文件夹
    for root, dirs, files in os.walk(ASSETS_DIR):
        for file in files:
            # 只处理 png 和 jpg，过滤掉 .mcmeta 这种动画元数据文件
            if file.lower().endswith(('.png', '.jpg', '.jpeg')):
                # 获取完整路径
                full_path = os.path.join(root, file)
                # 替换 Windows 的反斜杠为斜杠
                full_path = full_path.replace("\\", "/")
                
                # 计算相对路径 (比如 assets/block/dirt.png)
                # 提取分组名 (比如根目录下的第一层文件夹名，如 "block", "gui")
                rel_path = os.path.relpath(full_path, ASSETS_DIR).replace("\\", "/")
                parts = rel_path.split("/")
                
                if len(parts) > 1:
                    group_name = parts[0]  # 比如 "block" 或 "gui"
                    # 资源名字去掉后缀 (比如 "dirt")
                    asset_name = os.path.splitext(file)[0] 
                    
                    if group_name not in manifest["textures"]:
                        manifest["textures"][group_name] = {}
                        
                    manifest["textures"][group_name][asset_name] = full_path

    # 写入 JSON 文件
    with open(OUTPUT_JSON, "w", encoding="utf-8") as f:
        json.dump(manifest, f, indent=2, ensure_ascii=False)
    print(f"成功生成 {OUTPUT_JSON}，共有 {sum(len(v) for v in manifest['textures'].values())} 个纹理！")

if __name__ == "__main__":
    generate_manifest()