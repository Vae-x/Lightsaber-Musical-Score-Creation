# Lightsaber Musical Score Creation

基于 Qt5 的 Windows 曲谱编辑器，首个实机验收目标为 PICO Neo 3 上的《星穹绿洲》。当前自用版已可运行，已有谱编辑与 MP3/MP4 新歌制作的桌面闭环已验证。

## 目录结构

```text
Lightsaber Musical Score Creation/
├── src/
│   ├── app/           # 程序入口、启动和模块组装
│   ├── core/          # 核心源码：曲谱模型、时间轴、编辑规则
│   ├── gui/           # Qt 窗口、控件、视图与 .ui 文件
│   └── CMakeLists.txt # 可执行目标、源码清单与运行库部署
├── resources/         # 图标、图片、样式、翻译与 .qrc 文件
├── docs/              # 需求、架构说明和历史资料
├── tests/             # 测试代码及小型测试数据
├── third_party/       # 第三方库及其许可证
├── scripts/           # 构建、打包与开发辅助脚本
├── samples/
│   └── beatmaps/      # 本地曲谱样本，内容不提交 Git
├── dist/              # 本机便携目录与 ZIP，不提交 Git
├── CMakeLists.txt     # 整个项目的 CMake 入口
├── .gitignore
└── README.md
```

后续核心源码放在 **`src/core/`**。窗口和 Qt Designer 的 `.ui` 文件放在 `src/gui/`，`main.cpp` 等入口文件放在 `src/app/`。详细说明见 [工程目录说明](docs/工程目录说明.md)。

入口为 `src/app/main.cpp`，主窗口为 `src/gui/MainWindow.h/.cpp/.ui`，编辑画布为 `src/gui/EditorViews.h/.cpp`。曲谱文档、时间换算、工程存储、音频服务和节奏分析放在 `src/core/`。新增核心类时，将 `.h` 和 `.cpp` 放在一起，并添加到 `src/CMakeLists.txt` 的源码清单中。

在 CLion 中可打开根目录的 `CMakeLists.txt`；当前已经从 `src/` 打开的工程也可继续使用。可执行目标名称为 `src`。

## 项目状态

用户已整体确认 [首版需求与架构](docs/首版需求与架构.md)，并授权实现。曲谱文档、工程保存/恢复、基础编辑、时间模型、音频处理、节奏估计及工作台已经实现。**本机 Debug/Release 构建、五项 CTest 与便携运行验证均已通过**：Windows 11 / Qt 5.12.12，系统纯 PATH 下导入、音频、编辑、撤销/重做、保存、导出和重新打开正常。此前交接文档中的 Electron 方案为历史建议。

数据测试覆盖本地 **10 个 ZIP、20 张难度**的原样导出，逐文件 SHA256 一致；真实 Dry Hands 修改单个方向只改变对应字段，撤销后谱面恢复原字节。工程恢复、未知数据保留、恶意 ZIP 路径与整批保护检查也已通过。

真实鼠标和快捷键测试覆盖网格放置、选择、属性修改、删除、撤销/重做、难度切换、混合保护选区整批拒绝，以及轨道画面的实际色块绘制。音频模块已验证自制 MP3、双音轨 MP4、裁剪转 Ogg/Vorbis、波形、保持音高的慢放、节拍器、循环/定位和失败/取消处理。

新歌界面流程已实际验证 MP3、MP4 第二音轨、1～4 秒裁剪、预听解码期间立即转换、创建单难度空谱及自动估拍。保存/导出/重开后，原媒体 SHA256、音轨和裁剪记录一致；原媒体快照不进入游戏导出目录。

弧线、链条、灯光及模组数据按原格式保留，并保护可能破坏其关联的基础物件；首版暂不提供这些高级内容的专属编辑或完整效果绘制。轨道预览用于观察普通方块、炸弹和墙，游戏效果以头显实测为准。

## 启动自用版

