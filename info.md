# Minecraft 2D 源码说明

本文是 `src/` 下 C++ 代码的源码导航手册，按文件说明类、结构体、枚举、持久成员、文件级常量、函数及其职责。字段按名称逐项列出或按同一职责分组说明；函数参数和临时变量的含义结合所属函数的数据流描述。位置以源文件和类型/函数名为准；行号会随改动变化，建议用表中符号名在编辑器中定位。

## 项目结构与运行链路

| 位置 | 内容 |
|---|---|
| [`src/client/main.cpp`](./src/client/main.cpp) | raylib 游戏循环、输入、客户端预测、网络消息处理和所有游戏界面；容器背景及槽位绘制也在这里。 |
| [`src/client/assets.h`](./src/client/assets.h)、[`src/client/assets.cpp`](./src/client/assets.cpp) | 从 `assets/` 加载和缓存纹理、字体，提供资源路径解析和卸载。 |
| [`src/client/platform_win.h`](./src/client/platform_win.h)、[`src/client/platform_win.cpp`](./src/client/platform_win.cpp)、[`src/client/platform_win32.h`](./src/client/platform_win32.h)、[`src/client/platform_win32.cpp`](./src/client/platform_win32.cpp) | Windows 平台辅助功能，包括启动/停止本地服务端进程等平台实现。 |
| [`src/client/ui_chars.h`](./src/client/ui_chars.h) | `kUiGlyphText` 是字体初始化所需的中文/符号字形清单，由工具扫描生成。 |
| [`src/server/main.cpp`](./src/server/main.cpp) | 权威世界模拟、客户端连接、存档、玩家/实体、背包和容器交互处理。 |
| [`src/common/blocks.h`](./src/common/blocks.h)、[`src/common/blocks.cpp`](./src/common/blocks.cpp) | 方块、物品、配方、熔炼、燃料、堆叠和工具数据及其查询/变更帮助函数。 |
| [`src/common/world.h`](./src/common/world.h)、[`src/common/world.cpp`](./src/common/world.cpp) | 前景/背景世界网格、生成、碰撞查询、压缩传输和存档。 |
| [`src/common/physics.h`](./src/common/physics.h)、[`src/common/physics.cpp`](./src/common/physics.cpp) | 客户端预测与服务端共用的玩家移动物理。 |
| [`src/common/net.h`](./src/common/net.h)、[`src/common/net.cpp`](./src/common/net.cpp) | Winsock TCP 封装及长度前缀 JSON 消息收发。 |
| [`CMakeLists.txt`](./CMakeLists.txt) | `mc2d_client`、`mc2d_server` 两个 C++17 程序的构建定义。 |

基本数据流：客户端把鼠标/键盘意图编码为 JSON 发给服务端；服务端校验连接、容器和槽位后修改唯一权威状态，再发回 `inventory` / `container` 快照；客户端只负责把快照绘制到 GUI 贴图上。

## 容器界面与网络槽位

### GUI 资源与布局

容器界面使用 `assets/gui/container/` 内的原版背景纹理：

| 容器类型（协议 `gui`） | 贴图 | 背景源区域（像素） |
|---|---|---|
| `inventory` | [`inventory.png`](./assets/gui/container/inventory.png) | 176 × 166 |
| `crafting_table` | [`crafting_table.png`](./assets/gui/container/crafting_table.png) | 176 × 166 |
| `furnace` | [`furnace.png`](./assets/gui/container/furnace.png) | 176 × 166 |
| `chest` | [`generic_54.png`](./assets/gui/container/generic_54.png) | 176 × 222 |

客户端 `Game::buildInventoryLayout` 将上述 176 像素布局按窗口尺寸缩放，并生成鼠标命中矩形；`Game::drawInventory` 先画背景，再叠加服务端快照中的物品、数量、光标提示和熔炉进度。熔炉进度使用 `assets/gui/sprites/container/furnace/lit_progress.png` 与 `burn_progress.png`。

### 客户端/服务端共享槽位编号

编号定义在 [`src/server/main.cpp`](./src/server/main.cpp) 文件开头，客户端使用相同编号发请求：

| 编号 | 含义 | GUI 中的位置 |
|---|---|---|
| `0..35` | 玩家背包（`0..8` 为快捷栏，`9..35` 为主背包） | 所有容器底部的玩家物品栏 |
| `36..44` | 玩家合成网格 | 背包使用 `36..39` 的 2×2 区域；工作台使用完整 3×3 区域 |
| `45` | 合成输出 | 背包或工作台输出槽 |
| `46..48` | 熔炉输入、燃料、输出 | 仅熔炉有效 |
| `49..102` | 箱子 54 个槽 | `generic_54` 顶部 6×9 区域 |

左键、右键、Shift 快速移动、鼠标悬停后按 `1..9` 交换快捷栏都会发送 `slot_click` / `swap_hotbar` 请求。客户端不会自行决定配方、扣除材料或提交槽位变化。合成结果由服务器根据 `craftGrid` 重新计算；熔炼进度与产物由服务端 20 TPS 炉体更新生成。

## 客户端类型、成员状态与函数

### 文件级常量与小工具 — `src/client/main.cpp`

| 名称 | 位置 | 作用 |
|---|---|---|
| `INV_SIZE`、`HOTBAR_SIZE` | 文件开头匿名命名空间 | 玩家背包 36 格、快捷栏 9 格的客户端尺寸。 |
| `PROTOCOL_VERSION` | 同上 | 握手使用的客户端协议版本。 |
| `FIXED_DT` | 同上 | 客户端预测物理的固定步长，与服务器 tick 时长一致。 |
| `PI_F` | 同上 | 客户端天空、昼夜等绘制中的圆周常量。 |
| `scaleColor` | 匿名命名空间 | 按光照系数缩放 RGB，保留 alpha。 |
| `exeDirectory`、`looksLikeRoot`、`findRoot` | 匿名命名空间 | 获取程序目录、识别资源根目录并从工作目录/程序目录向上查找。 |
| `parseAddress` | 匿名命名空间 | 解析 `host` 或 `host:port`，并检查端口范围。 |
| `utf8Codepoint`、`popUtf8` | 匿名命名空间 | 将 Unicode 码点编码成 UTF-8，以及从输入字符串末尾删除一个 UTF-8 码点。 |

### 客户端结构体 — `src/client/main.cpp`

| 类型 / 位置 | 字段 | 作用 |
|---|---|---|
| `NetClient`，`Game` 前 | `socket`、`reader`、`connected`、`running` | TCP 套接字及独立读线程的生命周期状态。 |
| 同上 | `sendMutex`、`inboxMutex`、`reasonMutex` | 分别保护并发发送、消息队列和断开原因。 |
| 同上 | `inbox`、`reason` | 待主线程处理的 JSON 消息，以及最近网络错误原因。 |
| `RemotePlayer` | `id`、`name` | 远程玩家标识和显示名。 |
| 同上 | `tx`、`ty`、`x`、`y`、`vx`、`vy`、`walkPhase` | 目标/插值位置、速度和走路动画相位。 |
| 同上 | `facing`、`sneak`、`sprint`、`onGround`、`dead`、`creative`、`health` | 远程玩家动作和状态的渲染快照。 |
| `EntityView` | `id`、`type`、`item`、`count` | 实体身份、类别及掉落物内容。 |
| 同上 | `tx`、`ty`、`x`、`y`、`vx`、`vy`、`walkPhase` | 实体位置插值、速度和动画状态。 |
| 同上 | `facing`、`health`、`maxHealth`、`hurt`、`seen` | 朝向、生命、受击效果和本轮快照是否仍包含该实体。 |
| `Particle` | `x`、`y`、`vx`、`vy` | 粒子位置和速度。 |
| 同上 | `life`、`maxLife`、`size` | 剩余/总寿命和绘制尺寸。 |
| 同上 | `tex`、`src`、`tint`、`gravity` | 纹理、源矩形、颜色和是否受重力影响。 |
| `Toast` | `text`、`sub`、`life`、`maxLife` | 屏幕提示的主文案、次文案和淡出计时。 |
| `ChatLine` | `from`、`text`、`age` | 聊天来源、内容和显示时长。 |
| `PendingInput` | `input` | 尚未被服务端确认的玩家移动输入，用于预测回放。 |
| `ContainerSlotView` | `id` | 服务端协议槽位编号。 |
| 同上 | `bounds` | 与 GUI 背景槽位对应的屏幕命中矩形。 |
| `Screen` | `Title`、`Settings`、`Connecting`、`Playing`、`Disconnected` | 顶层应用画面状态。 |
| `Overlay` | `None`、`Pause`、`Inventory`、`Death` | 游戏画面上的模态层状态。 |
| `Options` | `connect`、`name`、`width`、`height` | 命令行传入的连接地址、玩家名和窗口尺寸。 |

### `Game` 成员变量 — `src/client/main.cpp` 中 `class Game`

| 字段 | 作用 |
|---|---|
| `opt_`、`assets_`、`net_`、`root_` | 启动选项、资源缓存、网络客户端和资源根目录。 |
| `screen_`、`overlay_`、`quit_` | 顶层画面、覆盖层以及主循环退出标志。 |
| `hasWorld_`、`world_`、`worldTime_` | 世界是否到齐、客户端世界副本和显示用世界时间。 |
| `myId_`、`myName_`、`body_` | 本地玩家网络 ID、显示名和本地预测物理状态。 |
| `inventory_`、`cursorStack_`、`selected_` | 最近服务端快照的玩家物品栏、鼠标携带物和快捷栏选择。 |
| `craftingGrid_`、`chestSlots_`、`furnaceSlots_`、`craftResult_` | 活动容器中的工作台/背包合成网格、箱子/熔炉槽及合成输出快照。 |
| `containerGui_`、`containerViewers_` | 当前容器类型和箱子查看人数提示。 |
| `furnaceBurnTime_`、`furnaceBurnTotal_`、`furnaceCookTime_`、`furnaceCookTotal_` | 服务端提供的炉火/冶炼进度，供 GUI 进度贴图显示。 |
| `containerScale_` | 176 像素 GUI 在当前窗口中的缩放倍数。 |
| `health_`、`food_`、`air_`、`dead_`、`creative_`、`flying_` | 本地玩家的生命、生存和模式显示状态。 |
| `seq_`、`ack_`、`pending_`、`tickAcc_` | 输入确认/预测队列和固定步长累计时间。 |
| `renderX_`、`renderY_`、`cameraX_`、`cameraY_`、`zoom_`、`zoomIndex_` | 渲染插值位置、相机位置及缩放。 |
| `walkPhase_`、`swing_`、`attackCooldown_`、`selectedNameTimer_`、`redFlash_`、`playTime_` | 动画、攻击冷却、快捷栏提示、受伤闪屏和游戏时间。 |
| `hoverX_`、`hoverY_`、`hoverValid_`、`hoveredEntity_` | 鼠标指向的世界方块/实体及命中状态。 |
| `mineX_`、`mineY_`、`mineActive_`、`mineProgress_`、`mineCooldown_`、`mineParticleTimer_` | 当前挖掘目标、进度、冷却和挖掘粒子计时。 |
| `facing_`、`desiredInput_`、`uiScale_` | 本地朝向、待发送输入和 HUD 缩放。 |
| `remotes_`、`entities_`、`particles_`、`toasts_`、`chats_` | 远程实体快照及短生命周期的渲染效果/文本队列。 |
| `chatOpen_`、`chatInput_`、`chatBlink_` | 聊天输入状态、文本和光标闪烁计时。 |
| `showHud_`、`showDebug_`、`showPlayerList_` | HUD、调试文本和玩家名单显示开关。 |
| `address_`、`lastAddress_`、`lastWasLocal_`、`disconnectReason_`、`connectingTimer_` | 当前/上次地址、重试模式、断开文案和连接界面计时。 |
| `ownsServer_`、`ownedServerProcess_` | 客户端是否负责终止本地服务端及其平台进程句柄。 |
| `spawnX_`、`spawnY_` | 服务端提供的出生位置。 |
| `titleButtons_`、`settingsBack_`、`discRetry_`、`discBack_`、`pauseResume_`、`pauseQuit_`、`deathRespawn_`、`deathTitle_` | 各画面按钮的屏幕区域。 |
| `invPanel_`、`containerSlots_` | 容器背景矩形和活动槽位 ID/命中矩形列表。 |
| `rng_` | 客户端视觉粒子等非权威随机效果的随机数生成器。 |

### `Game` 函数索引 — `src/client/main.cpp`

| 函数 | 职责 |
|---|---|
| `init`、`run`、`shutdown` | 初始化窗口/资源/网络；运行 update-draw 循环；关闭连接、进程及图形资源。 |
| `connectTo`、`launchLocalServer`、`stopOwnedServer`、`disconnectToTitle` | 建立远程/本地连接、管理本地服务端生命周期和返回标题画面时清理状态。 |
| `pollNet`、`processMessage` | 将网络线程收件箱交给主线程，并按 JSON 类型分派消息。 |
| `handleSelf`、`handleSnapshot`、`handleInventory`、`handleContainer`、`handleStats`、`handleBlock` | 分别合并本地权威状态、远程实体、玩家物品栏、完整容器快照、玩家统计和世界方块更新。 |
| `onWorldLoaded` | 世界数据到齐后初始化客户端世界/玩家显示状态。 |
| `update` | 顶层帧更新，处理全局热键、消息轮询和画面状态分派。 |
| `updateTitle`、`updateSettings`、`updateConnecting`、`updateDisconnected`、`updatePlaying` | 分别处理标题、设置、连接中、断开和游戏内画面输入/状态。 |
| `updateDesiredInput`、`fixedUpdate` | 采样玩家移动意图并按固定步长预测、发送输入。 |
| `updateCamera`、`updateSmoothing`、`updateHover` | 更新相机、远程对象插值和鼠标指向目标。 |
| `handleGameplayInput`、`handleInventoryInput`、`handlePauseInput`、`handleDeathInput`、`handleChatInput` | 处理游戏操作以及各覆盖界面的输入。容器点击只发槽位请求，不在本地改动库存。 |
| `startMining`、`findEntityAtMouse`、`currentHeldItem` | 更新挖掘动作、查找鼠标下实体和读取当前手持物品。 |
| `addChat`、`addToast` | 在客户端队列中加入聊天行和短暂通知。 |
| `spawnBlockParticles`、`spawnParticle`、`updateParticles` | 创建/更新方块破坏粒子和其他视觉粒子。 |
| `takeScreenshot` | 将当前 raylib 窗口画面保存到 `screenshots/`。 |
| `lightAt`、`daylightFactor` | 计算方块光照和昼夜亮度因子。 |
| `worldToScreen`、`screenToWorld`、`blockPixels` | 在世界格坐标与窗口像素坐标之间换算。 |
| `draw`、`drawTitle`、`drawSettings`、`drawConnecting`、`drawDisconnected`、`drawPlaying` | 顶层画面路由及各完整画面绘制。 |
| `drawSky`、`drawTile`、`drawTiles`、`drawTarget` | 绘制天空、单个前景/背景格、视口方块层和鼠标选中目标。 |
| `drawPig`、`drawZombie`、`drawItemEntity`、`drawRenderables`、`drawHumanoid` | 绘制生物/掉落物以及排序后的动态实体。 |
| `drawParticles`、`drawHud`、`drawInventory`、`drawPause`、`drawDeath`、`drawChat`、`drawToasts`、`drawDebug`、`drawPlayerList` | 绘制粒子、HUD、容器 GUI、暂停/死亡、聊天和辅助信息层。 |
| `buildInventoryLayout` | 将当前容器的原版纹理槽位映射成屏幕矩形及服务端槽号。 |
| `drawMenuDirt`、`drawPanel`、`drawButton`、`drawTooltip` | 通用菜单背景、面板、按钮和物品悬停提示。 |
| `drawText`、`drawTextCentered`、`measureText` | 绘制/测量带字体的文本。 |
| `drawItemIcon`、`drawItemStack` | 绘制物品材质以及带数量角标的物品堆。 |

文件末尾 `main(argc, argv)` 解析 `--connect`、`--name`、`--width`、`--height`，然后构造 `Game` 并运行主循环。

### 客户端资源与 Windows 平台辅助模块

| 类型/函数 | 位置和作用 |
|---|---|
| `Assets` | `src/client/assets.h`：纹理路径到 `Texture2D` 的缓存。成员 `root_` 是资源根目录，`textures_` 是已加载纹理，`fallback_` 是缺失资源占位贴图，`font_`/`hasFont_` 保存字体及是否成功加载标志。 |
| `Assets::init` | `assets.cpp`：创建占位纹理、加载包含 UI 字符集的系统字体并设定资源根目录。 |
| `Assets::pathOf` | 为相对资源名补扩展名并组合 `assets/` 根路径。 |
| `Assets::tex` | 查缓存或从磁盘加载纹理；失败时返回占位纹理。 |
| `Assets::unload` | 卸载缓存纹理、占位纹理和自定义字体。 |
| `Assets::font`、`hasFont`、`root` | 读取字体、字体有效标志和资源根目录。 |
| `Assets::` 字段 `root_`,`textures_`,`fallback_`,`font_`,`hasFont_` | 分别是资源目录、纹理缓存、占位纹理、字体对象及字体加载状态。 |
| `executableDirectory` | `platform_win.h/.cpp`：取得当前可执行文件所在目录。 |
| `launchProcessHidden` | `platform_win.h/.cpp`：隐藏启动指定 Windows 子进程，返回句柄、PID 和明确错误信息。 |
| `waitForProcess`、`terminateProcess`、`closeProcessHandle` | `platform_win.h/.cpp`：等待子进程退出、终止进程和关闭进程句柄。 |
| `utf8ToWide`、`windowsErrorText` | `platform_win.cpp` 匿名命名空间：命令行 UTF-8 转 Windows UTF-16，以及格式化系统错误。 |
| `getExecutableDirectory` | `platform_win32.h/.cpp`：当前被客户端调用的平台封装，定位程序目录。 |
| `launchServerProcess`、`stopServerProcess` | `platform_win32.h/.cpp`：以隐藏窗口启动服务端并记录句柄；退出时等待或终止它。 |

### 客户端网络与辅助函数 — `src/client/main.cpp`

| 类型/函数 | 位置 | 作用 |
|---|---|---|
| `NetClient` 析构函数 | `main.cpp`，`struct NetClient` | 调用 `close`，确保读线程和套接字退出。 |
| `NetClient::connectTo(host, port, error)` | 同上 | 建立连接并启动接收线程；线程把 JSON 放入 `inbox`，断线时放入 `__disconnect` 事件并更新 `reason`。 |
| `NetClient::send(msg)` | 同上 | 通过公共 `sendJson` 发送客户端请求；失败时标记断开。 |
| `NetClient::poll(out)` | 同上 | 从线程安全消息队列取出一条消息交给游戏主线程。 |
| `NetClient::getReason()` | 同上 | 线程安全地读取最近一次网络错误文本。 |
| `NetClient::close()` | 同上 | 停止读循环、关闭双向通信、join 读线程并清空收件箱。 |
| `scaleColor(c, b)` | 文件匿名命名空间 | 按亮度系数缩放 RGB 通道，用于方块/场景光照着色。 |
| `exeDirectory()`、`looksLikeRoot(path)`、`findRoot()` | 文件匿名命名空间 | 定位可执行文件和资源根目录；`looksLikeRoot` 用资源/构建标志判断候选路径是否为项目根。 |
| `parseAddress(input, host, port)` | 文件匿名命名空间 | 解析服务器地址及可选端口，校验格式后输出 host/port。 |
| `utf8Codepoint(cp)`、`popUtf8(text)` | 文件匿名命名空间 | 聊天输入的 Unicode 码点 UTF-8 编码，以及按完整码点删除末尾字符。 |
| `main(argc, argv)` | 文件末尾 | 读取连接地址、玩家名和窗口尺寸参数，初始化 `Game`、运行主循环并清理资源。 |

`NetClient` 字段均为网络会话状态：`socket` 是 TCP 连接；`reader` 是接收线程；`connected`/`running` 控制连接和线程循环；`sendMutex` 防止并发写帧交错；`inboxMutex` 保护消息队列；`reasonMutex` 保护断开原因；`inbox` 是待读 JSON；`reason` 是最后一次网络错误。`Game` 字段详见上方“Game 成员变量”表。网络线程中的 `msg`/`err` 是单帧接收缓冲，局部锁对象仅保护临界区。

## 服务端类型、成员状态与函数

### 服务端文件级状态 — `src/server/main.cpp`

