param([string]$Scratch = "$PSScriptRoot/../_Build/unified-analysis/linked-list-tests")
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $Scratch | Out-Null
$scratchRoot = (Resolve-Path -LiteralPath $Scratch).Path
$analyzer = "$PSScriptRoot/../tools/AnalyzeLinkedList.ps1"
function Words([uint32[]]$values) {
    $bytes = New-Object byte[] ($values.Length * 4)
    [Buffer]::BlockCopy($values,0,$bytes,0,$bytes.Length)
    return ,$bytes
}
function Case($name, [uint32[]]$heads, [uint32[]]$next, $capacity, $counter, $expected) {
    $folder = Join-Path $scratchRoot $name
    New-Item -ItemType Directory -Force $folder | Out-Null
    $nodes = New-Object uint32[] ($next.Length*2)
    for ($i=0; $i -lt $next.Length; ++$i) { $nodes[$i*2]=0x10000000; $nodes[$i*2+1]=$next[$i] }
    [IO.File]::WriteAllBytes((Join-Path $folder 'heads.bin'), (Words $heads))
    [IO.File]::WriteAllBytes((Join-Path $folder 'nodes.bin'), (Words $nodes))
    [IO.File]::WriteAllBytes((Join-Path $folder 'gds.bin'), (Words @(0,0,$counter,0,0,0,0,0)))
    $meta = @{version=1;node_stride=8;next_offset=4;terminator=[uint32]::MaxValue;draw_not_submitted=$true;
        head_binding=11;node_binding=12;copy_tick=1;timeline='0x1';s33_source=@{binding=9;byte_offset=10672};
        gds=@{file='gds.bin';bytes=32;counter_offset=8};buffers=@(
            @{binding=11;file='heads.bin';captured_bytes=$heads.Length*4;guest_bytes=$heads.Length*4},
            @{binding=12;file='nodes.bin';captured_bytes=$nodes.Length*4;guest_bytes=$capacity*8})}
    $manifest=Join-Path $folder 'capture.json'
    $meta | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $manifest -Encoding UTF8
    & $analyzer -Manifest $manifest | Out-Null
    $r=(Get-Content (Join-Path $folder 'analysis.json') -Raw | ConvertFrom-Json).analysis
    foreach ($key in $expected.Keys) { if ($r.$key -ne $expected[$key]) { throw "$name $key expected $($expected[$key]), got $($r.$key)" } }
    Write-Output "PASS $name"
}
$end=[uint32]::MaxValue
Case 'shared-terminating-free-cycle' @($end,0,1,0) @(1,$end,2) 3 2 @{TerminatingHeads=4;EmptyHeads=1;ReachableNodes=2;TerminatorNodes=1;MaxChainLength=2;AllHeadsTerminate=$true;ReachableAtOrBeyondCounter=$false}
Case 'self-link' @(0,0) @(0) 1 1 @{SelfLinkedHeads=2;ReachableNodes=1;AllHeadsTerminate=$false}
Case 'cycle-through-tail' @(0,1) @(1,2,1) 3 3 @{CyclicHeads=2;ReachableNodes=3}
Case 'head-oob' @(4) @($end) 4 0 @{OutOfRangeHeads=1;ReachableNodes=0}
Case 'next-oob' @(0,0) @(4) 4 1 @{OutOfRangeHeads=2;ReachableNodes=1}
Case 'partial-not-oob' @(0) @(2) 4 3 @{UncapturedHeads=1;OutOfRangeHeads=0;AllHeadsTerminate=$false}
Case 'counter-too-small' @(1) @($end,$end) 2 1 @{TerminatingHeads=1;MaxReachableIndex=1;ReachableAtOrBeyondCounter=$true}
Case 'counter-overflow' @($end) @($end) 1 2 @{CounterBeyondCapacity=$true;AllHeadsTerminate=$true}
# Exercise bounded graph traversal with a long chain; avoids quadratic per-head walks.
$long = New-Object uint32[] 100000
for ($i=0; $i -lt $long.Length-1; ++$i) { $long[$i]=$i+1 }
$long[$long.Length-1]=$end
Case 'long-shared-chain' @(0,1,50000,99999) $long 100000 100000 @{TerminatingHeads=4;ReachableNodes=100000;MaxReachableIndex=99999;MaxChainLength=100000}
# A truncated input must fail, not silently certify the lists.
$bad=Join-Path $scratchRoot 'head-oob/heads.bin'
[IO.File]::WriteAllBytes($bad, [byte[]]@(0,0))
$rejected=$false
try { & $analyzer -Manifest (Join-Path $scratchRoot 'head-oob/capture.json') | Out-Null } catch { $rejected=$true }
if (!$rejected) { throw 'truncated input accepted' }
Write-Output 'PASS truncated-input-rejected'

# Allocated capacity can include stale/inactive heads. Keep the whole-buffer
# report and a separately labelled active-domain report, never hide the tail.
Case 'inactive-tail' @(0,1) @($end,1) 2 1 @{TerminatingHeads=1;SelfLinkedHeads=1;ReachableAtOrBeyondCounter=$true}
$manifest = Join-Path $scratchRoot 'inactive-tail/capture.json'
$output = Join-Path $scratchRoot 'inactive-tail/analysis.json'
& $analyzer -Manifest $manifest -ActiveHeadCount 1 | Out-Null
$r = Get-Content $output -Raw | ConvertFrom-Json
if (!$r.active_domain.analysis.AllHeadsTerminate -or $r.active_domain.analysis.ReachableAtOrBeyondCounter -or
    $r.analysis.SelfLinkedHeads -ne 1) { throw 'active domain hid the invalid tail or used its counter comparison' }
Write-Output 'PASS explicit-active-domain'
$meta = Get-Content $manifest -Raw | ConvertFrom-Json
$meta | Add-Member producer_history @(@{timeline='0x1';groups=@(1,1,1);indirect_address='0x0'})
$constant = New-Object byte[] 10676
$constant[10672] = 1
[IO.File]::WriteAllBytes((Join-Path $scratchRoot 'inactive-tail/buffer-9.bin'),$constant)
$meta.buffers += @{binding=9;file='buffer-9.bin';captured_bytes=$constant.Length;guest_bytes=$constant.Length}
$meta | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest -Encoding UTF8
& $analyzer -Manifest $manifest | Out-Null
$r = Get-Content $output -Raw | ConvertFrom-Json
if ($r.active_domain.head_count -ne 1 -or !$r.active_domain.analysis.AllHeadsTerminate) { throw 'producer dimensions were not recognized' }
Write-Output 'PASS candidate-active-domain'
$meta.producer_history[0].indirect_address = '0x1000'
$meta | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifest -Encoding UTF8
& $analyzer -Manifest $manifest | Out-Null
$r = Get-Content $output -Raw | ConvertFrom-Json
if ($r.active_domain) { throw 'inferred active domain from unresolved indirect dispatch' }
Write-Output 'PASS indirect-domain-rejected'
$rejected=$false
try { & $analyzer -Manifest $manifest -ActiveHeadCount 3 | Out-Null } catch { $rejected=$true }
if (!$rejected) { throw 'active domain beyond head buffer accepted' }
Write-Output 'PASS oversized-active-domain-rejected'
