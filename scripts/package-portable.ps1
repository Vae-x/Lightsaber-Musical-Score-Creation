param(
    [string]$BuildDirectory = 'build/release',
    [string]$QtDirectory = 'F:/Qt/Qt5.12.12/5.12.12/mingw73_32',
    [string]$CompilerDirectory = 'F:/Qt/Qt5.12.12/Tools/mingw730_32/bin'
)

$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'dist'))
$workspacePrefix = $workspace.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
if (!$distRoot.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Output path escaped the workspace.'
}
$buildRoot = if ([IO.Path]::IsPathRooted($BuildDirectory)) {
    [IO.Path]::GetFullPath($BuildDirectory)
} else { [IO.Path]::GetFullPath((Join-Path $workspace $BuildDirectory)) }
if (!$buildRoot.StartsWith($workspacePrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Build directory must be inside this workspace.'
}
$sourceExe = Join-Path $buildRoot 'src/src.exe'
$deployTool = Join-Path $QtDirectory 'bin/windeployqt.exe'
$audioPlugin = Join-Path $QtDirectory 'plugins/audio/qtaudio_windows.dll'
$ffmpeg = Join-Path $workspace 'third_party/ffmpeg'
$mtpScript = Join-Path $workspace 'scripts/windows/mtp-import.ps1'
$projectLicense = Join-Path $workspace 'LICENSE'
$projectChangelog = Join-Path $workspace 'CHANGELOG.md'
foreach ($required in @($sourceExe, $deployTool, $audioPlugin,
                       (Join-Path $ffmpeg 'bin/ffmpeg.exe'), (Join-Path $ffmpeg 'bin/ffprobe.exe'),
                       $mtpScript, $projectLicense, $projectChangelog)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing required runtime: $required" }
}
$buildCache = Get-Content -LiteralPath (Join-Path $buildRoot 'CMakeCache.txt') -Raw
if ($buildCache -notmatch '(?m)^CMAKE_BUILD_TYPE:STRING=Release\s*$') {
    throw 'Portable packaging requires a Release build.'
}

# Every package gets a new directory. Existing packages are retained for review.
$package = Join-Path $distRoot '光剑曲谱制作'
if (Test-Path -LiteralPath $package) {
    $base = Join-Path $distRoot ('光剑曲谱制作-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
    $package = $base
    $suffix = 1
    while (Test-Path -LiteralPath $package) { $package = "$base-$suffix"; ++$suffix }
}
if (!$package.StartsWith($distRoot.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar,
                        [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid package target.' }
[IO.Directory]::CreateDirectory($package) | Out-Null
$targetExe = Join-Path $package 'LightsaberMusicalScoreCreation.exe'
Copy-Item -LiteralPath $sourceExe -Destination $targetExe
Copy-Item -LiteralPath $projectLicense, $projectChangelog -Destination $package
[IO.Directory]::CreateDirectory((Join-Path $package 'projects')) | Out-Null

$taskPreviousPath = $env:PATH
try {
    $env:PATH = $CompilerDirectory + ';' + (Join-Path $QtDirectory 'bin') + ';' + $taskPreviousPath
    & $deployTool --release --no-translations --compiler-runtime $targetExe
    if ($LASTEXITCODE -ne 0) { throw "Qt runtime deployment failed: $LASTEXITCODE" }
} finally { $env:PATH = $taskPreviousPath }

$audioDirectory = Join-Path $package 'audio'
[IO.Directory]::CreateDirectory($audioDirectory) | Out-Null
Copy-Item -LiteralPath $audioPlugin -Destination $audioDirectory -Force
$tools = Join-Path $package 'tools/ffmpeg'
[IO.Directory]::CreateDirectory($tools) | Out-Null
Get-ChildItem -LiteralPath (Join-Path $ffmpeg 'bin') -File | ForEach-Object {
    if ($_.Extension -in @('.exe', '.dll')) { Copy-Item -LiteralPath $_.FullName -Destination $tools }
}
Copy-Item -LiteralPath (Join-Path $ffmpeg 'LICENSE.txt'), (Join-Path $ffmpeg 'README.md') -Destination $tools
$mtpDirectory = Join-Path $package 'tools/mtp'
[IO.Directory]::CreateDirectory($mtpDirectory) | Out-Null
Copy-Item -LiteralPath $mtpScript -Destination $mtpDirectory

# License texts come from the exact locally installed Qt 5.12.12 source tree.
$qtSources = [IO.Path]::GetFullPath((Join-Path $QtDirectory '../Src'))
$licenseDirectory = Join-Path $package 'licenses/Qt'
[IO.Directory]::CreateDirectory($licenseDirectory) | Out-Null
foreach ($name in @('LICENSE.LGPLv3', 'LICENSE.LGPLv21', 'LICENSE.GPLv3', 'LICENSE.GPLv2', 'LICENSE.FDL')) {
    $sourceLicense = Join-Path $qtSources $name
    if (!(Test-Path -LiteralPath $sourceLicense -PathType Leaf)) { throw "Qt license text missing: $sourceLicense" }
    Copy-Item -LiteralPath $sourceLicense -Destination $licenseDirectory
}
foreach ($module in @('qtbase', 'qtmultimedia', 'qtsvg', 'qtimageformats')) {
    $moduleRoot = Join-Path $qtSources $module
    $thirdParty = Join-Path $moduleRoot 'src/3rdparty'
    if (Test-Path -LiteralPath $thirdParty -PathType Container) {
        Get-ChildItem -LiteralPath $thirdParty -Recurse -File | Where-Object {
            $_.Name -match '^(LICENSE|COPYING|COPYRIGHT|NOTICE|AUTHORS|PATENTS|qt_attribution)' -or
            $_.Name -in @('README', 'README.md', 'README.txt')
        } | ForEach-Object {
            $relative = $_.FullName.Substring($moduleRoot.Length).TrimStart('\', '/')
            $target = Join-Path (Join-Path $licenseDirectory $module) $relative
            [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($target)) | Out-Null
            Copy-Item -LiteralPath $_.FullName -Destination $target
        }
    }
}

$readme = @'
# 光剑曲谱制作 0.2.0

双击 LightsaberMusicalScoreCreation.exe 启动。请保留同目录的 DLL、audio、platforms、imageformats 和 tools 等资源；无需安装 Qt、FFmpeg 或 Python。

当前包面向 Windows 64 位电脑。Qt 编辑器为 32 位，随包音频工具为独立的 64 位进程。Windows 10 兼容性仍需独立验证。

1. “导入歌曲文件夹”的“电脑文件夹”页选择含 Info.dat 的本地歌曲；“导入曲谱 ZIP”打开本地 ZIP。连接并解锁 PICO、允许 USB 文件传输后，可在“PICO 头显”页刷新歌曲、选择并点击“导入选中歌曲”。歌曲先只读复制到电脑，不写入头显原文件。
2. “新歌 · MP3 / MP4”选择声音、裁剪片段；自动转为 Ogg，建立一张空谱。估拍后可校准 BPM 和第一拍。
3. 在 4×3 面板放置音符、炸弹或墙；支持多选、复制、粘贴、镜像、撤销/重做。受保护物件显示原因。
4. “保存工程”默认建议程序旁的 projects/<歌名>/project.lmsc；同名目录使用编号。程序目录无写权限时回退到用户文档的 光剑曲谱制作/工程。工程文件旁的 assets-*、source-* 目录与工程一起保存和移动；首次保存后自动保存恢复快照。
5. “导出歌曲目录”创建独立的新目录，再手动复制整份歌曲到 PICO Neo 3：
   此电脑\Pico Neo 3\内部共享存储空间\SoulTopia\BeatNote\Custom

新谱使用基础 v2.2 格式，已有 v2/v3 原数据和音频保留；新歌导出的第一拍偏移已写入物件拍数。游戏四档难度名称和新歌实际播放仍需《星穹绿洲》实机核验。

Ctrl+O 选择电脑或头显歌曲来源；Ctrl+S 保存；Ctrl+E 导出；Ctrl+Z 撤销；Ctrl+Shift+Z 重做；Ctrl+C/Ctrl+V 复制粘贴；Delete 删除。其余操作使用界面菜单和控制区。

软件提供手动编辑流程。编辑器导出的修改谱与新歌仍需戴头显实际游玩验收。自动制谱、模组效果的完整预览、《光之乐团》与 APK 在后续阶段。

项目采用 GNU GPL 第 3 版，官方条款全文在根目录 LICENSE；更新记录见 CHANGELOG.md。帮助菜单的“关于光剑曲谱制作”可查看版本，再点击“查看 GPLv3 许可”阅读原文。第三方组件遵循各自许可。

Qt 5.12.12 以动态库部署，许可证和第三方声明位于 licenses/Qt。对应源码版本：
https://download.qt.io/archive/qt/5.12/5.12.12/single/qt-everywhere-src-5.12.12.zip
FFmpeg 版本、来源、构建配置和对应源码入口见 tools/ffmpeg/README.md 与 LICENSE.txt。
'@
[IO.File]::WriteAllText((Join-Path $package '使用说明.md'), $readme, [Text.UTF8Encoding]::new($true))
$licenseNotice = @'
Qt runtime: Qt 5.12.12, dynamically linked. License texts and source dependency notices are provided in this directory.
Exact Qt source archive: https://download.qt.io/archive/qt/5.12/5.12.12/single/qt-everywhere-src-5.12.12.zip
This package includes replaceable Qt DLLs; changing those libraries does not require rebuilding the application.
FFmpeg license/configuration/source references: ../../tools/ffmpeg/README.md and LICENSE.txt.
'@
[IO.File]::WriteAllText((Join-Path $licenseDirectory 'README.txt'), $licenseNotice, [Text.UTF8Encoding]::new($false))

$inventory = Get-ChildItem -LiteralPath $package -Recurse -File | ForEach-Object {
    [pscustomobject]@{
        File = $_.FullName.Substring($package.Length).TrimStart('\', '/').Replace('\', '/')
        Bytes = $_.Length
        Sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$manifest = [pscustomobject]@{
    Application = '光剑曲谱制作'
    Version = '0.2.0'
    BuiltUtc = (Get-Date).ToUniversalTime().ToString('o')
    Configuration = 'Release'
    Qt = '5.12.12 / MinGW 7.3 32-bit'
    Files = @($inventory)
}
[IO.File]::WriteAllText((Join-Path $package 'package-manifest.json'), ($manifest | ConvertTo-Json -Depth 5),
                      [Text.UTF8Encoding]::new($false))
Write-Output "Portable package: $package"
