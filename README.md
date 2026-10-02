# 光剑曲谱制作

基于 **Qt5 / C++** 的 Windows 曲谱编辑器，基础编辑与音频处理可离线使用。首个实机目标是 PICO Neo 3 独立运行的《星穹绿洲》；APK、《光之乐团》和自动制谱放在后续阶段。

## 可以做什么

- 打开已有歌曲文件夹或 BeatSaver ZIP，选择一张难度，编辑普通红蓝方块、方向、炸弹和墙。
- 通过“PICO 头显”入口只读复制 `Custom` 中的歌曲到电脑，再建立独立工程；头显原文件保留。
- 导入 MP3/MP4、选择音轨、裁剪片段，自动转换为 Ogg，建立一张空白曲谱。
- 自动估算 BPM 和第一拍，也可试听后手动校准；使用轨道、4×3 面板和音频波形时间轴编排。
- 多选、复制粘贴、左右镜像、撤销重做，以及慢速试听、循环、节拍器和节拍吸附。
- 保存独立编辑工程与恢复快照，导出完整歌曲文件夹，再由用户手动复制到头显。
- 保留其他难度、原音频、未知 JSON 字段、弧线/链条、灯光和模组内容；可能破坏高级关联的操作会被保护规则阻止。
- 主窗口左侧提供可展开、收起的图标导航，在曲谱编辑、AI 识别与设置之间切换。设置支持浅色、深色、跟随系统；预设 DeepSeek、Kimi、MiMo、OpenAI、通义千问与自定义 OpenAI 兼容服务，填写密钥后自动读取模型，也支持 Codex / ChatGPT 账号授权和模型选择。
- 预留 AI 识别页面与服务接口，显示当前歌曲并提供模型配置入口；识别服务尚未接入，“开始识别”暂不可用。

当前高级内容以保存和保护为范围，未实现弧线、链条、灯光及模组效果的完整预览。用户确认游戏有四档难度；新歌先制作一张谱，准确游戏名称与文件标识映射仍待实机核验。

## 当前状态

首版桌面流程已实现。此前版本在 Windows 11 / Qt 5.12.12 上通过 Debug/Release、五项 CTest、已有谱编辑闭环、MP3/MP4 新歌界面流程与纯系统 PATH 便携运行验证。本地 10 ZIP、20 张难度原样导出逐文件 SHA256 一致；真实 Dry Hands 单方向修改仅改变对应字段，撤销后恢复原字节。

**0.2.0 已通过本机 Debug/Release、七项 CTest 和新版便携运行验证。** 中文名称/图标、PICO MTP 导入、默认工程目录及关于/许可入口均已接入；界面测试 10 项全部通过，包含真实 Neo 3 歌曲导入与 GPL 全文查看。Windows EXE 的中文产品名、文件说明及 0.2.0 版本资源已核验。更新范围见 [更新记录](CHANGELOG.md)。

原始 Dry Hands 在《星穹绿洲》中能游玩且有声音，这是用户确认的基准。**编辑器导出的修改谱和新歌尚未完成头显游玩验收**，Windows 10 发行兼容也仍待独立验证。格式校验和电脑试听不能代替这些实测。

## 使用

在 Windows 64 位上解压完整便携包，双击 `LightsaberMusicalScoreCreation.exe`。保留同目录的 DLL、`audio/`、`platforms/`、`tools/` 等资源；无需安装 Qt、FFmpeg 或 Python。基础编辑无需联网，AI 模型读取与账号授权需要联网；账号授权方式还需要本机 Codex CLI。

