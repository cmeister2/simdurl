[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [ValidateSet('Install', 'Build', 'Test')]
    [string] $Phase
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Invoke-Native {
    param([string] $FilePath, [string[]] $ArgumentList)
    $executable = Get-Command -Name $FilePath -CommandType Application -ErrorAction Stop
    $previousErrorActionPreference = $ErrorActionPreference
    try {
        # Windows PowerShell treats native stderr (including warnings) as errors.
        # Let the process finish and use its exit code to decide whether it failed.
        $ErrorActionPreference = 'Continue'
        & $executable.Source @ArgumentList
        $exitCode = $LASTEXITCODE
    }
    finally {
        $ErrorActionPreference = $previousErrorActionPreference
    }
    if ($exitCode -ne 0) {
        throw "$FilePath exited with code $exitCode"
    }
}

function Test-ToolsPresent {
    param([string] $Directory, [string[]] $Names)
    foreach ($name in $Names) {
        if (-not (Test-Path -LiteralPath (Join-Path $Directory $name) -PathType Leaf)) {
            return $false
        }
    }
    return $true
}

$cachePath = 'C:\simdurl-cache'
if ($Phase -eq 'Install') {
    New-Item -ItemType Directory -Force -Path $cachePath | Out-Null
}

$env:CHERE_INVOKING = 'yes'
switch ($env:SIMDURL_TOOLCHAIN) {
    'UCRT64' {
        $packagePrefix = 'mingw-w64-ucrt-x86_64'
        $compilerPackage = 'gcc'
    }
    'CLANG64' {
        $packagePrefix = 'mingw-w64-clang-x86_64'
        $compilerPackage = 'clang'
    }
    'MINGW32' {
        $packagePrefix = 'mingw-w64-i686'
        $compilerPackage = 'gcc'
    }
    'CYGWIN64' { }
    default { throw "Unknown toolchain: $env:SIMDURL_TOOLCHAIN" }
}

if ($env:SIMDURL_TOOLCHAIN -eq 'CYGWIN64') {
    $shellExecutable = 'C:\cygwin64\bin\bash.exe'
    Remove-Item Env:MSYSTEM -ErrorAction SilentlyContinue

    if ($Phase -eq 'Install' -and (Test-ToolsPresent 'C:\cygwin64\bin' @(
        'bash.exe', 'gcc.exe', 'g++.exe', 'cmake.exe', 'ctest.exe', 'make.exe'
    ))) {
        Write-Host 'Using the preinstalled Cygwin toolchain.'
    }
    elseif ($Phase -eq 'Install') {
        $setupPath = Join-Path $env:TEMP 'simdurl-cygwin-setup.exe'
        Invoke-WebRequest -UseBasicParsing -Uri 'https://cygwin.com/setup-x86_64.exe' -OutFile $setupPath
        $setupArguments = @(
            '--quiet-mode', '--no-shortcuts', '--only-site', '--upgrade-also',
            '--site', 'https://mirrors.kernel.org/sourceware/cygwin/',
            '--root', 'C:\cygwin64',
            '--local-package-dir', ('"' + $cachePath + '"'),
            '--packages', 'gcc-core,gcc-g++,cmake,make'
        )
        $setupProcess = Start-Process -FilePath $setupPath -ArgumentList $setupArguments -Wait -PassThru
        if ($setupProcess.ExitCode -ne 0) {
            throw "Cygwin setup exited with code $($setupProcess.ExitCode)"
        }
    }
}
else {
    $shellExecutable = 'C:\msys64\usr\bin\bash.exe'
    $env:MSYSTEM = $env:SIMDURL_TOOLCHAIN

    # Native CMake/Ninja already ship with the image and work with each MinGW ABI.
    $env:SIMDURL_CMAKE = (Get-Command cmake.exe -CommandType Application -ErrorAction Stop).Source
    $env:SIMDURL_CTEST = Join-Path (Split-Path $env:SIMDURL_CMAKE) 'ctest.exe'
    $env:SIMDURL_NINJA = (Get-Command ninja.exe -CommandType Application -ErrorAction Stop).Source
    $toolchainBin = 'C:\msys64\' + $env:SIMDURL_TOOLCHAIN.ToLowerInvariant() + '\bin'
    $cxx = if ($compilerPackage -eq 'clang') { 'clang++.exe' } else { 'g++.exe' }

    if ($Phase -eq 'Install' -and (Test-ToolsPresent $toolchainBin @("$compilerPackage.exe", $cxx))) {
        Write-Host "Using the preinstalled $env:SIMDURL_TOOLCHAIN compiler."
    }
    elseif ($Phase -eq 'Install') {
        # Package installation requires a full update; partial upgrades are unsupported.
        # Core updates can close the shell; finish the full upgrade in a new one.
        $pacman = 'pacman --cachedir /c/simdurl-cache --noconfirm'
        Invoke-Native $shellExecutable @('-lc', "$pacman -Syuu")
        Invoke-Native $shellExecutable @('-lc', "$pacman -Syuu")
        Invoke-Native $shellExecutable @('-lc', "$pacman -S --needed $packagePrefix-$compilerPackage")
    }
}

if ($Phase -eq 'Install') {
    . (Join-Path $PSScriptRoot 'appveyor-cache.ps1')
    Trim-DownloadCache -Path $cachePath
}
else {
    Set-Location -LiteralPath $env:APPVEYOR_BUILD_FOLDER
    Invoke-Native $shellExecutable @('--login', 'scripts/appveyor.sh', $Phase.ToLowerInvariant())
}
