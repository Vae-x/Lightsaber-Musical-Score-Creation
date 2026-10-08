param(
    [string]$OutputDirectory = ''
)

$ErrorActionPreference = 'Stop'
$repositoryDirectory = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputDirectory)) {
    $OutputDirectory = Join-Path $repositoryDirectory 'build/android-deps/audio'
}
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
function DownloadAudioArtifact([string]$artifactName, [string]$artifactUrl, [string]$expectedHash) {
    $artifactPath = Join-Path $OutputDirectory $artifactName

if (Test-Path -LiteralPath $artifactPath) {
    if ((Get-FileHash -LiteralPath $artifactPath -Algorithm SHA256).Hash -ne $expectedHash) {
        throw "现有 Android 音频组件的 SHA256 不匹配，未覆盖：$artifactPath"
    }
    return
}

New-Item -ItemType Directory -Path $OutputDirectory -Force | Out-Null
$downloadPath = Join-Path $OutputDirectory ($artifactName + '.download-' + [Guid]::NewGuid().ToString('N'))
try {
    Invoke-WebRequest -Uri $artifactUrl -OutFile $downloadPath -UseBasicParsing
    $actualHash = (Get-FileHash -LiteralPath $downloadPath -Algorithm SHA256).Hash
    if ($actualHash -ne $expectedHash) {
        throw "Android 音频组件的 SHA256 不匹配：预期 $expectedHash，实际 $actualHash"
    }
    if (Test-Path -LiteralPath $artifactPath) {
        throw "下载期间目标已被创建，未覆盖：$artifactPath"
    }
    Move-Item -LiteralPath $downloadPath -Destination $artifactPath
}
finally {
    if (Test-Path -LiteralPath $downloadPath) { Remove-Item -LiteralPath $downloadPath }
}
}

DownloadAudioArtifact 'ffmpeg-kit-audio-6.0.4.aar' `
    'https://repo.maven.apache.org/maven2/dev/ffmpegkit-maintained/ffmpeg-kit-audio/6.0.4/ffmpeg-kit-audio-6.0.4.aar' `
    '54BBC7FCA3F27811A9289EACEAC5B94837F87BD1D97D0F98148D8CBEDCFA93C1'
# Local AARs do not cause Gradle to resolve their Maven POM dependencies. Both
# Java archives are required by FFmpegKitConfig even before running a command.
DownloadAudioArtifact 'smart-exception-java-0.2.1.jar' `
    'https://repo.maven.apache.org/maven2/com/arthenica/smart-exception-java/0.2.1/smart-exception-java-0.2.1.jar' `
    '5B96AAA5F191DEDBEF72FB0C38F1A2B01807920AFC0D92A75A2ACD6E0CC7703C'
DownloadAudioArtifact 'smart-exception-common-0.2.1.jar' `
    'https://repo.maven.apache.org/maven2/com/arthenica/smart-exception-common/0.2.1/smart-exception-common-0.2.1.jar' `
    '1CAD0FB4DFA01755A014331B5ED199281D2C3FAB5ACA5C9D7ABD0B41D0EC3F7B'
Write-Output (Join-Path $OutputDirectory 'ffmpeg-kit-audio-6.0.4.aar')
