<#
Rebuilds libdatachannel + mbedtls from source and overlays the result onto
ppobs's local obs-deps folder, matching the actual recipe obsproject/obs-deps
uses (deps.ffmpeg/60-mbedtls.ps1 + 70-libdatachannel.ps1 in that repo) but at
a newer libdatachannel version than the currently published obs-deps release
ships.

Why this exists: the obs-deps release ppobs's buildspec pins (2025-08-23)
carries libdatachannel 0.21.0, whose PacingHandler::schedule() has an
inverted enable check that can crash the RTC worker thread (fixed upstream
in libdatachannel 0.23.3+). obs-deps master hasn't cut a new dated release
with the fix yet. Until it does, this script is how ppobs gets a genuinely
rebuilt fixed binary: build the same two dependencies obs-deps would, at
pinned commits, and drop the result into the existing local .deps folder.

This is NOT part of the normal CMake configure/build flow, and .deps/ is
gitignored -- re-run this manually any time .deps is fetched fresh (a clean
clone, deleting .deps, a new CI runner, another machine). Otherwise ppobs
silently links the old 0.21.0 DLL again with no error at build or link time.

Usage (from anywhere; paths are resolved relative to the repo root):
  .\build-aux\upgrade-libdatachannel.ps1
  .\build-aux\upgrade-libdatachannel.ps1 -DepsDir '.deps\obs-deps-2025-08-23-x64' -Clean

