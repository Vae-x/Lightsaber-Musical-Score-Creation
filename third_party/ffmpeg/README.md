# 随包音频工具

当前固定依赖：FFmpeg `n9.0.2-17-g2a571b6068-20260930`，BtbN Windows x86_64 **LGPL shared** 构建。应用通过 QProcess 调用，Qt 应用的 32 位构建可在 Windows 64 位运行该独立 64 位工具；此套工具不能用于 32 位 Windows。

来源是 [FFmpeg 官网 Windows 下载入口](https://ffmpeg.org/download.html)指向的 [BtbN 构建项目](https://github.com/BtbN/FFmpeg-Builds)。固定发布为 `autobuild-2026-09-30-13-08`，文件为 `ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0.zip`。

- [下载对应构建](https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0.zip)
- [发布者 SHA256 清单](https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/checksums.sha256)
- SHA256：`7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646`，本地下载已与发布清单核对一致。
- [对应 FFmpeg 源码](https://github.com/FFmpeg/FFmpeg/tree/2a571b6068)
- [对应构建配方及依赖源码入口](https://github.com/BtbN/FFmpeg-Builds/tree/6c9aec5)
- 本包 `LICENSE.txt` 为 LGPL v3。`--enable-gpl` 和 `--enable-nonfree` 均未启用；编译配置启用 `--enable-version3`、`--enable-libvorbis`、`--enable-libmp3lame`。

开发者执行 `powershell -ExecutionPolicy Bypass -File scripts/setup-audio-tools.ps1` 可重建 `bin/`；脚本验证固定 SHA256、拒绝 GPL/nonfree 构建并确认 Vorbis 编码器，不修改系统 PATH。使用者拿到部署包后直接使用，无需下载或安装工具。

`bin/` 的 EXE/DLL 排除在 Git 外，由部署脚本复制到程序旁的 `tools/ffmpeg/`。需要随发行包保留本说明和 LICENSE；公开发行前还应按 [FFmpeg 许可说明](https://ffmpeg.org/legal.html)整理所有启用依赖的对应许可证与源码提供材料。当前本地开发包没有声称完成公开发行合规审查。

音频导入用 ffprobe 检查音轨，ffmpeg 只提取所选声音，裁剪后写 Ogg/Vorbis（质量参数 5、44.1kHz、双声道）。声音预览为 44.1kHz 双声道 signed-16 little-endian 的临时磁盘缓存；保持音高的慢放由 `atempo` 建立缓存。工程导出仍保存 Ogg/Vorbis，实际游戏兼容性等待 Neo 3 实机确认。
