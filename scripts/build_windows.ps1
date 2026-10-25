# Any_Ps3 — build the Windows desktop host and package Any_Ps3.exe into a zip.
# Run on a Windows machine with Visual Studio 2022 (C++ workload) + CMake 3.16+.
# Dependencies (SDL2, Dear ImGui) are fetched automatically via CMake FetchContent.
param(
    [string]$Configuration = "Release",
    [string]$OutDir = "package"
)
$ErrorActionPreference = "Stop"

$root = $PSScriptRoot | Split-Path | Split-Path
Push-Location $root

cmake -S . -B build\windows "-DCMAKE_BUILD_TYPE=$Configuration" "-DANYPS3_BUILD_HOST_DESKTOP=ON" "-DANYPS3_BUILD_TESTS=OFF"
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
cmake --build build\windows --config $Configuration -- -m
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

$exe = "build\windows\hosts\desktop\$Configuration\Any_Ps3.exe"
if (-not (Test-Path $exe)) { $exe = "build\windows\hosts\desktop\Any_Ps3.exe" }
if (-not (Test-Path $exe)) { throw "built exe not found" }

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Copy-Item $exe "$OutDir\Any_Ps3.exe" -Force
$readme = @"
Any_Ps3 — experimental AOT-recompilation-based PS3 launcher

Contents:
  Any_Ps3.exe     desktop host (Dear ImGui UI)
  README.txt      usage + legal notes (also see the repository README.md)

Requirements:
  - Windows 10/11 x64, a Vulkan-capable GPU driver (graphics backend is
    under development; the UI and import/library pipeline work today)

First run:
  - The app asks for a one-time legal confirmation.
  - Add games via 'Add game...': a legally-dumped PS3 game folder (JB dump)
    or an ISO image. Encrypted retail executables cannot be loaded by design
    (Any_Ps3 ships no keys); homebrew/debug executables are supported.

This build does not include any Sony firmware, decryption keys, or game data.
"@
Set-Content "$OutDir\README.txt" $readme -Encoding UTF8

Compress-Archive -Path "$OutDir\Any_Ps3.exe", "$OutDir\README.txt" -DestinationPath "$OutDir\Any_Ps3_Windows.zip" -Force
Write-Host "Packaged: $OutDir\Any_Ps3_Windows.zip"
Pop-Location
