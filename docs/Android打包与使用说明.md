# Android 打包与使用说明

Android 预览版面向普通手机，同一 APK 可作为 PICO 等头显的普通二维应用尝试运行。保留 Qt5 / C++，没有改用其他主框架，也没有加入 VR 沉浸式编辑。Windows 安装版和便携版继续使用原有功能与工具链。

本轮构建和检查状态见文末。手机真机、PICO 二维运行、两款游戏分类识别及实际游玩分别验收，不能以 APK 生成、模拟器运行或谱面校验代替。

## 安装与首次使用

已完成本地双 ABI 测试签名 APK：`dist/光剑曲谱制作-0.6.0-安卓预览版-20261008-090935.apk`，旁边的 `.sha256` 记录校验值。此 APK 尚未发布到公开 Release，既有 Windows Release 不变；开发者也可按下文重新打包。预览包使用应用标识 `org.lmsc.score`，Android 安装包版本名为 `0.6.0-android-preview`，不改变 Windows 版本号。

最低版本为 **Android 7.0 / API 24**。APK 包含 `arm64-v8a`，用于 64 位 ARM 手机和相应头显；同时包含 `x86_64`，用于模拟器验证，不包含 32 位 ARM。最低系统版本与 ABI 匹配不代表所有厂商系统已经实测。

1. 将 `.apk` 复制到手机，使用系统文件管理器打开，按提示允许该文件来源安装，然后启动“光剑曲谱制作”。无需另装 Qt、Python、FFmpeg 或桌面 Codex。
2. 从“工程”页选择“导入歌曲”，通过系统选择 ZIP 或完整歌曲文件夹；新建歌曲点击“新歌 · MP3 / MP4”，选择媒体、音轨与裁剪范围。
3. 在“网格”页切换放置或选择，设置颜色和方向，点击格位；多选开关用于追加选择。“属性”进入工具页，可编辑拍点、格位、方向、墙属性；“删除”只删除当前可编辑物件。
4. “时间轴”页提供编辑/框选、平移、循环工具，拖动选中物件移动，空白处拖动框选；放大、缩小按钮替代鼠标滚轮。播放、速度、吸附和跳转在底部，循环与节拍器开关在工程页。
5. 顶部工作区可切换“生成与精修”“API 模型”等页面；“文件”和“编辑”按钮提供保存、导出、复制粘贴、镜像、撤销和重做。

头显安装方式以设备提供的普通 Android 应用安装入口为准，启动后是二维窗口。头显系统文件选择器、手柄指针、键盘与可访问目录需实际检查。本轮开发不自动连接、上传或删除头显歌曲。

测试签名用于当前预览。后续换用不同签名时，系统可能无法直接覆盖安装；请先分享完整工程，再按对应发行说明处理，避免为更换安装包而丢失工程。

## 工程保存、分享与恢复

“保存工程”将 `project.lmsc` 与资源保存在应用私有目录的 `projects/<歌名>/`，默认无需选择外部路径；另存为建立新的独立工程目录。已保存工程仍按每 30 秒周期写入恢复快照。系统文件选择器导入的媒体、ZIP、歌曲文件夹和外部工程都先建立应用内独立副本，原文件保持原样。

**卸载应用、清除应用数据会移除私有目录中的工程、媒体副本、歌曲导出副本和设置。系统自动备份未开启。** 请在卸载、换机或清除数据前执行以下操作：

1. 打开需要备份的工程，在“工程”页点击“分享完整工程”；软件先保存当前改动。
2. 在系统选择器中选择可写文件夹，例如自行建立的“文档/曲谱备份”。分享会在所选目录的 `光剑曲谱制作工程/` 分类中新增独立工程文件夹，重名追加编号。
3. 保留整个目录：`project.lmsc`、`assets-*`、`source-*`、恢复快照和其他工程资源。不要只复制 `.lmsc` 文件。
4. 恢复时选择“打开编辑工程 → 导入完整工程”，选包含 `project.lmsc` 的完整目录；软件复制后保存到应用中的工程目录。继续编辑应用内工程则选“打开编辑工程 → 应用中工程”。

分享工程用于续编，导出歌曲用于游戏；两者用途不同。API Key 和连接设置不进入工程包，需要在新设备重新填写；Windows 的 DPAPI 设置文件不能直接用作 Android Keystore 设置。

## 歌曲导出与头显目录

点击“导出”后先在应用中生成完整歌曲副本，然后通过系统选择器选择外部父目录。软件新增：

```text
所选目录/
  光剑曲谱制作/
    歌曲名-by光剑曲谱/
      Info.dat
      难度谱面与音频、封面等资源
```

