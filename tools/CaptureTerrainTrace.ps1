param(
    [Parameter(Mandatory = $true)][ValidateSet('Start', 'Stop')][string]$Action,
    [Parameter(Mandatory = $true)][ValidatePattern('^[A-Za-z0-9_-]+$')][string]$Label,
    [string]$DllPath = 'E:\Games\Skyrim_MO2 mods\Mods\NMN_DeformableTerrainReleaseV6\SKSE\Plugins\NMN_DeformableTerrain.dll'
)

$ErrorActionPreference = 'Stop'
$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (!$principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run this script from PowerShell opened as Administrator. Windows tracing requires elevation.'
}
$repoPath = Split-Path -Parent $PSScriptRoot
$traceFolder = Join-Path $repoPath 'build\performance-traces'
New-Item -ItemType Directory -Path $traceFolder -Force | Out-Null
$tracePath = Join-Path $traceFolder "$Label.etl"
$manifestPath = Join-Path $traceFolder "$Label.json"
if ($Action -eq 'Start') {
    if ((Test-Path -LiteralPath $tracePath) -or (Test-Path -LiteralPath $manifestPath)) {
        throw 'This label already exists. Use a new label to preserve previous captures.'
    }
    $dllHash = (Get-FileHash -LiteralPath $DllPath -Algorithm SHA256).Hash
    & wpr.exe -start CPU -filemode
    if ($LASTEXITCODE -ne 0) { throw 'WPR failed to start. Any existing recording was left untouched.' }
    @{ label = $Label; started = (Get-Date).ToString('o'); dllPath = $DllPath; dllSHA256 = $dllHash; profile = 'CPU.Verbose.File' } |
        ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    Write-Host 'Recording started. Return to Skyrim and run the route for 60-90 seconds.'
    Write-Host "Then invoke this script with -Action Stop -Label $Label. It does not monitor Skyrim or stop automatically."
} else {
    if (!(Test-Path -LiteralPath $manifestPath)) { throw 'No start manifest exists for this label.' }
    if (Test-Path -LiteralPath $tracePath) { throw 'The trace already exists; it will not be overwritten.' }
    & wpr.exe -stop $tracePath "Terrain comparison: $Label"
    if ($LASTEXITCODE -ne 0) { throw 'WPR failed to save. Check wpr -status before retrying.' }
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    $manifest | Add-Member -NotePropertyName stopped -NotePropertyValue (Get-Date).ToString('o')
    $manifest | ConvertTo-Json | Set-Content -LiteralPath $manifestPath -Encoding UTF8
    $logPath = 'D:\Libraries\Documents\My Games\Skyrim Special Edition\SKSE\NMN_DeformableTerrain.log'
    if (Test-Path -LiteralPath $logPath) { Copy-Item -LiteralPath $logPath -Destination (Join-Path $traceFolder "$Label.log") }
    Write-Host "Saved $tracePath"
}
