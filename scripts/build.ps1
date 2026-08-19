param(
    [switch]$Native,
    [switch]$Apk,
    [switch]$Clean
)

$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot

function Invoke-NativeBuild {
    Write-Host "[ARIFT] Building native core (arm64-v8a)..." -ForegroundColor Cyan
    $buildDir = Join-Path $root "build\native"
    if ($Clean -and (Test-Path $buildDir)) {
        Remove-Item -Recurse -Force $buildDir
    }
    New-Item -ItemType Directory -Force -Path $buildDir | Out-Null

    $ndkRoot = $env:ANDROID_NDK_HOME
    if (-not $ndkRoot) {
        $ndkRoot = Join-Path $env:LOCALAPPDATA "Android\Sdk\ndk\25.2.9519653"
    }
    if (-not (Test-Path $ndkRoot)) {
        throw "Android NDK not found. Set ANDROID_NDK_HOME."
    }

    & cmake -S $root -B $buildDir `
        -DCMAKE_TOOLCHAIN_FILE="$ndkRoot\build\cmake\android.toolchain.cmake" `
        -DANDROID_ABI=arm64-v8a `
        -DANDROID_PLATFORM=android-26 `
        -DCMAKE_BUILD_TYPE=Release
    if ($LASTEXITCODE -ne 0) { throw "CMake configure failed" }

    & cmake --build $buildDir --config Release --parallel
    if ($LASTEXITCODE -ne 0) { throw "CMake build failed" }

    $soPath = Join-Path $buildDir "lib\arm64-v8a\libarift_core.so"
    if (Test-Path $soPath) {
        $dst = Join-Path $root "vae\android\app\src\main\jniLibs\arm64-v8a"
        New-Item -ItemType Directory -Force -Path $dst | Out-Null
        Copy-Item $soPath (Join-Path $dst "libarift_core.so") -Force
        Write-Host "[ARIFT] libarift_core.so -> $dst" -ForegroundColor Green
    }
}

function Invoke-ApkBuild {
    Write-Host "[ARIFT] Building host loader APK..." -ForegroundColor Cyan
    $androidDir = Join-Path $root "vae\android"
    & ./gradlew.bat assembleRelease
    if ($LASTEXITCODE -ne 0) { throw "Gradle build failed" }
    Write-Host "[ARIFT] APK built at vae\android\app\build\outputs\apk\release\" -ForegroundColor Green
}

if ($Native) { Invoke-NativeBuild }
if ($Apk) {
    Push-Location (Join-Path $root "vae\android")
    try { Invoke-ApkBuild } finally { Pop-Location }
}

if (-not $Native -and -not $Apk) {
    Invoke-NativeBuild
    Push-Location (Join-Path $root "vae\android")
    try { Invoke-ApkBuild } finally { Pop-Location }
}