重名追加 `-2`、`-3`，不覆盖已有目录。取消系统选择仍保留完整应用副本；外部复制失败时报告可能残留的新目录，不自动删除。工程中的音频和拍点不变，导出只使用已确认的正式谱；开场缓冲只在导出的基础新歌中补静音并同步后移物件。

建议先导出到可访问的普通文档目录，再将整个歌曲目录按目标游戏支持的方式复制。Windows 版继续提供电脑导出与 USB/MTP；Android APK 不提供 Windows 的设备扫描、MTP 传输或单曲设备删除。

| 游戏 | 已有歌曲根目录 | Android APK 的边界 |
| --- | --- | --- |
| 星穹绿洲 | `SoulTopia/BeatNote/Custom` | 共享目录能否被系统选择器访问、分类是否识别，须在实际系统和游戏中检查 |
| 光之乐团 | `Android/data/com.StarRiverVR.LightBand/files/CustomMusic` | Android 11 及以上限制其他应用专用目录访问，不能保证 APK 可以读写这里 |

Android 11 及以上还限制系统目录选择器访问存储根、Downloads 根及 `Android/data`、`Android/obb`；可以选择普通文档文件夹或自己建立的子目录。APK 没有通过申请“所有文件访问”绕过这些限制。详见 [Android 官方存储说明](https://developer.android.com/about/versions/11/privacy/storage)。

APK 可安装、外部文件复制完成或格式校验通过，都不代表游戏已识别分类、声音、难度或新谱手感。不会在连接、导入或导出时自动删除已有歌曲，不修改成绩、收藏或配置；对头显游戏目录的任何实际测试都应使用独立合成歌曲并记录具体范围。

## 音频、模型与权限

- 音轨检查、解码、裁剪、Ogg/Vorbis 编码、波形与慢放通过 **FFmpegKit 6.0.4** 原生组件运行；任务后台执行，取消针对当前任务。来源、固定制品及原文许可见 [Android 音频组件](../third_party/android-ffmpeg/README.md)。
- 本地快速制谱可以离线使用；AI 建议、模型制谱与精修使用用户自行配置的 HTTPS API。原音频不上传，模型请求发送本地分析后的节奏和谱面信息，费用按服务商计费。
- API Key 使用 Android Keystore 加密保存在应用中，不进入歌曲、工程分享或诊断内容。关于 Keystore 的平台机制见 [Android 官方说明](https://developer.android.com/privacy-and-security/keystore)。
- APK 不开放 Codex 本机 CLI 账号方式，也不包含 InfernoSaber 的桌面 Python 环境或模型权重；Windows 原入口保留。
- 文件选择与外部目录授权由系统选择器处理，应用不要求遍历整台设备。应用内“关于”可查看 GPLv3 与 Android 第三方许可原文。

## 从源码打包

Android 工具链独立准备，Windows 仍使用 Qt 5.12.12 / MinGW 7.3。Android 固定 Qt **5.15.2**、NDK **r21d / 21.3.6528147** 和匹配 OpenSSL，音频固定 `dev.ffmpegkit-maintained:ffmpeg-kit-audio:6.0.4`。打包机须已有 Python 3、Android SDK **35** / Build Tools **35.0.0**、JDK **17–23**、CMake **4.0+** 与 Ninja；Qt、NDK 和音频/TLS 组件由准备脚本下载。SDK 安装和首次 Gradle 依赖解析需要网络；不得把下载的工具链、AAR、APK、签名密钥或构建目录提交到 Git。

在仓库根目录运行 PowerShell。工具准备脚本使用专用目录，不改 Windows Qt 配置；实际参数和解析出的路径以脚本输出为准：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup-android-tools.ps1
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup-android-audio.ps1
# 将 JDK 路径换为本机实际的 JDK 17–23；SDK 路径也可按实际安装位置修改。
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-android.ps1 -BuildDirectory build/android-preview -JdkDirectory 'C:\Program Files\Java\jdk-17' -AndroidSdkDirectory "$env:LOCALAPPDATA\Android\Sdk"
```

默认工具目录为 `build/tools/android/`，可用两个脚本的 `-ToolchainDirectory` 指定其他仓库内专用位置。`BuildDirectory` 默认 `build/android/`，示例使用独立的 `build/android-preview/`；默认 `arm64-v8a` 与 `x86_64` 分别交叉编译后合并打包。JDK 和 SDK 也可从 `JAVA_HOME`、`ANDROID_HOME` 读取；未设置 JDK 时必须传 `-JdkDirectory`。CMake、Ninja 不在 PATH 时使用 `-CMakeCommand`、`-NinjaCommand` 指定；`-Parallel` 默认为 4，可按打包机资源减少。

最终 APK 输出为 `dist/光剑曲谱制作-0.6.0-安卓预览版-时间戳.apk`，旁边的 `.sha256` 记录 SHA256；脚本执行 APK 签名校验，并报告完整路径。各 ABI 的库和 APK staging 保留在本次 `BuildDirectory` 中。不要将 Windows `build/debug/`、`build/release/` 或已有 CLion 输出直接用于 Android。

Android 使用 QtSvg 绘制主题中的箭头和勾选图标。下载 `qtsvg` 后仍须检查最终 APK 的两种 ABI 都部署 QtSvg 及 SVG image/icon 插件；只检查应用原生库编译不足以证明图标可以运行时显示。主题中的实际 SVG 渲染引用保留模块依赖，Windows 原部署不变。

Java 边界检查和平台合成测试 APK 可单独生成，参数中的 JDK/SDK 使用同一实际位置：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/test-android-storage.ps1 -JdkDirectory 'C:\Program Files\Java\jdk-17' -AndroidSdkDirectory "$env:LOCALAPPDATA\Android\Sdk"
powershell -NoProfile -ExecutionPolicy Bypass -File scripts/package-android.ps1 -PlatformTests -BuildDirectory build/android-platform-tests -JdkDirectory 'C:\Program Files\Java\jdk-17' -AndroidSdkDirectory "$env:LOCALAPPDATA\Android\Sdk"
```

平台合成验证 APK 是单独的测试入口，用于音频、ZIP、文件授权与密钥等平台检查；它不代替正式编辑器、手机或头显的实际验收。

部署到模拟器或自行选择的测试手机可使用 SDK 的 `adb install <生成的APK路径>`。安装测试不授权扫描或修改用户歌曲；验证夹具只使用新建的隔离合成数据。

## 许可与第三方来源

项目继续采用 [GNU GPL v3 原文](../LICENSE)。APK 随附项目许可、Qt 原文、FFmpegKit/FFmpeg 原文和第三方声明、OpenSSL 原文及来源说明，在“关于”页可读取。第三方组件按各自许可分发，项目 GPL 不替换其条款；官方原文不翻译或修改。公开发布 APK 时，应一起提供相应源码版本、依赖来源、原文声明和可复现的构建信息。

工具链与许可来源说明由打包脚本收集；音频 AAR 校验 SHA256，固定版本链接见 [音频组件来源](../third_party/android-ffmpeg/README.md)。工程、用户媒体、头显元数据、API Key、模型权重和本机构建文件不随源码或发布包公开。

## 本轮验证记录

截至 **2026-10-08**，双 ABI 主程序 APK 已完成构建和 v2 签名校验。以下仅记录已执行的检查；已按用户要求停止后续模拟器验证，不把未完成项目记为通过。

| 项目 | 当前状态 | 验证范围 |
| --- | --- | --- |
| Windows Debug 原工具链构建与回归 | 已通过，29 / 29 项 CTest；随后相关 GUI 回归 5 / 5；最终 Debug 构建成功 | 原编辑与保护逻辑、无键盘交互等回归；不等于 Android 触摸真机通过 |
| Windows Release 原工具链构建与回归 | 本轮未验证 | 不沿用历史 Release 检查作为本轮结果 |
| Android Java 编译与合成边界 | 已通过，83 个断言 | ZIP 57 个、导出位置与部分失败报告 26 个；不等于 SAF / Keystore 设备通过 |
| Android 主程序两种 ABI 编译与 APK 校验 | 已完成 | `arm64-v8a`、`x86_64` 编译与合并打包，APK v2 签名校验；Qt / SVG / 音频 / TLS 组件及随包许可 |
| x86_64 模拟器平台合成流程 | 2026-10-07 报告音频 4 项通过、存储 8 项通过 | 原生音频、TLS、Keystore、私有工程往返、ZIP 提取与路径越界拒绝；使用隔离合成数据 |
| 真实 SAF 文件选择与外部写入 | 往返与写入未确认 | 专用用例默认跳过；之后启用只打开系统选择器，未完成文件授权、导入与外部导出 |
| x86_64 模拟器主编辑器运行 | 启动与手机尺寸布局已检查 | 完整 GUI 编辑、撤销重做、保存重开与导出往返未确认；后续模拟器验证已停止 |
| 普通 Android 手机真机 | 尚未实测 | 触摸、性能、旋转、文件选择、音频与工程恢复 |
| PICO 二维应用运行 | 尚未实测 | 启动、手柄指针、系统选择器与目录权限 |
| 星穹绿洲游戏内验收 | 尚未实测 | 分类识别、声音、难度、方块显示及手感 |
| 光之乐团游戏内验收 | 尚未实测 | 分类识别、声音、难度、方块显示及手感 |

本轮开发不把此前 Windows 或 USB 检查写为 Android 验收，不操作用户原歌曲、成绩、收藏或配置。平台合成测试通过与主程序启动检查均不等于手机、PICO 文件权限或游戏游玩已经通过。
