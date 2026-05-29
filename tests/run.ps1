#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Smoke-tests the exception-lower pass against the fixture .cpp files.

.DESCRIPTION
    For each tests/*.cpp fixture this script:
      1. Compiles the source to LLVM bitcode (MSVC EH by default, Itanium EH
         for fixtures whose name starts with `itanium_`).
      2. Runs exception-lower.exe on the bitcode.
      3. Disassembles the lowered bitcode to text IR.
      4. Runs `opt -passes=verify` on the lowered bitcode.
      5. Verifies that the lowered text IR satisfies the // CHECK directives
         embedded in the fixture source.

    The // CHECK / CHECK-LABEL / CHECK-NOT / CHECK-DAG directives use a
    deliberate subset of LLVM's FileCheck syntax:
        CHECK:        pattern must appear (literal substring) somewhere.
        CHECK-LABEL:  pattern must appear; semantically marks a section.
        CHECK-DAG:    pattern must appear; order relative to other DAG/CHECK
                      lines is not enforced.
        CHECK-NOT:    pattern must NOT appear anywhere in the lowered IR.
    All matches are literal substring matches — no regex, no {{...}}.
    This is intentionally simpler than upstream FileCheck (which is not
    bundled in the standard LLVM Windows binary release).

.PARAMETER Filter
    Glob pattern matched against fixture basenames. Defaults to '*'.

.PARAMETER LlvmBin
    Override the LLVM bin directory. Falls back to $env:LLVM_BIN, then
    a default location, then PATH.

.PARAMETER ExcLow
    Override the exception-lower.exe path. Falls back to $env:EXCLOW_BIN,
    then build/exception-lower.exe under the repo root.

.EXAMPLE
    pwsh tests/run.ps1

.EXAMPLE
    pwsh tests/run.ps1 -Filter add_one_multi*
#>

[CmdletBinding()]
param(
    [string]$Filter = '*',
    [string]$LlvmBin,
    [string]$ExcLow
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
$TestsDir = $PSScriptRoot
$OutDir   = Join-Path $TestsDir 'out'

if (-not (Test-Path $OutDir)) {
    New-Item -ItemType Directory -Path $OutDir | Out-Null
}

# -- tool discovery --------------------------------------------------------

if (-not $LlvmBin) {
    $LlvmBin = if ($env:LLVM_BIN) { $env:LLVM_BIN }
               else { 'C:\Users\ameliapayne\clang+llvm-20.1.6-x86_64-pc-windows-msvc\bin' }
}

if (-not $ExcLow) {
    $ExcLow = if ($env:EXCLOW_BIN) { $env:EXCLOW_BIN }
              else { Join-Path $RepoRoot 'build/exception-lower.exe' }
}

function Resolve-Tool([string]$name) {
    $local = Join-Path $LlvmBin "$name.exe"
    if (Test-Path $local) { return $local }
    $cmd = Get-Command "$name.exe" -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    throw "Cannot find $name.exe. Set -LlvmBin or `$env:LLVM_BIN."
}

$Clang   = Resolve-Tool 'clang++'
$Opt     = Resolve-Tool 'opt'
$LlvmDis = Resolve-Tool 'llvm-dis'

if (-not (Test-Path $ExcLow)) {
    throw "exception-lower.exe not found at $ExcLow. Build the project first or set -ExcLow / `$env:EXCLOW_BIN."
}

Write-Host "clang++:         $Clang"
Write-Host "opt:             $Opt"
Write-Host "llvm-dis:        $LlvmDis"
Write-Host "exception-lower: $ExcLow"
Write-Host ""

# -- CHECK directive parser & verifier -------------------------------------

function Get-CheckDirectives([string]$Path) {
    $checks = New-Object System.Collections.Generic.List[object]
    foreach ($line in Get-Content -LiteralPath $Path) {
        if ($line -match '^\s*//\s*(CHECK(?:-(?:NOT|DAG|LABEL))?):\s*(.+?)\s*$') {
            $checks.Add([pscustomobject]@{
                Kind    = $Matches[1]
                Pattern = $Matches[2]
            })
        }
    }
    ,$checks
}

