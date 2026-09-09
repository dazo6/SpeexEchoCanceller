param(
    [Parameter(Mandatory = $true)]
    [string]$SourceDir,

    [Parameter(Mandatory = $true)]
    [string]$TargetDir,

    [Parameter(Mandatory = $true)]
    [string]$QtBinDir
)

$ErrorActionPreference = 'Stop'

$sourcePath = [System.IO.Path]::GetFullPath($SourceDir)
$targetPath = [System.IO.Path]::GetFullPath($TargetDir)
$exeName = 'SpeexEchoCanceller.exe'
$sourceExe = Join-Path $sourcePath $exeName
$targetExe = Join-Path $targetPath $exeName
$deployQt = Join-Path ([System.IO.Path]::GetFullPath($QtBinDir)) 'windeployqt.exe'

if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "Build output not found: $sourceExe"
}
if (-not (Test-Path -LiteralPath $deployQt -PathType Leaf)) {
    throw "windeployqt not found: $deployQt"
}
if ([System.IO.Path]::GetPathRoot($targetPath) -eq $targetPath) {
    throw "Refusing to deploy to a drive root: $targetPath"
}

# If the deployed copy is currently running, remember whether its window is
# visible, stop only that exact executable, then restore it after deployment.
$restartMode = $null
$targetComparison = $targetExe.TrimEnd('\').ToLowerInvariant()
function Stop-DeployedInstance {
    $runningTargets = @(Get-Process -Name 'SpeexEchoCanceller' -ErrorAction SilentlyContinue | Where-Object {
        try { $_.Path.TrimEnd('\').ToLowerInvariant() -eq $targetComparison } catch { $false }
    })
    if ($runningTargets.Count -gt 0) {
        if ($null -eq $script:restartMode) {
            $script:restartMode = if (($runningTargets | Where-Object { $_.MainWindowHandle -ne 0 }).Count -gt 0) {
                'visible'
            } else {
                'background'
            }
        }
        $runningTargets | Stop-Process -Force
        $runningTargets | Wait-Process -Timeout 10 -ErrorAction SilentlyContinue
    }
}

Stop-DeployedInstance

& $deployQt --release --no-translations --compiler-runtime $sourceExe
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

New-Item -ItemType Directory -Path $targetPath -Force | Out-Null

# A login/startup helper can race with a developer deployment. Check once more
# immediately before replacing the executable and retry if the file is locked.
Stop-DeployedInstance

$rootFiles = @(
    $exeName,
    'RealAEC.dll',
    'D3Dcompiler_47.dll',
    'opengl32sw.dll',
    'libgcc_s_seh-1.dll',
    'libstdc++-6.dll',
    'libwinpthread-1.dll'
)
foreach ($name in $rootFiles) {
    $sourceFile = Join-Path $sourcePath $name
    if (Test-Path -LiteralPath $sourceFile -PathType Leaf) {
        $destinationFile = Join-Path $targetPath $name
        for ($attempt = 1; $attempt -le 5; $attempt++) {
            try {
                Copy-Item -LiteralPath $sourceFile -Destination $destinationFile -Force
                break
            } catch {
                if ($attempt -eq 5) { throw }
                Stop-DeployedInstance
                Start-Sleep -Milliseconds 300
            }
        }
    }
}

Get-ChildItem -LiteralPath $sourcePath -Filter 'Qt6*.dll' -File | ForEach-Object {
    Copy-Item -LiteralPath $_.FullName -Destination (Join-Path $targetPath $_.Name) -Force
}

$pluginDirectories = @(
    'assets',
    'generic',
    'iconengines',
    'imageformats',
    'networkinformation',
    'platforms',
    'styles',
    'tls'
)
foreach ($directory in $pluginDirectories) {
    $sourceDirectory = Join-Path $sourcePath $directory
    if (Test-Path -LiteralPath $sourceDirectory -PathType Container) {
        $targetDirectory = Join-Path $targetPath $directory
        New-Item -ItemType Directory -Path $targetDirectory -Force | Out-Null
        Copy-Item -Path (Join-Path $sourceDirectory '*') -Destination $targetDirectory -Recurse -Force
    }
}

# Keep the deployed user's selected devices, background and engine state.
$sourceConfig = Join-Path $sourcePath 'config.ini'
$targetConfig = Join-Path $targetPath 'config.ini'
if ((Test-Path -LiteralPath $sourceConfig -PathType Leaf) -and
    -not (Test-Path -LiteralPath $targetConfig -PathType Leaf)) {
    Copy-Item -LiteralPath $sourceConfig -Destination $targetConfig
}

if ($restartMode -eq 'visible') {
    Start-Process -FilePath $targetExe -WorkingDirectory $targetPath
} elseif ($restartMode -eq 'background') {
    Start-Process -FilePath $targetExe -ArgumentList '--background' -WorkingDirectory $targetPath
}

Write-Host "Deployment complete: $targetPath"
