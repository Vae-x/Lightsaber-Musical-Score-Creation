param([string]$ProjectDirectory = (Split-Path -Parent $PSScriptRoot))

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$trialSetupPath = Join-Path $ProjectDirectory 'scripts/setup-infernosaber-trial.ps1'
$trialParseTokens = $null
$trialParseErrors = $null
$trialSetupAst = [Management.Automation.Language.Parser]::ParseFile(
    $trialSetupPath, [ref]$trialParseTokens, [ref]$trialParseErrors)
if ($trialParseErrors.Count -ne 0) { throw "Setup script parse failed: $trialParseErrors" }

# Load only pure preflight functions. The setup body and installer never run.
foreach ($trialFunctionName in @('Assert-TrialNoReparsePoint', 'Test-TrialGeneratedBytecode', 'Assert-TrialCleanUpstreamSource')) {
    $trialFunctionAst = $trialSetupAst.Find({ param($node)
        $node -is [Management.Automation.Language.FunctionDefinitionAst] -and
        $node.Name -eq $trialFunctionName
    }, $true)
    if ($null -eq $trialFunctionAst) { throw "Missing preflight function: $trialFunctionName" }
    . ([ScriptBlock]::Create($trialFunctionAst.Extent.Text))
}

$trialTrackedSources = @('main.py', 'tools/config/config.py', 'tools/config/__init__.py')
$trialChecks = @(
    @{ path = '__pycache__/main.cpython-310.pyc'; allowed = $true },
    @{ path = '__pycache__/main.cpython-310.opt-1.pyc'; allowed = $true },
    @{ path = 'tools/config/__pycache__/config.cpython-310.pyc'; allowed = $true },
    @{ path = 'tools/config/__pycache__/__init__.cpython-310.opt-2.pyc'; allowed = $true },
    @{ path = 'tools/config/__pycache__/new_module.cpython-310.pyc'; allowed = $false },
    @{ path = '__pycache__/main.cpython-312.pyc'; allowed = $false },
    @{ path = '__pycache__/main.pyc'; allowed = $false },
    @{ path = '__pycache__/notes.txt'; allowed = $false },
    @{ path = '__pycache__/main.py'; allowed = $false },
    @{ path = 'main.cpython-310.pyc'; allowed = $false },
    @{ path = 'tools/new_source.py'; allowed = $false },
    @{ path = '../__pycache__/main.cpython-310.pyc'; allowed = $false }
)
foreach ($trialCase in $trialChecks) {
    $trialAllowed = Test-TrialGeneratedBytecode $trialCase.path $trialTrackedSources
    if ($trialAllowed -ne $trialCase.allowed) { throw "Unexpected cache decision: $($trialCase.path)" }
}

$trialSetupText = [IO.File]::ReadAllText($trialSetupPath)
if ($trialSetupText -notmatch "'PYTHONDONTWRITEBYTECODE'" -or
    $trialSetupText -notmatch '\$env:PYTHONDONTWRITEBYTECODE\s*=\s*''1''') {
    throw 'The setup must save/restore and set the bytecode environment option.'
}

Write-Output "InfernoSaber setup parse and $($trialChecks.Count) cache protection cases passed. No installation performed."
