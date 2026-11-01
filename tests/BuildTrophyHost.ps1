# Compile affected translation units using the existing CMake flags, without
# running a game or replacing the emulator. Useful if portable CMake is denied.
param([switch]$HostTestsOnly)
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo '_Build/windows'
$output = Join-Path $build 'trophy-host'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$llvm = if ($env:KYTY_LLVM_BIN) { $env:KYTY_LLVM_BIN } else { Join-Path $PSScriptRoot '..\..\herramientas\llvm\bin' }
$msvc = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231'
$sdk = 'C:/Program Files (x86)/Windows Kits/10'
$env:INCLUDE = "$msvc/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um;$sdk/Include/10.0.26100.0/winrt"
$env:LIB = "$msvc/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
$manifest = [IO.File]::ReadAllText((Join-Path $build 'build.ninja')).Replace('\','/')
function Block($target) {
    $m = [regex]::Match($manifest, '(?m)^build ' + [regex]::Escape($target) + ':[^\r\n]*\r?\n(?:  [^\r\n]*\r?\n)*')
    if (!$m.Success) { throw "Missing target $target" }
    return $m.Value
}
function Variable($block, $name) {
    [regex]::Match($block, '(?m)^  '+$name+' = ([^\r\n]*)').Groups[1].Value
}
function Compile($source, $target, $name, [switch]$IsTest) {
    $block = Block $target
    $flags = Variable $block 'FLAGS'
    if ($IsTest) { $flags = $flags -replace '-flto=thin','' -replace '-fprofile-instr-use=\S+','' }
    $rsp = Join-Path $output "$name.rsp"
    $obj = Join-Path $output "$name.obj"
    [IO.File]::WriteAllText($rsp, "$(Variable $block 'DEFINES') $(Variable $block 'INCLUDES') $flags /I`"$output`" /c `"$repo/$source`" /Fo`"$obj`"")
    $ErrorActionPreference = 'Continue'
    & "$llvm/clang-cl.exe" "@$rsp" 2>&1 | ForEach-Object { "$_" }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw "Compile failed: $source" }
    Write-Output "Compiled $source"
}
# Generate the same embedded byte catalogs as src/embed_localization.cmake when CMake is unavailable.
$header = "#pragma once`n#include <span>`n#include <string_view>`nnamespace {`n"
$lookup = "std::span<const unsigned char> EmbeddedCatalog(std::string_view locale) {`n"
foreach ($catalog in Get-ChildItem "$repo/assets/localization/*.json") {
    $bytes = [IO.File]::ReadAllBytes($catalog.FullName)
    $symbol = $catalog.BaseName.Replace('-','_')
    $header += "constexpr unsigned char catalog_$symbol[] = {" + (($bytes | ForEach-Object {"0x{0:x2}" -f $_}) -join ',') + "};`n"
    $lookup += "if (locale == `"$($catalog.BaseName)`") return catalog_$symbol;`n"
}
[IO.File]::WriteAllText("$output/localizationData.h", $header+$lookup+"return {};`n}`n}`n")
Push-Location $build
try {
    if (!$HostTestsOnly) {
        Compile 'src/common/emulatorConfig.cpp' 'common/CMakeFiles/common.dir/emulatorConfig.cpp.obj' 'emulatorConfig'
        Compile 'src/libs/libNet.cpp' 'CMakeFiles/kyty_emulator.dir/src/libs/libNet.cpp.obj' 'libNet'
        Compile 'src/libs/libTrophy.cpp' 'CMakeFiles/kyty_emulator.dir/src/libs/libNet.cpp.obj' 'libTrophy'
        Compile 'src/graphics/presentation/systemOverlay.cpp' 'CMakeFiles/kyty_emulator.dir/src/graphics/presentation/systemOverlay.cpp.obj' 'systemOverlay'
    }
    Compile 'src/common/trophies.cpp' 'common/CMakeFiles/common.dir/file.cpp.obj' 'trophies' -IsTest
    Compile 'src/common/localization.cpp' 'common/CMakeFiles/common.dir/file.cpp.obj' 'localization' -IsTest
    Compile 'tests/TrophySystemTests.cpp' 'common/CMakeFiles/common.dir/file.cpp.obj' 'tests' -IsTest
    $libs = Variable (Block 'archive_file_tests.exe') 'LINK_LIBRARIES'
    [IO.File]::WriteAllText("$output/link.rsp", "`"$output/trophies.obj`" `"$output/localization.obj`" `"$output/tests.obj`" $libs /nologo /machine:x64 /out:`"$output/trophy_system_tests.exe`"")
    & "$llvm/lld-link.exe" "@$output/link.rsp"
    if ($LASTEXITCODE) { throw 'Trophy test link failed' }
    $ErrorActionPreference='Continue'
    & "$output/trophy_system_tests.exe"
    $code=$LASTEXITCODE
    $ErrorActionPreference='Stop'
    if ($code) { throw "Trophy tests failed ($code)" }
    $zarTarget = '3rdparty/CMakeFiles/zarchive_tool.dir/__/tools/zarchive/main.cpp.obj'
    if (!$manifest.Contains("build ${zarTarget}:")) {
        $zarTarget = '3rdparty/CMakeFiles/zarchive_tool.dir/__/_Build/deps/zarchive_source-src/src/main.cpp.obj'
    }
    Compile 'tools/zarchive/main.cpp' $zarTarget 'zarchive' -IsTest
    & "$llvm/lld-link.exe" "$output/zarchive.obj" /nologo /machine:x64 "/out:$output/zarchive.exe" '3rdparty/zarchive.lib' '_deps/zstd-build/lib/zstd_static.lib' kernel32.lib
    if ($LASTEXITCODE) { throw 'ZArchive CLI link failed' }
    & "$repo/tests/ZArchiveCliTests.ps1" -Exe "$output/zarchive.exe"
} finally { Pop-Location }
