# Ori LVGL desktop simulator — PowerShell build script
# Works without a Unix shell. Uses MinGW gcc/g++ from CodeBlocks.
#
# Usage:
#   cd sim
#   .\build.ps1          # compile + link only
#   .\build.ps1 -Run     # compile, link, then run to generate screenshots

param([switch]$Run, [switch]$Clean)

$ErrorActionPreference = "Stop"
$env:PATH = "C:\Program Files\CodeBlocks\MinGW\bin;" + $env:PATH

$SimDir   = $PSScriptRoot
$LvglRoot = "$SimDir\..\firmware\.pio\libdeps\ori\lvgl"
$FwInc    = "$SimDir\..\firmware\include"
$FwSrc    = "$SimDir\..\firmware\src"
$Build    = "$SimDir\build"
$Target   = "$Build\ori_sim.exe"

if ($Clean) {
    Remove-Item -Recurse -Force $Build -ErrorAction SilentlyContinue
    Remove-Item -Recurse -Force "$SimDir\screenshots" -ErrorAction SilentlyContinue
    Write-Host "[build] cleaned."
    exit 0
}

$CppFlags = @(
    "-DLV_CONF_INCLUDE_SIMPLE",
    "-DLV_LVGL_H_INCLUDE_SIMPLE",
    "-DLV_MEM_SIZE=2097152",
    "-DLV_USE_PERF_MONITOR=0",   # disable the bottom-right ? overlay in sim
    "-DLV_USE_MEM_MONITOR=0",
    "-I$SimDir",
    "-I$FwInc",
    "-I$FwSrc",
    "-I$LvglRoot",
    "-I$LvglRoot\src"
)
$CFlags   = @("-O2", "-w", "-std=gnu11")
$CxxFlags = @("-O2", "-w", "-std=gnu++14")

# ---- source lists --------------------------------------------------------

$LvglSrcs = Get-ChildItem "$LvglRoot\src" -Recurse -Filter "*.c" |
    Select-Object -ExpandProperty FullName

$FwCSrcs = @(Get-ChildItem "$FwSrc\fonts" -Filter "*.c" -ErrorAction SilentlyContinue |
    Select-Object -ExpandProperty FullName)

$FwCppSrcs = @(
    "$FwSrc\theme.cpp",
    "$FwSrc\mock_data.cpp",
    "$FwSrc\ui_helpers.cpp"
) + @(Get-ChildItem "$FwSrc\widgets" -Filter "*.cpp" | Select-Object -ExpandProperty FullName) +
    @(Get-ChildItem "$FwSrc\screens" -Filter "*.cpp" | Select-Object -ExpandProperty FullName)

$SimCppSrcs = @(
    "$SimDir\main.cpp",
    "$SimDir\arduino_shim.cpp"
)

# ---- object path helper --------------------------------------------------
# Mirrors the source path into build/, replacing the drive letter and colons.

$Repo = (Resolve-Path "$SimDir\..").Path.TrimEnd('\')

function Get-Obj($src) {
    $rel = $src
    if ($rel.StartsWith($Repo)) { $rel = $rel.Substring($Repo.Length).TrimStart('\') }
    else { $rel = [System.IO.Path]::GetFileName($src) }
    return "$Build\$rel.o"
}

# ---- compile helpers -----------------------------------------------------

$Errors = 0

function Compile-C($src, $obj) {
    $dir = Split-Path $obj
    New-Item -ItemType Directory -Force $dir | Out-Null
    $out = & gcc @CppFlags @CFlags -c -o $obj $src 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] $src"
        $out | Write-Host
        $script:Errors++
    }
}

function Compile-Cpp($src, $obj) {
    $dir = Split-Path $obj
    New-Item -ItemType Directory -Force $dir | Out-Null
    $out = & g++ @CppFlags @CxxFlags -c -o $obj $src 2>&1
    if ($LASTEXITCODE -ne 0) {
        Write-Host "[FAIL] $src"
        $out | Write-Host
        $script:Errors++
    }
}

# ---- build LVGL (skip files that haven't changed) ------------------------

Write-Host "[build] compiling LVGL ($($LvglSrcs.Count) C files)..."
$lvglObjs = @()
foreach ($src in $LvglSrcs) {
    $obj = Get-Obj $src
    $lvglObjs += $obj
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge (Get-Item $src).LastWriteTime)) {
        continue  # up to date
    }
    Compile-C $src $obj
}
Write-Host "[build] LVGL done ($Errors errors so far)."

# ---- build firmware C sources (fonts) ------------------------------------

Write-Host "[build] compiling firmware C sources ($($FwCSrcs.Count) files)..."
$fwCObjs = @()
foreach ($src in $FwCSrcs) {
    $obj = Get-Obj $src
    $fwCObjs += $obj
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge (Get-Item $src).LastWriteTime)) {
        continue
    }
    Compile-C $src $obj
}

# ---- build firmware C++ sources ------------------------------------------

Write-Host "[build] compiling firmware C++ sources ($($FwCppSrcs.Count) files)..."
$fwObjs = @()
foreach ($src in $FwCppSrcs) {
    $obj = Get-Obj $src
    $fwObjs += $obj
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge (Get-Item $src).LastWriteTime)) {
        continue
    }
    Write-Host "  $([System.IO.Path]::GetFileName($src))"
    Compile-Cpp $src $obj
}

# ---- build sim sources ---------------------------------------------------

Write-Host "[build] compiling sim sources..."
$simObjs = @()
foreach ($src in $SimCppSrcs) {
    $obj = Get-Obj $src
    $simObjs += $obj
    if ((Test-Path $obj) -and ((Get-Item $obj).LastWriteTime -ge (Get-Item $src).LastWriteTime)) {
        continue
    }
    Write-Host "  $([System.IO.Path]::GetFileName($src))"
    Compile-Cpp $src $obj
}

if ($Errors -gt 0) {
    Write-Host "[build] $Errors compile error(s). Aborting link."
    exit 1
}

# ---- link ----------------------------------------------------------------

$AllObjs = $lvglObjs + $fwCObjs + $fwObjs + $simObjs
Write-Host "[build] linking $($AllObjs.Count) objects -> ori_sim.exe ..."

$linkOut = & g++ -o $Target @AllObjs 2>&1
if ($LASTEXITCODE -ne 0) {
    Write-Host "[FAIL] link"
    $linkOut | Write-Host
    exit 1
}
Write-Host "[build] link OK -> $Target"

# ---- run -----------------------------------------------------------------

if ($Run) {
    New-Item -ItemType Directory -Force "$SimDir\screenshots" | Out-Null
    Write-Host "[build] running sim..."
    Set-Location $SimDir
    & $Target
    Write-Host "[build] screenshots generated in $SimDir\screenshots\"
}
