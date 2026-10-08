param(
    [string]$BuildDirectory = 'build/android',
    [string]$ToolchainDirectory = 'build/tools/android',
    [string]$AndroidSdkDirectory = $env:ANDROID_HOME,
    [string]$JdkDirectory = $env:JAVA_HOME,
    [string]$CMakeCommand = 'cmake',
    [string]$NinjaCommand = 'ninja',
    [string[]]$Abis = @('arm64-v8a', 'x86_64'),
    [switch]$PlatformTests,
    [int]$Parallel = 4
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
function WorkspacePath([string]$value) {
    $resolved = [IO.Path]::GetFullPath($(if ([IO.Path]::IsPathRooted($value)) { $value } else { Join-Path $workspace $value }))
    if (!$resolved.StartsWith($workspace.TrimEnd('\','/') + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
        throw '构建和工具输出必须位于本仓库内部。'
    }
    return $resolved
}
function InvokeChecked([string]$program, [string[]]$arguments) {
    & $program @arguments
    if ($LASTEXITCODE -ne 0) { throw "命令失败（$LASTEXITCODE）：$program" }
}
$buildRoot = WorkspacePath $BuildDirectory
$toolRoot = WorkspacePath $ToolchainDirectory
$qt = Join-Path $toolRoot 'qt/5.15.2/android'
$ndk = Join-Path $toolRoot 'android-ndk-r21d'
if (!$AndroidSdkDirectory) { $AndroidSdkDirectory = Join-Path $env:LOCALAPPDATA 'Android/Sdk' }
if (!$JdkDirectory) { throw '请用 -JdkDirectory 指定 JDK 17 或以上（Gradle 8.10.2 支持至 JDK 23）。' }
$AndroidSdkDirectory = (Resolve-Path -LiteralPath $AndroidSdkDirectory).Path
$JdkDirectory = (Resolve-Path -LiteralPath $JdkDirectory).Path
$cmake = (Get-Command $CMakeCommand -ErrorAction Stop).Source
$ninja = (Get-Command $NinjaCommand -ErrorAction Stop).Source
$version = (& $cmake --version | Select-Object -First 1)
if ($version -notmatch '(\d+)\.(\d+)\.(\d+)' -or [Version]$Matches[0] -lt [Version]'4.0.0') {
    throw '本项目需要 CMake 4.0 或以上，请用 -CMakeCommand 指定。'
}
foreach ($abi in $Abis) { if ($abi -notin @('arm64-v8a','x86_64')) { throw "未支持的 ABI：$abi" } }
if (!$Abis.Count -or ($Abis | Select-Object -Unique).Count -ne $Abis.Count) { throw 'ABI 列表不能为空或重复。' }
foreach ($path in @((Join-Path $qt 'bin/androiddeployqt.exe'), (Join-Path $ndk 'build/cmake/android.toolchain.cmake'),
    (Join-Path $JdkDirectory 'bin/java.exe'), (Join-Path $AndroidSdkDirectory 'platforms/android-35/android.jar'))) {
    if (!(Test-Path -LiteralPath $path -PathType Leaf)) { throw "缺少 Android 构建依赖：$path" }
}
$target = if ($PlatformTests) { 'lmsc_android_platform_tests' } else { 'src' }
$binary = if ($PlatformTests) { 'LightsaberMusicalScoreCreationPlatformTests' } else { 'LightsaberMusicalScoreCreation' }
foreach ($abi in $Abis) {
    $abiRoot = Join-Path $buildRoot $abi
    InvokeChecked $cmake @('-S', $workspace, '-B', $abiRoot, '-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$ninja", "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake",
        '-DCMAKE_POLICY_VERSION_MINIMUM=3.5', "-DANDROID_ABI=$abi", '-DANDROID_PLATFORM=android-24',
        '-DANDROID_STL=c++_shared', "-DQt5_DIR=$qt/lib/cmake/Qt5", "-DCMAKE_FIND_ROOT_PATH=$qt",
        "-DANDROID_SDK=$AndroidSdkDirectory", '-DCMAKE_BUILD_TYPE=Release', '-DBUILD_TESTING=OFF',
        "-DLMSC_ANDROID_PLATFORM_TESTS=$(if ($PlatformTests) {'ON'} else {'OFF'})")
    InvokeChecked $cmake @('--build', $abiRoot, '--target', $target, '--parallel', "$Parallel")
}

$stage = Join-Path $buildRoot ('apk-' + [DateTime]::Now.ToString('yyyyMMdd-HHmmss') + '-' + [Guid]::NewGuid().ToString('N').Substring(0,6))
[IO.Directory]::CreateDirectory($stage) | Out-Null
& (Join-Path $PSScriptRoot 'setup-android-audio.ps1') | Out-Null
$aar = Join-Path $workspace 'build/android-deps/audio/ffmpeg-kit-audio-6.0.4.aar'
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$audioArchive = [IO.Compression.ZipFile]::OpenRead($aar)
$runtime = Join-Path $stage 'runtime'
$patchedAar = Join-Path $stage 'libs/ffmpeg-kit-audio-6.0.4.aar'
[IO.Directory]::CreateDirectory((Split-Path -Parent $patchedAar)) | Out-Null
foreach ($name in @('smart-exception-java-0.2.1.jar','smart-exception-common-0.2.1.jar')) {
    Copy-Item -LiteralPath (Join-Path $workspace "build/android-deps/audio/$name") -Destination (Join-Path $stage 'libs')
}
$outputArchive = [IO.Compression.ZipFile]::Open($patchedAar, [IO.Compression.ZipArchiveMode]::Create)
$architectures = [ordered]@{}
$extraLibraries = @()
try {
    foreach ($abi in $Abis) {
        $triple = if ($abi -eq 'arm64-v8a') { 'aarch64-linux-android' } else { 'x86_64-linux-android' }
        $architectures[$abi] = $triple
        $abiLibs = Join-Path $stage "libs/$abi"
        [IO.Directory]::CreateDirectory($abiLibs) | Out-Null
        Copy-Item -LiteralPath (Join-Path $buildRoot "$abi/android-build/libs/$abi/lib${binary}_${abi}.so") -Destination $abiLibs
        foreach ($name in @('libcrypto_1_1.so','libssl_1_1.so')) {
            $ssl = Join-Path $toolRoot "openssl/$abi/$name"
            if (!(Test-Path -LiteralPath $ssl -PathType Leaf)) { throw "缺少 HTTPS 组件：$ssl" }
            $extraLibraries += $ssl.Replace('\','/')
        }
        $runtimeDirectory = Join-Path $runtime $triple
        [IO.Directory]::CreateDirectory($runtimeDirectory) | Out-Null
        # Qt5 exports the NDK21 unwinder. Mixing it with the AAR's newer
        # hidden LLVM unwinder crashes C++ exception handling on x86_64.
        $qtRuntime = Join-Path $ndk "toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/$triple/libc++_shared.so"
        Copy-Item -LiteralPath $qtRuntime -Destination (Join-Path $runtimeDirectory 'libc++_shared.so')
        Copy-Item -LiteralPath (Join-Path $runtimeDirectory 'libc++_shared.so') -Destination $abiLibs
    }
    foreach ($entry in $audioArchive.Entries) {
        # Supply exactly one runtime from the pinned Qt-compatible NDK.
        if ($entry.FullName.EndsWith('/libc++_shared.so')) { continue }
        if ($entry.FullName.StartsWith('jni/') -and $entry.FullName.Split('/')[1] -notin $Abis) { continue }
        $copy = $outputArchive.CreateEntry($entry.FullName, [IO.Compression.CompressionLevel]::Optimal)
        $inputStream = $entry.Open(); $outputStream = $copy.Open()
        try { $inputStream.CopyTo($outputStream) } finally { $inputStream.Dispose(); $outputStream.Dispose() }
    }
} finally { $outputArchive.Dispose(); $audioArchive.Dispose() }

$settings = [ordered]@{
    description = '光剑曲谱制作 Android 构建配置，由 package-android.ps1 生成。'
    qt = $qt.Replace('\','/'); sdk = $AndroidSdkDirectory.Replace('\','/'); sdkBuildToolsRevision = '35.0.0'
    ndk = $ndk.Replace('\','/'); 'ndk-host' = 'windows-x86_64'; 'toolchain-prefix' = 'llvm'; 'tool-prefix' = 'llvm'
    architectures = $architectures; 'application-binary' = $binary; 'stdcpp-path' = $runtime.Replace('\','/')
    'android-package-source-directory' = (Join-Path $workspace 'android').Replace('\','/')
    'android-min-sdk-version' = '24'; 'android-target-sdk-version' = '34'
    'android-extra-libs' = $extraLibraries -join ','
}
$settingsPath = Join-Path $stage 'deployment.json'
[IO.File]::WriteAllText($settingsPath, ($settings | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
$oldJava = $env:JAVA_HOME; $oldSdk = $env:ANDROID_SDK_ROOT; $oldGradle = $env:GRADLE_USER_HOME
try {
    $env:JAVA_HOME = $JdkDirectory; $env:ANDROID_SDK_ROOT = $AndroidSdkDirectory
    $env:GRADLE_USER_HOME = Join-Path $toolRoot 'gradle-cache'
    # Auxiliary mode expects the Qt template to have been prepared by the IDE.
    foreach ($template in (Get-ChildItem -LiteralPath (Join-Path $qt 'src/android/templates'))) {
        Copy-Item -LiteralPath $template.FullName -Destination $stage -Recurse -Force
    }
    foreach ($wrapper in (Get-ChildItem -LiteralPath (Join-Path $qt 'src/3rdparty/gradle'))) {
        Copy-Item -LiteralPath $wrapper.FullName -Destination $stage -Recurse -Force
    }
    foreach ($source in (Get-ChildItem -LiteralPath (Join-Path $workspace 'android'))) {
        Copy-Item -LiteralPath $source.FullName -Destination $stage -Recurse -Force
    }
    if ($PlatformTests) {
        Copy-Item -LiteralPath (Join-Path $workspace 'tests/android/java/org/lmsc/PlatformStorageHarness.java') -Destination (Join-Path $stage 'src/org/lmsc')
    }
    # Qt5's qmlimportscanner command fails when the Qt path contains spaces.
    # Use a temporary, unused drive only while preparing this package.
    $aliasDrive = $null
    try {
        if ($qt.Contains(' ')) {
            foreach ($letter in @('Q','R','S','T','U','V','W','X','Y','Z')) {
                if (!(Get-PSDrive -Name $letter -ErrorAction SilentlyContinue) -and !(Test-Path "${letter}:\")) {
                    $aliasDrive = "${letter}:"; break
                }
            }
            if (!$aliasDrive) { throw 'Qt5 打包需要一个空闲盘符处理工具路径中的空格。' }
            InvokeChecked 'subst.exe' @($aliasDrive, $toolRoot)
            $settings['qt'] = $aliasDrive + $qt.Substring($toolRoot.Length).Replace('\','/')
            [IO.File]::WriteAllText($settingsPath, ($settings | ConvertTo-Json -Depth 5), [Text.UTF8Encoding]::new($false))
        }
        InvokeChecked (Join-Path $qt 'bin/androiddeployqt.exe') @('--input',$settingsPath,'--output',$stage,
            '--android-platform','android-35','--jdk',$JdkDirectory,'--aux-mode')
    } finally { if ($aliasDrive) { & subst.exe $aliasDrive /D } }
    # Qt5's deployment tool requires package=; AGP8 uses the explicit namespace.
    $manifestPath = Join-Path $stage 'AndroidManifest.xml'
    $manifest = [xml][IO.File]::ReadAllText($manifestPath)
    $manifest.manifest.RemoveAttribute('package')
    $manifest.Save($manifestPath)
    [IO.File]::AppendAllText((Join-Path $stage 'gradle.properties'),
        "`nqt5AndroidDir=$($qt.Replace('\','/'))/src/android/java`n", [Text.UTF8Encoding]::new($false))
    $licenseRoot = Join-Path $stage 'assets/licenses'
    [IO.Directory]::CreateDirectory($licenseRoot) | Out-Null
    Copy-Item -LiteralPath (Join-Path $workspace 'LICENSE') -Destination (Join-Path $licenseRoot 'PROJECT-GPL3.txt')
    foreach ($name in @('android-ffmpeg','android-qt','android-openssl')) {
        $source = Join-Path $workspace "third_party/$name"
        $destination = Join-Path $licenseRoot $(switch ($name) {'android-qt' {'qt'} 'android-openssl' {'openssl'} default {$name}})
        Copy-Item -LiteralPath $source -Destination $destination -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $workspace 'android/licenses/README.md') -Destination $licenseRoot
    $keyDirectory = Join-Path $toolRoot 'signing'
    [IO.Directory]::CreateDirectory($keyDirectory) | Out-Null
    $keystore = Join-Path $keyDirectory 'android-preview.jks'
    if (!(Test-Path -LiteralPath $keystore)) {
        InvokeChecked (Join-Path $JdkDirectory 'bin/keytool.exe') @('-genkeypair','-keystore',$keystore,
            '-alias','lmsc-preview','-storepass','android','-keypass','android','-keyalg','RSA','-keysize','2048',
            '-validity','10000','-dname','CN=LMSC Android Preview','-storetype','JKS')
    }
    Push-Location $stage
    try {
        $gradleArguments = @('assembleDebug','--no-daemon', "-PlmscDebugKeystore=$($keystore.Replace('\','/'))")
        if ($PlatformTests) { $gradleArguments += '-PlmscTestApk=true' }
        InvokeChecked (Join-Path $stage 'gradlew.bat') $gradleArguments
    } finally { Pop-Location }
    $builtApk = Join-Path $stage 'build/outputs/apk/debug/android-build-debug.apk'
    if (!(Test-Path -LiteralPath $builtApk)) {
        $builtApk = (Get-ChildItem -LiteralPath (Join-Path $stage 'build/outputs/apk/debug') -Filter '*.apk' -File | Select-Object -First 1).FullName
    }
    if (!$builtApk) { throw 'Gradle 未生成 APK。' }
    InvokeChecked (Join-Path $AndroidSdkDirectory 'build-tools/35.0.0/apksigner.bat') @('verify','--verbose',$builtApk)
    $dist = WorkspacePath 'dist'
    [IO.Directory]::CreateDirectory($dist) | Out-Null
    $label = if ($PlatformTests) { '平台合成验证' } else { '安卓预览版' }
    $result = Join-Path $dist ("光剑曲谱制作-0.6.0-$label-" + [DateTime]::Now.ToString('yyyyMMdd-HHmmss') + '.apk')
    if (Test-Path -LiteralPath $result) { throw "输出已存在，未覆盖：$result" }
    Copy-Item -LiteralPath $builtApk -Destination $result
    $hash = (Get-FileHash -LiteralPath $result -Algorithm SHA256).Hash
    [IO.File]::WriteAllText($result + '.sha256', $hash + '  ' + [IO.Path]::GetFileName($result) + "`n", [Text.UTF8Encoding]::new($false))
    Write-Output $result
} finally { $env:JAVA_HOME = $oldJava; $env:ANDROID_SDK_ROOT = $oldSdk; $env:GRADLE_USER_HOME = $oldGradle }