function Test-CheckDirectives([string]$IrPath, $Checks) {
    $body = Get-Content -LiteralPath $IrPath -Raw
    $failures = New-Object System.Collections.Generic.List[string]
    foreach ($c in $Checks) {
        $present = $body.Contains($c.Pattern)
        switch ($c.Kind) {
            'CHECK'       { if (-not $present) { $failures.Add("missing: $($c.Pattern)") } }
            'CHECK-LABEL' { if (-not $present) { $failures.Add("missing label: $($c.Pattern)") } }
            'CHECK-DAG'   { if (-not $present) { $failures.Add("missing (dag): $($c.Pattern)") } }
            'CHECK-NOT'   { if ($present)      { $failures.Add("forbidden present: $($c.Pattern)") } }
        }
    }
    ,$failures
}

# -- fixture loop ----------------------------------------------------------

$fixtures = Get-ChildItem -LiteralPath $TestsDir -Filter '*.cpp' |
            Where-Object { $_.BaseName -like $Filter } |
            Sort-Object Name

if ($fixtures.Count -eq 0) {
    Write-Error "No fixtures matched filter '$Filter'."
    exit 1
}

$totalPass = 0
$totalFail = 0
$failureNames = New-Object System.Collections.Generic.List[string]

foreach ($f in $fixtures) {
    $name = $f.BaseName
    Write-Host "=== $name ===" -ForegroundColor Cyan

    $triple = if ($name -like 'itanium_*') { 'x86_64-pc-linux-gnu' }
              else { 'x86_64-pc-windows-msvc' }

    $bc     = Join-Path $OutDir "$name.bc"
    $low_bc = Join-Path $OutDir "${name}_lowered.bc"
    $low_ll = Join-Path $OutDir "${name}_lowered.ll"

    & $Clang -target $triple -O0 -emit-llvm -c $f.FullName -o $bc 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL (clang++ exit $LASTEXITCODE)" -ForegroundColor Red
        $totalFail++; $failureNames.Add("$name (compile)"); continue
    }

    & $ExcLow $bc -o $low_bc 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL (exception-lower exit $LASTEXITCODE)" -ForegroundColor Red
        $totalFail++; $failureNames.Add("$name (lower)"); continue
    }

    & $LlvmDis $low_bc -o $low_ll 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL (llvm-dis exit $LASTEXITCODE)" -ForegroundColor Red
        $totalFail++; $failureNames.Add("$name (disas)"); continue
    }

    & $Opt -passes=verify $low_bc -disable-output 2>&1 | Write-Host
    if ($LASTEXITCODE -ne 0) {
        Write-Host "  FAIL (opt -passes=verify exit $LASTEXITCODE)" -ForegroundColor Red
        $totalFail++; $failureNames.Add("$name (verify)"); continue
    }

    $checks = Get-CheckDirectives -Path $f.FullName
    if ($checks.Count -eq 0) {
        Write-Host "  PASS (no CHECK directives; verifier-only)" -ForegroundColor Yellow
        $totalPass++
        continue
    }

    $failures = Test-CheckDirectives -IrPath $low_ll -Checks $checks
    if ($failures.Count -eq 0) {
        Write-Host "  PASS ($($checks.Count) CHECK directives)" -ForegroundColor Green
        $totalPass++
    } else {
        Write-Host "  FAIL ($($failures.Count) of $($checks.Count) CHECK directives)" -ForegroundColor Red
        foreach ($msg in $failures) { Write-Host "    $msg" -ForegroundColor Red }
        $totalFail++
        $failureNames.Add($name)
    }
    Write-Host ""
}

Write-Host ""
$color = if ($totalFail -eq 0) { 'Green' } else { 'Red' }
Write-Host "Summary: $totalPass passed, $totalFail failed" -ForegroundColor $color
if ($totalFail -gt 0) {
    Write-Host "Failed: $($failureNames -join ', ')" -ForegroundColor Red
    exit 1
}
exit 0
