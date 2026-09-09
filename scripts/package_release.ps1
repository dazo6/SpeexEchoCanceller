param(
    [Parameter(Mandatory = $true)]
    [string]$BuildDir,

    [Parameter(Mandatory = $true)]
    [string]$OutputDir,

    [string]$QtBinDir = ''
)

$ErrorActionPreference = 'Stop'
$buildPath = [System.IO.Path]::GetFullPath($BuildDir)
$outputPath = [System.IO.Path]::GetFullPath($OutputDir)
$stagePath = Join-Path $outputPath 'SpeexEchoCanceller-windows-x64'
$sourceExe = Join-Path $buildPath 'SpeexEchoCanceller.exe'

if (-not (Test-Path -LiteralPath $sourceExe -PathType Leaf)) {
    throw "Build output not found: $sourceExe"
}
if ([System.IO.Path]::GetPathRoot($outputPath) -eq $outputPath) {
    throw "Refusing to use a drive root as package output: $outputPath"
}

if (Test-Path -LiteralPath $stagePath) {
    Remove-Item -LiteralPath $stagePath -Recurse -Force
}
New-Item -ItemType Directory -Path $stagePath -Force | Out-Null

Copy-Item -LiteralPath $sourceExe -Destination $stagePath
Copy-Item -LiteralPath (Join-Path $buildPath 'RealAEC.dll') -Destination $stagePath
if (Test-Path -LiteralPath (Join-Path $buildPath 'assets')) {
    Copy-Item -LiteralPath (Join-Path $buildPath 'assets') -Destination $stagePath -Recurse
}

if ([string]::IsNullOrWhiteSpace($QtBinDir)) {
    $deployQt = (Get-Command windeployqt.exe -ErrorAction Stop).Source
} else {
    $deployQt = Join-Path ([System.IO.Path]::GetFullPath($QtBinDir)) 'windeployqt.exe'
}
if (-not (Test-Path -LiteralPath $deployQt -PathType Leaf)) {
    throw "windeployqt not found: $deployQt"
}

& $deployQt --release --no-translations --compiler-runtime (Join-Path $stagePath 'SpeexEchoCanceller.exe')
if ($LASTEXITCODE -ne 0) {
    throw "windeployqt failed with exit code $LASTEXITCODE"
}

$archive = Join-Path $outputPath 'SpeexEchoCanceller-windows-x64.zip'
if (Test-Path -LiteralPath $archive) {
    Remove-Item -LiteralPath $archive -Force
}
Compress-Archive -Path (Join-Path $stagePath '*') -DestinationPath $archive -CompressionLevel Optimal
Write-Host "Package created: $archive"
