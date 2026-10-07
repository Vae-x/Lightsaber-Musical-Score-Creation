<#
.SYNOPSIS
从已验证的 Release 便携目录创建每用户 Windows x64 安装程序。
.EXAMPLE
./scripts/package-installer.ps1 -PortableDirectory 'dist/光剑曲谱制作' -CompilerPath 'C:/Program Files (x86)/Inno Setup 6/ISCC.exe'
.NOTES
使用 Inno Setup 6.7.3 或更新版本；官方下载与验证说明：
https://jrsoftware.org/isdl.php
https://jrsoftware.org/ishelp/topic_compilercmdline.htm
简体中文原文来自 Inno 官方源代码固定提交（文件注明兼容 6.5.0+）：
https://raw.githubusercontent.com/jrsoftware/issrc/43b4723c9b8a0179350aa9975270a9377d5ccc56/Files/Languages/ChineseSimplified.isl
上游原文件 SHA256：e0b0b350e2245f3c5e65586dfe43d574f6e7f06f2261149aba284954b3fc9a8d
译者信息原样保留；Inno 原许可见 installer/InnoSetup-LICENSE.txt（6.7.3 工具发行版）。
每次输出到 dist/installers/ 下的新目录，保留 staging、编译输入、命令参数和日志。
脚本不删除、覆盖、修改便携目录，也不收集工程、歌曲、模型、设置或账号数据。
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$PortableDirectory,
    [string]$CompilerPath = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Assert-ChildPath([string]$Path, [string]$Parent) {
    $prefix = $Parent.TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (!$Path.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase)) {
        throw "Path must remain inside $Parent : $Path"
    }
}

