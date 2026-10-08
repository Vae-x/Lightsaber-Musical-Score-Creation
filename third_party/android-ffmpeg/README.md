# Android 音频组件

Android 安装包使用 FFmpegKit 的音频版原生库，保持桌面版的导入音轨检查、MP3/MP4/OGG 解码、裁剪、Ogg Vorbis 编码、保持音高变速和导出开场缓冲。Windows 继续使用 `third_party/ffmpeg/bin/` 的桌面工具。

固定制品：`dev.ffmpegkit-maintained:ffmpeg-kit-audio:6.0.4`。该版本支持 Android 7.0 / API 24 及以上，提供 `arm64-v8a` 与 `x86_64`；手机与头显安装同一种普通二维 Android 应用。它的可加载性和本项目的运行功能须分别验证，不能用依赖方测试代替本项目的设备测试。

- [源代码和构建脚本（固定版本）](https://github.com/ffmpegkit-maintained/ffmpeg/tree/v6.0.4-lts-android)
- [发布说明与预编译 AAR](https://github.com/ffmpegkit-maintained/ffmpeg/releases/tag/v6.0.4-lts-android)
- [Maven Central 制品](https://repo.maven.apache.org/maven2/dev/ffmpegkit-maintained/ffmpeg-kit-audio/6.0.4/)
- 制品 SHA256：`54BBC7FCA3F27811A9289EACEAC5B94837F87BD1D97D0F98148D8CBEDCFA93C1`

本地 AAR 不自动解析 Maven POM 的传递依赖，必须同时打包 `com.arthenica:smart-exception-java:0.2.1` 与 `smart-exception-common:0.2.1` 两个 JAR。准备脚本对三项依赖分别校验固定 SHA256；打包脚本将两个 JAR 纳入 `libs/`，供 Gradle 的 `fileTree` 加载。缺少任一 JAR 会导致 FFmpegKit 初始化失败。Smart Exception 采用 BSD 3-Clause 许可，原文和来源保存在 `licenses/`。

运行 `powershell -NoProfile -ExecutionPolicy Bypass -File scripts/setup-android-audio.ps1`，将经过校验的 AAR 下载到忽略的 `build/android-deps/audio/`。Android 打包脚本将它纳入 Gradle 依赖，二进制不提交到 Git。Android 构建还需要 Qt5 的 AndroidExtras 模块；`NativeAudioTool` 通过 JNI 调用 FFmpegKit，不启动桌面 `.exe`，不修改系统 PATH。

参数通过 Java 字符串数组传递，包含中文、空格或引号的路径保持为独立参数。任务在工作线程运行；进度来自独立会话，取消只针对当前任务。FFmpegKit 与 Qt 都携带 `libc++_shared.so`，本预览包统一使用 Qt 工具链的 NDK r21d 运行库，移除 AAR 自带的较新版本，避免 Qt5 的 GCC 异常展开实现与新运行库冲突。原生库加载、音频运行和异常恢复分别验证。

该音频版按 LGPL v3 分发；FFmpegKit 原始许可和上游声明保存于 `LICENSE.txt`、`THIRD-PARTY-NOTICES.txt`，外部音频库的许可原文及其来源补充在 `licenses/` 目录。本项目自己的 GPL v3 许可保持原样。公开发布 APK 时递归提供这些文件及上述固定源码链接。许可文件保持原文。
