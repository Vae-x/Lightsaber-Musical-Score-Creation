param(
    [string]$ToolchainDirectory = 'build/tools/android',
    [string]$PythonCommand = 'python'
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$toolRoot = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($ToolchainDirectory)) { $ToolchainDirectory } else { Join-Path $workspace $ToolchainDirectory }))
if (!$toolRoot.StartsWith($workspace.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
    throw 'Android 工具输出必须位于本仓库内部。'
}
[IO.Directory]::CreateDirectory($toolRoot) | Out-Null
function DownloadVerified([string]$url, [string]$destination, [string]$hash) {
    if (Test-Path -LiteralPath $destination) {
        if ((Get-FileHash -LiteralPath $destination -Algorithm SHA256).Hash -ne $hash) { throw "现有依赖校验失败，未覆盖：$destination" }
        return
    }
    [IO.Directory]::CreateDirectory((Split-Path -Parent $destination)) | Out-Null
    $temporary = $destination + '.download-' + [Guid]::NewGuid().ToString('N')
    try {
        Invoke-WebRequest -Uri $url -OutFile $temporary -UseBasicParsing
        if ((Get-FileHash -LiteralPath $temporary -Algorithm SHA256).Hash -ne $hash) { throw "下载依赖 SHA256 不匹配：$url" }
        if (Test-Path -LiteralPath $destination) { throw "下载期间目标已被创建，未覆盖：$destination" }
        Move-Item -LiteralPath $temporary -Destination $destination
    } finally { if (Test-Path -LiteralPath $temporary) { Remove-Item -LiteralPath $temporary } }
}
$python = Join-Path $toolRoot 'python/Scripts/python.exe'
if (!(Test-Path -LiteralPath $python)) {
    & $PythonCommand -m venv (Join-Path $toolRoot 'python')
    if ($LASTEXITCODE -ne 0) { throw '创建 Android 构建工具环境失败。' }
}
& $python -m pip install 'aqtinstall==3.3.0'
if ($LASTEXITCODE -ne 0) { throw '安装 Qt 下载工具失败。' }
$qt = Join-Path $toolRoot 'qt/5.15.2/android'
$qtFiles = @('bin/androiddeployqt.exe','bin/qmlimportscanner.exe','lib/libQt5Core_arm64-v8a.so','lib/libQt5Multimedia_arm64-v8a.so',
    'lib/libQt5AndroidExtras_arm64-v8a.so','lib/libQt5Svg_arm64-v8a.so','lib/libQt5Core_x86_64.so')
$missingQt = @($qtFiles | Where-Object { !(Test-Path -LiteralPath (Join-Path $qt $_)) })
if ($missingQt.Count) {
    Push-Location $toolRoot
    try {
        & $python -m aqt install-qt windows android 5.15.2 android --outputdir (Join-Path $toolRoot 'qt') `
            --archives qtbase qtmultimedia qtandroidextras qttools qtimageformats qtsvg qtdeclarative --keep `
            --archive-dest (Join-Path $toolRoot 'cache') --internal
        if ($LASTEXITCODE -ne 0) { throw '下载或校验 Qt5 Android 组件失败。' }
    } finally { Pop-Location }
}
$ndk = Join-Path $toolRoot 'android-ndk-r21d'
if (!(Test-Path -LiteralPath (Join-Path $ndk 'source.properties'))) {
    if (Test-Path -LiteralPath $ndk) { throw "NDK 目标已有不完整内容，未覆盖：$ndk" }
    $ndkArchive = Join-Path $toolRoot 'cache/android-ndk-r21d-windows-x86_64.zip'
    DownloadVerified 'https://dl.google.com/android/repository/android-ndk-r21d-windows-x86_64.zip' $ndkArchive `
        '18335E57F8ACAB5A4ACF6A2204130E64F99153015D55EB2667F8C28D4724D927'
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    [IO.Compression.ZipFile]::ExtractToDirectory($ndkArchive, $toolRoot)
}
if ([IO.File]::ReadAllText((Join-Path $ndk 'source.properties')) -notmatch 'Pkg.Revision\s*=\s*21\.3\.6528147') {
    throw '本轮 Qt5 Android 工具链要求 NDK 21.3.6528147，未修改现有 NDK。'
}
$tlsRevision = 'b71f1470962019bd89534a2919f5925f93bc5779'
$tlsFiles = @(
    @('arm64-v8a','libcrypto_1_1.so','B4A932AB225B8B4B22A6E45A7D0E401AD22816A954609CC2410935C773F7483D'),
    @('arm64-v8a','libssl_1_1.so','594E891C449033D479EF855255249CA6947C13974B56881B568C5B56B2162A2B'),
    @('x86_64','libcrypto_1_1.so','BB40C89FFC1807FD4B1C9F16E50AB60055DA6704F5C65F6F72289614AD23C182'),
    @('x86_64','libssl_1_1.so','78DCAA8690600DE0AAFFD051E57F78F959A15F5265B720BAF6D60A873259EB94')
)
foreach ($file in $tlsFiles) {
    DownloadVerified "https://raw.githubusercontent.com/KDAB/android_openssl/$tlsRevision/ssl_1.1/$($file[0])/$($file[1])" `
        (Join-Path $toolRoot "openssl/$($file[0])/$($file[1])") $file[2]
}
& (Join-Path $PSScriptRoot 'setup-android-audio.ps1')
Write-Output 'Qt5 Android、NDK 和音频/HTTPS 组件已准备；另需 Android SDK 35/Build Tools 35.0.0、JDK 17–23、CMake 4.0+、Ninja。'
