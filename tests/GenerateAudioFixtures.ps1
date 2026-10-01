param(
    [string]$OutputDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'build-audio-test'),
    [string]$ToolsDirectory = (Join-Path (Split-Path -Parent $PSScriptRoot) 'third_party\ffmpeg\bin')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$audioFixtureFfmpeg = Join-Path $ToolsDirectory 'ffmpeg.exe'
$audioClicks = Join-Path $OutputDirectory 'clicks.wav'
$audioMp3 = Join-Path $OutputDirectory 'clicks.mp3'
$audioMp4 = Join-Path $OutputDirectory 'two-tracks.mp4'
$audioVideoOnly = Join-Path $OutputDirectory 'video-only.mp4'
# Twelve seconds of synthetic 120 BPM clicks, grid offset 0.25 seconds.
$audioExpression = 'aevalsrc=if(lt(mod(t-0.25\,0.5)\,0.03)*gte(t\,0.25)\,0.2*sin(2*PI*1200*mod(t-0.25\,0.5))*exp(-120*mod(t-0.25\,0.5))\,0):s=44100:d=12'
& $audioFixtureFfmpeg -v error -y -f lavfi -i $audioExpression -ac 2 $audioClicks
if ($LASTEXITCODE -ne 0) { throw 'Synthetic click generation failed.' }
& $audioFixtureFfmpeg -v error -y -i $audioClicks -c:a libmp3lame $audioMp3
if ($LASTEXITCODE -ne 0) { throw 'MP3 fixture generation failed.' }
& $audioFixtureFfmpeg -v error -y -f lavfi -i 'color=c=black:s=64x64:r=5:d=12' -i $audioClicks -i $audioClicks -map 0:v -map 1:a -map 2:a -c:v mpeg4 -c:a aac -metadata:s:a:0 title=Clicks -metadata:s:a:1 title=Alternate -shortest $audioMp4
if ($LASTEXITCODE -ne 0) { throw 'MP4 fixture generation failed.' }
& $audioFixtureFfmpeg -v error -y -i $audioMp4 -map 0:v:0 -c:v copy -an $audioVideoOnly
if ($LASTEXITCODE -ne 0) { throw 'Video-only fixture generation failed.' }
Write-Output "Synthetic audio fixtures ready: $OutputDirectory"
