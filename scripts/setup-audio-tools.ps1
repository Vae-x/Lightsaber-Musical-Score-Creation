param([string]$ProjectDirectory = (Split-Path -Parent $PSScriptRoot))
$ErrorActionPreference = 'Stop'
$audioWorkspace = [IO.Path]::GetFullPath($ProjectDirectory)
$audioBuildDirectory = Join-Path $audioWorkspace 'build-audio-tools'
$audioToolDirectory = Join-Path $audioWorkspace 'third_party\ffmpeg'
$audioAsset = 'ffmpeg-n9.0.2-17-g2a571b6068-win64-lgpl-shared-9.0.zip'
$audioUrl = "https://github.com/BtbN/FFmpeg-Builds/releases/download/autobuild-2026-09-30-13-08/$audioAsset"
$audioExpectedSha256 = '7157177b8a6cb2174c1650ba8c71b363f2c78cba5330f88c4c02cf5b2b880646'
New-Item -ItemType Directory -Path $audioBuildDirectory -Force | Out-Null
$audioArchive = Join-Path $audioBuildDirectory $audioAsset
if (-not (Test-Path -LiteralPath $audioArchive)) {
    Invoke-WebRequest -Uri $audioUrl -OutFile $audioArchive
}
if ((Get-FileHash -Algorithm SHA256 -LiteralPath $audioArchive).Hash.ToLowerInvariant() -ne $audioExpectedSha256) {
    throw 'FFmpeg archive checksum does not match the pinned upstream release.'
}
$audioExtractDirectory = Join-Path $audioBuildDirectory 'pinned-lgpl-unpacked'
Expand-Archive -LiteralPath $audioArchive -DestinationPath $audioExtractDirectory -Force
$audioPackageRoot = Join-Path $audioExtractDirectory ($audioAsset -replace '\.zip$', '')
$audioBinaryDirectory = Join-Path $audioToolDirectory 'bin'
New-Item -ItemType Directory -Path $audioBinaryDirectory -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $audioPackageRoot 'bin\ffmpeg.exe'),(Join-Path $audioPackageRoot 'bin\ffprobe.exe') -Destination $audioBinaryDirectory -Force
Get-ChildItem -LiteralPath (Join-Path $audioPackageRoot 'bin') -Filter '*.dll' | Copy-Item -Destination $audioBinaryDirectory -Force
Copy-Item -LiteralPath (Join-Path $audioPackageRoot 'LICENSE.txt') -Destination $audioToolDirectory -Force
$audioVersion = & (Join-Path $audioBinaryDirectory 'ffmpeg.exe') -version
if ($LASTEXITCODE -ne 0) { throw 'FFmpeg binary failed to start.' }
if (($audioVersion -join "`n") -match '--enable-gpl|--enable-nonfree') { throw 'GPL/nonfree build is not accepted by this setup script.' }
$audioEncoderList = & (Join-Path $audioBinaryDirectory 'ffmpeg.exe') -hide_banner -encoders
if (-not ($audioEncoderList -match 'libvorbis')) { throw 'This FFmpeg build does not include libvorbis.' }
Write-Output $audioVersion[0]
Write-Output "Audio tools ready: $audioBinaryDirectory"
