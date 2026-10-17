# The whole no-MMU round (docs/system/nommu.md): the build scripts, the flat root, the image, the test set.
#   pwsh linux/nommu/all.ps1 [-Builds userland/build.sh,...] [-NoBuild] [-Kernel] [-Cases "doom-1 nxray"]
param([string[]]$Builds = @(), [switch]$NoBuild, [switch]$Kernel, [string]$Cases = '')
$repo = Split-Path (Split-Path $PSScriptRoot)
$w = (wsl wslpath -a ($PSScriptRoot -replace '\\', '/')).Trim()
$t0 = Get-Date
function at { "$([int]((Get-Date) - $t0).TotalSeconds) s" }
if ($Kernel) { wsl bash "$w/kernel.sh"; "kernel done at $(at)" }
if (-not $NoBuild) { wsl bash "$w/build.sh" @Builds; "build done at $(at)" }
wsl bash "$w/root.sh"
python "$PSScriptRoot\image.py" | Select-Object -Last 1
"image done at $(at)"
Push-Location $repo
$sel = if ($Cases) { $Cases -split ' ' } else { @() }
python "$PSScriptRoot\test.py" @sel 2>&1 | Out-File "$repo\build\nommu\test.txt"
Pop-Location
Get-Content "$repo\build\nommu\test.txt" | Select-String -NotMatch '^PASS' | ForEach-Object { $_.Line.Substring(0, [Math]::Min(200, $_.Line.Length)) }
"all done at $(at)"
