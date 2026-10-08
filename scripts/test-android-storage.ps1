param(
    [Parameter(Mandatory = $true)][string]$JdkDirectory,
    [Parameter(Mandatory = $true)][string]$AndroidSdkDirectory,
    [string]$AndroidPlatform = 'android-35'
)

$ErrorActionPreference = 'Stop'
$repositoryDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$outputDirectory = Join-Path $repositoryDirectory 'build/android-storage-java-check'
$androidJar = Join-Path $AndroidSdkDirectory "platforms/$AndroidPlatform/android.jar"
$javaCompiler = Join-Path $JdkDirectory 'bin/javac.exe'
$javaRuntime = Join-Path $JdkDirectory 'bin/java.exe'
foreach ($requiredFile in @($androidJar, $javaCompiler, $javaRuntime)) {
    if (!(Test-Path -LiteralPath $requiredFile -PathType Leaf)) { throw "找不到验证工具：$requiredFile" }
}
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$sources = @(
    (Join-Path $repositoryDirectory 'android/src/org/lmsc/SafeZip.java'),
    (Join-Path $repositoryDirectory 'android/src/org/lmsc/ExportSafety.java'),
    (Join-Path $repositoryDirectory 'android/src/org/lmsc/StorageBridge.java'),
    (Join-Path $repositoryDirectory 'tests/android/SafeZipTest.java'),
    (Join-Path $repositoryDirectory 'tests/android/ExportSafetyTest.java'),
    (Join-Path $repositoryDirectory 'tests/android/java/org/lmsc/PlatformStorageHarness.java')
)
& $javaCompiler -encoding UTF-8 -source 8 -target 8 -cp $androidJar -d $outputDirectory @sources
if ($LASTEXITCODE -ne 0) { throw "Android Java 编译失败，退出码：$LASTEXITCODE" }
& $javaRuntime -cp $outputDirectory SafeZipTest $outputDirectory
if ($LASTEXITCODE -ne 0) { throw "合成 ZIP 边界验证失败，退出码：$LASTEXITCODE" }
& $javaRuntime -cp $outputDirectory ExportSafetyTest
if ($LASTEXITCODE -ne 0) { throw "导出部分失败位置报告验证失败，退出码：$LASTEXITCODE" }
Write-Host 'Java 编译、合成 ZIP 边界及导出部分失败报告验证通过。SAF 和 Keystore 仍需 Android 设备或模拟器验证。'