| 名称 | 作用 |
|---|---|
| `g_running` | 服务器主循环继续运行的原子标志。 |
| `g_logMutex` | 保护并发线程输出的控制台日志。 |
| `logLine` | 在互斥锁保护下输出一行日志。 |
| `consoleHandler` | 收到 Windows 控制台关闭事件时通知主循环退出。 |
| `INV_SIZE`、`HOTBAR_SIZE`、`SNAPSHOT_INTERVAL` | 玩家库存尺寸及快照广播 tick 间隔。 |
| `CRAFT_SLOT`、`CRAFT_GRID_SLOTS`、`CRAFT_OUT` | 合成网格起始号、网格容量和输出槽号。 |
| `FURNACE_INPUT`、`FURNACE_FUEL`、`FURNACE_OUTPUT`、`SMELT_TICKS` | 熔炉三个槽号以及单件冶炼时长。 |
| `CHEST_SLOT_BASE`、`CHEST_SIZE` | 箱子槽号起点和 54 槽容量。 |

### 服务端结构体 — `src/server/main.cpp`

| 类型 | 字段及作用 |
|---|---|
| `ContainerPos` | `x`,`y` 为世界格坐标；`gui` 表示容器类别。比较运算符用于 map/set 中按位置排序和判等。玩家背包/工作台用零位置键。 |
| `FurnaceState` | `input` 待熔炼物、`fuel` 燃料、`output` 产物；`burnTime`/`burnTotal` 是火焰剩余/总时长，`cookTime` 是当前进度。 |
| `Entity` | `id`/`type` 标识生物或掉落物；`x`,`y`,`vx`,`vy`,`onGround`,`facing` 描述运动；`health`,`maxHealth`,`hurt` 描述生命；`age`,`wander`,`dir`,`attackCooldown` 控制寿命、游荡和攻击；`item`,`count` 保存掉落物堆栈。 |
| `Player` | `id`,`name`,`body` 是玩家身份和物理；`inv`,`cursor`,`selected` 是权威库存、手持堆和快捷栏选择；`health`,`food`,`air`,`dead`,`creative`,`flying` 是生存状态；`ackSeq`,`pendingInput`,`hasInput` 是输入确认/消费状态；`attackCooldown`,`useCooldown`,`hurtCooldown` 是动作冷却；`foodTick`,`regenTick`,`starveTick`,`drownTick`,`lavaTick`,`lastHurtTick` 是生存计时；`spawnX`,`spawnY` 是重生点；`joined`,`joinedTick` 是入服状态；`craftGrid` 是玩家专属合成槽；`container`,`containerTick` 是正在打开的 GUI 和打开 tick。 |
| `Client` | `id`,`socket` 是连接 ID 和共享套接字；`reader` 读取网络帧；`sendMutex` 串行化发送；`alive`,`joined` 记录连接/握手状态。 |
| `Message` | `clientId` 标明消息来源；`payload` 是已经解码的 JSON 请求。 |

### `Server` 成员变量 — `src/server/main.cpp` 中 `class Server`

| 字段 | 作用 |
|---|---|
| `port_`、`worldPath_`、`requestedSeed_`、`fresh_` | 监听端口、世界文件、请求种子及是否忽略旧存档。 |
| `world_`、`listener_`、`playersPath_` | 权威世界、TCP 监听器和玩家数据文件位置。 |
| `savedPlayers_` | 从磁盘读取、等待按玩家名恢复的 JSON 数据。 |
| `time_`、`tick_` | 世界时间和服务器 20 TPS 计数器。 |
| `nextClientId_`、`nextEntityId_` | 分配连接/实体唯一 ID 的计数器。 |
| `clients_`、`players_`、`entities_` | 活动连接、玩家和世界实体索引/集合。 |
| `inboxMutex_`、`inbox_` | 保护网络读线程写入、主线程读取的消息队列。 |
| `consoleLine_`、`rng_` | 控制台输入缓冲及服务端权威随机源。 |
| `spawnX_`、`spawnY_` | 世界生成后选定的出生点。 |
| `lastSaveTick_`、`shutdownDone_` | 自动存档计时和幂等停服保护。 |
| `worldChangeDirty_` | 世界方块自上次保存后是否发生变化。 |
| `furnaces_`、`chests_` | 按方块位置索引的熔炉状态和箱子槽位。 |
| `containersDirty_`、`containersSaved_` | 容器数据修改/落盘状态。 |
| `furnaceVector` | 在处理熔炉点击时暂存三个槽位的兼容向量；提交时写回炉体。 |
| `chestViewers_` | 当前查看各箱子的玩家名集合。 |
| `containerSentSerial_` | 控制容器快照发送顺序的状态标志。 |

### `Server` 函数索引 — `src/server/main.cpp`

| 函数 | 职责 |
|---|---|
| `Server` 构造函数、`~Server` | 保存启动配置并确保析构时关闭连接。 |
| `init`、`run`、`shutdown` | 初始化网络/世界/存档；以 20 TPS 驱动服务端；幂等关闭 listener 和连接线程。 |
| `pollConsole`、`acceptClients`、`cleanupClients`、`drainMessages` | 读取控制台输入、接受 TCP 连接、回收断开客户端，并在主线程校验/分派客户端请求。 |
| `loadPlayerData`、`savePlayerData`、`applySavedPlayer` | 读取、持久化和恢复玩家库存/状态。 |
| `handleHello`、`sendDenied`、`sanitizeName` | 验证协议握手、拒绝无效客户端、清理玩家名并创建初始玩家状态。 |
| `findSpawn`、`findGroundY`、`seedPigs`、`maybeSpawnMonsters` | 选安全出生点、查询地面、布置猪并按昼夜生成敌对生物。 |
| `tick` | 服务端主 tick：处理玩家输入/冷却、生存、实体、熔炉、快照和存档。 |
| `storeFurnaceSlots`、`commitContainer` | 将临时炉槽同步回持久炉状态并标记容器数据变脏。 |
| `updateFurnaces` | 依据服务器配方、燃料和输出容量推进火焰/冶炼，产出物品。 |
| `updatePlayerCooldowns`、`updatePlayerEnvironment`、`updateHunger` | 玩家冷却、液体/危险环境和饥饿/恢复/伤害计时。 |
| `moveEntity`、`updateEntities`、`findEntity`、`spawnItem` | 更新实体碰撞运动和 AI、查找实体，以及生成掉落物。 |
| `handleBreak`、`dropContainerContents`、`handlePlace`、`handleAttack` | 权威校验方块挖掘/放置/攻击；破坏容器时溢出其中物品。 |
| `openContainer`、`containerStillValid`、`closeInventory` | 校验并打开/关闭 GUI；容器方块必须存在且玩家仍在服务端允许距离内。 |
| `reachable` | 计算玩家到目标方块的服务端交互距离。 |
| `returnGridToInventory` | 关闭背包/工作台时退还未合成网格内容。 |
| `containerJson`、`sendContainer`、`tickOpenContainers` | 生成/发送含玩家槽、合成槽、输出、炉体进度或箱子 54 格的权威快照，并周期刷新已打开容器。 |
| `recipeOutputForGrid`、`consumeGridForCraft`、`craftFromGrid`、`gridLimit` | 匹配当前网格、扣除原料、制作服务端输出以及选择 2×2/3×3 网格容量。 |
| `handleSlotClick` | 验证有效容器和槽号，执行左键/右键/Shift 交换或移动，再提交并广播新快照。 |
| `handleSwapHotbar` | 响应鼠标悬停槽位上的数字键快捷栏交换；服务端更新后发库存和容器快照。 |
| `handleCraft` | 处理快捷合成配方请求，并确认网格、材料、配方和背包容量。 |
| `handleDrop`、`handleEat` | 丢弃指定玩家槽物品或消费食物。 |
| `hurtPlayer`、`respawn` | 执行伤害/死亡状态及权威重生。 |
| `handleChat`、`executeCommand`、`handleConsoleCommand` | 处理聊天广播、玩家斜杠指令和服务器控制台指令。 |
| `loadContainers`、`saveContainers`、`saveWorld` | 读取/原子保存炉体与箱子数据，并保存世界/世界时间。 |
| `broadcastChat`、`sendSystem`、`sendInventory`、`sendSelfState`、`sendStats` | 发送聊天、系统提示、玩家库存、权威玩家状态和生存属性。 |
| `broadcastSnapshot`、`broadcastBlock`、`broadcast` | 广播玩家/实体快照、单个方块状态或任意 JSON 事件。 |
| `playerJson`、`entityJson` | 将玩家/实体只读状态转换为网络 JSON。 |
| `sendTo(Client&)`、`sendTo(playerId)` | 对指定连接发送 JSON 并处理发送错误。 |

文件末尾 `main(argc, argv)` 解析 `--port`、`--seed`、`--world`、`--fresh`，再依次初始化、运行并关闭服务器。

### 服务端文件级符号、辅助函数与参数 — `src/server/main.cpp`

| 名称 | 位置 | 作用/参数含义 |
|---|---|---|
| `logLine(text)` | 文件匿名命名空间 | 输出带服务端时间戳的日志；`text` 是日志正文。 |
| `consoleHandler(event)` | 文件匿名命名空间 | Windows 控制台关闭/中断回调，将 `g_running` 设为 false；`event` 是系统事件码。 |
| `g_running`、`g_logMutex` | 文件匿名命名空间 | 主循环开关和并发日志输出互斥量。 |
| `INV_SIZE`、`HOTBAR_SIZE`、`SNAPSHOT_INTERVAL` | 文件常量 | 玩家背包尺寸、快捷栏尺寸、世界快照广播间隔 tick 数。 |
| `CRAFT_SLOT`、`CRAFT_GRID_SLOTS`、`CRAFT_OUT` | 文件常量 | 合成网格编号起始值、网格最多槽数、合成输出槽编号。 |
| `FURNACE_INPUT`、`FURNACE_FUEL`、`FURNACE_OUTPUT`、`SMELT_TICKS` | 文件常量 | 熔炉输入/燃料/输出槽编号及每件物品冶炼 tick 数。 |
| `CHEST_SLOT_BASE`、`CHEST_SIZE` | 文件常量 | 箱子槽编号起点和箱子容量。 |
| `ContainerPos::operator<(other)` | `struct ContainerPos` | 以 y、x、GUI 类型建立稳定排序，供 map/set 作为键。 |
| `ContainerPos::operator==(other)` | 同上 | 比较世界位置和 GUI 类型，判断是否为同一个容器。 |
| `containersDirtySaved()` | `Server` | 返回容器数据是否没有待保存修改。 |
| `containerSlots(player, slot, ok)` | `Server` | 返回玩家背包向量；`ok` 只对 `0..INV_SIZE-1` 玩家物品槽置真。此小辅助不负责映射工作台/炉体/箱子槽。 |
| `makeFurnaceVector(furnace)` | `Server` 静态辅助函数 | 将炉体的 input/fuel/output 三个成员拷贝成点击处理使用的顺序向量。 |
| `storeFurnaceSlots(pos)` | `Server` | 将临时炉槽向量写回对应 `FurnaceState`。 |
| `commitContainer(player)` | `Server` | 槽位操作完成后提交炉体临时副本，并标记箱子/炉体数据需要保存。 |
| `entityWidth(entity)`、`entityHeight(entity)` | `Server` | 按实体类型返回碰撞箱宽/高；参数是要查询的世界实体。 |
| `withinReach(player, x, y, reach)` | `Server` | 检查玩家对世界格坐标的交互距离；`reach` 是允许半径。 |
| `intersectsPlayer(player, x, y)` | `Server` | 判断方块放置位置是否与玩家碰撞箱相交，防止把实体卡进方块。 |
| `containersPath()` | `Server` | 从世界文件路径推导容器存档文件路径。 |
| `stackJson(stack)`、`stackFromJson(value)` | `Server` | 物品堆与网络/存档 JSON 之间转换；读取函数校验对象、物品 ID 和数量，非法/空值规范为空堆。 |
| `gridJson(stacks)` | `Server` 静态辅助函数 | 将合成网格物品堆数组序列化为 JSON 数组。 |
| `main(argc, argv)` | 文件末尾 | 解析端口、种子、存档路径和 fresh 选项；创建 `Server` 并执行 init/run/shutdown。 |

容器路径相关核心参数和临时变量：

| 函数 | 参数/局部数据的职责 |
|---|---|
| `openContainer(player, gui, x, y)` | `player` 是请求方；`gui` 是客户端请求的容器类型；`x`,`y` 是世界容器坐标。函数用方块类型、距离和 `ContainerPos` 校验请求，验证后写入玩家的活动容器。 |
| `containerJson(player)` | `player` 决定库存/光标/合成网格；局部 `message` 是最终权威快照，`slots`/`grid`/`chest` 是序列化数组，`size`/`resultId`/`result` 用于计算当前合成输出，`it`/`furnace` 表示查找到的炉体状态。 |
| `recipeOutputForGrid(grid, gridSize)` | `grid` 是待匹配槽位，`gridSize` 是 2 或 3；局部 `need`/`have` 分别累计配方需求和网格物品数量，`valid` 标记材料是否超额或混入无关物品。 |
| `craftFromGrid(player, gridSize, all)` | `player` 提供权威网格和鼠标光标堆；`gridSize` 选择 2×2/3×3；`all` 表示 Shift 连续合成。局部 `recipeId`、`recipe`、`limit`、`rounds` 控制当前配方、输出堆叠上限和合成轮数。 |
| `handleSlotClick(player, slot, button, shift)` | `slot` 是协议槽号；`button` 是左/右键；`shift` 是快速移动标志。局部 `storage`/`index` 定位权威物品向量/索引，`target`/`cursor` 是正在交换的堆，`moveInto` 将堆叠尽量并入目标容器。 |
| `handleSwapHotbar(player, slot, hotbar)` | `slot` 是悬停的容器槽号，`hotbar` 是 0..8 快捷栏索引；本函数验证两者后在服务端交换或按光标堆行为移动物品。 |
| `updateFurnaces()` | 每个 tick 查看炉体的 `input`、`fuel`、`output`；局部配方/容量状态决定能否继续烧炼，`burnTime`/`cookTime` 达到条件后由服务端写入产物。 |
| `handlePlace(player, x, y)`、`handleBreak(player, x, y)` | `x`,`y` 是目标世界格。函数先做范围/距离/方块/物品校验，再改世界并广播方块变化；破坏容器时由 `dropContainerContents` 处理槽中物品。 |

`Player`/`Entity` 等状态结构体的每个成员字段见上一节的结构体表；服务器事件循环中的计数器、迭代器、解析结果等临时变量由其所属函数消费，不跨 tick 作为持久状态保存。

## 公用数据模型与函数

### 方块/物品/合成 — `src/common/blocks.h`、`src/common/blocks.cpp`

| 类型/函数 | 位置和作用 |
|---|---|
| `ToolType`、`BlockId`、`ContainerGui` | `blocks.h`：工具类别、稳定方块 ID、关联的容器 GUI 类型。 |
| `BlockDef` | `blocks.h`：静态方块定义字段依次为资源 ID/显示名/贴图、实体碰撞/遮光/可替换/透明裁剪/液体标志、硬度、工具/等级、光照、掉落物及数量区间、GUI 类型。 |
| `SmeltRecipe` | `blocks.h`：输入物品、产物 ID 和产物数量。 |
| `ItemDef` | `blocks.h`：物品 ID/显示名/贴图、最大堆叠、是否方块及方块 ID、工具属性、工具速度、攻击力、食物值。 |
| `ItemStack` | `blocks.h`：`id` 和 `count` 构成一个堆；`empty` 检查空堆，`clear` 清空。 |
| `Recipe` | `blocks.h`：配方 ID/名称/输出/输出数量、材料及允许网格尺寸。 |
| `kBlocks`、`kBlockCount` | `blocks.cpp` 匿名命名空间：完整方块属性主表及其条目数；`static_assert` 确保主表和 `BlockId` 数量一致。 |
| `buildItems`、`buildItemList`、`buildRecipes`、`buildSmelts` | `blocks.cpp` 匿名命名空间：创建物品、合成配方、熔炼配方数据表；`buildItemList` 以 `buildItems` 生成基础物品集合。 |
| `blockDef`（`BlockId` / `int`） | 按方块 ID 查询定义；整数重载用于网络/存档解码的 ID。 |
| `itemDef`、`allItems`、`allRecipes`、`allSmeltRecipes` | 查询单个物品或取得只读物品/配方表。 |
| `blockGui`、`smeltRecipe` | 查询方块对应 GUI、输入物对应的熔炼配方。 |
| `isFuel`、`fuelTicks` | 判断燃料有效性并取得燃烧 tick 数。 |
| `isBlockItem` | 判断物品是否为方块并可选返回方块 ID。 |
| `isSolidBlock`、`isOpaqueBlock`、`isReplaceableBlock` | 根据方块定义查询碰撞、遮光及放置替换性质。 |
| `itemName`、`blockName` | 获取物品/方块本地化显示名。 |
| `breakSeconds`、`attackDamage`、`toolTier` | 根据方块硬度和手持工具计算采掘时间、伤害及工具等级。 |
| `makeStack`、`stackLimit`、`canAddItem`、`addItem`、`countItem`、`removeItem` | 创建堆叠、查询堆叠上限、检查背包容量、加入/计数/移除物品；`addItem` 返回未能加入的剩余数量。 |

`BlockDef` 的成员逐项含义：`id` 稳定机器 ID；`name` 显示名；`tex` 资源路径；`solid` 是否参与碰撞；`opaque` 是否完全遮光；`replaceable` 是否可被放置替换；`cutout` 是否使用透明裁剪绘制；`liquid` 是否液体；`hardness` 采掘硬度；`tool`/`tier` 所需工具及等级；`light` 发光等级；`drop`/`dropMin`/`dropMax` 掉落品及数量区间；`gui` 右键打开的容器类型。`ItemDef` 字段分别代表物品 ID/名称/贴图/堆叠上限、是否为方块及方块枚举、工具类别/等级/速度、攻击力和食物恢复值。`Recipe.inputs` 保存材料 ID 与数量，`grid` 表示所需最小工作台网格边长。

`blocks.cpp` 的辅助函数 `stackLimit(id)` 返回物品最大堆叠数，是 `canAddItem`/`addItem` 计算可放入数量时的共用规则；匿名命名空间中的 `kBlocks` 是方块属性主表，`kBlockCount` 与 `BlockId::BLOCK_COUNT` 的 `static_assert` 用来防止枚举和表大小不一致。

### 世界 — `src/common/world.h`、`src/common/world.cpp`

| 类型/函数 | 作用 |
|---|---|
| `World` 构造函数、`clear` | 创建指定宽高网格或以一种方块填充前景/背景。 |
| `width`、`height`、`seed`、`inBounds` | 查询世界尺寸、种子以及格坐标范围。 |
| `fg`、`bg`、`setFg`、`setBg` | 查询/修改前景方块和背景板。 |
| `solid`、`opaque`、`water`、`lava` | 查询方块碰撞、遮光或液体类别。 |
| `collidesBox`、`boxInWater`、`boxInLava` | 查询 AABB 碰撞或玩家包围盒是否处于液体中。 |
| `surfaceY`、`clearanceAbove` | 查询地表高度和头顶可用空间。 |
| `fgData`、`bgData` | 访问连续方块数组；网络解码时用于批量复制。 |
| `encodeFg`、`encodeBg`、`decodeRuns` | 对前景/背景使用 `[value,count,...]` RLE 编码/解码。 |
| `generate` | 根据给定种子程序化生成地形、生物群落和装饰。 |
| `save`、`load` | 持久化/读取世界方块、种子和世界时间。 |
| 私有 `hash64`、`hashFloat`、`smooth`、`valueNoise1`、`valueNoise2`、`fbm2` | `world.cpp` 匿名命名空间：为可复现地形生成提供哈希、平滑插值和分形噪声。 |
| 私有 `Header`、`U` | `world.cpp`：存档头部格式和方块 ID 到 16 位存储类型的转换。 |

`World` 成员 `width_`/`height_` 保存网格尺寸，`seed_` 保存生成种子，`fg_`/`bg_` 分别保存前景与背景连续数组。

### 物理 — `src/common/physics.h`、`src/common/physics.cpp`