本机最新交付：[Windows 便携 ZIP](dist/Lightsaber-Musical-Score-Creation-Windows-20261001.zip)（约 86 MiB），或直接打开[已解压目录](<dist/Lightsaber Musical Score Creation-20261001-155550/>)中的 [LightsaberMusicalScoreCreation.exe](<dist/Lightsaber Musical Score Creation-20261001-155550/LightsaberMusicalScoreCreation.exe>)。

解压整份 ZIP 后双击 **`LightsaberMusicalScoreCreation.exe`**；保留同目录的 DLL、`audio/`、`platforms/`、`tools/` 等文件。使用者无需开发环境、Python 或自行安装 FFmpeg。最新目录已在仅有 Windows 系统 PATH 的环境中运行验证，ZIP 内资产与该目录经 SHA256 比对一致；交付文件不提交 Git。

开发者用 `scripts/package-portable.ps1` 从 Release 构建生成新的 `dist/Lightsaber Musical Score Creation*/` 便携目录，保留旧包；完整目录压缩为 Windows ZIP 交付。具体导入、放块、保存工程和导出步骤见 [使用说明](docs/使用说明.md)。

## 已确认的目标

- 在 Windows 10/11 上解压即用、离线运行，使用者无需自行安装 Python。
- 首版先供自己使用，适配《星穹绿洲》；导出歌曲文件夹后手动复制到头显。
- MP3/MP4 导入、音轨选择、片段裁剪、离线自动转换和空白新谱制作。
- 游戏有四档难度；新歌首版只制作一个难度，准确名称与文件映射待实机核验。
- 3D 预览、4×3 放块面板、波形时间轴；自动估算 BPM/第一拍并允许校准。
- 普通方块、炸弹、普通墙的基础编辑，以及多选、复制粘贴、镜像、撤销重做。
- 慢速播放、选区循环、节拍器和节拍吸附；独立工程、自动保存与恢复。
- 保留已有谱的高级内容，对可能破坏关联的编辑采取保护规则。
- 后续离线自动制谱以基本能直接游玩为目标，音乐情绪参与编排；《光之乐团》和 APK 后续考虑。

## 框架与验证

模块边界、工程默认值、实施顺序和验收标准见 [首版需求与架构](docs/首版需求与架构.md)，实际操作流程见 [使用说明](docs/使用说明.md)。

当前沿用 Qt 5.12.12 / MinGW 7.3 的 32 位应用工具链。随包 FFmpeg/ffprobe 使用固定的 Windows **64 位 LGPL shared** 构建，因此整套音频功能要求 Windows 64 位。工具通过独立进程离线运行，不要求使用者安装 Python 或修改系统 PATH。

开发者首次准备音频工具时运行 `scripts/setup-audio-tools.ps1`；该脚本下载固定版本并核对 SHA256。工具版本、来源和许可证见 [第三方音频工具说明](third_party/ffmpeg/README.md)。交付包需要同时携带 Qt 运行库、`tools/ffmpeg/` 中的 EXE/DLL 与许可证；只复制 `src.exe` 不足以运行完整功能。

自动 BPM/第一拍只是估计，可手动修正；节奏较弱、变速或半速/倍速歧义的歌曲尤其需要试听校准。已有曲谱的原始 BPM 和偏移保持原样。

用户已确认原始 Dry Hands 在《星穹绿洲》中可游玩且有声音；**编辑器导出的修改谱和新歌尚未在头显验收**。导出到电脑后，由用户手动复制到 `SoulTopia/BeatNote/Custom` 并游玩核验。其他高级格式支持不能仅凭导入成功推定，Windows 10 发行兼容也仍待独立验证。

## 背景资料

- [CX交接说明.md](docs/CX交接说明.md)：已有需求、格式样本结论与历史技术建议。
- [聊天记录.md](docs/聊天记录.md)：原始需求和此前讨论。

上述文档作为项目背景，后续用户明确的选择优先。当前已只读核实本地十个 ZIP 的格式范围和头显指定歌曲目录，证据与限制记录在首版架构文档中。

原本位于 `music/` 的十个曲谱 ZIP 已移到 `samples/beatmaps/`，不纳入 Git 版本控制。
