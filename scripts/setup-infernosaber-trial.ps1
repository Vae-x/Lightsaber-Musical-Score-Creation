param(
    [string]$RuntimeDirectory = 'E:/lmsc-infernosaber-runtime',
    [string]$ProjectDirectory = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$trialWorkspace = [IO.Path]::GetFullPath($ProjectDirectory)
$trialRuntime = [IO.Path]::GetFullPath($RuntimeDirectory).TrimEnd('\', '/')
$trialBuild = Join-Path $trialWorkspace 'build/infernosaber-trial'
$trialSourceUrl = 'https://github.com/fred-brenner/InfernoSaber---BeatSaber-Automapper.git'
$trialSourceCommit = '60780a5acda4da67d0c67d01a4705719a77a91e2'
$trialMiniforgeRelease = '26.7.2-0'
$trialInstallerName = "Miniforge3-$trialMiniforgeRelease-Windows-x86_64.exe"
$trialInstallerSize = 148256480
$trialInstallerUrl = "https://github.com/conda-forge/miniforge/releases/download/$trialMiniforgeRelease/$trialInstallerName"
$trialOwnerKind = 'lmsc-infernosaber-trial-runtime'
$trialOwnerPath = Join-Path $trialRuntime '.lmsc-infernosaber-runtime.json'
$trialMiniforge = Join-Path $trialRuntime 'miniforge'
$trialEnvironment = Join-Path $trialRuntime 'env'
$trialSource = Join-Path $trialRuntime 'source'
$trialConda = Join-Path $trialMiniforge 'Scripts/conda.exe'
$trialPython = Join-Path $trialEnvironment 'python.exe'
$trialRunId = Get-Date -Format 'yyyyMMdd-HHmmss-fff'
$trialLogRoot = Join-Path $trialBuild ('setup-' + $trialRunId)
$trialLogIndex = 0
$trialLock = $null
$trialSavedEnvironment = @{}
$trialEnvironmentKeys = @('PATH', 'CONDARC', 'CONDA_PKGS_DIRS', 'CONDA_ENVS_PATH',
    'PYTHONHOME', 'PYTHONPATH', 'PYTHONNOUSERSITE', 'PYTHONDONTWRITEBYTECODE', 'PIP_CONFIG_FILE',
    'PIP_DISABLE_PIP_VERSION_CHECK', 'TF_CPP_MIN_LOG_LEVEL', 'CUDA_VISIBLE_DEVICES')

function Assert-TrialNoReparsePoint([string]$Path) {
    $trialChecked = [IO.Path]::GetFullPath($Path)
    while ($trialChecked) {
        if (Test-Path -LiteralPath $trialChecked) {
            $trialItem = Get-Item -LiteralPath $trialChecked -Force
            if (($trialItem.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0) {
                throw "Refusing reparse point in managed path: $trialChecked"
            }
        }
        $trialParent = [IO.Path]::GetDirectoryName($trialChecked.TrimEnd('\', '/'))
        if ($trialParent -eq $trialChecked) { break }
        $trialChecked = $trialParent
    }
}

function Test-TrialGeneratedBytecode([string]$RelativePath, [string[]]$TrackedPythonPaths) {
    # Only caches of tracked modules produced by this runtime's Python 3.10.
    # Other new files remain protected, including files inside __pycache__.
    $trialCacheMatch = [regex]::Match($RelativePath,
        '^(?<parent>(?:[^/]+/)*)__pycache__/(?<module>[^/]+)\.cpython-310(?:\.opt-[12])?\.pyc$')
    if (-not $trialCacheMatch.Success) { return $false }
    $trialModulePath = $trialCacheMatch.Groups['parent'].Value +
        $trialCacheMatch.Groups['module'].Value + '.py'
    return $TrackedPythonPaths -ccontains $trialModulePath
}

function Assert-TrialCleanUpstreamSource([string]$Git, [string]$Source) {
    $trialTrackedChanges = @(& $Git -C $Source status --porcelain --untracked-files=no)
    if ($LASTEXITCODE -ne 0 -or $trialTrackedChanges.Count -gt 0) {
        throw 'Existing upstream source has tracked changes; setup will not overwrite them.'
    }
    # -z returns literal paths, rather than Git's quoted/escaped display paths.
    $trialTrackedRaw = (& $Git -C $Source ls-files -z -- '*.py') -join "`n"
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect tracked upstream Python files.' }
    $trialTrackedPaths = @($trialTrackedRaw.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries))
    $trialUntrackedRaw = (& $Git -C $Source ls-files --others --exclude-standard -z) -join "`n"
    if ($LASTEXITCODE -ne 0) { throw 'Could not inspect new upstream files.' }
    foreach ($trialNewPath in $trialUntrackedRaw.Split([char]0, [StringSplitOptions]::RemoveEmptyEntries)) {
        if (-not (Test-TrialGeneratedBytecode $trialNewPath $trialTrackedPaths)) {
            throw "Existing upstream source has a new file; setup will not overwrite it: $trialNewPath"
        }
        Assert-TrialNoReparsePoint (Join-Path $Source $trialNewPath)
    }
}

function ConvertTo-TrialWindowsArgument([string]$Argument) {
    # NSIS requires an unquoted /D= argument at the very end; paths were validated above.
    if ($Argument -match '^/D=[A-Za-z]:[\\/][A-Za-z0-9_./\\-]+$') { return $Argument }
    # NSIS parses switches from the raw command line, so /S and other simple
    # ASCII arguments must remain unquoted. No shell executes this argument line.
    if ($Argument -cmatch '^[\x21-\x7e]+$' -and -not $Argument.Contains('"')) { return $Argument }
    # Start-Process joins ArgumentList; quote using the Windows argv rules.
    $trialQuoted = [regex]::Replace($Argument, '(\\*)"', '$1$1\"')
    $trialQuoted = [regex]::Replace($trialQuoted, '(\\+)$', '$1$1')
    return '"' + $trialQuoted + '"'
}

function Invoke-TrialProcess {
    param([string]$FilePath, [string[]]$Arguments, [string]$Label,
          [string]$WorkingDirectory = $trialWorkspace)
    $script:trialLogIndex++
    $trialStep = '{0:D2}-{1}' -f $script:trialLogIndex, $Label
    $trialStdout = Join-Path $trialLogRoot ($trialStep + '.stdout.log')
    $trialStderr = Join-Path $trialLogRoot ($trialStep + '.stderr.log')
    Write-Host "[$trialStep] Running; logs: $trialLogRoot"
    $trialArgumentLine = ($Arguments | ForEach-Object { ConvertTo-TrialWindowsArgument $_ }) -join ' '
    $trialProcess = Start-Process -FilePath $FilePath -ArgumentList $trialArgumentLine `
        -WorkingDirectory $WorkingDirectory -WindowStyle Hidden -Wait -PassThru `
        -RedirectStandardOutput $trialStdout -RedirectStandardError $trialStderr
    if ($trialProcess.ExitCode -ne 0) {
        throw "$Label failed (exit $($trialProcess.ExitCode)). Inspect $trialStdout and $trialStderr; partial files were retained."
    }
    return $trialStdout
}

function Get-TrialDownload([string]$Uri, [string]$Destination, [int]$TimeoutSeconds = 60) {
    $trialPart = $Destination + '.partial'
    for ($trialAttempt = 1; $trialAttempt -le 3; $trialAttempt++) {
        try {
            Invoke-WebRequest -UseBasicParsing -TimeoutSec $TimeoutSeconds -Uri $Uri -OutFile $trialPart
            Move-Item -LiteralPath $trialPart -Destination $Destination -Force
            return
        } catch {
            if ($trialAttempt -eq 3) {
                $trialHttpStatus = 'unavailable'
                try {
                    if ($null -ne $_.Exception.Response) { $trialHttpStatus = [string][int]$_.Exception.Response.StatusCode }
                } catch { }
                throw "Download failed after 3 attempts (HTTP $trialHttpStatus): $Uri. Partial files remain in build/infernosaber-trial."
            }
            Write-Output "Download attempt $trialAttempt failed; retrying. Partial download remains in build/infernosaber-trial."
            Start-Sleep -Seconds (2 * $trialAttempt)
        }
    }
}

function Get-TrialExistingPython {
    # Skip Windows Store aliases and the py launcher: discovering a downloader
    # must never trigger installation of a system Python.
    $trialCandidates = @(Get-Command python, python3 -CommandType Application -ErrorAction SilentlyContinue |
        Where-Object { $_.Source -notmatch '[\\/]WindowsApps[\\/]' } |
        Select-Object -ExpandProperty Source -Unique)
    foreach ($trialCandidate in $trialCandidates) {
        try {
            $trialPythonProbeLog = Invoke-TrialProcess $trialCandidate @('-I', '-c',
                'import sys; sys.exit(0 if sys.version_info >= (3, 10) else 3)') 'probe-existing-downloader-python'
            return $trialCandidate
        } catch {
            Write-Host 'Existing Python is unsuitable for the optional downloader; trying the next candidate.'
        }
    }
    return $null
}

function Get-TrialInstaller([string]$Uri, [string]$Destination, [long]$Size, [string]$ExpectedHash) {
    if (Test-Path -LiteralPath $Destination -PathType Leaf) {
        throw "Cached installer has an unexpected checksum; retained for diagnosis: $Destination"
    }
    $trialDownloaderScript = Join-Path $trialWorkspace 'scripts/fetch-infernosaber-models.py'
    $trialDownloaderPython = Get-TrialExistingPython
    if ($null -ne $trialDownloaderPython -and (Test-Path -LiteralPath $trialDownloaderScript -PathType Leaf)) {
        # Import only the shared bounded/resumable download function. This does
        # not invoke the model downloader or install any Python dependencies.
        $trialDownloadCode = 'import runpy,sys; from pathlib import Path; runpy.run_path(sys.argv[1])["download"](sys.argv[2], Path(sys.argv[3]), int(sys.argv[4]), sys.argv[5])'
        $trialInstallerDownloadLog = Invoke-TrialProcess $trialDownloaderPython @('-I', '-c',
            $trialDownloadCode, $trialDownloaderScript, $Uri, $Destination, [string]$Size,
            $ExpectedHash) 'download-installer-resumable'
        return
    }
    Write-Host 'No suitable existing Python downloader; using a single-stream installer download with a 600-second timeout.'
    Get-TrialDownload $Uri $Destination 600
}

if (-not [Environment]::Is64BitOperatingSystem -or $env:OS -ne 'Windows_NT') {
    throw 'The InfernoSaber trial runtime requires 64-bit Windows.'
}
if (-not [IO.Path]::IsPathRooted($RuntimeDirectory) -or
    $trialRuntime -eq [IO.Path]::GetPathRoot($trialRuntime).TrimEnd('\', '/') -or
    $trialRuntime -cmatch '[^\x21-\x7e]') {
    throw 'RuntimeDirectory must be an absolute, non-root directory without spaces or non-ASCII characters.'
}
Assert-TrialNoReparsePoint $trialRuntime
Assert-TrialNoReparsePoint $trialBuild
$trialGit = (Get-Command git -CommandType Application -ErrorAction Stop | Select-Object -First 1).Source

if (Test-Path -LiteralPath $trialRuntime) {
    if (-not (Test-Path -LiteralPath $trialOwnerPath -PathType Leaf)) {
        throw "Existing runtime directory is not owned by this script: $trialRuntime. No files were changed."
    }
    $trialOwner = Get-Content -LiteralPath $trialOwnerPath -Raw | ConvertFrom-Json
    if ($trialOwner.kind -ne $trialOwnerKind -or $trialOwner.schema -ne 1 -or
        $trialOwner.runtimeDirectory -ne $trialRuntime -or
        $trialOwner.sourceCommit -ne $trialSourceCommit) {
        throw "Runtime owner marker does not match this setup: $trialOwnerPath"
    }
} else {
    [IO.Directory]::CreateDirectory($trialRuntime) | Out-Null
    $trialOwnerJson = [ordered]@{ schema = 1; kind = $trialOwnerKind;
        runtimeDirectory = $trialRuntime; sourceUrl = $trialSourceUrl;
        sourceCommit = $trialSourceCommit; createdUtc = [DateTime]::UtcNow.ToString('o') } |
        ConvertTo-Json
    [IO.File]::WriteAllText($trialOwnerPath, $trialOwnerJson, [Text.UTF8Encoding]::new($false))
}

try {
    $trialLock = [IO.File]::Open((Join-Path $trialRuntime '.setup.lock'),
        [IO.FileMode]::OpenOrCreate, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
    [IO.Directory]::CreateDirectory($trialLogRoot) | Out-Null
    foreach ($trialKey in $trialEnvironmentKeys) {
        $trialSavedEnvironment[$trialKey] = [Environment]::GetEnvironmentVariable($trialKey, 'Process')
    }
    $trialConfig = Join-Path $trialRuntime 'condarc-trial.yml'
    [IO.File]::WriteAllText($trialConfig, "channels:`n  - conda-forge`nchannel_priority: strict`nauto_activate_base: false`n", [Text.UTF8Encoding]::new($false))
    $env:CONDARC = $trialConfig
    $env:CONDA_PKGS_DIRS = Join-Path $trialRuntime 'conda-pkgs'
    $env:CONDA_ENVS_PATH = Join-Path $trialRuntime 'conda-envs'
    $env:PYTHONHOME = $null
    $env:PYTHONPATH = $null
    $env:PYTHONNOUSERSITE = '1'
    $env:PYTHONDONTWRITEBYTECODE = '1'
    $env:PIP_CONFIG_FILE = 'NUL'
    $env:PIP_DISABLE_PIP_VERSION_CHECK = '1'
    $env:TF_CPP_MIN_LOG_LEVEL = '2'
    # Windows TensorFlow 2.15 uses CPU; GPU training is outside this trial.
    $env:CUDA_VISIBLE_DEVICES = '-1'

    if (-not (Test-Path -LiteralPath $trialConda -PathType Leaf)) {
        if (Test-Path -LiteralPath $trialMiniforge) {
            throw "Incomplete Miniforge install retained at $trialMiniforge. Refusing to overwrite it automatically; inspect the setup logs."
        }
        $trialInstaller = Join-Path $trialBuild $trialInstallerName
        $trialChecksum = $trialInstaller + '.sha256'
        Get-TrialDownload ($trialInstallerUrl + '.sha256') $trialChecksum
        $trialHashMatch = [regex]::Match((Get-Content -LiteralPath $trialChecksum -Raw), '(?i)\b[0-9a-f]{64}\b')
        if (-not $trialHashMatch.Success) { throw 'Official Miniforge SHA256 file is invalid.' }
        $trialExpectedHash = $trialHashMatch.Value.ToLowerInvariant()
        if (-not (Test-Path -LiteralPath $trialInstaller -PathType Leaf) -or
            (Get-FileHash -LiteralPath $trialInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -ne $trialExpectedHash) {
            Get-TrialInstaller $trialInstallerUrl $trialInstaller $trialInstallerSize $trialExpectedHash
        }
        if ((Get-FileHash -LiteralPath $trialInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -ne $trialExpectedHash) {
            throw 'Miniforge installer checksum does not match the official release. Installer was not executed.'
        }
        # /D must remain last. No PATH registration, default Python registration or conda init.
        $trialInstallLog = Invoke-TrialProcess $trialInstaller @('/S', '/InstallationType=JustMe',
            '/AddToPath=0', '/RegisterPython=0', ('/D=' + $trialMiniforge)) 'install-miniforge'
        if (-not (Test-Path -LiteralPath $trialConda -PathType Leaf)) { throw 'Miniforge did not create Scripts/conda.exe.' }
    }
    Assert-TrialNoReparsePoint $trialMiniforge
    Assert-TrialNoReparsePoint $trialEnvironment
    Assert-TrialNoReparsePoint $trialSource

    if (Test-Path -LiteralPath $trialSource) {
        if (-not (Test-Path -LiteralPath (Join-Path $trialSource '.git') -PathType Container)) {
            throw "Incomplete source clone retained at $trialSource. No replacement or deletion was attempted."
        }
        $trialRemote = & $trialGit -C $trialSource remote get-url origin
        if ($LASTEXITCODE -ne 0 -or $trialRemote.Trim() -ne $trialSourceUrl) {
            throw 'Existing InfernoSaber source origin differs from the pinned upstream.'
        }
        $trialSourceFiles = @(Get-ChildItem -LiteralPath $trialSource -Force | Where-Object { $_.Name -ne '.git' })
        if ($trialSourceFiles.Count -gt 0) {
            Assert-TrialCleanUpstreamSource $trialGit $trialSource
        }
    } else {
        $trialCloneLog = Invoke-TrialProcess $trialGit @('clone', '--filter=blob:none', '--no-checkout',
            '--single-branch', '--branch', 'main_app', $trialSourceUrl, $trialSource) 'clone-upstream'
    }
    $trialCheckoutLog = Invoke-TrialProcess $trialGit @('-C', $trialSource, 'checkout', '--detach',
        $trialSourceCommit) 'pin-upstream'
    $trialActualCommit = & $trialGit -C $trialSource rev-parse HEAD
    if ($LASTEXITCODE -ne 0 -or $trialActualCommit.Trim() -ne $trialSourceCommit) { throw 'Upstream commit verification failed.' }

    $trialAudioDirectory = Join-Path $trialWorkspace 'third_party/ffmpeg/bin'
    $trialReuseAudio = (Test-Path -LiteralPath (Join-Path $trialAudioDirectory 'ffmpeg.exe') -PathType Leaf) -and
        (Test-Path -LiteralPath (Join-Path $trialAudioDirectory 'ffprobe.exe') -PathType Leaf)
    $trialCondaArguments = @('create', '--prefix', $trialEnvironment, '--yes', '--override-channels',
        '--channel', 'conda-forge', '--strict-channel-priority', 'python=3.10', 'pip',
        'numpy=1.26.4', 'aubio=0.4.9', 'pydub=0.25.1')
    if (Test-Path -LiteralPath (Join-Path $trialEnvironment 'conda-meta/history') -PathType Leaf) {
        $trialCondaArguments[0] = 'install'
    } elseif (Test-Path -LiteralPath $trialEnvironment) {
        throw "Existing environment lacks conda metadata: $trialEnvironment. It was retained for inspection."
    }
    if (-not $trialReuseAudio) { $trialCondaArguments += 'ffmpeg' }
    $trialCondaLog = Invoke-TrialProcess $trialConda $trialCondaArguments 'create-python-audio-environment'
    if (-not (Test-Path -LiteralPath $trialPython -PathType Leaf)) { throw 'Isolated Python executable is missing.' }

    $trialRuntimePath = @($trialEnvironment, (Join-Path $trialEnvironment 'Scripts'),
        (Join-Path $trialEnvironment 'Library/bin'))
    if ($trialReuseAudio) { $trialRuntimePath = @($trialAudioDirectory) + $trialRuntimePath }
    $env:PATH = ($trialRuntimePath -join ';') + ';' + $trialSavedEnvironment['PATH']
    $trialPackages = @('numpy==1.26.4', 'tensorflow==2.15.1', 'keras==2.15.0', 'keras-tcn==3.5.4',
        'scipy==1.13.1', 'scikit-learn==1.3.2', 'librosa==0.11.0', 'ffmpy==0.5.0',
        'huggingface-hub==0.29.3', 'tabulate==0.9.0', 'pillow==10.4.0', 'joblib==1.4.2',
        'progressbar2==4.5.0', 'requests==2.32.3', 'mutagen==1.47.0')
    $trialRequirements = Join-Path $trialRuntime 'requirements-trial.txt'
    [IO.File]::WriteAllText($trialRequirements, (($trialPackages -join "`n") + "`n"), [Text.UTF8Encoding]::new($false))
    $trialWheelDirectory = Join-Path $trialRuntime 'pip-wheels'
    [IO.Directory]::CreateDirectory($trialWheelDirectory) | Out-Null
    $trialPipArguments = @('-m', 'pip', '--isolated', 'install',
        '--index-url', 'https://pypi.org/simple', '--only-binary=:all:', '--retries', '3',
        '--find-links', $trialWheelDirectory,
        '--cache-dir', (Join-Path $trialRuntime 'pip-cache'),
        '--requirement', $trialRequirements)
    # An explicit verified wheel avoids pip choosing the remote link over an
    # identical --find-links candidate when a large download was pre-cached.
    $trialIntelWheel = Join-Path $trialWheelDirectory 'tensorflow_intel-2.15.1-cp310-cp310-win_amd64.whl'
    if (Test-Path -LiteralPath $trialIntelWheel -PathType Leaf) {
        $trialIntelHash = '9f305142b3c5e239c82c463429b1f88726dd27d9f23523871f825493a9ffc5f4'
        if ((Get-Item -LiteralPath $trialIntelWheel).Length -ne 300872148 -or
            (Get-FileHash -LiteralPath $trialIntelWheel -Algorithm SHA256).Hash.ToLowerInvariant() -ne $trialIntelHash) {
            throw 'Cached TensorFlow Intel wheel differs from the pinned PyPI release; file retained.'
        }
        $trialPipArguments += $trialIntelWheel
    }
    $trialPipLog = Invoke-TrialProcess $trialPython $trialPipArguments 'install-inference-dependencies'
    $trialCheckLog = Invoke-TrialProcess $trialPython @('-m', 'pip', '--isolated', 'check') 'pip-check'

    $trialProbe = @'
import importlib, importlib.metadata, json, platform, subprocess, sys
from pathlib import Path
import numpy as np
import tensorflow as tf
from tcn import TCN
import aubio
from pydub import AudioSegment
for name in ('scipy', 'sklearn', 'librosa', 'ffmpy', 'huggingface_hub', 'tabulate',
             'PIL', 'joblib', 'progressbar', 'requests', 'mutagen'):
    importlib.import_module(name)
assert sys.version_info[:2] == (3, 10), sys.version
assert np.__version__ == '1.26.4', np.__version__
assert tf.__version__ == '2.15.1', tf.__version__
# Execute an actual TCN forward pass rather than only checking package metadata.
y = TCN(nb_filters=4, kernel_size=2, dilations=(1,), return_sequences=False)(tf.zeros((1, 8, 2)))
assert tuple(y.shape) == (1, 4), tuple(y.shape)
aubio.tempo('default', 512, 256, 22050)
AudioSegment.silent(duration=10)
for command in ('ffmpeg', 'ffprobe'):
    subprocess.run([command, '-version'], check=True, capture_output=True)
# Import inference entry points without calling main(), which would download weights.
import main
import map_creation.gen_beats
import beat_prediction.find_beats
versions = {d.metadata['Name']: d.version for d in importlib.metadata.distributions()}
Path(sys.argv[1]).write_text(json.dumps({'python': sys.version, 'platform': platform.platform(),
    'tensorflowDevices': [d.name for d in tf.config.list_physical_devices()],
    'packages': versions, 'inferenceImportsPassed': True, 'tcnForwardPassed': True},
    indent=2, ensure_ascii=False), encoding='utf-8')
print('Inference imports, TCN forward pass, aubio and FFmpeg probes passed.')
'@
    $trialProbePath = Join-Path $trialRuntime 'probe-inference.py'
    [IO.File]::WriteAllText($trialProbePath, $trialProbe, [Text.UTF8Encoding]::new($false))
    # The upstream finds its root from cwd. Give the probe only this pinned source on PYTHONPATH.
    $env:PYTHONPATH = $trialSource
    $trialProbeLog = Invoke-TrialProcess $trialPython @($trialProbePath, (Join-Path $trialRuntime 'environment-info.json')) 'probe-inference' $trialSource
    $trialFreezeLog = Invoke-TrialProcess $trialPython @('-m', 'pip', '--isolated', 'freeze', '--all') 'freeze-python'
    $trialExplicitLog = Invoke-TrialProcess $trialConda @('list', '--prefix', $trialEnvironment, '--explicit') 'freeze-conda'
    Copy-Item -LiteralPath $trialFreezeLog -Destination (Join-Path $trialRuntime 'requirements-freeze.txt') -Force
    Copy-Item -LiteralPath $trialExplicitLog -Destination (Join-Path $trialRuntime 'conda-explicit.txt') -Force
    $trialReady = [ordered]@{ schema = 1; kind = $trialOwnerKind; sourceCommit = $trialSourceCommit;
        python = $trialPython; source = $trialSource; ffmpegDirectory = $(if ($trialReuseAudio) { $trialAudioDirectory } else { Join-Path $trialEnvironment 'Library/bin' });
        setupLogs = $trialLogRoot; completedUtc = [DateTime]::UtcNow.ToString('o');
        modelsDownloaded = $false; generationTested = $false } | ConvertTo-Json
    [IO.File]::WriteAllText((Join-Path $trialRuntime 'runtime-ready.json'), $trialReady, [Text.UTF8Encoding]::new($false))
    Write-Output "InfernoSaber inference environment ready: $trialPython"
    Write-Output 'Setup does not download models or generate songs. Run the separate trial command next.'
} finally {
    foreach ($trialKey in $trialSavedEnvironment.Keys) {
        [Environment]::SetEnvironmentVariable($trialKey, $trialSavedEnvironment[$trialKey], 'Process')
    }
    if ($null -ne $trialLock) { $trialLock.Dispose() }
}