| 类型/函数 | 作用 |
|---|---|
| `PLAYER_WIDTH`、`PLAYER_HEIGHT`、`PLAYER_SNEAK_HEIGHT`、`TICK_SECONDS` | 玩家 AABB 尺寸和共享固定 tick 时长。 |
| `MoveInput` | `move` 为左右输入，`jump`/`sneak`/`sprint` 为动作，`facing` 为朝向，`seq` 为输入确认序号。 |
| `PlayerBody` | `x`,`y` 是中心 X/脚底 Y；`vx`,`vy` 为速度；其余布尔量记录地面/潜行/疾跑/飞行；`facing` 为朝向；`fallDistance`,`justLanded`,`landingFall` 跟踪坠落和着陆。`height`/`eyeY`/`centerX`/`centerY` 提供碰撞/视线位置。 |
| `approach` | `physics.cpp` 匿名命名空间：将速度逐步逼近目标值并限制变化量。 |
| `hasGroundAt`、`hasGroundSupport` | 查询坐标或玩家脚底是否有支撑方块。 |
| `tickPlayer` | 执行客户端与服务器共用的重力、水平移动、跳跃、潜行、游泳和飞行物理。 |

### TCP/JSON — `src/common/net.h`、`src/common/net.cpp`

| 类型/函数 | 作用 |
|---|---|
| `json` | `net.h` 中对 `nlohmann::json` 的项目级别名。 |
| `NetSocket` | 封装一个不可复制的 Winsock socket；`sock_` 是不透明平台句柄，`kInvalid` 表示未打开。 |
| `NetSocket` 构造/析构函数 | 创建空句柄或接管句柄；析构时自动关闭。 |
| `NetSocket::connectTo` | 解析 IPv4 地址、在超时范围内建立连接并设置 TCP 选项。 |
| `NetSocket::close`、`shutdownBoth`、`valid` | 关闭连接、双向关闭收发、检查句柄有效。 |
| `NetSocket::sendBytes`、`recvExact` | 可靠发送指定字节或循环读取指定长度。 |
| `TcpListener` | 监听 socket 的 RAII 包装。 |
| `TcpListener::start`、`acceptOne`、`close`、`valid` | 绑定/监听端口、接受连接、关闭监听 socket、检查有效状态。 |
| `netInit`、`netShutdown`、`netErrorText` | 初始化/清理 Winsock 并将系统网络错误转成文本。 |
| `sendJson`、`recvJson` | 对 JSON 使用 4 字节小端长度前缀封帧/解帧，并检查最大消息尺寸和 JSON 合法性。 |
| `toSocket`、`fromSocket` | `net.cpp` 匿名命名空间：在内部 Winsock 类型与公开不透明整数句柄之间转换。 |
| `putU32LE`、`getU32LE` | 写入/读取协议长度头部中的小端 32 位整数。 |
| `configureSocket` | 设置 TCP_NODELAY、keepalive 和发送超时。 |
| `MAX_MESSAGE_SIZE`、`g_wsaReady` | 最大 JSON 帧字节数及进程级 Winsock 初始化状态。 |

### 其他源码文件的类型和字段

| 类型/字段 | 位置 | 作用 |
|---|---|---|
| `World::SEA_LEVEL` | `src/common/world.h` | 程序生成地形时使用的海平面高度常量。 |
| `World::width_`、`height_` | `World` 私有成员 | 方块网格的水平/垂直尺寸。 |
| `World::seed_` | `World` 私有成员 | 最近一次生成或加载的世界种子。 |
| `World::fg_`、`bg_` | `World` 私有成员 | 按行连续存储的前景方块和背景墙 ID。 |
| `Header` | `src/common/world.cpp` 匿名命名空间 | 存档文件头，记录格式标识、版本、尺寸、种子和世界时间。 |
| `U(blockId)` | 同上 | 将 `BlockId` 转为存档/RLE 使用的 `uint16_t` 表示。 |
| `MoveInput` | `src/common/physics.h` | `move` 为 -1/0/1 水平意图；`jump`、`sneak`、`sprint` 是按键状态；`facing` 是方向；`seq` 用于服务器确认输入。 |
| `PlayerBody` | 同上 | `x`,`y`,`vx`,`vy` 保存玩家位置速度；`onGround`,`sneaking`,`sprinting`,`flying` 是物理模式；`facing` 是朝向；`fallDistance`,`justLanded`,`landingFall` 跟踪坠落和着陆。成员函数 `height`、`eyeY`、`centerX`、`centerY` 分别计算碰撞高度、视线高度、横向中心和纵向中心。 |
| `NetSocket::sock_`、`TcpListener::sock_` | `src/common/net.h` | 以 `uintptr_t` 保存不暴露 Winsock 头文件的底层句柄。 |
| `Assets::root_` | `src/client/assets.h` | 游戏资源根目录。 |
| `Assets::textures_` | 同上 | 以资源路径索引已加载的 raylib 纹理。 |
| `Assets::fallback_` | 同上 | 资源缺失时供绘制使用的占位纹理。 |
| `Assets::font_`、`Assets::hasFont_` | 同上 | 已加载字体及字体是否可用的标记。 |
| `kUiGlyphText` | `src/client/ui_chars.h` | 生成的 UI 字体 glyph 覆盖字符串；扫描生成，不应手工编辑。 |
| `fileExists(path)` | `src/client/assets.cpp` 匿名命名空间 | 检查资源文件是否存在。 |
| `collectUtf8(text, out)` | 同上 | 从 UTF-8 文本收集字体加载所需的 Unicode 码点。 |
| `utf8ToWide(text)` | `src/client/platform_win.cpp` 匿名命名空间 | 将 UTF-8 命令行参数转为 Windows UTF-16。 |
| `windowsErrorText(code)` | 同上 | 将 Windows 错误码格式化为可显示文本。 |
| `getExecutableDirectory()` | `src/client/platform_win32.cpp` | 通过 Win32 模块路径 API 取得可执行文件所在目录。 |
| `launchServerProcess(exe, cwd, port, processHandle, error)` | 同上 | 隐藏启动本地服务端；输入服务端路径/工作目录/端口，输出进程句柄或错误。 |
| `stopServerProcess(processHandle)` | 同上 | 等待本地服务器正常退出，超时后终止，并关闭句柄。 |
| `launchProcessHidden(executable, arguments, workingDirectory, processHandle, processId, error)` | `src/client/platform_win.cpp` | 通用隐藏子进程启动接口；相比 `launchServerProcess` 支持任意参数字符串并返回 PID。 |

`platform_win.*` 与 `platform_win32.*` 是两套 Windows 包装：客户端当前入口调用 `getExecutableDirectory`、`launchServerProcess`、`stopServerProcess`（`platform_win32.*`）；`platform_win.*` 提供更通用的隐藏进程接口。两套 API 都在构建中，但职责并不完全相同。

### 函数参数与局部数据如何理解

| 模块/函数 | 入参的含义 | 重要局部值的作用 |
|---|---|---|
| `Assets::pathOf(relativePathOrName)` | 资源相对名或名称 | 规范化扩展名及路径，再由 `root_` 构造磁盘路径。 |
| `Assets::init(gameRoot, error)` | 游戏根目录及错误输出 | 确认资源根、占位纹理和字体；`error` 返回初始化失败/缺失资源说明。 |
| `World::generate(seed)` | 可复现世界种子 | 地形高度、噪声、洞穴与装饰坐标等中间值只用于生成本次世界网格。 |
| `World::save(path, time)` / `load(path, time)` | 存档路径、世界时间（加载时输出） | 文件流、头信息和编码后的前/背景数组负责序列化/校验；失败通过 `false` 返回。 |
| `tickPlayer(world, body, input, creative)` | 权威/预测共享世界、玩家状态、输入和创造模式 | `dt` 是固定 tick；`inWater`/`inLava` 决定运动分支；`height` 是当前碰撞高度；`speed`/`accel`/`targetVx` 控制水平运动；`nx`/`ny` 是本 tick 的候选位置。 |
| `sendJson(socket, mutex, message, error)` / `recvJson(socket, message, error)` | socket、JSON 和错误输出 | 序列化 payload、4 字节帧头和长度用于封帧；接收端先读取长度，再验证上限、收取 JSON 字节并解析。 |
| `Game::handleContainer(message)` | 服务端容器快照 | 将 `gui`、槽数组、光标与炉体进度解析到快照成员，并重新构建布局；不执行服务端状态变更。 |
| `Game::buildInventoryLayout()` | 当前 GUI 状态和窗口尺寸（成员） | 计算缩放/背景区域，把纹理槽坐标转换成 `containerSlots_` 的命中框及协议槽号。 |
| `Game::handleInventoryInput()` | 当前鼠标/键盘状态（raylib） | 命中框确定目标槽；发 `slot_click` 或 `swap_hotbar` 意图，Shift 标志随点击请求传递。 |
| `Game::drawInventory()` | 当前 GUI 快照、贴图和窗口（成员） | 背景先绘制；再按快照绘制物品、数量、光标、提示文本和炉体进度。 |
| `main(argc, argv)` | 进程命令行 | `i` 遍历参数；`arg`/`a` 是当前选项；解析后的配置放入 `Options` 或服务端启动变量，随后进入对应程序生命周期。 |

## 全部函数参数和局部变量索引

本节按 `src/**/*.cpp` 中函数定义列出其显式参数和函数体中可识别的局部变量，附上类型、源码行和用途提示。前文的类型/函数表说明所属算法和跨调用状态；本表用变量名、类型和所属函数定位每个局部值。

重复的 `i`/`j` 一类循环索引、错误码和迭代器按其常用意义标注；其他局部量的具体作用需结合所属函数上文和其读写点理解。结构体字段与文件级变量已在前文按所属类型列出。

