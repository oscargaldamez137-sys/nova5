# Host-only loop guard and SCC vote regressions. Reuses build dependencies;
# writes only under unified-compat and never replaces/runs the emulator.
$ErrorActionPreference = 'Stop'
$repo = Split-Path $PSScriptRoot
$build = Join-Path $repo '_Build/windows'
$output = Join-Path $build 'unified-compat'
New-Item -ItemType Directory -Force $output | Out-Null
$llvm = if ($env:KYTY_LLVM_BIN) { $env:KYTY_LLVM_BIN } else { Join-Path $PSScriptRoot '..\..\herramientas\llvm\bin' }
$msvc = 'C:/Program Files/Microsoft Visual Studio/18/Community/VC/Tools/MSVC/14.51.36231'
$sdk = 'C:/Program Files (x86)/Windows Kits/10'
$env:INCLUDE = "$msvc/include;$sdk/Include/10.0.26100.0/ucrt;$sdk/Include/10.0.26100.0/shared;$sdk/Include/10.0.26100.0/um;$sdk/Include/10.0.26100.0/winrt"
$env:LIB = "$msvc/lib/x64;$sdk/Lib/10.0.26100.0/ucrt/x64;$sdk/Lib/10.0.26100.0/um/x64"
$manifest = [IO.File]::ReadAllText("$build/build.ninja").Replace('\','/')
function Block($target) {
    $m = [regex]::Match($manifest, '(?m)^build '+[regex]::Escape($target)+':[^\r\n]*\r?\n(?:  [^\r\n]*\r?\n)*')
    if (!$m.Success) { throw "Missing target $target" }; $m.Value
}
function Variable($b,$n) { [regex]::Match($b,'(?m)^  '+$n+' = ([^\r\n]*)').Groups[1].Value }
function Compile($source,$target) {
    $b = Block $target
    $obj = "$output/" + ($source -replace '[/\\]','_') + '.obj'
    $flags = (Variable $b 'FLAGS') -replace '-flto=thin','' -replace '-fprofile-instr-use=\S+',''
    [IO.File]::WriteAllText("$obj.rsp", "$(Variable $b 'DEFINES') $(Variable $b 'INCLUDES') $flags /c `"$repo/$source`" /Fo`"$obj`"")
    $ErrorActionPreference = 'Continue'
    & "$llvm/clang-cl.exe" "@$obj.rsp" 2>&1 | ForEach-Object { Write-Host "$_" }
    $ErrorActionPreference = 'Stop'
    if ($LASTEXITCODE) { throw "Compile failed: $source" }
    Write-Host "Compiled $source"
    return $obj
}
Push-Location $build
try {
 $changed = @{}
 foreach($source in @("src/graphics/shader/recompiler/frontend/translate/Translate.cpp","src/graphics/shader/recompiler/backend/spirv/spirvEmitterProgram.cpp","src/graphics/host_gpu/renderer/pipeline/pipelineCache.cpp")) { $changed[$source] = Compile $source "CMakeFiles/kyty_emulator.dir/$source.obj" }
 $obj = Compile 'tests/ShaderLoopGuardTests.cpp' 'CMakeFiles/shader_cfg_tests.dir/tests/shaderCfgTests.cpp.obj'
 $b = Block 'shader_cfg_tests.exe'
 $deps = (($b -split "`n")[0] -split ' \| ')[0]
 $objects = foreach ($m in [regex]::Matches($deps,'CMakeFiles/[^ ]+?\.dir/([^ ]+)\.obj')) {
   if ($m.Groups[1].Value.StartsWith('tests/')) { $obj } elseif($changed.ContainsKey($m.Groups[1].Value)) { $changed[$m.Groups[1].Value] } else { 'CMakeFiles/kyty_emulator.dir/' + $m.Groups[1].Value + '.obj' }
 }
 $exe = "$output/shader-loop-guard-tests.exe"
 $rsp = ($objects | ForEach-Object { '"'+$_+'"' }) -join ' '
 $rsp += " $(Variable $b 'LINK_LIBRARIES') $(Variable $b 'LINK_FLAGS') /out:`"$exe`""
 [IO.File]::WriteAllText("$exe.rsp",$rsp)
 & "$llvm/lld-link.exe" "@$exe.rsp"
 if ($LASTEXITCODE) { throw 'link failed' }
} finally { Pop-Location }
$savedEnvironment=@{}
foreach($name in @('KYTY_SHADER_LOOP_LIMIT','KYTY_SHADER_LOOP_HASH','KYTY_SHADER_LOOP_PC','KYTY_SHADER_SCC_VOTE','PATH')) { $savedEnvironment[$name]=[Environment]::GetEnvironmentVariable($name,'Process') }
try {
$env:PATH="$build;$env:PATH"
foreach($name in @('KYTY_SHADER_LOOP_LIMIT','KYTY_SHADER_LOOP_HASH','KYTY_SHADER_LOOP_PC','KYTY_SHADER_SCC_VOTE')) { [Environment]::SetEnvironmentVariable($name,$null,'Process') }
$cases=@(
 @{name='off'},
 @{name='zero'; limit='0'},
 @{name='bounded'; limit='2'},
 @{name='sufficient'; limit='256'},
 @{name='other-hash'; limit='2'; hash='5678'},
 @{name='selected-pc'; limit='2'; pc='4'},
 @{name='other-pc'; limit='2'; pc='8'},
 @{name='vote'; vote='1'},
 @{name='vote-other-hash'; vote='1'; hash='5678'},
 @{name='dispatcher'; limit='2'; dispatcher=$true}
)
foreach($case in $cases) {
 [Environment]::SetEnvironmentVariable('KYTY_SHADER_LOOP_LIMIT',$case.limit,'Process')
 [Environment]::SetEnvironmentVariable('KYTY_SHADER_LOOP_HASH',$case.hash,'Process')
 [Environment]::SetEnvironmentVariable('KYTY_SHADER_LOOP_PC',$case.pc,'Process')
 [Environment]::SetEnvironmentVariable('KYTY_SHADER_SCC_VOTE',$case.vote,'Process')
 $file="$output/guard-$($case.name).spv"
 $ErrorActionPreference='Continue'
 if($case.dispatcher) { & $exe $file dispatcher } else { & $exe $file }
 $code=$LASTEXITCODE
 $ErrorActionPreference='Stop'
 if($code) { throw "Guard test failed: $($case.name) ($code)" }
 Write-Host "PASS $($case.name)"
}
$base=(Get-FileHash "$output/guard-off.spv").Hash
foreach($name in @('zero','other-hash','other-pc','vote-other-hash')) {
 if((Get-FileHash "$output/guard-$name.spv").Hash -ne $base){throw "Disabled path changed SPIR-V: $name"}
}
Write-Host 'PASS disabled and nonselected paths are byte-identical'
} finally { foreach($name in $savedEnvironment.Keys) { [Environment]::SetEnvironmentVariable($name,$savedEnvironment[$name],'Process') } }