function Assert-NoReparsePoint([string]$Path) {
    $current = $Path
    while ($current) {
        $item = Get-Item -LiteralPath $current -Force
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
            throw "Reparse points are not accepted as packaging inputs or outputs: $current"
        }
        $parent = [IO.Path]::GetDirectoryName($current.TrimEnd('\', '/'))
        if (!$parent -or $parent -eq $current) { break }
        $current = $parent
    }
}

function Get-ReleaseRelativePath($Entry) {
    if ($Entry.File -isnot [string] -or [string]::IsNullOrWhiteSpace($Entry.File)) {
        throw 'Manifest file paths must be non-empty strings.'
    }
    $relative = $Entry.File.Replace('\', '/')
    if ([IO.Path]::IsPathRooted($relative) -or $relative -match '[\x00-\x1f<>:"|?*{}]' -or
        $relative -match '(^|/)(\.|\.\.)(/|$)' -or $relative -match '(^/|/$|//)' -or
        @($relative.Split('/') | Where-Object { $_ -match '[. ]$' }).Count -gt 0) {
        throw "Invalid relative path in release manifest: $relative"
    }
    # Only program runtimes and original release notices may be installed.
    # A manifest cannot expand the payload to live projects, settings or models.
    $allowed = $relative -match '^(LightsaberMusicalScoreCreation\.exe|LICENSE|CHANGELOG\.md|使用说明\.md|[a-z0-9][a-z0-9_.+-]*\.dll)$' -or
        $relative -match '^(audio|bearer|iconengines|imageformats|mediaservice|platforms|playlistformats|styles)/[a-z0-9_.-]+\.dll$' -or
        $relative -match '^tools/ffmpeg/(ffmpeg\.exe|ffprobe\.exe|LICENSE\.txt|README\.md|[a-z0-9_.-]+\.dll)$' -or
        $relative -match '^tools/mtp/(mtp-import\.ps1|WpdTransfer\.exe)$' -or
        $relative -eq 'tools/infernosaber/infernosaber_trial.py' -or
        $relative -match '^licenses/Qt/([a-z0-9_.-]+/)*(LICENSE[^/]*|COPYING[^/]*|COPYRIGHT[^/]*|NOTICE[^/]*|AUTHORS[^/]*|PATENTS[^/]*|qt_attribution[^/]*|README(\.md|\.txt)?)$'
    if (!$allowed) { throw "Manifest contains a file outside the release resource allowlist: $relative" }
    return $relative
}

function Assert-ManifestFile([string]$Path, [long]$Bytes, [string]$Sha256) {
    if (!(Test-Path -LiteralPath $Path -PathType Leaf)) { throw "Missing release file: $Path" }
    Assert-NoReparsePoint $Path
    if ((Get-Item -LiteralPath $Path).Length -ne $Bytes) { throw "Release file size mismatch: $Path" }
    if ((Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash -ne $Sha256) {
        throw "Release file SHA256 mismatch: $Path"
    }
}

$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$distRoot = [IO.Path]::GetFullPath((Join-Path $workspace 'dist'))
Assert-ChildPath $distRoot $workspace
$sourceRoot = if ([IO.Path]::IsPathRooted($PortableDirectory)) {
    [IO.Path]::GetFullPath($PortableDirectory)
} else { [IO.Path]::GetFullPath((Join-Path $workspace $PortableDirectory)) }
Assert-ChildPath $sourceRoot $distRoot
if (!(Test-Path -LiteralPath $sourceRoot -PathType Container)) { throw "Portable package directory does not exist: $sourceRoot" }
Assert-NoReparsePoint $sourceRoot
$manifestPath = Join-Path $sourceRoot 'package-manifest.json'
if (!(Test-Path -LiteralPath $manifestPath -PathType Leaf)) { throw 'A package-portable.ps1 release manifest is required.' }
Assert-NoReparsePoint $manifestPath
$manifestHash = (Get-FileHash -LiteralPath $manifestPath -Algorithm SHA256).Hash.ToLowerInvariant()
$manifest = [IO.File]::ReadAllText($manifestPath, [Text.Encoding]::UTF8) | ConvertFrom-Json
if ($manifest.Application -ne '光剑曲谱制作' -or $manifest.Configuration -ne 'Release' -or
    $manifest.Version -isnot [string] -or $manifest.Version -notmatch '^\d+\.\d+\.\d+$' -or
    @($manifest.Files).Count -eq 0) {
    throw 'The manifest must describe a versioned 光剑曲谱制作 Release package.'
}
$applicationVersion = $manifest.Version
$seen = New-Object 'System.Collections.Generic.HashSet[string]' ([StringComparer]::OrdinalIgnoreCase)
$payload = foreach ($entry in $manifest.Files) {
    $relative = Get-ReleaseRelativePath $entry
    if (!$seen.Add($relative)) { throw "Duplicate manifest path: $relative" }
    $fileBytes = [long]0
    if (![long]::TryParse([string]$entry.Bytes, [ref]$fileBytes) -or $fileBytes -lt 0 -or
        $entry.Sha256 -isnot [string] -or $entry.Sha256 -notmatch '^[a-fA-F0-9]{64}$') {
        throw "Invalid size or SHA256 in release manifest: $relative"
    }
    $sourceFile = [IO.Path]::GetFullPath((Join-Path $sourceRoot $relative))
    Assert-ChildPath $sourceFile $sourceRoot
    Assert-ManifestFile $sourceFile $fileBytes $entry.Sha256
    [pscustomobject]@{ File = $relative; Bytes = $fileBytes; Sha256 = $entry.Sha256.ToLowerInvariant() }
}
$payload = @($payload | Sort-Object File)
$requiredFiles = @(
    'LightsaberMusicalScoreCreation.exe', 'LICENSE', 'CHANGELOG.md', '使用说明.md',
    'Qt5Core.dll', 'Qt5Gui.dll', 'Qt5Widgets.dll', 'Qt5Multimedia.dll', 'Qt5Network.dll',
    'libgcc_s_dw2-1.dll', 'libstdc++-6.dll', 'libwinpthread-1.dll',
    'platforms/qwindows.dll', 'audio/qtaudio_windows.dll',
    'tools/ffmpeg/ffmpeg.exe', 'tools/ffmpeg/ffprobe.exe', 'tools/ffmpeg/LICENSE.txt', 'tools/ffmpeg/README.md',
    'tools/mtp/mtp-import.ps1', 'tools/mtp/WpdTransfer.exe',
    'tools/infernosaber/infernosaber_trial.py',
    'licenses/Qt/LICENSE.LGPLv3', 'licenses/Qt/LICENSE.GPLv3', 'licenses/Qt/README.txt'
)
foreach ($required in $requiredFiles) {
    if (!$seen.Contains($required)) { throw "Required runtime or notice is absent from the release manifest: $required" }
}
$binaryVersion = [Diagnostics.FileVersionInfo]::GetVersionInfo((Join-Path $sourceRoot 'LightsaberMusicalScoreCreation.exe'))
if ($binaryVersion.FileVersion -ne $applicationVersion -or $binaryVersion.ProductVersion -ne $applicationVersion -or
    $manifest.ExecutableFileVersion -ne $applicationVersion -or $manifest.ExecutableProductVersion -ne $applicationVersion) {
    throw 'Executable, manifest and release version must agree.'
}

# Enumerate without following links. Refuse used or modified packages instead of
# accidentally collecting files added after package-portable generated its list.
$directories = New-Object 'System.Collections.Generic.Queue[string]'
$directories.Enqueue($sourceRoot)
while ($directories.Count -gt 0) {
    foreach ($item in Get-ChildItem -LiteralPath $directories.Dequeue() -Force) {
        if (($item.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) { throw "Package contains a reparse point: $($item.FullName)" }
        if ($item.PSIsContainer) { $directories.Enqueue($item.FullName); continue }
        $relative = $item.FullName.Substring($sourceRoot.Length).TrimStart('\', '/').Replace('\', '/')
        if ($relative -ne 'package-manifest.json' -and !$seen.Contains($relative)) {
            throw "Unlisted file in portable package; create a fresh release package before building an installer: $relative"
        }
    }
}

if (!$CompilerPath) {
    $compilerCommand = Get-Command ISCC.exe -ErrorAction SilentlyContinue
    if ($compilerCommand) { $CompilerPath = $compilerCommand.Source }
    else {
        foreach ($candidate in @('C:/Program Files (x86)/Inno Setup 6/ISCC.exe', 'C:/Program Files/Inno Setup 7/ISCC.exe')) {
            if (Test-Path -LiteralPath $candidate -PathType Leaf) { $CompilerPath = $candidate; break }
        }
    }
}
if (!$CompilerPath -or !(Test-Path -LiteralPath $CompilerPath -PathType Leaf)) {
    throw 'Inno Setup ISCC.exe was not found. Supply -CompilerPath; download Inno Setup 6.7.3 or newer from https://jrsoftware.org/isdl.php .'
}
$compiler = [IO.Path]::GetFullPath($CompilerPath)
$compilerVersionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($compiler)
# ISCC's PE resource can be 0.0.0.0. installer.iss uses Inno's actual Ver
# preprocessor variable to enforce the minimum and record the compiler version.
$compilerDirectory = Split-Path -Parent $compiler
$languageSource = Join-Path $PSScriptRoot 'installer/ChineseSimplified.isl'
$translationLicense = Join-Path $PSScriptRoot 'installer/InnoSetup-LICENSE.txt'
$compilerLicense = Join-Path $compilerDirectory 'LICENSE.TXT'
$installerSource = Join-Path $PSScriptRoot 'installer.iss'
$iconSource = Join-Path $workspace 'resources/icons/app.ico'
foreach ($required in @($languageSource, $translationLicense, $compilerLicense, $installerSource, $iconSource)) {
    if (!(Test-Path -LiteralPath $required -PathType Leaf)) { throw "Missing installer build resource: $required" }
}

$installerRoot = [IO.Path]::GetFullPath((Join-Path $distRoot 'installers'))
Assert-ChildPath $installerRoot $distRoot
if (Test-Path -LiteralPath $installerRoot) { Assert-NoReparsePoint $installerRoot }
[void][IO.Directory]::CreateDirectory($installerRoot)
$base = Join-Path $installerRoot ('v' + $applicationVersion + '-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
$output = $base
$suffix = 1
while (Test-Path -LiteralPath $output) { $output = "$base-$suffix"; ++$suffix }
Assert-ChildPath $output $installerRoot
[void][IO.Directory]::CreateDirectory($output)
$staging = Join-Path $output 'staging'
$inputs = Join-Path $output 'build-inputs'
[void][IO.Directory]::CreateDirectory($staging)
[void][IO.Directory]::CreateDirectory($inputs)
foreach ($file in $payload) {
    $destination = Join-Path $staging $file.File
    Assert-ChildPath ([IO.Path]::GetFullPath($destination)) $staging
    [void][IO.Directory]::CreateDirectory((Split-Path -Parent $destination))
    Copy-Item -LiteralPath (Join-Path $sourceRoot $file.File) -Destination $destination
    Assert-ManifestFile $destination $file.Bytes $file.Sha256
}
Copy-Item -LiteralPath $manifestPath -Destination (Join-Path $staging 'package-manifest.json')
if ((Get-FileHash -LiteralPath (Join-Path $staging 'package-manifest.json') -Algorithm SHA256).Hash -ne $manifestHash) {
    throw 'The source manifest changed while preparing the installer.'
}
[IO.File]::WriteAllText((Join-Path $staging 'installed-mode.ini'), "[Deployment]`r`nMode=Installed`r`n", [Text.Encoding]::ASCII)
Copy-Item -LiteralPath $installerSource -Destination (Join-Path $inputs 'installer.iss')
Copy-Item -LiteralPath $PSCommandPath -Destination (Join-Path $inputs 'package-installer.ps1')
Copy-Item -LiteralPath $iconSource -Destination (Join-Path $inputs 'app.ico')
# Upstream uses UTF-8 without BOM; add a BOM to the compiler input so older
# Unicode Inno compilers never interpret Chinese text using a local code page.
# The vendored upstream file and translated message text remain unchanged.
[IO.File]::WriteAllText((Join-Path $inputs 'ChineseSimplified.isl'),
    [IO.File]::ReadAllText($languageSource, [Text.Encoding]::UTF8), [Text.UTF8Encoding]::new($true))
Copy-Item -LiteralPath $translationLicense -Destination (Join-Path $inputs 'ChineseTranslation-LICENSE.txt')
Copy-Item -LiteralPath $compilerLicense -Destination (Join-Path $inputs 'Compiler-LICENSE.txt')

$installedPayload = @($payload)
foreach ($addition in @('package-manifest.json', 'installed-mode.ini')) {
    $file = Get-Item -LiteralPath (Join-Path $staging $addition)
    $installedPayload += [pscustomobject]@{
        File = $addition; Bytes = $file.Length
        Sha256 = (Get-FileHash -LiteralPath $file.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
    }
}
$fileEntries = foreach ($file in $installedPayload | Sort-Object File) {
    $innoRelative = $file.File.Replace('/', '\')
    $subdirectory = [IO.Path]::GetDirectoryName($innoRelative)
    $destinationDirectory = if ($subdirectory) { '{app}\' + $subdirectory } else { '{app}' }
    'Source: "{#StagingDir}\' + $innoRelative + '"; DestDir: "' + $destinationDirectory + '"; Flags: ignoreversion; Hash: "' + $file.Sha256 + '"'
}
$filesInclude = Join-Path $inputs 'files.iss'
[IO.File]::WriteAllLines($filesInclude, [string[]]$fileEntries, [Text.UTF8Encoding]::new($true))
$compilerArguments = @(
    '/Qp', "/DAppVersion=$applicationVersion", "/DStagingDir=$staging", "/DFilesInclude=$filesInclude",
    "/DOutputDirectory=$output", "/DIconFile=$(Join-Path $inputs 'app.ico')",
    "/DLanguageFile=$(Join-Path $inputs 'ChineseSimplified.isl')",
    "/DCompilerVersionFile=$(Join-Path $inputs 'compiler-version.txt')", (Join-Path $inputs 'installer.iss')
)
$buildRecord = [ordered]@{
    Application = '光剑曲谱制作'; Version = $applicationVersion; BuiltUtc = (Get-Date).ToUniversalTime().ToString('o')
    SourceDirectory = $sourceRoot; SourceManifestSha256 = $manifestHash
    CompilerPath = $compiler; CompilerVersion = 'Verified by preprocessor during compilation'
    CompilerResourceVersion = $compilerVersionInfo.FileVersion
    CompilerSha256 = (Get-FileHash -LiteralPath $compiler -Algorithm SHA256).Hash.ToLowerInvariant()
    CompilerArguments = $compilerArguments; Payload = @($installedPayload | Sort-Object File)
    InstallerScriptSha256 = (Get-FileHash -LiteralPath (Join-Path $inputs 'installer.iss') -Algorithm SHA256).Hash.ToLowerInvariant()
    PackagingScriptSha256 = (Get-FileHash -LiteralPath (Join-Path $inputs 'package-installer.ps1') -Algorithm SHA256).Hash.ToLowerInvariant()
    ChineseTranslationSource = 'https://raw.githubusercontent.com/jrsoftware/issrc/43b4723c9b8a0179350aa9975270a9377d5ccc56/Files/Languages/ChineseSimplified.isl'
    ChineseTranslationSha256 = (Get-FileHash -LiteralPath $languageSource -Algorithm SHA256).Hash.ToLowerInvariant()
    CompilerTranslationSha256 = (Get-FileHash -LiteralPath (Join-Path $inputs 'ChineseSimplified.isl') -Algorithm SHA256).Hash.ToLowerInvariant()
    Status = 'Prepared'
}
$buildRecordPath = Join-Path $output 'installer-build.json'
[IO.File]::WriteAllText($buildRecordPath, ($buildRecord | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Write-Output "Verified release payload: $($payload.Count) files. Installer build directory: $output"
& $compiler @compilerArguments | Tee-Object -FilePath (Join-Path $output 'compiler.log')
if ($LASTEXITCODE -ne 0) { throw "Inno Setup compilation failed ($LASTEXITCODE); inputs and log were preserved in $output" }
$installer = Join-Path $output ("光剑曲谱制作-Windows-v$applicationVersion-Setup.exe")
if (!(Test-Path -LiteralPath $installer -PathType Leaf)) { throw "Compiler did not produce the expected installer: $installer" }
$buildRecord.CompilerVersion = [IO.File]::ReadAllText((Join-Path $inputs 'compiler-version.txt')).Trim()
$buildRecord.Status = 'Compiled'
$buildRecord.Installer = [ordered]@{
    File = [IO.Path]::GetFileName($installer); Bytes = (Get-Item -LiteralPath $installer).Length
    Sha256 = (Get-FileHash -LiteralPath $installer -Algorithm SHA256).Hash.ToLowerInvariant()
}
[IO.File]::WriteAllText($buildRecordPath, ($buildRecord | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))
Write-Output "Installer package: $installer"
