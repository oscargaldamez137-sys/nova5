param([Parameter(Mandatory=$true)][string]$Exe)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path $Exe) ('zar-test-' + [Guid]::NewGuid().ToString('N'))
$inputDir = Join-Path $root ('input-' + [char]0x00f1 + [char]0x7a7a)
$archive = Join-Path $root 'game.zar'
$unpacked = Join-Path $root 'unpacked'
New-Item -ItemType Directory -Force "$inputDir/sce_sys", "$inputDir/empty", "$inputDir/nested" | Out-Null
[IO.File]::WriteAllText("$inputDir/sce_sys/param.json", '{"titleId":"PPSA00001"}')
$bytes = New-Object byte[] (1024 * 1024 + 79)
$rng = New-Object Random 12345
$rng.NextBytes($bytes)
$unicodeFile = "nested/" + [char]0x7a7a + '.bin'
[IO.File]::WriteAllBytes((Join-Path $inputDir $unicodeFile), $bytes)
[IO.File]::WriteAllBytes("$inputDir/empty.bin", [byte[]]@())
& $Exe $inputDir $archive
if ($LASTEXITCODE) { throw 'ZArchive packing failed' }
& $Exe $archive $unpacked
if ($LASTEXITCODE) { throw 'ZArchive extraction failed' }
foreach ($file in @('sce_sys/param.json', 'empty.bin', $unicodeFile)) {
    if ((Get-FileHash (Join-Path $inputDir $file)).Hash -ne (Get-FileHash (Join-Path $unpacked $file)).Hash) {
        throw "ZArchive roundtrip mismatch: $file"
    }
}
if (!(Test-Path "$unpacked/empty" -PathType Container)) { throw 'Empty directory lost' }
$originalHash = (Get-FileHash $archive).Hash
& $Exe $inputDir $archive
if (!$LASTEXITCODE -or (Get-FileHash $archive).Hash -ne $originalHash) { throw 'Existing archive overwritten' }
& $Exe $inputDir "$inputDir/self.zar"
if (!$LASTEXITCODE -or (Test-Path "$inputDir/self.zar")) { throw 'Self-packing must fail before creating output' }
'ZArchiveCliTests: Unicode paths, binary roundtrip, empty entries, overwrite and self-packing checks passed'
