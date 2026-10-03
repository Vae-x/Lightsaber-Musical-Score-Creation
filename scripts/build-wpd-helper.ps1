[CmdletBinding()]
param(
    [string]$BuildDirectory = 'build/tools/mtp',
    [string]$OutputDirectory = ''
)
$ErrorActionPreference = 'Stop'
$workspace = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildPath = if ([IO.Path]::IsPathRooted($BuildDirectory)) { [IO.Path]::GetFullPath($BuildDirectory) } else { [IO.Path]::GetFullPath((Join-Path $workspace $BuildDirectory)) }
$outputPath = if ([string]::IsNullOrWhiteSpace($OutputDirectory)) { $buildPath } elseif ([IO.Path]::IsPathRooted($OutputDirectory)) { [IO.Path]::GetFullPath($OutputDirectory) } else { [IO.Path]::GetFullPath((Join-Path $workspace $OutputDirectory)) }
foreach ($candidate in @($buildPath, $outputPath)) {
    if (-not $candidate.StartsWith($workspace + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) { throw 'WPD 辅助程序输出必须处于项目工作区。' }
}
$compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
if (-not (Test-Path -LiteralPath $compiler -PathType Leaf)) { throw '需要 64 位 Windows 的 .NET Framework 4 编译器来构建 WPD 辅助程序。' }
New-Item -ItemType Directory -Force -Path $buildPath | Out-Null
$executable = Join-Path $buildPath 'WpdTransfer.exe'
$source = Join-Path $workspace 'scripts/windows/WpdTransfer.cs'
& $compiler /nologo /target:exe /platform:x64 /optimize+ /utf8output /codepage:65001 /reference:System.Web.Extensions.dll ('/out:' + $executable) $source
if ($LASTEXITCODE -ne 0) { throw ('WPD 辅助程序编译失败：' + $LASTEXITCODE) }
if (-not [string]::Equals($buildPath, $outputPath, [StringComparison]::OrdinalIgnoreCase)) {
    New-Item -ItemType Directory -Force -Path $outputPath | Out-Null
    Copy-Item -LiteralPath $executable -Destination (Join-Path $outputPath 'WpdTransfer.exe') -Force
}
Write-Output ('已构建 64 位 WPD 辅助程序：' + (Join-Path $outputPath 'WpdTransfer.exe'))