After it finishes, rebuild obs-webrtc (cmake auto-reconfigures on the next
build since LibDataChannelConfig.cmake's timestamp changes), e.g.:
  MSBuild build_x64\plugins\obs-webrtc\obs-webrtc.vcxproj /t:Build /p:Configuration=RelWithDebInfo /p:Platform=x64 /m
#>
param(
    [string] $DepsDir,
    [string] $WorkDir = "$env:TEMP\ppobs-libdatachannel-build",
    [string] $Configuration = 'Release',
    [string] $LibDataChannelRef = '4e4f4892dccb2a57fe3a490d0c9d958de4244e74', # v0.24.2, per obs-deps deps.ffmpeg/70-libdatachannel.ps1
    [string] $MbedtlsRef = 'c765c831e5c2a0971410692f92f7a81d6ec65ec2',        # v3.6.4, per obs-deps deps.ffmpeg/60-mbedtls.ps1
    [switch] $Clean
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path $PSScriptRoot -Parent

function Invoke-CMakeChecked {
    param([string[]] $CMakeArgs)
    & $script:Cmake @CMakeArgs
    if ($LASTEXITCODE -ne 0) { throw "cmake failed (exit $LASTEXITCODE): $CMakeArgs" }
}

function Get-PinnedRepo {
    param([string] $Url, [string] $Ref, [string] $Dir)
    if ($Clean -and (Test-Path $Dir)) { Remove-Item -Recurse -Force $Dir }
    if (-not (Test-Path $Dir)) {
        git clone --no-checkout $Url $Dir
        if ($LASTEXITCODE -ne 0) { throw "git clone failed for $Url" }
    } else {
        git -C $Dir fetch origin
    }
    git -C $Dir checkout $Ref
    if ($LASTEXITCODE -ne 0) { throw "git checkout $Ref failed in $Dir" }
    git -C $Dir -c core.longpaths=true submodule update --init --recursive
    if ($LASTEXITCODE -ne 0) { throw "git submodule update failed in $Dir" }
}

# ---- Resolve target deps folder --------------------------------------------
if (-not $DepsDir) {
    $cacheFile = Join-Path $RepoRoot 'build_x64\CMakeCache.txt'
    if (Test-Path $cacheFile) {
        $prefixLine = Select-String -Path $cacheFile -Pattern '^CMAKE_PREFIX_PATH:PATH=' | Select-Object -First 1
        if ($prefixLine) {
            $candidates = ($prefixLine.Line -replace '^CMAKE_PREFIX_PATH:PATH=', '') -split ';'
            $DepsDir = $candidates | Where-Object { $_ -match 'obs-deps-[\d-]+-x64$' } | Select-Object -First 1
        }
    }
    if (-not $DepsDir) {
        $DepsDir = Get-ChildItem (Join-Path $RepoRoot '.deps') -Directory -Filter 'obs-deps-*-x64' -ErrorAction SilentlyContinue |
            Where-Object { $_.Name -notmatch 'qt6' } | Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
    }
}
if (-not $DepsDir -or -not (Test-Path $DepsDir)) {
    throw "Could not resolve the obs-deps folder to patch. Pass -DepsDir explicitly (e.g. .deps\obs-deps-2025-08-23-x64)."
}
Write-Host "[upgrade-libdatachannel] Target deps folder: $DepsDir"

# ---- Locate the VS-bundled CMake (recognizes this machine's VS generator) --
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vsInstallPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
if (-not $vsInstallPath) { throw "vswhere could not find a Visual Studio install with the MSBuild component." }
$Cmake = Join-Path $vsInstallPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
if (-not (Test-Path $Cmake)) { throw "Could not find the VS-bundled cmake.exe under $vsInstallPath" }
Write-Host "[upgrade-libdatachannel] Using cmake: $Cmake (generator left to its default so it matches this VS install)"

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# ---- mbedtls: static, patched for DTLS-SRTP --------------------------------
$mbedtlsSrc = Join-Path $WorkDir 'mbedtls'
$mbedtlsInstall = Join-Path $WorkDir 'install\mbedtls'
Write-Host "[upgrade-libdatachannel] Fetching mbedtls @ $MbedtlsRef"
Get-PinnedRepo -Url 'https://github.com/Mbed-TLS/mbedtls.git' -Ref $MbedtlsRef -Dir $mbedtlsSrc

$configHeader = Join-Path $mbedtlsSrc 'include\mbedtls\mbedtls_config.h'
(Get-Content $configHeader) -replace '^//#define MBEDTLS_SSL_DTLS_SRTP$', '#define MBEDTLS_SSL_DTLS_SRTP' |
    Set-Content $configHeader

Push-Location $mbedtlsSrc
try {
    if ($Clean -and (Test-Path 'build_x64')) { Remove-Item -Recurse -Force 'build_x64' }
    # CMAKE_POLICY_VERSION_MINIMUM must be its own arg -- PowerShell mis-splits
    # "3.5" into "3" and ".5" if it's inlined into a longer -D string on some
    # continuation styles.
    $policyArg = '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
    Invoke-CMakeChecked @(
        '-S', '.', '-B', 'build_x64', '-A', 'x64'
        '-DUSE_SHARED_MBEDTLS_LIBRARY=OFF'
        '-DUSE_STATIC_MBEDTLS_LIBRARY=ON'
        '-DENABLE_PROGRAMS=OFF'
        '-DENABLE_TESTING=OFF'
        '-DGEN_FILES=OFF'
        # obs-deps' CI apparently doesn't hit new warnings on this old code;
        # a newer/different MSVC here can, so don't let that block a vendored
        # dependency build over warnings (not correctness).
        '-DMBEDTLS_FATAL_WARNINGS=OFF'
        $policyArg
        "-DCMAKE_INSTALL_PREFIX=$mbedtlsInstall"
    )
    Invoke-CMakeChecked @('--build', 'build_x64', '--config', $Configuration, '--parallel')
    Invoke-CMakeChecked @('--install', 'build_x64', '--config', $Configuration)
} finally { Pop-Location }

# ---- libdatachannel: shared, against the mbedtls above ---------------------
$dcSrc = Join-Path $WorkDir 'libdatachannel'
$dcInstall = Join-Path $WorkDir 'install\libdatachannel'
Write-Host "[upgrade-libdatachannel] Fetching libdatachannel @ $LibDataChannelRef"
Get-PinnedRepo -Url 'https://github.com/paullouisageneau/libdatachannel.git' -Ref $LibDataChannelRef -Dir $dcSrc

Push-Location $dcSrc
try {
    if ($Clean -and (Test-Path 'build_x64')) { Remove-Item -Recurse -Force 'build_x64' }
    $policyArg = '-DCMAKE_POLICY_VERSION_MINIMUM=3.5'
    Invoke-CMakeChecked @(
        '-S', '.', '-B', 'build_x64', '-A', 'x64'
        '-DBUILD_SHARED_LIBS=ON'
        '-DUSE_MBEDTLS=ON'
        '-DNO_WEBSOCKET=ON'
        '-DNO_TESTS=ON'
        '-DNO_EXAMPLES=ON'
        $policyArg
        "-DCMAKE_PREFIX_PATH=$mbedtlsInstall"
        "-DCMAKE_INSTALL_PREFIX=$dcInstall"
    )
    Invoke-CMakeChecked @('--build', 'build_x64', '--config', $Configuration, '--target', 'install', '--parallel')
} finally { Pop-Location }

# ---- Overlay the result onto the deps folder, backing up the old one ------
$backupDir = Join-Path (Split-Path $DepsDir -Parent) "_backup-libdatachannel-$(Get-Date -Format yyyyMMdd-HHmmss)"
Write-Host "[upgrade-libdatachannel] Backing up current libdatachannel to $backupDir"
New-Item -ItemType Directory -Force -Path "$backupDir\bin", "$backupDir\lib\cmake", "$backupDir\include" | Out-Null
Copy-Item "$DepsDir\bin\datachannel.dll" "$backupDir\bin\" -ErrorAction SilentlyContinue
Copy-Item "$DepsDir\lib\datachannel.lib" "$backupDir\lib\" -ErrorAction SilentlyContinue
Copy-Item "$DepsDir\lib\cmake\LibDataChannel" "$backupDir\lib\cmake\" -Recurse -ErrorAction SilentlyContinue
Copy-Item "$DepsDir\include\rtc" "$backupDir\include\" -Recurse -ErrorAction SilentlyContinue

$oldHash = if (Test-Path "$DepsDir\bin\datachannel.dll") { (Get-FileHash "$DepsDir\bin\datachannel.dll" -Algorithm MD5).Hash } else { '(none)' }

Copy-Item "$dcInstall\bin\datachannel.dll" "$DepsDir\bin\datachannel.dll" -Force
Copy-Item "$dcInstall\lib\datachannel.lib" "$DepsDir\lib\datachannel.lib" -Force
Remove-Item "$DepsDir\lib\cmake\LibDataChannel" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item "$dcInstall\lib\cmake\LibDataChannel" "$DepsDir\lib\cmake\" -Recurse -Force
Remove-Item "$DepsDir\include\rtc" -Recurse -Force -ErrorAction SilentlyContinue
Copy-Item "$dcInstall\include\rtc" "$DepsDir\include\" -Recurse -Force

$newHash = (Get-FileHash "$DepsDir\bin\datachannel.dll" -Algorithm MD5).Hash
Write-Host "[upgrade-libdatachannel] datachannel.dll MD5: $oldHash -> $newHash"
Write-Host "[upgrade-libdatachannel] Done. Old files backed up under $backupDir."
Write-Host "[upgrade-libdatachannel] Next: reconfigure + rebuild obs-webrtc, e.g.:"
Write-Host "  MSBuild build_x64\plugins\obs-webrtc\obs-webrtc.vcxproj /t:Build /p:Configuration=RelWithDebInfo /p:Platform=x64 /m"
