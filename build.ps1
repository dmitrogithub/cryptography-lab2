param([switch]$Debug)
$ErrorActionPreference = 'Stop'
$projectRoot = $PSScriptRoot
$vsLocator = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if (!(Test-Path -LiteralPath $vsLocator)) { throw 'Install Visual Studio Build Tools with Desktop development with C++.' }
$vsRoot = & $vsLocator -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vsRoot) {
    # Some existing BuildTools installations have a cancelled installer state,
    # even though their compiler, libraries and SDK work. Validate files directly.
    foreach ($candidate in (& $vsLocator -all -products '*' -property installationPath)) {
        $candidateSetup = Join-Path $candidate 'VC\Auxiliary\Build\vcvars64.bat'
        $candidateTools = Join-Path $candidate 'VC\Tools\MSVC'
        if ((Test-Path -LiteralPath $candidateSetup) -and (Test-Path -LiteralPath $candidateTools)) {
            $vsRoot = $candidate
            break
        }
    }
}
if (!$vsRoot) { throw 'Visual Studio C++ tools were not found. Install Desktop development with C++.' }
$vcSetup = Join-Path $vsRoot 'VC\Auxiliary\Build\vcvars64.bat'
$buildDir = Join-Path $projectRoot 'build'
New-Item -ItemType Directory -Path $buildDir -Force | Out-Null
$binDir = Join-Path $projectRoot 'bin'
New-Item -ItemType Directory -Path $binDir -Force | Out-Null
$executablePath = Join-Path $binDir 'kupyna-lab.exe'
$optimization = if ($Debug) { '/Od /Zi /MTd' } else { '/O2 /MT' }
# Compiler command is written to a task-local .cmd; paths are quoted for spaces.
$batchPath = Join-Path $buildDir 'compile.cmd'
$batch = @"
@echo off
call "$vcSetup" >nul
if errorlevel 1 exit /b 1
cd /d "$buildDir"
cl /nologo /std:c++17 /EHsc /utf-8 /W4 $optimization /I"$projectRoot\src" /I"$projectRoot\tests" "$projectRoot\src\main.cpp" "$projectRoot\src\kupyna.cpp" "$projectRoot\tests\selftest.cpp" /Fe:"$executablePath"
exit /b %errorlevel%
"@
Set-Content -LiteralPath $batchPath -Value $batch -Encoding Default
& $env:ComSpec /d /c $batchPath
if ($LASTEXITCODE -ne 0) { throw "C++ build failed with exit code $LASTEXITCODE" }
& $executablePath selftest
if ($LASTEXITCODE -ne 0) { throw 'Kupyna self-tests failed.' }
Write-Output "Built: $executablePath"