| 函数定义及位置 | 显式参数（类型与用途） | 局部变量（类型、用途和声明行） |
|---|---|---|
| `fileExists` ([L11](./src/client/assets.cpp#L11)) | `p` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解 | `ec` : `std::error_code` - 系统错误状态 ([L12](./src/client/assets.cpp#L12)) |
| `collectUtf8` ([L16](./src/client/assets.cpp#L16)) | `text` : `const std::string&` - 文本内容<br>`out` : `std::vector&lt;int&gt;&` - 本函数计算/生成的结果 | `c` : `unsigned char` - 本函数的局部临时值；结合函数责任和使用点理解 ([L18](./src/client/assets.cpp#L18))<br>`cp` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L19](./src/client/assets.cpp#L19))<br>`len` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L20](./src/client/assets.cpp#L20)) |
| `Assets::pathOf` ([L36](./src/client/assets.cpp#L36)) | `relativePathOrName` : `const std::string&` - 文件路径/文件状态 | `name` : `std::string` - 显示名称 ([L37](./src/client/assets.cpp#L37))<br>`hasExt` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L38](./src/client/assets.cpp#L38)) |
| `Assets::init` ([L48](./src/client/assets.cpp#L48)) | `gameRoot` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`error` : `std::string&` - 错误信息输出 | `fallbackImage` : `Image` - 本函数的局部临时值；结合函数责任和使用点理解 ([L50](./src/client/assets.cpp#L50))<br>`codepoints` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L55](./src/client/assets.cpp#L55))<br>`fonts` : `const std::vector&lt;std::string&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L61](./src/client/assets.cpp#L61))<br>`f` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L69](./src/client/assets.cpp#L69))<br>`missing` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L73](./src/client/assets.cpp#L73))<br>`cp` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L74](./src/client/assets.cpp#L74)) |
| `Assets::tex` ([L101](./src/client/assets.cpp#L101)) | `relativePathOrName` : `const std::string&` - 文件路径/文件状态 | `path` : `const std::string` - 文件或资源路径 ([L102](./src/client/assets.cpp#L102))<br>`it` : `auto` - 容器迭代器 ([L103](./src/client/assets.cpp#L103))<br>`t` : `Texture2D` - 本函数的局部临时值；结合函数责任和使用点理解 ([L105](./src/client/assets.cpp#L105))<br>`result` : `auto` - 计算或解析结果 ([L112](./src/client/assets.cpp#L112)) |
| `Assets::unload` ([L117](./src/client/assets.cpp#L117)) | 无显式参数 | 无局部变量 |
| `scaleColor` ([L42](./src/client/main.cpp#L42)) | `c` : `Color` - 本函数的局部临时值；结合函数责任和使用点理解<br>`b` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `exeDirectory` ([L52](./src/client/main.cpp#L52)) | 无显式参数 | 无局部变量 |
| `looksLikeRoot` ([L56](./src/client/main.cpp#L56)) | `p` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解 | `ec` : `std::error_code` - 系统错误状态 ([L57](./src/client/main.cpp#L57)) |
| `findRoot` ([L61](./src/client/main.cpp#L61)) | 无显式参数 | `starts` : `std::vector&lt;std::filesystem::path&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L62](./src/client/main.cpp#L62))<br>`start` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L66](./src/client/main.cpp#L66))<br>`p` : `std::filesystem::path` - 本函数的局部临时值；结合函数责任和使用点理解 ([L67](./src/client/main.cpp#L67)) |
| `parseAddress` ([L77](./src/client/main.cpp#L77)) | `input` : `const std::string&` - 输入状态<br>`host` : `std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`port` : `int&` - 本函数的局部临时值；结合函数责任和使用点理解 | `s` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L78](./src/client/main.cpp#L78))<br>`colon` : `const size_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L82](./src/client/main.cpp#L82)) |
| `utf8Codepoint` ([L95](./src/client/main.cpp#L95)) | `cp` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 | `s` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L96](./src/client/main.cpp#L96)) |
| `popUtf8` ([L115](./src/client/main.cpp#L115)) | `s` : `std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | `i` : `size_t` - 循环或数组索引 ([L117](./src/client/main.cpp#L117)) |
| `~NetClient` ([L133](./src/client/main.cpp#L133)) | 无显式参数 | 无局部变量 |
| `connectTo` ([L135](./src/client/main.cpp#L135)) | `host` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`error` : `std::string&` - 错误信息输出 | `msg` : `json` - 网络消息 ([L142](./src/client/main.cpp#L142))<br>`err` : `std::string` - 错误文本或错误结果 ([L143](./src/client/main.cpp#L143))<br>`l` : `std::lock_guard&lt;std::mutex&gt;` - 临界区互斥锁保护对象 ([L148](./src/client/main.cpp#L148))<br>`d` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L151](./src/client/main.cpp#L151))<br>`l` : `std::lock_guard&lt;std::mutex&gt;` - 临界区互斥锁保护对象 ([L152](./src/client/main.cpp#L152))<br>`l` : `std::lock_guard&lt;std::mutex&gt;` - 临界区互斥锁保护对象 ([L158](./src/client/main.cpp#L158)) |
| `send` ([L167](./src/client/main.cpp#L167)) | `msg` : `const json&` - 网络消息 | `err` : `std::string` - 错误文本或错误结果 ([L169](./src/client/main.cpp#L169)) |
| `poll` ([L177](./src/client/main.cpp#L177)) | `out` : `json&` - 本函数计算/生成的结果 | `l` : `std::lock_guard&lt;std::mutex&gt;` - 临界区互斥锁保护对象 ([L178](./src/client/main.cpp#L178)) |
| `getReason` ([L185](./src/client/main.cpp#L185)) | 无显式参数 | `l` : `std::lock_guard&lt;std::mutex&gt;` - 临界区互斥锁保护对象 ([L186](./src/client/main.cpp#L186)) |
| `close` ([L190](./src/client/main.cpp#L190)) | 无显式参数 | `l` : `std::lock_guard&lt;std::mutex&gt;` - 临界区互斥锁保护对象 ([L196](./src/client/main.cpp#L196)) |
| `blockPixels` ([L464](./src/client/main.cpp#L464)) | 无显式参数 | 无局部变量 |
| `Game::init` ([L508](./src/client/main.cpp#L508)) | `opt` : `const Options&` - 本函数的局部临时值；结合函数责任和使用点理解 | `ec` : `std::error_code` - 系统错误状态 ([L511](./src/client/main.cpp#L511))<br>`err` : `std::string` - 错误文本或错误结果 ([L520](./src/client/main.cpp#L520))<br>`netErr` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L528](./src/client/main.cpp#L528)) |
| `Game::run` ([L543](./src/client/main.cpp#L543)) | 无显式参数 | `dt` : `float` - 本次更新的时间步长 ([L545](./src/client/main.cpp#L545)) |
| `Game::shutdown` ([L555](./src/client/main.cpp#L555)) | 无显式参数 | 无局部变量 |
| `Game::connectTo` ([L571](./src/client/main.cpp#L571)) | `addr` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`launchLocal` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `host` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L575](./src/client/main.cpp#L575))<br>`port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L577](./src/client/main.cpp#L577))<br>`err` : `std::string` - 错误文本或错误结果 ([L588](./src/client/main.cpp#L588))<br>`ok` : `bool` - 操作成功/有效标记 ([L590](./src/client/main.cpp#L590))<br>`hello` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L609](./src/client/main.cpp#L609)) |
| `Game::launchLocalServer` ([L621](./src/client/main.cpp#L621)) | `port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`err` : `std::string&` - 错误文本或错误结果 | `serverExe` : `std::filesystem::path` - 本函数的局部临时值；结合函数责任和使用点理解 ([L622](./src/client/main.cpp#L622)) |
| `Game::stopOwnedServer` ([L637](./src/client/main.cpp#L637)) | 无显式参数 | 无局部变量 |
| `Game::disconnectToTitle` ([L646](./src/client/main.cpp#L646)) | `reason` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `Game::pollNet` ([L669](./src/client/main.cpp#L669)) | 无显式参数 | `msg` : `json` - 网络消息 ([L670](./src/client/main.cpp#L670)) |
| `Game::processMessage` ([L688](./src/client/main.cpp#L688)) | `m` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | `type` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L689](./src/client/main.cpp#L689))<br>`w` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L704](./src/client/main.cpp#L704))<br>`h` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L705](./src/client/main.cpp#L705))<br>`fgRuns` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L706](./src/client/main.cpp#L706))<br>`bgRuns` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L707](./src/client/main.cpp#L707))<br>`fg` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L710](./src/client/main.cpp#L710))<br>`bg` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L711](./src/client/main.cpp#L711))<br>`kind` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L741](./src/client/main.cpp#L741)) |
| `Game::handleSelf` ([L776](./src/client/main.cpp#L776)) | `self` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | `ack` : `const uint32_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L790](./src/client/main.cpp#L790))<br>`sb` : `PlayerBody` - 本函数的局部临时值；结合函数责任和使用点理解 ([L796](./src/client/main.cpp#L796))<br>`dx` : `const float` - X 坐标偏移 ([L807](./src/client/main.cpp#L807))<br>`dy` : `const float` - Y 坐标偏移 ([L809](./src/client/main.cpp#L809))<br>`p` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L813](./src/client/main.cpp#L813)) |
| `Game::handleSnapshot` ([L821](./src/client/main.cpp#L821)) | `m` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | `seen` : `std::map&lt;int, bool&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L823](./src/client/main.cpp#L823))<br>`p` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L824](./src/client/main.cpp#L824))<br>`id` : `int` - 对象或物品标识 ([L825](./src/client/main.cpp#L825))<br>`r` : `RemotePlayer&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L828](./src/client/main.cpp#L828))<br>`e` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L857](./src/client/main.cpp#L857))<br>`id` : `int` - 对象或物品标识 ([L858](./src/client/main.cpp#L858))<br>`v` : `EntityView&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L860](./src/client/main.cpp#L860)) |
| `Game::handleInventory` ([L888](./src/client/main.cpp#L888)) | `m` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | `s` : `auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L890](./src/client/main.cpp#L890))<br>`i` : `int` - 循环或数组索引 ([L891](./src/client/main.cpp#L891))<br>`item` : `const auto&` - 物品堆/物品标识数据 ([L892](./src/client/main.cpp#L892)) |
| `Game::handleContainer` ([L910](./src/client/main.cpp#L910)) | `m` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | `readStacks` : `auto` - 物品堆/物品标识数据 ([L920](./src/client/main.cpp#L920))<br>`stack` : `auto&` - 物品堆 ([L922](./src/client/main.cpp#L922))<br>`item` : `const auto&` - 物品堆/物品标识数据 ([L925](./src/client/main.cpp#L925))<br>`furnace` : `const auto&` - 熔炉状态 ([L936](./src/client/main.cpp#L936))<br>`names` : `const std::array&lt;std::string, 3&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L937](./src/client/main.cpp#L937))<br>`item` : `const auto&` - 物品堆/物品标识数据 ([L940](./src/client/main.cpp#L940)) |
| `Game::handleStats` ([L962](./src/client/main.cpp#L962)) | `m` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `Game::handleBlock` ([L974](./src/client/main.cpp#L974)) | `m` : `const json&` - 本函数的局部临时值；结合函数责任和使用点理解 | `x` : `int` - X 坐标 ([L975](./src/client/main.cpp#L975))<br>`y` : `int` - Y 坐标 ([L976](./src/client/main.cpp#L976))<br>`fg` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L977](./src/client/main.cpp#L977)) |
| `Game::onWorldLoaded` ([L986](./src/client/main.cpp#L986)) | 无显式参数 | 无局部变量 |
| `Game::update` ([L1005](./src/client/main.cpp#L1005)) | `dt` : `float` - 本次更新的时间步长 | 无局部变量 |
| `Game::updateTitle` ([L1039](./src/client/main.cpp#L1039)) | 无显式参数 | `w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1040](./src/client/main.cpp#L1040))<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1041](./src/client/main.cpp#L1041))<br>`bw` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1042](./src/client/main.cpp#L1042))<br>`bh` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1043](./src/client/main.cpp#L1043))<br>`x` : `float` - X 坐标 ([L1044](./src/client/main.cpp#L1044))<br>`y` : `float` - Y 坐标 ([L1045](./src/client/main.cpp#L1045))<br>`mouse` : `Vector2` - 鼠标坐标 ([L1051](./src/client/main.cpp#L1051)) |
| `Game::updateSettings` ([L1062](./src/client/main.cpp#L1062)) | 无显式参数 | `w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1063](./src/client/main.cpp#L1063))<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1064](./src/client/main.cpp#L1064)) |
| `Game::updateConnecting` ([L1077](./src/client/main.cpp#L1077)) | `dt` : `float` - 本次更新的时间步长 | 无局部变量 |
| `Game::updateDisconnected` ([L1094](./src/client/main.cpp#L1094)) | 无显式参数 | `w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1095](./src/client/main.cpp#L1095))<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1096](./src/client/main.cpp#L1096))<br>`m` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1105](./src/client/main.cpp#L1105)) |
| `Game::updatePlaying` ([L1114](./src/client/main.cpp#L1114)) | `dt` : `float` - 本次更新的时间步长 | 无局部变量 |
| `Game::updateHover` ([L1186](./src/client/main.cpp#L1186)) | 无显式参数 | `m` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1193](./src/client/main.cpp#L1193))<br>`wpos` : `Vector2` - 世界/屏幕坐标 ([L1195](./src/client/main.cpp#L1195))<br>`dx` : `const float` - X 坐标偏移 ([L1200](./src/client/main.cpp#L1200))<br>`dy` : `const float` - Y 坐标偏移 ([L1201](./src/client/main.cpp#L1201)) |
| `Game::updateDesiredInput` ([L1208](./src/client/main.cpp#L1208)) | 无显式参数 | `move` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1216](./src/client/main.cpp#L1216))<br>`m` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1229](./src/client/main.cpp#L1229))<br>`wpos` : `Vector2` - 世界/屏幕坐标 ([L1230](./src/client/main.cpp#L1230)) |
| `Game::fixedUpdate` ([L1236](./src/client/main.cpp#L1236)) | 无显式参数 | `input` : `MoveInput` - 输入状态 ([L1238](./src/client/main.cpp#L1238))<br>`m` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1243](./src/client/main.cpp#L1243)) |
| `Game::updateCamera` ([L1261](./src/client/main.cpp#L1261)) | `dt` : `float` - 本次更新的时间步长 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1263](./src/client/main.cpp#L1263))<br>`halfW` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1264](./src/client/main.cpp#L1264))<br>`halfH` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1265](./src/client/main.cpp#L1265))<br>`tx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1266](./src/client/main.cpp#L1266))<br>`ty` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1268](./src/client/main.cpp#L1268))<br>`k` : `const float` - 循环索引或临时键 ([L1275](./src/client/main.cpp#L1275)) |
| `Game::updateSmoothing` ([L1281](./src/client/main.cpp#L1281)) | `dt` : `float` - 本次更新的时间步长 | `k1` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1283](./src/client/main.cpp#L1283))<br>`k` : `const float` - 循环索引或临时键 ([L1290](./src/client/main.cpp#L1290))<br>`k` : `const float` - 循环索引或临时键 ([L1298](./src/client/main.cpp#L1298)) |
| `Game::updateParticles` ([L1306](./src/client/main.cpp#L1306)) | `dt` : `float` - 本次更新的时间步长 | `p` : `auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1307](./src/client/main.cpp#L1307))<br>`t` : `auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1319](./src/client/main.cpp#L1319))<br>`c` : `auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1322](./src/client/main.cpp#L1322)) |
| `Game::handleGameplayInput` ([L1330](./src/client/main.cpp#L1330)) | `dt` : `float` - 本次更新的时间步长 | `wheel` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1344](./src/client/main.cpp#L1344))<br>`dir` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1347](./src/client/main.cpp#L1347))<br>`leftDown` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1352](./src/client/main.cpp#L1352))<br>`leftPressed` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1354](./src/client/main.cpp#L1354))<br>`it` : `auto` - 容器迭代器 ([L1357](./src/client/main.cpp#L1357))<br>`dx` : `const float` - X 坐标偏移 ([L1359](./src/client/main.cpp#L1359))<br>`dy` : `const float` - Y 坐标偏移 ([L1360](./src/client/main.cpp#L1360))<br>`held` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1379](./src/client/main.cpp#L1379))<br>`item` : `const ItemDef*` - 物品堆/物品标识数据 ([L1380](./src/client/main.cpp#L1380))<br>`b` : `BlockId` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1411](./src/client/main.cpp#L1411)) |
| `Game::startMining` ([L1419](./src/client/main.cpp#L1419)) | `dt` : `float` - 本次更新的时间步长 | `b` : `BlockId` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1425](./src/client/main.cpp#L1425))<br>`seconds` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1439](./src/client/main.cpp#L1439)) |
| `Game::findEntityAtMouse` ([L1460](./src/client/main.cpp#L1460)) | `mouse` : `Vector2` - 鼠标坐标 | `best` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1461](./src/client/main.cpp#L1461))<br>`p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1462](./src/client/main.cpp#L1462))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1465](./src/client/main.cpp#L1465))<br>`w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1466](./src/client/main.cpp#L1466))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1467](./src/client/main.cpp#L1467))<br>`r` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1468](./src/client/main.cpp#L1468))<br>`dx` : `const float` - X 坐标偏移 ([L1470](./src/client/main.cpp#L1470))<br>`dy` : `const float` - Y 坐标偏移 ([L1471](./src/client/main.cpp#L1471)) |
| `Game::currentHeldItem` ([L1478](./src/client/main.cpp#L1478)) | 无显式参数 | 无局部变量 |
| `Game::handleInventoryInput` ([L1483](./src/client/main.cpp#L1483)) | 无显式参数 | `mouse` : `Vector2` - 鼠标坐标 ([L1484](./src/client/main.cpp#L1484))<br>`slot` : `const auto&` - 协议槽位编号 ([L1488](./src/client/main.cpp#L1488))<br>`left` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1495](./src/client/main.cpp#L1495))<br>`right` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1497](./src/client/main.cpp#L1497))<br>`slot` : `const auto&` - 协议槽位编号 ([L1499](./src/client/main.cpp#L1499)) |
| `Game::handlePauseInput` ([L1512](./src/client/main.cpp#L1512)) | 无显式参数 | `w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1513](./src/client/main.cpp#L1513))<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1514](./src/client/main.cpp#L1514))<br>`m` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1519](./src/client/main.cpp#L1519)) |
| `Game::handleDeathInput` ([L1527](./src/client/main.cpp#L1527)) | 无显式参数 | `w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1528](./src/client/main.cpp#L1528))<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1529](./src/client/main.cpp#L1529))<br>`m` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1534](./src/client/main.cpp#L1534)) |
| `Game::handleChatInput` ([L1542](./src/client/main.cpp#L1542)) | 无显式参数 | `cp` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1543](./src/client/main.cpp#L1543)) |
| `Game::addChat` ([L1563](./src/client/main.cpp#L1563)) | `from` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`text` : `const std::string&` - 文本内容 | `c` : `ChatLine` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1564](./src/client/main.cpp#L1564)) |
| `Game::addToast` ([L1572](./src/client/main.cpp#L1572)) | `text` : `const std::string&` - 文本内容<br>`sub` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | `t` : `Toast` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1573](./src/client/main.cpp#L1573)) |
| `Game::spawnBlockParticles` ([L1581](./src/client/main.cpp#L1581)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标<br>`block` : `BlockId` - 方块 ID 或方块定义 | `def` : `const BlockDef&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1582](./src/client/main.cpp#L1582))<br>`sx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1586](./src/client/main.cpp#L1586))<br>`sy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1587](./src/client/main.cpp#L1587))<br>`px` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1588](./src/client/main.cpp#L1588))<br>`py` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1589](./src/client/main.cpp#L1589)) |
| `Game::spawnParticle` ([L1599](./src/client/main.cpp#L1599)) | `x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`tex` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`src` : `Rectangle` - 源数据或绘制区域<br>`tint` : `Color` - 颜色/着色状态<br>`vx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`vy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`life` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`size` : `float` - 尺寸或容量<br>`gravity` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `p` : `Particle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1601](./src/client/main.cpp#L1601)) |
| `Game::takeScreenshot` ([L1616](./src/client/main.cpp#L1616)) | 无显式参数 | `dir` : `std::filesystem::path` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1617](./src/client/main.cpp#L1617))<br>`ec` : `std::error_code` - 系统错误状态 ([L1618](./src/client/main.cpp#L1618))<br>`now` : `std::time_t` - 当前时间点 ([L1620](./src/client/main.cpp#L1620))<br>`tm` : `std::tm` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1622](./src/client/main.cpp#L1622))<br>`name` : `std::ostringstream` - 显示名称 ([L1624](./src/client/main.cpp#L1624))<br>`path` : `std::filesystem::path` - 文件或资源路径 ([L1627](./src/client/main.cpp#L1627)) |
| `Game::lightAt` ([L1637](./src/client/main.cpp#L1637)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `surface` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1638](./src/client/main.cpp#L1638))<br>`depth` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1639](./src/client/main.cpp#L1639))<br>`light` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1640](./src/client/main.cpp#L1640)) |
| `Game::daylightFactor` ([L1646](./src/client/main.cpp#L1646)) | 无显式参数 | `p` : `const double` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1647](./src/client/main.cpp#L1647))<br>`elevation` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1648](./src/client/main.cpp#L1648)) |
| `Game::worldToScreen` ([L1652](./src/client/main.cpp#L1652)) | `wx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`wy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1653](./src/client/main.cpp#L1653)) |
| `Game::screenToWorld` ([L1660](./src/client/main.cpp#L1660)) | `sx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`sy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1661](./src/client/main.cpp#L1661)) |
| `Game::drawText` ([L1668](./src/client/main.cpp#L1668)) | `text` : `const std::string&` - 文本内容<br>`x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`size` : `float` - 尺寸或容量<br>`color` : `Color` - 绘制颜色<br>`shadow` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `f` : `Font&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1669](./src/client/main.cpp#L1669))<br>`spacing` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1670](./src/client/main.cpp#L1670)) |
| `Game::measureText` ([L1677](./src/client/main.cpp#L1677)) | `text` : `const std::string&` - 文本内容<br>`size` : `float` - 尺寸或容量 | 无局部变量 |
| `Game::drawTextCentered` ([L1681](./src/client/main.cpp#L1681)) | `text` : `const std::string&` - 文本内容<br>`cx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`y` : `float` - Y 坐标<br>`size` : `float` - 尺寸或容量<br>`color` : `Color` - 绘制颜色<br>`shadow` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `Game::drawPanel` ([L1685](./src/client/main.cpp#L1685)) | `r` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解<br>`alpha` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `Game::drawButton` ([L1690](./src/client/main.cpp#L1690)) | `r` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解<br>`text` : `const std::string&` - 文本内容 | `hover` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1691](./src/client/main.cpp#L1691))<br>`t` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1692](./src/client/main.cpp#L1692))<br>`scale` : `const float` - 绘制缩放值 ([L1694](./src/client/main.cpp#L1694))<br>`edge` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1695](./src/client/main.cpp#L1695)) |
| `Game::drawItemIcon` ([L1710](./src/client/main.cpp#L1710)) | `id` : `const std::string&` - 对象或物品标识<br>`r` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 | `def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1712](./src/client/main.cpp#L1712))<br>`tex` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1713](./src/client/main.cpp#L1713))<br>`t` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1714](./src/client/main.cpp#L1714)) |
| `Game::drawItemStack` ([L1718](./src/client/main.cpp#L1718)) | `stack` : `const ItemStack&` - 物品堆<br>`r` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解<br>`scale` : `float` - 绘制缩放值 | `fs` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1722](./src/client/main.cpp#L1722))<br>`s` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1723](./src/client/main.cpp#L1723)) |
| `Game::drawMenuDirt` ([L1728](./src/client/main.cpp#L1728)) | 无显式参数 | `dirt` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1729](./src/client/main.cpp#L1729))<br>`tile` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1730](./src/client/main.cpp#L1730)) |
| `Game::drawTooltip` ([L1740](./src/client/main.cpp#L1740)) | `text` : `const std::string&` - 文本内容<br>`mouse` : `Vector2` - 鼠标坐标 | `fs` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1742](./src/client/main.cpp#L1742))<br>`w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1743](./src/client/main.cpp#L1743))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1744](./src/client/main.cpp#L1744))<br>`x` : `const float` - X 坐标 ([L1745](./src/client/main.cpp#L1745))<br>`y` : `const float` - Y 坐标 ([L1746](./src/client/main.cpp#L1746)) |
| `Game::draw` ([L1756](./src/client/main.cpp#L1756)) | 无显式参数 | 无局部变量 |
| `Game::drawTitle` ([L1766](./src/client/main.cpp#L1766)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1767](./src/client/main.cpp#L1767))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1768](./src/client/main.cpp#L1768))<br>`bg` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1769](./src/client/main.cpp#L1769))<br>`logo` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1774](./src/client/main.cpp#L1774))<br>`lw` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1776](./src/client/main.cpp#L1776))<br>`lh` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1777](./src/client/main.cpp#L1777)) |
| `Game::drawSettings` ([L1792](./src/client/main.cpp#L1792)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1794](./src/client/main.cpp#L1794))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1795](./src/client/main.cpp#L1795))<br>`panel` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1796](./src/client/main.cpp#L1796)) |
| `Game::drawConnecting` ([L1835](./src/client/main.cpp#L1835)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1837](./src/client/main.cpp#L1837))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1838](./src/client/main.cpp#L1838)) |
| `Game::drawDisconnected` ([L1845](./src/client/main.cpp#L1845)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1847](./src/client/main.cpp#L1847))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1848](./src/client/main.cpp#L1848))<br>`panel` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1849](./src/client/main.cpp#L1849)) |
| `Game::drawSky` ([L1863](./src/client/main.cpp#L1863)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1864](./src/client/main.cpp#L1864))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1865](./src/client/main.cpp#L1865))<br>`day` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1866](./src/client/main.cpp#L1866))<br>`top` : `Color` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1867](./src/client/main.cpp#L1867))<br>`bottom` : `Color` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1874](./src/client/main.cpp#L1874))<br>`sx` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1881](./src/client/main.cpp#L1881))<br>`surface` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1883](./src/client/main.cpp#L1883))<br>`horizon` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1884](./src/client/main.cpp#L1884))<br>`c` : `Color` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1887](./src/client/main.cpp#L1887))<br>`n` : `float` - 数量或循环边界 ([L1889](./src/client/main.cpp#L1889))<br>`clouds` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1896](./src/client/main.cpp#L1896))<br>`scroll` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1898](./src/client/main.cpp#L1898))<br>`prog` : `const double` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1904](./src/client/main.cpp#L1904))<br>`t` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1907](./src/client/main.cpp#L1907))<br>`sunX` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1908](./src/client/main.cpp#L1908))<br>`sunY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1909](./src/client/main.cpp#L1909))<br>`sun` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1910](./src/client/main.cpp#L1910))<br>`t` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1914](./src/client/main.cpp#L1914))<br>`moonX` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1915](./src/client/main.cpp#L1915))<br>`moonY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1916](./src/client/main.cpp#L1916))<br>`moon` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1917](./src/client/main.cpp#L1917)) |
| `Game::drawTile` ([L1923](./src/client/main.cpp#L1923)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标<br>`background` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `id` : `const BlockId` - 对象或物品标识 ([L1924](./src/client/main.cpp#L1924))<br>`def` : `const BlockDef&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1926](./src/client/main.cpp#L1926))<br>`t` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1929](./src/client/main.cpp#L1929))<br>`src` : `Rectangle` - 源数据或绘制区域 ([L1931](./src/client/main.cpp#L1931))<br>`frames` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1934](./src/client/main.cpp#L1934))<br>`frame` : `const int` - 网络帧 ([L1935](./src/client/main.cpp#L1935))<br>`p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1938](./src/client/main.cpp#L1938))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1940](./src/client/main.cpp#L1940))<br>`dst` : `Rectangle` - 目标容器数据 ([L1941](./src/client/main.cpp#L1941))<br>`light` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1942](./src/client/main.cpp#L1942))<br>`b` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1945](./src/client/main.cpp#L1945))<br>`tint` : `Color` - 颜色/着色状态 ([L1946](./src/client/main.cpp#L1946)) |
| `Game::drawTiles` ([L1954](./src/client/main.cpp#L1954)) | `background` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1955](./src/client/main.cpp#L1955))<br>`w` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1956](./src/client/main.cpp#L1956))<br>`h` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1957](./src/client/main.cpp#L1957))<br>`x0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1958](./src/client/main.cpp#L1958))<br>`x1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1960](./src/client/main.cpp#L1960))<br>`y0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1961](./src/client/main.cpp#L1961))<br>`y1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1962](./src/client/main.cpp#L1962)) |
| `Game::drawTarget` ([L1980](./src/client/main.cpp#L1980)) | 无显式参数 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1983](./src/client/main.cpp#L1983))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1985](./src/client/main.cpp#L1985))<br>`stage` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1989](./src/client/main.cpp#L1989))<br>`cracks` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1990](./src/client/main.cpp#L1990)) |
| `Game::drawHumanoid` ([L1996](./src/client/main.cpp#L1996)) | `skin` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`wx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`wy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`facing` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`moving` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解<br>`walkPhase` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`swing` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`tint` : `Color` - 颜色/着色状态<br>`held` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`heightBlocks` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`drawHeld` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `t` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1999](./src/client/main.cpp#L1999))<br>`p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2000](./src/client/main.cpp#L2000))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2001](./src/client/main.cpp#L2001))<br>`u` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2002](./src/client/main.cpp#L2002))<br>`flip` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2005](./src/client/main.cpp#L2005))<br>`walk` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2007](./src/client/main.cpp#L2007))<br>`legA` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2008](./src/client/main.cpp#L2008))<br>`armA` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2009](./src/client/main.cpp#L2009))<br>`part` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2010](./src/client/main.cpp#L2010))<br>`size` : `const float` - 尺寸或容量 ([L2035](./src/client/main.cpp#L2035))<br>`hx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2036](./src/client/main.cpp#L2036))<br>`hy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2037](./src/client/main.cpp#L2037)) |
| `Game::drawPig` ([L2042](./src/client/main.cpp#L2042)) | `e` : `const EntityView&` - 本函数的局部临时值；结合函数责任和使用点理解 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2043](./src/client/main.cpp#L2043))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2044](./src/client/main.cpp#L2044))<br>`light` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2045](./src/client/main.cpp#L2045))<br>`body` : `Color` - 玩家/实体物理状态 ([L2046](./src/client/main.cpp#L2046))<br>`dark` : `Color` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2047](./src/client/main.cpp#L2047))<br>`pig` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2058](./src/client/main.cpp#L2058))<br>`headSrc` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2060](./src/client/main.cpp#L2060))<br>`hx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2062](./src/client/main.cpp#L2062)) |
| `Game::drawZombie` ([L2070](./src/client/main.cpp#L2070)) | `e` : `const EntityView&` - 本函数的局部临时值；结合函数责任和使用点理解 | `light` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2071](./src/client/main.cpp#L2071))<br>`tint` : `Color` - 颜色/着色状态 ([L2072](./src/client/main.cpp#L2072)) |
| `Game::drawItemEntity` ([L2077](./src/client/main.cpp#L2077)) | `e` : `const EntityView&` - 本函数的局部临时值；结合函数责任和使用点理解 | `p` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2078](./src/client/main.cpp#L2078))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2079](./src/client/main.cpp#L2079))<br>`bob` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2080](./src/client/main.cpp#L2080))<br>`size` : `const float` - 尺寸或容量 ([L2081](./src/client/main.cpp#L2081)) |
| `Game::drawRenderables` ([L2092](./src/client/main.cpp#L2092)) | 无显式参数 | `R` : `struct` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2093](./src/client/main.cpp#L2093))<br>`type` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2094](./src/client/main.cpp#L2094))<br>`id` : `int` - 对象或物品标识 ([L2095](./src/client/main.cpp#L2095))<br>`y` : `float` - Y 坐标 ([L2096](./src/client/main.cpp#L2096))<br>`list` : `std::vector&lt;R&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2098](./src/client/main.cpp#L2098))<br>`item` : `const auto&` - 物品堆/物品标识数据 ([L2110](./src/client/main.cpp#L2110))<br>`light` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2113](./src/client/main.cpp#L2113))<br>`tint` : `Color` - 颜色/着色状态 ([L2114](./src/client/main.cpp#L2114))<br>`it` : `auto` - 容器迭代器 ([L2119](./src/client/main.cpp#L2119))<br>`r` : `const RemotePlayer&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2121](./src/client/main.cpp#L2121))<br>`skin` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2133](./src/client/main.cpp#L2133))<br>`light` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2134](./src/client/main.cpp#L2134))<br>`tint` : `Color` - 颜色/着色状态 ([L2135](./src/client/main.cpp#L2135))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2138](./src/client/main.cpp#L2138))<br>`fs` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2140](./src/client/main.cpp#L2140))<br>`tw` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2141](./src/client/main.cpp#L2141))<br>`it` : `auto` - 容器迭代器 ([L2148](./src/client/main.cpp#L2148)) |
| `Game::drawParticles` ([L2157](./src/client/main.cpp#L2157)) | 无显式参数 | `p` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2158](./src/client/main.cpp#L2158))<br>`s` : `Vector2` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2159](./src/client/main.cpp#L2159))<br>`a` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2160](./src/client/main.cpp#L2160))<br>`size` : `const float` - 尺寸或容量 ([L2161](./src/client/main.cpp#L2161))<br>`tint` : `Color` - 颜色/着色状态 ([L2162](./src/client/main.cpp#L2162))<br>`t` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2169](./src/client/main.cpp#L2169)) |
| `Game::drawHud` ([L2179](./src/client/main.cpp#L2179)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2181](./src/client/main.cpp#L2181))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2183](./src/client/main.cpp#L2183))<br>`scale` : `const float` - 绘制缩放值 ([L2184](./src/client/main.cpp#L2184))<br>`hbW` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2185](./src/client/main.cpp#L2185))<br>`hbH` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2187](./src/client/main.cpp#L2187))<br>`hbX` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2188](./src/client/main.cpp#L2188))<br>`hbY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2189](./src/client/main.cpp#L2189))<br>`hotbar` : `Texture2D&` - 快捷栏槽编号 ([L2190](./src/client/main.cpp#L2190))<br>`slotW` : `const float` - 协议槽位或物品栏索引 ([L2196](./src/client/main.cpp#L2196))<br>`sx` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2197](./src/client/main.cpp#L2197))<br>`sy` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2198](./src/client/main.cpp#L2198))<br>`sel` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2201](./src/client/main.cpp#L2201))<br>`icon` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2209](./src/client/main.cpp#L2209))<br>`heartsY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2211](./src/client/main.cpp#L2211))<br>`x` : `const float` - X 坐标 ([L2213](./src/client/main.cpp#L2213))<br>`container` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2214](./src/client/main.cpp#L2214))<br>`v` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2217](./src/client/main.cpp#L2217))<br>`full` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2220](./src/client/main.cpp#L2220))<br>`half` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2224](./src/client/main.cpp#L2224))<br>`foodX0` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2229](./src/client/main.cpp#L2229))<br>`x` : `const float` - X 坐标 ([L2232](./src/client/main.cpp#L2232))<br>`empty` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2233](./src/client/main.cpp#L2233))<br>`v` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2236](./src/client/main.cpp#L2236))<br>`full` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2239](./src/client/main.cpp#L2239))<br>`half` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2243](./src/client/main.cpp#L2243))<br>`fs` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2250](./src/client/main.cpp#L2250))<br>`name` : `const std::string` - 显示名称 ([L2251](./src/client/main.cpp#L2251)) |
| `Game::buildInventoryLayout` ([L2256](./src/client/main.cpp#L2256)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2257](./src/client/main.cpp#L2257))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2259](./src/client/main.cpp#L2259))<br>`chest` : `const bool` - 箱子槽位数据 ([L2260](./src/client/main.cpp#L2260))<br>`sourceHeight` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2261](./src/client/main.cpp#L2261))<br>`panelWidth` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2264](./src/client/main.cpp#L2264))<br>`panelHeight` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2265](./src/client/main.cpp#L2265))<br>`addSlot` : `auto` - 协议槽位或物品栏索引 ([L2270](./src/client/main.cpp#L2270))<br>`mainY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2275](./src/client/main.cpp#L2275))<br>`hotbarY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2277](./src/client/main.cpp#L2277)) |
| `Game::drawInventory` ([L2298](./src/client/main.cpp#L2298)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2301](./src/client/main.cpp#L2301))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2303](./src/client/main.cpp#L2303))<br>`sourceHeight` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2305](./src/client/main.cpp#L2305))<br>`texture` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2306](./src/client/main.cpp#L2306))<br>`background` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2309](./src/client/main.cpp#L2309))<br>`title` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2312](./src/client/main.cpp#L2312))<br>`mouse` : `Vector2` - 鼠标坐标 ([L2327](./src/client/main.cpp#L2327))<br>`tooltip` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2329](./src/client/main.cpp#L2329))<br>`stackAt` : `auto` - 物品堆/物品标识数据 ([L2330](./src/client/main.cpp#L2330))<br>`empty` : `static const ItemStack` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2336](./src/client/main.cpp#L2336))<br>`slot` : `const auto&` - 协议槽位编号 ([L2339](./src/client/main.cpp#L2339))<br>`r` : `const Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2340](./src/client/main.cpp#L2340))<br>`hover` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2341](./src/client/main.cpp#L2341))<br>`stack` : `const ItemStack&` - 物品堆 ([L2345](./src/client/main.cpp#L2345))<br>`inset` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2346](./src/client/main.cpp#L2346))<br>`burnRatio` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2357](./src/client/main.cpp#L2357))<br>`cookRatio` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2360](./src/client/main.cpp#L2360))<br>`flame` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2362](./src/client/main.cpp#L2362))<br>`height` : `const float` - 几何高度 ([L2363](./src/client/main.cpp#L2363))<br>`arrow` : `Texture2D&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2372](./src/client/main.cpp#L2372))<br>`size` : `const float` - 尺寸或容量 ([L2383](./src/client/main.cpp#L2383)) |
| `Game::drawPause` ([L2390](./src/client/main.cpp#L2390)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2391](./src/client/main.cpp#L2391))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2392](./src/client/main.cpp#L2392))<br>`panel` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2394](./src/client/main.cpp#L2394)) |
| `Game::drawDeath` ([L2405](./src/client/main.cpp#L2405)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2406](./src/client/main.cpp#L2406))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2407](./src/client/main.cpp#L2407))<br>`panel` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2409](./src/client/main.cpp#L2409)) |
| `Game::drawChat` ([L2419](./src/client/main.cpp#L2419)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2420](./src/client/main.cpp#L2420))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2421](./src/client/main.cpp#L2421))<br>`fs` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2422](./src/client/main.cpp#L2422))<br>`lineH` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2423](./src/client/main.cpp#L2423))<br>`bottom` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2424](./src/client/main.cpp#L2424))<br>`visible` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2425](./src/client/main.cpp#L2425))<br>`y` : `float` - Y 坐标 ([L2431](./src/client/main.cpp#L2431))<br>`skipped` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2433](./src/client/main.cpp#L2433))<br>`line` : `const std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2441](./src/client/main.cpp#L2441))<br>`tw` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2442](./src/client/main.cpp#L2442))<br>`boxY` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2450](./src/client/main.cpp#L2450))<br>`text` : `const std::string` - 文本内容 ([L2452](./src/client/main.cpp#L2452))<br>`cx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2455](./src/client/main.cpp#L2455)) |
| `Game::drawToasts` ([L2461](./src/client/main.cpp#L2461)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2462](./src/client/main.cpp#L2462))<br>`y` : `float` - Y 坐标 ([L2463](./src/client/main.cpp#L2463))<br>`t` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2464](./src/client/main.cpp#L2464))<br>`a` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2465](./src/client/main.cpp#L2465))<br>`bw` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2467](./src/client/main.cpp#L2467))<br>`bh` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2469](./src/client/main.cpp#L2469))<br>`x` : `const float` - X 坐标 ([L2470](./src/client/main.cpp#L2470)) |
| `Game::drawDebug` ([L2483](./src/client/main.cpp#L2483)) | 无显式参数 | `fs` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2485](./src/client/main.cpp#L2485))<br>`y` : `float` - Y 坐标 ([L2487](./src/client/main.cpp#L2487))<br>`line` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2488](./src/client/main.cpp#L2488)) |
| `Game::drawPlayerList` ([L2507](./src/client/main.cpp#L2507)) | 无显式参数 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2509](./src/client/main.cpp#L2509))<br>`pw` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2511](./src/client/main.cpp#L2511))<br>`rowH` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2512](./src/client/main.cpp#L2512))<br>`count` : `int` - 数量计数 ([L2513](./src/client/main.cpp#L2513))<br>`panel` : `Rectangle` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2514](./src/client/main.cpp#L2514))<br>`y` : `float` - Y 坐标 ([L2517](./src/client/main.cpp#L2517)) |
| `Game::drawPlaying` ([L2528](./src/client/main.cpp#L2528)) | 无显式参数 | 无局部变量 |
| `main` ([L2559](./src/client/main.cpp#L2559)) | `argc` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`argv` : `char**` - 本函数的局部临时值；结合函数责任和使用点理解 | `opt` : `Options` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2560](./src/client/main.cpp#L2560))<br>`a` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2563](./src/client/main.cpp#L2563))<br>`game` : `Game` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2573](./src/client/main.cpp#L2573)) |
| `utf8ToWide` ([L14](./src/client/platform_win.cpp#L14)) | `text` : `const std::string&` - 文本内容 | `length` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L16](./src/client/platform_win.cpp#L16))<br>`result` : `std::wstring` - 计算或解析结果 ([L17](./src/client/platform_win.cpp#L17)) |
| `windowsErrorText` ([L23](./src/client/platform_win.cpp#L23)) | `code` : `DWORD` - 本函数的局部临时值；结合函数责任和使用点理解 | `buffer` : `char*` - 读写缓冲区 ([L24](./src/client/platform_win.cpp#L24))<br>`text` : `std::string` - 文本内容 ([L27](./src/client/platform_win.cpp#L27)) |
| `executableDirectory` ([L35](./src/client/platform_win.cpp#L35)) | 无显式参数 | `size` : `DWORD` - 尺寸或容量 ([L37](./src/client/platform_win.cpp#L37)) |
| `launchProcessHidden` ([L42](./src/client/platform_win.cpp#L42)) | `executable` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`arguments` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`workingDirectory` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`processHandle` : `void**` - 本函数的局部临时值；结合函数责任和使用点理解<br>`processId` : `unsigned long&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`error` : `std::string&` - 错误信息输出 | `command` : `std::wstring` - 指令名称/文本 ([L53](./src/client/platform_win.cpp#L53))<br>`cwd` : `std::wstring` - 本函数的局部临时值；结合函数责任和使用点理解 ([L54](./src/client/platform_win.cpp#L54))<br>`startup` : `STARTUPINFOW` - 本函数的局部临时值；结合函数责任和使用点理解 ([L55](./src/client/platform_win.cpp#L55))<br>`process` : `PROCESS_INFORMATION` - 本函数的局部临时值；结合函数责任和使用点理解 ([L59](./src/client/platform_win.cpp#L59))<br>`commandBuffer` : `std::vector&lt;wchar_t&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L60](./src/client/platform_win.cpp#L60))<br>`ok` : `BOOL` - 操作成功/有效标记 ([L62](./src/client/platform_win.cpp#L62))<br>`code` : `DWORD` - 本函数的局部临时值；结合函数责任和使用点理解 ([L65](./src/client/platform_win.cpp#L65)) |
| `waitForProcess` ([L75](./src/client/platform_win.cpp#L75)) | `processHandle` : `void*` - 本函数的局部临时值；结合函数责任和使用点理解<br>`timeoutMs` : `int` - 计时/进度值 | `result` : `DWORD` - 计算或解析结果 ([L77](./src/client/platform_win.cpp#L77)) |
| `terminateProcess` ([L81](./src/client/platform_win.cpp#L81)) | `processHandle` : `void*` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `closeProcessHandle` ([L85](./src/client/platform_win.cpp#L85)) | `processHandle` : `void*` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `getExecutableDirectory` ([L11](./src/client/platform_win32.cpp#L11)) | 无显式参数 | `n` : `DWORD` - 数量或循环边界 ([L13](./src/client/platform_win32.cpp#L13)) |
| `launchServerProcess` ([L18](./src/client/platform_win32.cpp#L18)) | `exe` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`cwd` : `const std::filesystem::path&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`processHandle` : `void*&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`error` : `std::string&` - 错误信息输出 | `cmd` : `std::wstring` - 本函数的局部临时值；结合函数责任和使用点理解 ([L23](./src/client/platform_win32.cpp#L23))<br>`cwdW` : `std::wstring` - 本函数的局部临时值；结合函数责任和使用点理解 ([L24](./src/client/platform_win32.cpp#L24))<br>`si` : `STARTUPINFOW` - 本函数的局部临时值；结合函数责任和使用点理解 ([L25](./src/client/platform_win32.cpp#L25))<br>`pi` : `PROCESS_INFORMATION` - 本函数的局部临时值；结合函数责任和使用点理解 ([L30](./src/client/platform_win32.cpp#L30))<br>`cmdBuf` : `std::vector&lt;wchar_t&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L32](./src/client/platform_win32.cpp#L32))<br>`ok` : `BOOL` - 操作成功/有效标记 ([L34](./src/client/platform_win32.cpp#L34)) |
| `stopServerProcess` ([L58](./src/client/platform_win32.cpp#L58)) | `processHandle` : `void*` - 本函数的局部临时值；结合函数责任和使用点理解 | `h` : `HANDLE` - 本函数的局部临时值；结合函数责任和使用点理解 ([L60](./src/client/platform_win32.cpp#L60))<br>`r` : `DWORD` - 本函数的局部临时值；结合函数责任和使用点理解 ([L61](./src/client/platform_win32.cpp#L61)) |
| `buildItems` ([L46](./src/common/blocks.cpp#L46)) | 无显式参数 | `items` : `std::vector&lt;ItemDef&gt;` - 物品堆/物品标识数据 ([L47](./src/common/blocks.cpp#L47))<br>`b` : `const BlockDef&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L50](./src/common/blocks.cpp#L50))<br>`item` : `ItemDef` - 物品堆/物品标识数据 ([L51](./src/common/blocks.cpp#L51))<br>`add` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L65](./src/common/blocks.cpp#L65))<br>`item` : `ItemDef` - 物品堆/物品标识数据 ([L68](./src/common/blocks.cpp#L68)) |
| `buildItemList` ([L114](./src/common/blocks.cpp#L114)) | 无显式参数 | 无局部变量 |
| `buildRecipes` ([L118](./src/common/blocks.cpp#L118)) | 无显式参数 | 无局部变量 |
| `buildSmelts` ([L144](./src/common/blocks.cpp#L144)) | 无显式参数 | 无局部变量 |
| `blockDef` ([L156](./src/common/blocks.cpp#L156)) | `id` : `BlockId` - 对象或物品标识 | `i` : `int` - 循环或数组索引 ([L157](./src/common/blocks.cpp#L157)) |
| `blockDef` ([L162](./src/common/blocks.cpp#L162)) | `id` : `int` - 对象或物品标识 | 无局部变量 |
| `allItems` ([L166](./src/common/blocks.cpp#L166)) | 无显式参数 | `items` : `static const std::vector&lt;ItemDef&gt;` - 物品堆/物品标识数据 ([L167](./src/common/blocks.cpp#L167)) |
| `itemDef` ([L171](./src/common/blocks.cpp#L171)) | `id` : `const std::string&` - 对象或物品标识 | `map` : `static const std::unordered_map&lt;std::string, const ItemDef*&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L172](./src/common/blocks.cpp#L172))<br>`result` : `std::unordered_map&lt;std::string, const ItemDef*&gt;` - 计算或解析结果 ([L173](./src/common/blocks.cpp#L173))<br>`item` : `const auto&` - 物品堆/物品标识数据 ([L174](./src/common/blocks.cpp#L174))<br>`it` : `auto` - 容器迭代器 ([L177](./src/common/blocks.cpp#L177)) |
| `allRecipes` ([L181](./src/common/blocks.cpp#L181)) | 无显式参数 | `recipes` : `static const std::vector&lt;Recipe&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L182](./src/common/blocks.cpp#L182)) |
| `blockGui` ([L186](./src/common/blocks.cpp#L186)) | `id` : `BlockId` - 对象或物品标识 | 无局部变量 |
| `allSmeltRecipes` ([L188](./src/common/blocks.cpp#L188)) | 无显式参数 | `recipes` : `static const std::vector&lt;SmeltRecipe&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L189](./src/common/blocks.cpp#L189)) |
| `smeltRecipe` ([L193](./src/common/blocks.cpp#L193)) | `input` : `const std::string&` - 输入状态 | `recipe` : `const auto&` - 配方定义 ([L195](./src/common/blocks.cpp#L195)) |
| `isFuel` ([L201](./src/common/blocks.cpp#L201)) | `id` : `const std::string&` - 对象或物品标识 | 无局部变量 |
| `fuelTicks` ([L203](./src/common/blocks.cpp#L203)) | `id` : `const std::string&` - 对象或物品标识 | 无局部变量 |
| `isBlockItem` ([L214](./src/common/blocks.cpp#L214)) | `item` : `const std::string&` - 物品堆/物品标识数据<br>`out` : `BlockId*` - 本函数计算/生成的结果 | `def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L215](./src/common/blocks.cpp#L215)) |
| `isSolidBlock` ([L221](./src/common/blocks.cpp#L221)) | `id` : `BlockId` - 对象或物品标识 | 无局部变量 |
| `isOpaqueBlock` ([L222](./src/common/blocks.cpp#L222)) | `id` : `BlockId` - 对象或物品标识 | 无局部变量 |
| `isReplaceableBlock` ([L223](./src/common/blocks.cpp#L223)) | `id` : `BlockId` - 对象或物品标识 | 无局部变量 |
| `blockName` ([L225](./src/common/blocks.cpp#L225)) | `id` : `BlockId` - 对象或物品标识 | 无局部变量 |
| `itemName` ([L227](./src/common/blocks.cpp#L227)) | `id` : `const std::string&` - 对象或物品标识 | `def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L228](./src/common/blocks.cpp#L228)) |
| `toolTier` ([L232](./src/common/blocks.cpp#L232)) | `heldItem` : `const std::string&` - 物品堆/物品标识数据 | `def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L233](./src/common/blocks.cpp#L233)) |
| `attackDamage` ([L237](./src/common/blocks.cpp#L237)) | `heldItem` : `const std::string&` - 物品堆/物品标识数据 | `def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L238](./src/common/blocks.cpp#L238)) |
| `breakSeconds` ([L242](./src/common/blocks.cpp#L242)) | `block` : `BlockId` - 方块 ID 或方块定义<br>`heldItem` : `const std::string&` - 物品堆/物品标识数据 | `def` : `const BlockDef&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L243](./src/common/blocks.cpp#L243))<br>`speed` : `float` - 移动或采掘速度 ([L246](./src/common/blocks.cpp#L246))<br>`item` : `const ItemDef*` - 物品堆/物品标识数据 ([L247](./src/common/blocks.cpp#L247)) |
| `makeStack` ([L254](./src/common/blocks.cpp#L254)) | `id` : `const std::string&` - 对象或物品标识<br>`count` : `int` - 数量计数 | `stack` : `ItemStack` - 物品堆 ([L255](./src/common/blocks.cpp#L255)) |
| `stackLimit` ([L262](./src/common/blocks.cpp#L262)) | `id` : `const std::string&` - 对象或物品标识 | `def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L263](./src/common/blocks.cpp#L263)) |
| `canAddItem` ([L267](./src/common/blocks.cpp#L267)) | `inv` : `const std::vector&lt;ItemStack&gt;&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`id` : `const std::string&` - 对象或物品标识<br>`count` : `int` - 数量计数 | `limit` : `const int` - 允许上限 ([L269](./src/common/blocks.cpp#L269))<br>`room` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L270](./src/common/blocks.cpp#L270))<br>`slot` : `const auto&` - 协议槽位编号 ([L271](./src/common/blocks.cpp#L271)) |
| `addItem` ([L279](./src/common/blocks.cpp#L279)) | `inv` : `std::vector&lt;ItemStack&gt;&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`id` : `const std::string&` - 对象或物品标识<br>`count` : `int` - 数量计数 | `limit` : `const int` - 允许上限 ([L281](./src/common/blocks.cpp#L281))<br>`remaining` : `int` - 尚未处理的数量 ([L282](./src/common/blocks.cpp#L282))<br>`slot` : `auto&` - 协议槽位编号 ([L283](./src/common/blocks.cpp#L283))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L286](./src/common/blocks.cpp#L286))<br>`slot` : `auto&` - 协议槽位编号 ([L291](./src/common/blocks.cpp#L291))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L294](./src/common/blocks.cpp#L294)) |
| `countItem` ([L303](./src/common/blocks.cpp#L303)) | `inv` : `const std::vector&lt;ItemStack&gt;&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`id` : `const std::string&` - 对象或物品标识 | `total` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L304](./src/common/blocks.cpp#L304))<br>`slot` : `const auto&` - 协议槽位编号 ([L305](./src/common/blocks.cpp#L305)) |
| `removeItem` ([L309](./src/common/blocks.cpp#L309)) | `inv` : `std::vector&lt;ItemStack&gt;&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`id` : `const std::string&` - 对象或物品标识<br>`count` : `int` - 数量计数 | `remaining` : `int` - 尚未处理的数量 ([L312](./src/common/blocks.cpp#L312))<br>`slot` : `auto&` - 协议槽位编号 ([L313](./src/common/blocks.cpp#L313))<br>`taken` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L316](./src/common/blocks.cpp#L316)) |
| `toSocket` ([L23](./src/common/net.cpp#L23)) | `value` : `std::uintptr_t` - 当前处理值 | 无局部变量 |
| `fromSocket` ([L24](./src/common/net.cpp#L24)) | `value` : `SOCKET` - 当前处理值 | 无局部变量 |
| `putU32LE` ([L26](./src/common/net.cpp#L26)) | `dst` : `char*` - 目标容器数据<br>`value` : `uint32_t` - 当前处理值 | 无局部变量 |
| `getU32LE` ([L33](./src/common/net.cpp#L33)) | `src` : `const char*` - 源数据或绘制区域 | 无局部变量 |
| `configureSocket` ([L40](./src/common/net.cpp#L40)) | `s` : `SOCKET` - 本函数的局部临时值；结合函数责任和使用点理解 | `noDelay` : `BOOL` - 本函数的局部临时值；结合函数责任和使用点理解 ([L41](./src/common/net.cpp#L41))<br>`keepAlive` : `BOOL` - 本函数的局部临时值；结合函数责任和使用点理解 ([L43](./src/common/net.cpp#L43))<br>`timeout` : `DWORD` - 计时/进度值 ([L45](./src/common/net.cpp#L45)) |
| `netErrorText` ([L51](./src/common/net.cpp#L51)) | `code` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 | `buffer` : `char*` - 读写缓冲区 ([L53](./src/common/net.cpp#L53))<br>`text` : `std::string` - 文本内容 ([L57](./src/common/net.cpp#L57)) |
| `netInit` ([L63](./src/common/net.cpp#L63)) | `error` : `std::string&` - 错误信息输出 | `data` : `WSADATA` - 输入/输出数据 ([L65](./src/common/net.cpp#L65))<br>`rc` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L66](./src/common/net.cpp#L66)) |
| `netShutdown` ([L75](./src/common/net.cpp#L75)) | 无显式参数 | 无局部变量 |
| `sock_` ([L83](./src/common/net.cpp#L83)) | `socket` : `parameter` - 网络套接字 | 无局部变量 |
| `NetSocket::~NetSocket` ([L85](./src/common/net.cpp#L85)) | 无显式参数 | 无局部变量 |
| `NetSocket::close` ([L87](./src/common/net.cpp#L87)) | 无显式参数 | 无局部变量 |
| `NetSocket::shutdownBoth` ([L94](./src/common/net.cpp#L94)) | 无显式参数 | 无局部变量 |
| `NetSocket::connectTo` ([L98](./src/common/net.cpp#L98)) | `host` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`timeoutMs` : `int` - 计时/进度值<br>`error` : `std::string&` - 错误信息输出 | `hints` : `addrinfo` - 本函数的局部临时值；结合函数责任和使用点理解 ([L100](./src/common/net.cpp#L100))<br>`result` : `addrinfo*` - 计算或解析结果 ([L104](./src/common/net.cpp#L104))<br>`service` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L105](./src/common/net.cpp#L105))<br>`rc` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L106](./src/common/net.cpp#L106))<br>`s` : `SOCKET` - 本函数的局部临时值；结合函数责任和使用点理解 ([L112](./src/common/net.cpp#L112))<br>`nonBlocking` : `u_long` - 本函数的局部临时值；结合函数责任和使用点理解 ([L118](./src/common/net.cpp#L118))<br>`conn` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L120](./src/common/net.cpp#L120))<br>`err` : `int` - 错误文本或错误结果 ([L123](./src/common/net.cpp#L123))<br>`writeSet` : `fd_set` - 本函数的局部临时值；结合函数责任和使用点理解 ([L129](./src/common/net.cpp#L129))<br>`errSet` : `fd_set` - 本函数的局部临时值；结合函数责任和使用点理解 ([L132](./src/common/net.cpp#L132))<br>`tv` : `timeval` - 本函数的局部临时值；结合函数责任和使用点理解 ([L135](./src/common/net.cpp#L135))<br>`selected` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L138](./src/common/net.cpp#L138))<br>`soError` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L144](./src/common/net.cpp#L144))<br>`len` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L145](./src/common/net.cpp#L145))<br>`blocking` : `u_long` - 本函数的局部临时值；结合函数责任和使用点理解 ([L153](./src/common/net.cpp#L153)) |
| `NetSocket::sendBytes` ([L160](./src/common/net.cpp#L160)) | `data` : `const void*` - 输入/输出数据<br>`size` : `size_t` - 尺寸或容量<br>`error` : `std::string&` - 错误信息输出 | `ptr` : `const char*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L161](./src/common/net.cpp#L161))<br>`left` : `size_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L162](./src/common/net.cpp#L162))<br>`sent` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L164](./src/common/net.cpp#L164)) |
| `NetSocket::recvExact` ([L179](./src/common/net.cpp#L179)) | `data` : `void*` - 输入/输出数据<br>`size` : `size_t` - 尺寸或容量<br>`error` : `std::string&` - 错误信息输出 | `ptr` : `char*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L180](./src/common/net.cpp#L180))<br>`left` : `size_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L181](./src/common/net.cpp#L181))<br>`got` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L183](./src/common/net.cpp#L183)) |
| `TcpListener::~TcpListener` ([L199](./src/common/net.cpp#L199)) | 无显式参数 | 无局部变量 |
| `TcpListener::close` ([L201](./src/common/net.cpp#L201)) | 无显式参数 | 无局部变量 |
| `TcpListener::start` ([L208](./src/common/net.cpp#L208)) | `port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`error` : `std::string&` - 错误信息输出 | `s` : `SOCKET` - 本函数的局部临时值；结合函数责任和使用点理解 ([L210](./src/common/net.cpp#L210))<br>`reuse` : `BOOL` - 本函数的局部临时值；结合函数责任和使用点理解 ([L215](./src/common/net.cpp#L215))<br>`address` : `sockaddr_in` - 本函数的局部临时值；结合函数责任和使用点理解 ([L217](./src/common/net.cpp#L217))<br>`nonBlocking` : `u_long` - 本函数的局部临时值；结合函数责任和使用点理解 ([L231](./src/common/net.cpp#L231)) |
| `TcpListener::acceptOne` ([L237](./src/common/net.cpp#L237)) | `error` : `std::string&` - 错误信息输出 | `clientAddress` : `sockaddr_in` - 本函数的局部临时值；结合函数责任和使用点理解 ([L238](./src/common/net.cpp#L238))<br>`length` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L239](./src/common/net.cpp#L239))<br>`client` : `SOCKET` - 本函数的局部临时值；结合函数责任和使用点理解 ([L240](./src/common/net.cpp#L240))<br>`err` : `int` - 错误文本或错误结果 ([L242](./src/common/net.cpp#L242))<br>`blocking` : `u_long` - 本函数的局部临时值；结合函数责任和使用点理解 ([L247](./src/common/net.cpp#L247)) |
| `sendJson` ([L252](./src/common/net.cpp#L252)) | `socket` : `NetSocket&` - 网络套接字<br>`mutex` : `std::mutex&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`message` : `const json&` - 待处理或待发送消息<br>`error` : `std::string&` - 错误信息输出 | `payload` : `std::string` - 序列化消息主体 ([L253](./src/common/net.cpp#L253))<br>`frame` : `std::vector&lt;char&gt;` - 网络帧 ([L258](./src/common/net.cpp#L258))<br>`lock` : `std::lock_guard&lt;std::mutex&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L261](./src/common/net.cpp#L261)) |
| `recvJson` ([L265](./src/common/net.cpp#L265)) | `socket` : `NetSocket&` - 网络套接字<br>`message` : `json&` - 待处理或待发送消息<br>`error` : `std::string&` - 错误信息输出 | `size` : `uint32_t` - 尺寸或容量 ([L268](./src/common/net.cpp#L268))<br>`payload` : `std::string` - 序列化消息主体 ([L277](./src/common/net.cpp#L277)) |
| `approach` ([L9](./src/common/physics.cpp#L9)) | `value` : `float` - 当前处理值<br>`target` : `float` - 本次操作的目标<br>`delta` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `hasGroundAt` ([L15](./src/common/physics.cpp#L15)) | `world` : `const World&` - 世界状态<br>`x` : `float` - X 坐标<br>`y` : `float` - Y 坐标 | `dx` : `float` - X 坐标偏移 ([L17](./src/common/physics.cpp#L17))<br>`bx` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L18](./src/common/physics.cpp#L18))<br>`by` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L19](./src/common/physics.cpp#L19)) |
| `hasGroundSupport` ([L27](./src/common/physics.cpp#L27)) | `world` : `const World&` - 世界状态<br>`body` : `const PlayerBody&` - 玩家/实体物理状态 | 无局部变量 |
| `tickPlayer` ([L31](./src/common/physics.cpp#L31)) | `world` : `const World&` - 世界状态<br>`body` : `PlayerBody&` - 玩家/实体物理状态<br>`input` : `const MoveInput&` - 输入状态<br>`creative` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `dt` : `const float` - 本次更新的时间步长 ([L32](./src/common/physics.cpp#L32))<br>`inWater` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L33](./src/common/physics.cpp#L33))<br>`inLava` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L34](./src/common/physics.cpp#L34))<br>`height` : `float` - 几何高度 ([L39](./src/common/physics.cpp#L39))<br>`speed` : `float` - 移动或采掘速度 ([L42](./src/common/physics.cpp#L42))<br>`targetVx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L50](./src/common/physics.cpp#L50))<br>`accel` : `float` - 加速度 ([L52](./src/common/physics.cpp#L52))<br>`nx` : `float` - 下一步候选 X 坐标 ([L80](./src/common/physics.cpp#L80))<br>`ny` : `float` - 下一步候选 Y 坐标 ([L100](./src/common/physics.cpp#L100)) |
| `hash64` ([L14](./src/common/world.cpp#L14)) | `x` : `uint64_t` - X 坐标 | 无局部变量 |
| `hashFloat` ([L21](./src/common/world.cpp#L21)) | `x` : `int64_t` - X 坐标<br>`y` : `int64_t` - Y 坐标<br>`seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 | `h` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L22](./src/common/world.cpp#L22)) |
| `smooth` ([L27](./src/common/world.cpp#L27)) | `t` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `valueNoise1` ([L29](./src/common/world.cpp#L29)) | `x` : `float` - X 坐标<br>`seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 | `x0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L30](./src/common/world.cpp#L30))<br>`t` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L31](./src/common/world.cpp#L31))<br>`a` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L32](./src/common/world.cpp#L32))<br>`b` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L33](./src/common/world.cpp#L33)) |
| `valueNoise2` ([L37](./src/common/world.cpp#L37)) | `x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 | `x0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L38](./src/common/world.cpp#L38))<br>`y0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L39](./src/common/world.cpp#L39))<br>`tx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L40](./src/common/world.cpp#L40))<br>`ty` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L41](./src/common/world.cpp#L41))<br>`a` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L42](./src/common/world.cpp#L42))<br>`b` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L43](./src/common/world.cpp#L43))<br>`c` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L44](./src/common/world.cpp#L44))<br>`d` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L45](./src/common/world.cpp#L45))<br>`ab` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L46](./src/common/world.cpp#L46))<br>`cd` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L47](./src/common/world.cpp#L47)) |
| `fbm2` ([L51](./src/common/world.cpp#L51)) | `x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `U` ([L57](./src/common/world.cpp#L57)) | `id` : `BlockId` - 对象或物品标识 | 无局部变量 |
| `bg_` ([L72](./src/common/world.cpp#L72)) | `height` : `static_cast&lt;size_t&gt;(width) *` - 几何高度<br>`AIR` : `parameter` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `World::clear` ([L74](./src/common/world.cpp#L74)) | `fill` : `BlockId` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `World::fg` ([L79](./src/common/world.cpp#L79)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | 无局部变量 |
| `World::bg` ([L84](./src/common/world.cpp#L84)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | 无局部变量 |
| `World::setFg` ([L89](./src/common/world.cpp#L89)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标<br>`block` : `BlockId` - 方块 ID 或方块定义 | 无局部变量 |
| `World::setBg` ([L93](./src/common/world.cpp#L93)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标<br>`block` : `BlockId` - 方块 ID 或方块定义 | 无局部变量 |
| `World::solid` ([L97](./src/common/world.cpp#L97)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | 无局部变量 |
| `World::opaque` ([L104](./src/common/world.cpp#L104)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | 无局部变量 |
| `World::water` ([L109](./src/common/world.cpp#L109)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | 无局部变量 |
| `World::lava` ([L110](./src/common/world.cpp#L110)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | 无局部变量 |
| `World::collidesBox` ([L112](./src/common/world.cpp#L112)) | `x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `x0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L113](./src/common/world.cpp#L113))<br>`x1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L114](./src/common/world.cpp#L114))<br>`y0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L115](./src/common/world.cpp#L115))<br>`y1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L116](./src/common/world.cpp#L116)) |
| `World::boxInWater` ([L125](./src/common/world.cpp#L125)) | `x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `x0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L126](./src/common/world.cpp#L126))<br>`x1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L127](./src/common/world.cpp#L127))<br>`y0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L128](./src/common/world.cpp#L128))<br>`y1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L129](./src/common/world.cpp#L129)) |
| `World::boxInLava` ([L136](./src/common/world.cpp#L136)) | `x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`w` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`h` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `x0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L137](./src/common/world.cpp#L137))<br>`x1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L138](./src/common/world.cpp#L138))<br>`y0` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L139](./src/common/world.cpp#L139))<br>`y1` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L140](./src/common/world.cpp#L140)) |
| `World::surfaceY` ([L147](./src/common/world.cpp#L147)) | `x` : `int` - X 坐标 | 无局部变量 |
| `World::clearanceAbove` ([L155](./src/common/world.cpp#L155)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `n` : `int` - 数量或循环边界 ([L156](./src/common/world.cpp#L156)) |
| `World::generate` ([L164](./src/common/world.cpp#L164)) | `seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 | `rng` : `std::mt19937_64` - 本函数的局部临时值；结合函数责任和使用点理解 ([L167](./src/common/world.cpp#L167))<br>`heights` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L168](./src/common/world.cpp#L168))<br>`hills` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L173](./src/common/world.cpp#L173))<br>`h` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L176](./src/common/world.cpp#L176))<br>`biome` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L178](./src/common/world.cpp#L178))<br>`desert` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L180](./src/common/world.cpp#L180))<br>`surface` : `BlockId` - 本函数的局部临时值；结合函数责任和使用点理解 ([L183](./src/common/world.cpp#L183))<br>`under` : `BlockId` - 本函数的局部临时值；结合函数责任和使用点理解 ([L184](./src/common/world.cpp#L184))<br>`block` : `BlockId` - 方块 ID 或方块定义 ([L186](./src/common/world.cpp#L186))<br>`h` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L197](./src/common/world.cpp#L197))<br>`n1` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L199](./src/common/world.cpp#L199))<br>`n2` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L200](./src/common/world.cpp#L200))<br>`vein` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L207](./src/common/world.cpp#L207))<br>`x` : `int` - X 坐标 ([L211](./src/common/world.cpp#L211))<br>`y` : `int` - Y 坐标 ([L212](./src/common/world.cpp#L212))<br>`h` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L229](./src/common/world.cpp#L229))<br>`spawnCenter` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L237](./src/common/world.cpp#L237))<br>`treeTimer` : `int` - 计时/进度值 ([L240](./src/common/world.cpp#L240))<br>`h` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L246](./src/common/world.cpp#L246))<br>`density` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L249](./src/common/world.cpp#L249))<br>`trunk` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L251](./src/common/world.cpp#L251))<br>`top` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L252](./src/common/world.cpp#L252))<br>`radius` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L260](./src/common/world.cpp#L260))<br>`lx` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L263](./src/common/world.cpp#L263))<br>`ly` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L264](./src/common/world.cpp#L264))<br>`h` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L276](./src/common/world.cpp#L276))<br>`above` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L278](./src/common/world.cpp#L278))<br>`r` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L280](./src/common/world.cpp#L280))<br>`sx` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L287](./src/common/world.cpp#L287))<br>`sh` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L290](./src/common/world.cpp#L290))<br>`x` : `int` - X 坐标 ([L292](./src/common/world.cpp#L292))<br>`y` : `int` - Y 坐标 ([L293](./src/common/world.cpp#L293))<br>`x` : `int` - X 坐标 ([L302](./src/common/world.cpp#L302)) |
| `World::encodeFg` ([L310](./src/common/world.cpp#L310)) | 无显式参数 | `runs` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L311](./src/common/world.cpp#L311))<br>`current` : `uint16_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L313](./src/common/world.cpp#L313))<br>`count` : `int` - 数量计数 ([L314](./src/common/world.cpp#L314)) |
| `World::encodeBg` ([L330](./src/common/world.cpp#L330)) | 无显式参数 | `runs` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L331](./src/common/world.cpp#L331))<br>`current` : `uint16_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L333](./src/common/world.cpp#L333))<br>`count` : `int` - 数量计数 ([L334](./src/common/world.cpp#L334)) |
| `World::decodeRuns` ([L350](./src/common/world.cpp#L350)) | `runs` : `const std::vector&lt;int&gt;&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`expected` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 | `result` : `std::vector&lt;uint16_t&gt;` - 计算或解析结果 ([L351](./src/common/world.cpp#L351))<br>`value` : `int` - 当前处理值 ([L354](./src/common/world.cpp#L354))<br>`count` : `int` - 数量计数 ([L355](./src/common/world.cpp#L355)) |
| `World::save` ([L364](./src/common/world.cpp#L364)) | `path` : `const std::string&` - 文件或资源路径<br>`time` : `uint64_t` - 世界时间或计时值 | `ec` : `std::error_code` - 系统错误状态 ([L365](./src/common/world.cpp#L365))<br>`out` : `std::ofstream` - 本函数计算/生成的结果 ([L367](./src/common/world.cpp#L367))<br>`header` : `Header` - 协议/文件头 ([L369](./src/common/world.cpp#L369)) |
| `World::load` ([L382](./src/common/world.cpp#L382)) | `path` : `const std::string&` - 文件或资源路径<br>`time` : `uint64_t&` - 世界时间或计时值 | `in` : `std::ifstream` - 本函数的局部临时值；结合函数责任和使用点理解 ([L383](./src/common/world.cpp#L383))<br>`header` : `Header` - 协议/文件头 ([L385](./src/common/world.cpp#L385)) |
| `logLine` ([L43](./src/server/main.cpp#L43)) | `text` : `const std::string&` - 文本内容 | `lock` : `std::lock_guard&lt;std::mutex&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L44](./src/server/main.cpp#L44)) |
| `consoleHandler` ([L48](./src/server/main.cpp#L48)) | `DWORD` : `parameter` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `fresh_` ([L165](./src/server/main.cpp#L165)) | `fresh` : `parameter` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `~Server` ([L167](./src/server/main.cpp#L167)) | 无显式参数 | 无局部变量 |
| `init` ([L169](./src/server/main.cpp#L169)) | `error` : `std::string&` - 错误信息输出 | `loaded` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L172](./src/server/main.cpp#L172))<br>`seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L179](./src/server/main.cpp#L179))<br>`rd` : `std::random_device` - 本函数的局部临时值；结合函数责任和使用点理解 ([L181](./src/server/main.cpp#L181))<br>`legacy` : `std::filesystem::path` - 本函数的局部临时值；结合函数责任和使用点理解 ([L194](./src/server/main.cpp#L194)) |
| `run` ([L206](./src/server/main.cpp#L206)) | 无显式参数 | `clock` : `using` - 本函数的局部临时值；结合函数责任和使用点理解 ([L208](./src/server/main.cpp#L208))<br>`nextTick` : `auto` - 计时/进度值 ([L209](./src/server/main.cpp#L209))<br>`now` : `auto` - 当前时间点 ([L217](./src/server/main.cpp#L217)) |
| `shutdown` ([L228](./src/server/main.cpp#L228)) | 无显式参数 | 无局部变量 |
| `pollConsole` ([L277](./src/server/main.cpp#L277)) | 无显式参数 | `ch` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L281](./src/server/main.cpp#L281)) |
| `acceptClients` ([L298](./src/server/main.cpp#L298)) | 无显式参数 | `error` : `std::string` - 错误信息输出 ([L300](./src/server/main.cpp#L300))<br>`socket` : `auto` - 网络套接字 ([L301](./src/server/main.cpp#L301))<br>`id` : `int` - 对象或物品标识 ([L304](./src/server/main.cpp#L304))<br>`deny` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L306](./src/server/main.cpp#L306))<br>`temporary` : `std::mutex` - 本函数的局部临时值；结合函数责任和使用点理解 ([L307](./src/server/main.cpp#L307))<br>`ignored` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L308](./src/server/main.cpp#L308))<br>`client` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L313](./src/server/main.cpp#L313))<br>`raw` : `Client*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L316](./src/server/main.cpp#L316))<br>`message` : `json` - 待处理或待发送消息 ([L319](./src/server/main.cpp#L319))<br>`error` : `std::string` - 错误信息输出 ([L320](./src/server/main.cpp#L320))<br>`lock` : `std::lock_guard&lt;std::mutex&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L326](./src/server/main.cpp#L326)) |
| `cleanupClients` ([L338](./src/server/main.cpp#L338)) | 无显式参数 | `dead` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L339](./src/server/main.cpp#L339))<br>`id` : `int` - 对象或物品标识 ([L343](./src/server/main.cpp#L343))<br>`it` : `auto` - 容器迭代器 ([L344](./src/server/main.cpp#L344))<br>`client` : `Client&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L346](./src/server/main.cpp#L346))<br>`player` : `Player&` - 玩家状态 ([L348](./src/server/main.cpp#L348)) |
| `drainMessages` ([L366](./src/server/main.cpp#L366)) | 无显式参数 | `messages` : `std::deque&lt;Message&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L367](./src/server/main.cpp#L367))<br>`lock` : `std::lock_guard&lt;std::mutex&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L369](./src/server/main.cpp#L369))<br>`message` : `auto&` - 待处理或待发送消息 ([L372](./src/server/main.cpp#L372))<br>`clientIt` : `auto` - 容器迭代器/遍历位置 ([L373](./src/server/main.cpp#L373))<br>`client` : `Client&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L375](./src/server/main.cpp#L375))<br>`payload` : `const json&` - 序列化消息主体 ([L378](./src/server/main.cpp#L378))<br>`type` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L379](./src/server/main.cpp#L379))<br>`playerIt` : `auto` - 容器迭代器/遍历位置 ([L384](./src/server/main.cpp#L384))<br>`player` : `Player&` - 玩家状态 ([L386](./src/server/main.cpp#L386))<br>`input` : `MoveInput` - 输入状态 ([L388](./src/server/main.cpp#L388))<br>`slot` : `int` - 协议槽位编号 ([L406](./src/server/main.cpp#L406))<br>`x` : `int` - X 坐标 ([L417](./src/server/main.cpp#L417))<br>`y` : `int` - Y 坐标 ([L418](./src/server/main.cpp#L418))<br>`gui` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L419](./src/server/main.cpp#L419))<br>`gui` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L424](./src/server/main.cpp#L424))<br>`x` : `int` - X 坐标 ([L427](./src/server/main.cpp#L427))<br>`y` : `int` - Y 坐标 ([L428](./src/server/main.cpp#L428))<br>`block` : `BlockId` - 方块 ID 或方块定义 ([L430](./src/server/main.cpp#L430)) |
| `loadPlayerData` ([L458](./src/server/main.cpp#L458)) | 无显式参数 | `in` : `std::ifstream` - 本函数的局部临时值；结合函数责任和使用点理解 ([L459](./src/server/main.cpp#L459))<br>`data` : `json` - 输入/输出数据 ([L461](./src/server/main.cpp#L461)) |
| `savePlayerData` ([L477](./src/server/main.cpp#L477)) | 无显式参数 | `slots` : `json` - 协议槽位或物品栏索引 ([L482](./src/server/main.cpp#L482))<br>`stack` : `const auto&` - 物品堆 ([L483](./src/server/main.cpp#L483))<br>`ec` : `std::error_code` - 系统错误状态 ([L495](./src/server/main.cpp#L495))<br>`temp` : `std::filesystem::path` - 本函数的局部临时值；结合函数责任和使用点理解 ([L497](./src/server/main.cpp#L497))<br>`out` : `std::ofstream` - 本函数计算/生成的结果 ([L500](./src/server/main.cpp#L500))<br>`root` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L502](./src/server/main.cpp#L502)) |
| `applySavedPlayer` ([L509](./src/server/main.cpp#L509)) | `player` : `Player&` - 玩家状态 | `it` : `auto` - 容器迭代器 ([L510](./src/server/main.cpp#L510))<br>`data` : `const json&` - 输入/输出数据 ([L512](./src/server/main.cpp#L512))<br>`index` : `int` - 数组或槽位索引 ([L516](./src/server/main.cpp#L516))<br>`value` : `const auto&` - 当前处理值 ([L517](./src/server/main.cpp#L517))<br>`id` : `std::string` - 对象或物品标识 ([L520](./src/server/main.cpp#L520))<br>`count` : `int` - 数量计数 ([L521](./src/server/main.cpp#L521))<br>`x` : `float` - X 坐标 ([L527](./src/server/main.cpp#L527))<br>`y` : `float` - Y 坐标 ([L528](./src/server/main.cpp#L528)) |
| `handleHello` ([L544](./src/server/main.cpp#L544)) | `client` : `Client&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`payload` : `const json&` - 序列化消息主体 | `version` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L546](./src/server/main.cpp#L546))<br>`rawName` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L551](./src/server/main.cpp#L551))<br>`name` : `std::string` - 显示名称 ([L552](./src/server/main.cpp#L552))<br>`suffix` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L555](./src/server/main.cpp#L555))<br>`taken` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L557](./src/server/main.cpp#L557))<br>`candidate` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L558](./src/server/main.cpp#L558))<br>`player` : `Player` - 玩家状态 ([L569](./src/server/main.cpp#L569))<br>`welcome` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L592](./src/server/main.cpp#L592))<br>`worldMessage` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L606](./src/server/main.cpp#L606))<br>`toast` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L620](./src/server/main.cpp#L620)) |
| `sendDenied` ([L624](./src/server/main.cpp#L624)) | `client` : `Client&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`reason` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | `message` : `json` - 待处理或待发送消息 ([L625](./src/server/main.cpp#L625)) |
| `sanitizeName` ([L630](./src/server/main.cpp#L630)) | `input` : `const std::string&` - 输入状态 | `name` : `std::string` - 显示名称 ([L631](./src/server/main.cpp#L631))<br>`c` : `char&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L633](./src/server/main.cpp#L633)) |
| `findSpawn` ([L642](./src/server/main.cpp#L642)) | 无显式参数 | `center` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L643](./src/server/main.cpp#L643))<br>`sign` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L645](./src/server/main.cpp#L645))<br>`x` : `int` - X 坐标 ([L646](./src/server/main.cpp#L646))<br>`y` : `int` - Y 坐标 ([L648](./src/server/main.cpp#L648))<br>`clear` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L651](./src/server/main.cpp#L651)) |
| `tick` ([L665](./src/server/main.cpp#L665)) | 无显式参数 | `input` : `MoveInput` - 输入状态 ([L672](./src/server/main.cpp#L672))<br>`fall` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L683](./src/server/main.cpp#L683)) |
| `containersDirtySaved` ([L710](./src/server/main.cpp#L710)) | 无显式参数 | 无局部变量 |
| `containerSlots` ([L712](./src/server/main.cpp#L712)) | `player` : `Player&` - 玩家状态<br>`slot` : `int` - 协议槽位编号<br>`ok` : `bool*` - 操作成功/有效标记 | 无局部变量 |
| `makeFurnaceVector` ([L717](./src/server/main.cpp#L717)) | `furnace` : `const FurnaceState&` - 熔炉状态 | `slots` : `std::vector&lt;ItemStack&gt;` - 协议槽位或物品栏索引 ([L718](./src/server/main.cpp#L718)) |
| `storeFurnaceSlots` ([L725](./src/server/main.cpp#L725)) | `pos` : `const ContainerPos&` - 世界/屏幕坐标 | `it` : `auto` - 容器迭代器 ([L726](./src/server/main.cpp#L726)) |
| `commitContainer` ([L736](./src/server/main.cpp#L736)) | `player` : `Player&` - 玩家状态 | 无局部变量 |
| `updateFurnaces` ([L741](./src/server/main.cpp#L741)) | 无显式参数 | `recipe` : `const SmeltRecipe*` - 配方定义 ([L745](./src/server/main.cpp#L745))<br>`limit` : `const int` - 允许上限 ([L746](./src/server/main.cpp#L746))<br>`canOutput` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L747](./src/server/main.cpp#L747))<br>`ticks` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L752](./src/server/main.cpp#L752))<br>`isBucket` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L754](./src/server/main.cpp#L754)) |
| `updatePlayerCooldowns` ([L780](./src/server/main.cpp#L780)) | `player` : `Player&` - 玩家状态 | 无局部变量 |
| `updatePlayerEnvironment` ([L823](./src/server/main.cpp#L823)) | `player` : `Player&` - 玩家状态 | `headInWater` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L825](./src/server/main.cpp#L825)) |
| `updateHunger` ([L841](./src/server/main.cpp#L841)) | `player` : `Player&` - 玩家状态 | `limit` : `int` - 允许上限 ([L844](./src/server/main.cpp#L844)) |
| `findGroundY` ([L851](./src/server/main.cpp#L851)) | `x` : `int` - X 坐标 | 无局部变量 |
| `seedPigs` ([L858](./src/server/main.cpp#L858)) | 无显式参数 | `count` : `int` - 数量计数 ([L859](./src/server/main.cpp#L859))<br>`x` : `int` - X 坐标 ([L862](./src/server/main.cpp#L862))<br>`ground` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L863](./src/server/main.cpp#L863))<br>`pig` : `Entity` - 本函数的局部临时值；结合函数责任和使用点理解 ([L867](./src/server/main.cpp#L867)) |
| `maybeSpawnMonsters` ([L881](./src/server/main.cpp#L881)) | 无显式参数 | `dayTime` : `uint64_t` - 计时/进度值 ([L883](./src/server/main.cpp#L883))<br>`night` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L884](./src/server/main.cpp#L884))<br>`zombies` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L886](./src/server/main.cpp#L886))<br>`e` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L887](./src/server/main.cpp#L887))<br>`alive` : `std::vector&lt;Player*&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L890](./src/server/main.cpp#L890))<br>`target` : `Player*` - 本次操作的目标 ([L897](./src/server/main.cpp#L897))<br>`side` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L898](./src/server/main.cpp#L898))<br>`distance` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L899](./src/server/main.cpp#L899))<br>`x` : `int` - X 坐标 ([L900](./src/server/main.cpp#L900))<br>`ground` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L902](./src/server/main.cpp#L902))<br>`zombie` : `Entity` - 本函数的局部临时值；结合函数责任和使用点理解 ([L905](./src/server/main.cpp#L905)) |
| `entityWidth` ([L917](./src/server/main.cpp#L917)) | `e` : `const Entity&` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `entityHeight` ([L923](./src/server/main.cpp#L923)) | `e` : `const Entity&` - 本函数的局部临时值；结合函数责任和使用点理解 | 无局部变量 |
| `moveEntity` ([L929](./src/server/main.cpp#L929)) | `e` : `Entity&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`dt` : `float` - 本次更新的时间步长 | `w` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L930](./src/server/main.cpp#L930))<br>`h` : `const float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L931](./src/server/main.cpp#L931))<br>`blockedX` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L932](./src/server/main.cpp#L932))<br>`landed` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L933](./src/server/main.cpp#L933))<br>`nx` : `float` - 下一步候选 X 坐标 ([L934](./src/server/main.cpp#L934))<br>`ny` : `float` - 下一步候选 Y 坐标 ([L944](./src/server/main.cpp#L944)) |
| `updateEntities` ([L961](./src/server/main.cpp#L961)) | 无显式参数 | `removeIds` : `std::vector&lt;int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L962](./src/server/main.cpp#L962))<br>`e` : `auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L963](./src/server/main.cpp#L963))<br>`dx` : `float` - X 坐标偏移 ([L975](./src/server/main.cpp#L975))<br>`dy` : `float` - Y 坐标偏移 ([L976](./src/server/main.cpp#L976))<br>`toast` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L980](./src/server/main.cpp#L980))<br>`r` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1003](./src/server/main.cpp#L1003))<br>`targetVx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1007](./src/server/main.cpp#L1007))<br>`blocked` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1009](./src/server/main.cpp#L1009))<br>`closest` : `Player*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1019](./src/server/main.cpp#L1019))<br>`best` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1020](./src/server/main.cpp#L1020))<br>`dx` : `float` - X 坐标偏移 ([L1024](./src/server/main.cpp#L1024))<br>`dy` : `float` - Y 坐标偏移 ([L1025](./src/server/main.cpp#L1025))<br>`dist` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1026](./src/server/main.cpp#L1026))<br>`dx` : `float` - X 坐标偏移 ([L1033](./src/server/main.cpp#L1033))<br>`dy` : `float` - Y 坐标偏移 ([L1034](./src/server/main.cpp#L1034))<br>`dir` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1035](./src/server/main.cpp#L1035))<br>`blocked` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1039](./src/server/main.cpp#L1039))<br>`wallAhead` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1043](./src/server/main.cpp#L1043))<br>`msg` : `json` - 网络消息 ([L1054](./src/server/main.cpp#L1054)) |
| `findEntity` ([L1078](./src/server/main.cpp#L1078)) | `id` : `int` - 对象或物品标识 | `e` : `auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1079](./src/server/main.cpp#L1079)) |
| `spawnItem` ([L1083](./src/server/main.cpp#L1083)) | `item` : `const std::string&` - 物品堆/物品标识数据<br>`count` : `int` - 数量计数<br>`x` : `float` - X 坐标<br>`y` : `float` - Y 坐标<br>`vx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解<br>`vy` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `e` : `Entity` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1085](./src/server/main.cpp#L1085)) |
| `withinReach` ([L1099](./src/server/main.cpp#L1099)) | `player` : `const Player&` - 玩家状态<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标<br>`reach` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 | `bx` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1100](./src/server/main.cpp#L1100))<br>`by` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1101](./src/server/main.cpp#L1101))<br>`dx` : `float` - X 坐标偏移 ([L1102](./src/server/main.cpp#L1102))<br>`dy` : `float` - Y 坐标偏移 ([L1103](./src/server/main.cpp#L1103)) |
| `handleBreak` ([L1107](./src/server/main.cpp#L1107)) | `player` : `Player&` - 玩家状态<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `block` : `BlockId` - 方块 ID 或方块定义 ([L1110](./src/server/main.cpp#L1110))<br>`def` : `const BlockDef&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1113](./src/server/main.cpp#L1113))<br>`event` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1119](./src/server/main.cpp#L1119))<br>`count` : `int` - 数量计数 ([L1122](./src/server/main.cpp#L1122)) |
| `dropContainerContents` ([L1135](./src/server/main.cpp#L1135)) | `block` : `BlockId` - 方块 ID 或方块定义<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `spill` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1136](./src/server/main.cpp#L1136))<br>`stack` : `const auto&` - 物品堆 ([L1137](./src/server/main.cpp#L1137))<br>`changed` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1143](./src/server/main.cpp#L1143))<br>`pos` : `ContainerPos` - 世界/屏幕坐标 ([L1145](./src/server/main.cpp#L1145))<br>`it` : `auto` - 容器迭代器 ([L1146](./src/server/main.cpp#L1146))<br>`pos` : `ContainerPos` - 世界/屏幕坐标 ([L1153](./src/server/main.cpp#L1153))<br>`it` : `auto` - 容器迭代器 ([L1154](./src/server/main.cpp#L1154)) |
| `intersectsPlayer` ([L1172](./src/server/main.cpp#L1172)) | `player` : `const Player&` - 玩家状态<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `left` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1173](./src/server/main.cpp#L1173))<br>`right` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1174](./src/server/main.cpp#L1174))<br>`top` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1175](./src/server/main.cpp#L1175))<br>`bottom` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1176](./src/server/main.cpp#L1176))<br>`px0` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1177](./src/server/main.cpp#L1177))<br>`px1` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1178](./src/server/main.cpp#L1178))<br>`py0` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1179](./src/server/main.cpp#L1179))<br>`py1` : `float` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1180](./src/server/main.cpp#L1180)) |
| `handlePlace` ([L1184](./src/server/main.cpp#L1184)) | `player` : `Player&` - 玩家状态<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `stack` : `ItemStack&` - 物品堆 ([L1187](./src/server/main.cpp#L1187))<br>`block` : `BlockId` - 方块 ID 或方块定义 ([L1189](./src/server/main.cpp#L1189))<br>`current` : `BlockId` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1193](./src/server/main.cpp#L1193))<br>`def` : `const BlockDef&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1195](./src/server/main.cpp#L1195))<br>`supported` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1197](./src/server/main.cpp#L1197))<br>`d` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1199](./src/server/main.cpp#L1199))<br>`event` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1220](./src/server/main.cpp#L1220)) |
| `handleAttack` ([L1225](./src/server/main.cpp#L1225)) | `player` : `Player&` - 玩家状态<br>`entityId` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 | `e` : `Entity*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1227](./src/server/main.cpp#L1227))<br>`dx` : `float` - X 坐标偏移 ([L1229](./src/server/main.cpp#L1229))<br>`dy` : `float` - Y 坐标偏移 ([L1230](./src/server/main.cpp#L1230))<br>`held` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1233](./src/server/main.cpp#L1233))<br>`damage` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1236](./src/server/main.cpp#L1236))<br>`event` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1242](./src/server/main.cpp#L1242))<br>`x` : `int` - X 坐标 ([L1246](./src/server/main.cpp#L1246))<br>`y` : `int` - Y 坐标 ([L1247](./src/server/main.cpp#L1247))<br>`death` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1254](./src/server/main.cpp#L1254))<br>`deadId` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1257](./src/server/main.cpp#L1257)) |
| `openContainer` ([L1268](./src/server/main.cpp#L1268)) | `player` : `Player&` - 玩家状态<br>`gui` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `pos` : `ContainerPos` - 世界/屏幕坐标 ([L1289](./src/server/main.cpp#L1289))<br>`furnace` : `FurnaceState` - 熔炉状态 ([L1291](./src/server/main.cpp#L1291)) |
| `reachable` ([L1304](./src/server/main.cpp#L1304)) | `player` : `const Player&` - 玩家状态<br>`x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `dx` : `float` - X 坐标偏移 ([L1305](./src/server/main.cpp#L1305))<br>`dy` : `float` - Y 坐标偏移 ([L1306](./src/server/main.cpp#L1306)) |
| `containerStillValid` ([L1310](./src/server/main.cpp#L1310)) | `player` : `const Player&` - 玩家状态 | `block` : `BlockId` - 方块 ID 或方块定义 ([L1313](./src/server/main.cpp#L1313)) |
| `returnGridToInventory` ([L1318](./src/server/main.cpp#L1318)) | `player` : `Player&` - 玩家状态 | `stack` : `auto&` - 物品堆 ([L1319](./src/server/main.cpp#L1319))<br>`remaining` : `int` - 尚未处理的数量 ([L1321](./src/server/main.cpp#L1321)) |
| `closeInventory` ([L1329](./src/server/main.cpp#L1329)) | `player` : `Player&` - 玩家状态 | `remaining` : `int` - 尚未处理的数量 ([L1331](./src/server/main.cpp#L1331))<br>`closed` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1344](./src/server/main.cpp#L1344)) |
| `containerJson` ([L1348](./src/server/main.cpp#L1348)) | `player` : `Player&` - 玩家状态 | `message` : `json` - 待处理或待发送消息 ([L1349](./src/server/main.cpp#L1349))<br>`size` : `const int` - 尺寸或容量 ([L1363](./src/server/main.cpp#L1363))<br>`grid` : `std::vector&lt;ItemStack&gt;` - 合成网格数据 ([L1364](./src/server/main.cpp#L1364))<br>`resultId` : `const std::string` - 本函数计算/生成的结果 ([L1365](./src/server/main.cpp#L1365))<br>`result` : `ItemStack` - 计算或解析结果 ([L1366](./src/server/main.cpp#L1366))<br>`recipe` : `const auto&` - 配方定义 ([L1368](./src/server/main.cpp#L1368))<br>`slots` : `json` - 协议槽位或物品栏索引 ([L1377](./src/server/main.cpp#L1377))<br>`stack` : `const auto&` - 物品堆 ([L1378](./src/server/main.cpp#L1378))<br>`grid` : `json` - 合成网格数据 ([L1381](./src/server/main.cpp#L1381))<br>`stack` : `const auto&` - 物品堆 ([L1382](./src/server/main.cpp#L1382))<br>`it` : `auto` - 容器迭代器 ([L1385](./src/server/main.cpp#L1385))<br>`furnace` : `const FurnaceState&` - 熔炉状态 ([L1387](./src/server/main.cpp#L1387))<br>`it` : `auto` - 容器迭代器 ([L1399](./src/server/main.cpp#L1399))<br>`chest` : `json` - 箱子槽位数据 ([L1401](./src/server/main.cpp#L1401)) |
| `sendContainer` ([L1412](./src/server/main.cpp#L1412)) | `player` : `Player&` - 玩家状态 | 无局部变量 |
| `tickOpenContainers` ([L1416](./src/server/main.cpp#L1416)) | 无显式参数 | 无局部变量 |
| `recipeOutputForGrid` ([L1430](./src/server/main.cpp#L1430)) | `grid` : `const std::vector&lt;ItemStack&gt;&` - 合成网格数据<br>`gridSize` : `int` - 数量或容量计数 | `recipe` : `const auto&` - 配方定义 ([L1434](./src/server/main.cpp#L1434))<br>`need` : `std::map&lt;std::string, int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1436](./src/server/main.cpp#L1436))<br>`have` : `std::map&lt;std::string, int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1438](./src/server/main.cpp#L1438))<br>`valid` : `bool` - 校验结果 ([L1439](./src/server/main.cpp#L1439))<br>`stack` : `const auto&` - 物品堆 ([L1440](./src/server/main.cpp#L1440)) |
| `consumeGridForCraft` ([L1458](./src/server/main.cpp#L1458)) | `player` : `Player&` - 玩家状态<br>`gridSize` : `int` - 数量或容量计数<br>`recipeId` : `const std::string&` - 配方标识 | `recipe` : `const auto&` - 配方定义 ([L1459](./src/server/main.cpp#L1459))<br>`need` : `std::map&lt;std::string, int&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1461](./src/server/main.cpp#L1461))<br>`stack` : `auto&` - 物品堆 ([L1463](./src/server/main.cpp#L1463))<br>`it` : `auto` - 容器迭代器 ([L1465](./src/server/main.cpp#L1465))<br>`take` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1467](./src/server/main.cpp#L1467)) |
| `craftFromGrid` ([L1477](./src/server/main.cpp#L1477)) | `player` : `Player&` - 玩家状态<br>`gridSize` : `int` - 数量或容量计数<br>`all` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `grid` : `std::vector&lt;ItemStack&gt;` - 合成网格数据 ([L1478](./src/server/main.cpp#L1478))<br>`recipeId` : `std::string` - 配方标识 ([L1480](./src/server/main.cpp#L1480))<br>`recipe` : `const Recipe*` - 配方定义 ([L1482](./src/server/main.cpp#L1482))<br>`candidate` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1483](./src/server/main.cpp#L1483))<br>`limit` : `const int` - 允许上限 ([L1487](./src/server/main.cpp#L1487))<br>`rounds` : `const int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1490](./src/server/main.cpp#L1490))<br>`next` : `std::vector&lt;ItemStack&gt;` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1494](./src/server/main.cpp#L1494)) |
| `gridLimit` ([L1504](./src/server/main.cpp#L1504)) | `player` : `Player&` - 玩家状态 | 无局部变量 |
| `handleSlotClick` ([L1508](./src/server/main.cpp#L1508)) | `player` : `Player&` - 玩家状态<br>`slot` : `int` - 协议槽位编号<br>`button` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`shift` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `tableOpen` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1516](./src/server/main.cpp#L1516))<br>`inventoryOpen` : `const bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1517](./src/server/main.cpp#L1517))<br>`gridSize` : `const int` - 数量或容量计数 ([L1518](./src/server/main.cpp#L1518))<br>`storage` : `std::vector&lt;ItemStack&gt;*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1527](./src/server/main.cpp#L1527))<br>`index` : `int` - 数组或槽位索引 ([L1528](./src/server/main.cpp#L1528))<br>`it` : `auto` - 容器迭代器 ([L1541](./src/server/main.cpp#L1541))<br>`it` : `auto` - 容器迭代器 ([L1550](./src/server/main.cpp#L1550))<br>`target` : `ItemStack&` - 本次操作的目标 ([L1561](./src/server/main.cpp#L1561))<br>`moveInto` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1564](./src/server/main.cpp#L1564))<br>`count` : `int` - 数量计数 ([L1566](./src/server/main.cpp#L1566))<br>`limit` : `const int` - 允许上限 ([L1567](./src/server/main.cpp#L1567))<br>`d` : `ItemStack&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1569](./src/server/main.cpp#L1569))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1571](./src/server/main.cpp#L1571))<br>`d` : `ItemStack&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1577](./src/server/main.cpp#L1577))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1579](./src/server/main.cpp#L1579))<br>`it` : `auto` - 容器迭代器 ([L1594](./src/server/main.cpp#L1594))<br>`furnace` : `FurnaceState&` - 熔炉状态 ([L1596](./src/server/main.cpp#L1596))<br>`dst` : `ItemStack&` - 目标容器数据 ([L1598](./src/server/main.cpp#L1598))<br>`limit` : `const int` - 允许上限 ([L1599](./src/server/main.cpp#L1599))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1600](./src/server/main.cpp#L1600))<br>`dst` : `ItemStack&` - 目标容器数据 ([L1607](./src/server/main.cpp#L1607))<br>`limit` : `const int` - 允许上限 ([L1608](./src/server/main.cpp#L1608))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1609](./src/server/main.cpp#L1609))<br>`it` : `auto` - 容器迭代器 ([L1617](./src/server/main.cpp#L1617))<br>`limit` : `const int` - 允许上限 ([L1635](./src/server/main.cpp#L1635))<br>`moved` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1636](./src/server/main.cpp#L1636))<br>`take` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1646](./src/server/main.cpp#L1646))<br>`limit` : `const int` - 允许上限 ([L1656](./src/server/main.cpp#L1656)) |
| `handleSwapHotbar` ([L1670](./src/server/main.cpp#L1670)) | `player` : `Player&` - 玩家状态<br>`slot` : `int` - 协议槽位编号<br>`hotbar` : `int` - 快捷栏槽编号 | `storage` : `std::vector&lt;ItemStack&gt;*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1677](./src/server/main.cpp#L1677))<br>`index` : `int` - 数组或槽位索引 ([L1678](./src/server/main.cpp#L1678))<br>`it` : `auto` - 容器迭代器 ([L1687](./src/server/main.cpp#L1687))<br>`it` : `auto` - 容器迭代器 ([L1695](./src/server/main.cpp#L1695))<br>`target` : `ItemStack&` - 本次操作的目标 ([L1703](./src/server/main.cpp#L1703))<br>`hot` : `ItemStack&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1704](./src/server/main.cpp#L1704))<br>`limit` : `const int` - 允许上限 ([L1711](./src/server/main.cpp#L1711)) |
| `handleCraft` ([L1726](./src/server/main.cpp#L1726)) | `player` : `Player&` - 玩家状态<br>`recipeId` : `const std::string&` - 配方标识 | `grid` : `std::vector&lt;ItemStack&gt;` - 合成网格数据 ([L1729](./src/server/main.cpp#L1729))<br>`recipe` : `const auto&` - 配方定义 ([L1739](./src/server/main.cpp#L1739))<br>`toast` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1758](./src/server/main.cpp#L1758)) |
| `handleDrop` ([L1766](./src/server/main.cpp#L1766)) | `player` : `Player&` - 玩家状态<br>`slot` : `int` - 协议槽位编号 | `stack` : `ItemStack&` - 物品堆 ([L1768](./src/server/main.cpp#L1768))<br>`id` : `std::string` - 对象或物品标识 ([L1770](./src/server/main.cpp#L1770)) |
| `handleEat` ([L1778](./src/server/main.cpp#L1778)) | `player` : `Player&` - 玩家状态<br>`slot` : `int` - 协议槽位编号 | `stack` : `ItemStack&` - 物品堆 ([L1780](./src/server/main.cpp#L1780))<br>`def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1782](./src/server/main.cpp#L1782))<br>`event` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1796](./src/server/main.cpp#L1796)) |
| `hurtPlayer` ([L1800](./src/server/main.cpp#L1800)) | `player` : `Player&` - 玩家状态<br>`damage` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`reason` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | `death` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1817](./src/server/main.cpp#L1817)) |
| `respawn` ([L1826](./src/server/main.cpp#L1826)) | `player` : `Player&` - 玩家状态 | `respawned` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1839](./src/server/main.cpp#L1839)) |
| `handleChat` ([L1845](./src/server/main.cpp#L1845)) | `player` : `Player&` - 玩家状态<br>`raw` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | `text` : `std::string` - 文本内容 ([L1846](./src/server/main.cpp#L1846)) |
| `splitWords` ([L1857](./src/server/main.cpp#L1857)) | `text` : `const std::string&` - 文本内容 | `words` : `std::vector&lt;std::string&gt;` - 指令词元 ([L1858](./src/server/main.cpp#L1858))<br>`stream` : `std::istringstream` - 文件或文本流 ([L1859](./src/server/main.cpp#L1859))<br>`word` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1860](./src/server/main.cpp#L1860)) |
| `executeCommand` ([L1865](./src/server/main.cpp#L1865)) | `player` : `Player&` - 玩家状态<br>`text` : `const std::string&` - 文本内容 | `words` : `std::vector&lt;std::string&gt;` - 指令词元 ([L1866](./src/server/main.cpp#L1866))<br>`command` : `std::string` - 指令名称/文本 ([L1868](./src/server/main.cpp#L1868))<br>`arg` : `auto` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1869](./src/server/main.cpp#L1869))<br>`value` : `std::string` - 当前处理值 ([L1877](./src/server/main.cpp#L1877))<br>`mode` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1889](./src/server/main.cpp#L1889))<br>`id` : `std::string` - 对象或物品标识 ([L1906](./src/server/main.cpp#L1906))<br>`count` : `int` - 数量计数 ([L1907](./src/server/main.cpp#L1907))<br>`def` : `const ItemDef*` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1911](./src/server/main.cpp#L1911))<br>`left` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1916](./src/server/main.cpp#L1916))<br>`x` : `float` - X 坐标 ([L1921](./src/server/main.cpp#L1921))<br>`y` : `float` - Y 坐标 ([L1921](./src/server/main.cpp#L1921))<br>`list` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1954](./src/server/main.cpp#L1954))<br>`message` : `std::string` - 待处理或待发送消息 ([L1970](./src/server/main.cpp#L1970)) |
| `handleConsoleCommand` ([L1978](./src/server/main.cpp#L1978)) | `raw` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解 | `line` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1979](./src/server/main.cpp#L1979))<br>`words` : `std::vector&lt;std::string&gt;` - 指令词元 ([L1982](./src/server/main.cpp#L1982))<br>`command` : `std::string` - 指令名称/文本 ([L1983](./src/server/main.cpp#L1983))<br>`list` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L1993](./src/server/main.cpp#L1993))<br>`message` : `std::string` - 待处理或待发送消息 ([L2010](./src/server/main.cpp#L2010)) |
| `containersPath` ([L2018](./src/server/main.cpp#L2018)) | 无显式参数 | 无局部变量 |
| `loadContainers` ([L2022](./src/server/main.cpp#L2022)) | 无显式参数 | `in` : `std::ifstream` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2023](./src/server/main.cpp#L2023))<br>`data` : `json` - 输入/输出数据 ([L2025](./src/server/main.cpp#L2025))<br>`furnaces` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2033](./src/server/main.cpp#L2033))<br>`chests` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2034](./src/server/main.cpp#L2034))<br>`entry` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2036](./src/server/main.cpp#L2036))<br>`pos` : `ContainerPos` - 世界/屏幕坐标 ([L2038](./src/server/main.cpp#L2038))<br>`furnace` : `FurnaceState` - 熔炉状态 ([L2040](./src/server/main.cpp#L2040))<br>`entry` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2054](./src/server/main.cpp#L2054))<br>`pos` : `ContainerPos` - 世界/屏幕坐标 ([L2056](./src/server/main.cpp#L2056))<br>`slots` : `std::vector&lt;ItemStack&gt;` - 协议槽位或物品栏索引 ([L2058](./src/server/main.cpp#L2058))<br>`items` : `const json&` - 物品堆/物品标识数据 ([L2059](./src/server/main.cpp#L2059))<br>`index` : `int` - 数组或槽位索引 ([L2060](./src/server/main.cpp#L2060))<br>`item` : `const auto&` - 物品堆/物品标识数据 ([L2061](./src/server/main.cpp#L2061)) |
| `saveContainers` ([L2078](./src/server/main.cpp#L2078)) | 无显式参数 | `furnaces` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2079](./src/server/main.cpp#L2079))<br>`chests` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2093](./src/server/main.cpp#L2093))<br>`items` : `json` - 物品堆/物品标识数据 ([L2096](./src/server/main.cpp#L2096))<br>`stack` : `const auto&` - 物品堆 ([L2097](./src/server/main.cpp#L2097))<br>`root` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2100](./src/server/main.cpp#L2100))<br>`ec` : `std::error_code` - 系统错误状态 ([L2101](./src/server/main.cpp#L2101))<br>`temp` : `std::filesystem::path` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2103](./src/server/main.cpp#L2103))<br>`out` : `std::ofstream` - 本函数计算/生成的结果 ([L2106](./src/server/main.cpp#L2106)) |
| `saveWorld` ([L2115](./src/server/main.cpp#L2115)) | `announce` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 | `ec` : `std::error_code` - 系统错误状态 ([L2116](./src/server/main.cpp#L2116))<br>`ok` : `bool` - 操作成功/有效标记 ([L2118](./src/server/main.cpp#L2118)) |
| `broadcastChat` ([L2128](./src/server/main.cpp#L2128)) | `from` : `const std::string&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`text` : `const std::string&` - 文本内容 | `message` : `json` - 待处理或待发送消息 ([L2129](./src/server/main.cpp#L2129)) |
| `sendSystem` ([L2133](./src/server/main.cpp#L2133)) | `player` : `Player&` - 玩家状态<br>`text` : `const std::string&` - 文本内容 | `message` : `json` - 待处理或待发送消息 ([L2134](./src/server/main.cpp#L2134)) |
| `stackJson` ([L2138](./src/server/main.cpp#L2138)) | `stack` : `const ItemStack&` - 物品堆 | 无局部变量 |
| `stackFromJson` ([L2143](./src/server/main.cpp#L2143)) | `value` : `const json&` - 当前处理值 | `id` : `std::string` - 对象或物品标识 ([L2145](./src/server/main.cpp#L2145))<br>`count` : `int` - 数量计数 ([L2146](./src/server/main.cpp#L2146)) |
| `gridJson` ([L2151](./src/server/main.cpp#L2151)) | `stacks` : `const std::vector&lt;ItemStack&gt;&` - 物品堆/物品标识数据 | `result` : `json` - 计算或解析结果 ([L2152](./src/server/main.cpp#L2152))<br>`stack` : `const auto&` - 物品堆 ([L2153](./src/server/main.cpp#L2153)) |
| `sendInventory` ([L2159](./src/server/main.cpp#L2159)) | `player` : `Player&` - 玩家状态 | `slots` : `json` - 协议槽位或物品栏索引 ([L2160](./src/server/main.cpp#L2160))<br>`stack` : `const auto&` - 物品堆 ([L2161](./src/server/main.cpp#L2161))<br>`message` : `json` - 待处理或待发送消息 ([L2162](./src/server/main.cpp#L2162)) |
| `playerJson` ([L2171](./src/server/main.cpp#L2171)) | `player` : `const Player&` - 玩家状态 | 无局部变量 |
| `entityJson` ([L2189](./src/server/main.cpp#L2189)) | `e` : `const Entity&` - 本函数的局部临时值；结合函数责任和使用点理解 | `j` : `json` - 嵌套循环索引 ([L2190](./src/server/main.cpp#L2190)) |
| `sendSelfState` ([L2209](./src/server/main.cpp#L2209)) | `player` : `Player&` - 玩家状态 | `self` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2210](./src/server/main.cpp#L2210))<br>`message` : `json` - 待处理或待发送消息 ([L2228](./src/server/main.cpp#L2228)) |
| `broadcastSnapshot` ([L2232](./src/server/main.cpp#L2232)) | 无显式参数 | `players` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2233](./src/server/main.cpp#L2233))<br>`entities` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2238](./src/server/main.cpp#L2238))<br>`e` : `const auto&` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2239](./src/server/main.cpp#L2239))<br>`snapshot` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2240](./src/server/main.cpp#L2240)) |
| `sendStats` ([L2254](./src/server/main.cpp#L2254)) | `player` : `Player&` - 玩家状态 | `stats` : `json` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2255](./src/server/main.cpp#L2255)) |
| `broadcastBlock` ([L2268](./src/server/main.cpp#L2268)) | `x` : `int` - X 坐标<br>`y` : `int` - Y 坐标 | `message` : `json` - 待处理或待发送消息 ([L2269](./src/server/main.cpp#L2269)) |
| `broadcast` ([L2274](./src/server/main.cpp#L2274)) | `message` : `const json&` - 待处理或待发送消息 | 无局部变量 |
| `sendTo` ([L2282](./src/server/main.cpp#L2282)) | `client` : `Client&` - 本函数的局部临时值；结合函数责任和使用点理解<br>`message` : `const json&` - 待处理或待发送消息 | `error` : `std::string` - 错误信息输出 ([L2284](./src/server/main.cpp#L2284)) |
| `sendTo` ([L2292](./src/server/main.cpp#L2292)) | `playerId` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`message` : `const json&` - 待处理或待发送消息 | `it` : `auto` - 容器迭代器 ([L2293](./src/server/main.cpp#L2293)) |
| `main` ([L2301](./src/server/main.cpp#L2301)) | `argc` : `int` - 本函数的局部临时值；结合函数责任和使用点理解<br>`argv` : `char**` - 本函数的局部临时值；结合函数责任和使用点理解 | `port` : `int` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2302](./src/server/main.cpp#L2302))<br>`seed` : `uint64_t` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2303](./src/server/main.cpp#L2303))<br>`fresh` : `bool` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2304](./src/server/main.cpp#L2304))<br>`worldPath` : `std::filesystem::path` - 文件路径/文件状态 ([L2305](./src/server/main.cpp#L2305))<br>`arg` : `std::string` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2307](./src/server/main.cpp#L2307))<br>`server` : `Server` - 本函数的局部临时值；结合函数责任和使用点理解 ([L2319](./src/server/main.cpp#L2319))<br>`error` : `std::string` - 错误信息输出 ([L2320](./src/server/main.cpp#L2320)) |

## 维护约定

1. 容器贴图尺寸、客户端 hitbox 和服务端槽位编号必须同步修改；当前 ID 映射在服务端文件开头。
2. 容器中的库存、输出和进度以服务端为准。客户端不得本地扣料、直接产出或写箱子/熔炉状态。
3. 新增 GUI 时同步更新 `ContainerGui`、服务端打开/有效性校验、JSON 快照、槽位点击/数字键交换、客户端布局/绘制，以及本文槽位表。
4. `World` 和 `tickPlayer` 由客户端/服务端共用，物理或存档格式变更应同时验证两端。
5. 标准 Windows 构建命令见 [`README.md`](./README.md) 的“构建”小节；客户端输出在 `bin/`。
6. 新增/删除类、字段、协议常量或函数时，同步更新本文对应的“类型/成员/函数”表；源码移动后请重新核对文件与符号位置。