正式便携包和 SHA256 校验文件见 [v0.2.0 Release](https://github.com/Vae-x/Lightsaber-Musical-Score-Creation/releases/tag/v0.2.0)。开发时生成的便携目录与 ZIP 放在本机 `dist/`，不提交到源码仓库。该版本在纯系统 PATH 下通过导入、音频、编辑、撤销/重做、保存、导出、重新打开与渲染验证，133 个清单文件的 SHA256 核验通过。

默认编辑工程按歌曲分目录保存：`projects/<歌名>/project.lmsc`。源码开发与便携包各使用自己的 `projects/`；应用目录无写权限时回退到用户文档的 `光剑曲谱制作/工程/`。`.lmsc` 入口、旁边的 `assets-*`、`source-*` 和恢复快照需要一起保留和移动。

导出在电脑上创建独立歌曲目录，再手动复制到：

```text
此电脑\Pico Neo 3\内部共享存储空间\SoulTopia\BeatNote\Custom
```

具体操作见 [使用说明](docs/使用说明.md)，功能边界和验收基准见 [首版需求与架构](docs/首版需求与架构.md)。

开发版在主窗口集成“曲谱编辑、AI 识别、外观、大语言模型、账号授权、网络、关于”七个导航入口。左侧导航默认收起，点击三横线可展开；切换页面保留当前曲谱、选择、播放位置和设置草稿。菜单“设置 → 外观设置”或 `Ctrl+,` 切换到外观页，“保存并返回”和“取消并返回”回到曲谱编辑，“应用”保存后留在当前设置页；取消会恢复最近成功保存的全部设置与主题。

“网络”默认自动，也可手动填写 HTTP 代理地址和端口；“关于”显示版本、作者 Vae-x、项目主页，并可在页面内展开 GPLv3 全文。帮助菜单的“关于光剑曲谱制作”也进入这个页面。AI 音频识别服务与自动制谱尚未实现，当前页面不会上传音频或自动写入谱面；开发接口与后续设计见 [AI 设置与自动制谱方案](docs/AI设置与自动制谱方案.md)。已发布的 v0.2.0 包不包含这些后续开发功能。

## 源码与本地文件

```text
Lightsaber Musical Score Creation/
├── src/
│   ├── app/               # 入口、启动与组装
│   ├── core/              # 曲谱、时间、编辑规则、工程、音频和设备导入
│   ├── gui/               # Qt 窗口、控件与对应 .ui
│   └── CMakeLists.txt
├── resources/             # 图标、Qt 资源与 Windows 版本资源
├── docs/                  # 当前需求、架构与使用说明
├── tests/                 # 核心、音频和界面验证
├── third_party/           # 依赖版本、来源与第三方许可
├── scripts/               # 依赖准备和便携打包
├── samples/beatmaps/      # 本地歌曲样本，不提交内容
├── projects/              # 本地编辑工程，不提交内容
├── build/                 # debug/、release/、验证与 archive/，不提交
├── dist/                  # 本机便携目录和 ZIP，不提交
├── AGENTS.md              # 持续协作与提交规则
├── CHANGELOG.md
├── LICENSE
└── CMakeLists.txt
```

核心源码继续放在 **`src/core/`**，界面放在 `src/gui/`，入口放在 `src/app/`。同一类的 `.h/.cpp` 放在一起；窗口 `.ui` 放在窗口类旁。详细职责、构建和迁移约定见 [工程目录说明](docs/工程目录说明.md)。

当前沿用 Qt 5.12.12 / MinGW 7.3 的 32 位应用工具链；随包 FFmpeg/ffprobe 是固定的 Windows **64 位 LGPL shared** 构建，因此完整音频功能要求 Windows 64 位。开发者运行 `scripts/setup-audio-tools.ps1` 准备固定依赖并核对 SHA256；版本、源码入口和许可见 [音频工具说明](third_party/ffmpeg/README.md)。

根 `CMakeLists.txt` 是推荐入口，`src/CMakeLists.txt` 仍兼容已有 CLion 工程，可执行目标为 `src`。新构建集中到 `build/debug/`、`build/release/`；已有 `src/cmake-build-debug/` 保留。Release 构建后使用 `scripts/package-portable.ps1` 生成便携目录，整个目录压缩后交付。

## 协议与协作

项目采用 **GNU GPL v3**，官方许可全文保存在 [LICENSE](LICENSE)。Qt、FFmpeg 及其依赖各自保留原许可证；本项目协议不替换第三方条款。[GNU 官方许可说明](https://www.gnu.org/licenses/gpl-3.0.html)

用户要求每完成一次逻辑改动，在相关验证成功后以中文提交并推送；本地歌曲、工程、设备元数据、构建产物、便携包与 FFmpeg 二进制不推送。持续规则见 [AGENTS.md](AGENTS.md)。
