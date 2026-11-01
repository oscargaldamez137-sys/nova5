# Analyze a completed GPU capture. No emulator, GPU, or game is started.
param(
    [Parameter(Mandatory=$true)][string]$Manifest,
    [string]$Output = '',
    [ValidateRange(1,1024)][int]$MaxExamples = 32,
    [ValidateRange(0,2147483647)][int]$ActiveHeadCount = 0
)
$ErrorActionPreference = 'Stop'
$manifestPath = (Resolve-Path -LiteralPath $Manifest).Path
$folder = Split-Path -Parent $manifestPath
$meta = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if ($meta.version -ne 1 -or $meta.node_stride -ne 8 -or $meta.next_offset -ne 4 -or
    [uint64]$meta.terminator -ne 4294967295 -or !$meta.draw_not_submitted) {
    throw 'Unsupported or incomplete capture manifest'
}
function CaptureFile($name) {
    if (!$name -or [IO.Path]::GetFileName($name) -ne $name) { throw 'Invalid capture filename' }
    Join-Path $folder $name
}
$head = @($meta.buffers | Where-Object binding -eq $meta.head_binding)
$node = @($meta.buffers | Where-Object binding -eq $meta.node_binding)
if ($head.Count -ne 1 -or $node.Count -ne 1) { throw 'Missing/duplicate linked-list descriptors' }
$heads = [IO.File]::ReadAllBytes((CaptureFile $head[0].file))
$nodes = [IO.File]::ReadAllBytes((CaptureFile $node[0].file))
$gds = [IO.File]::ReadAllBytes((CaptureFile $meta.gds.file))
if ($heads.Length -ne $head[0].captured_bytes -or $nodes.Length -ne $node[0].captured_bytes -or
    $gds.Length -ne $meta.gds.bytes -or $head[0].captured_bytes -ne $head[0].guest_bytes -or
    $nodes.Length -gt $node[0].guest_bytes -or $heads.Length % 4 -or $nodes.Length % 8 -or
    $node[0].guest_bytes % 8 -or $meta.gds.counter_offset -lt 0 -or
    $meta.gds.counter_offset + 4 -gt $gds.Length) { throw 'Truncated/inconsistent capture files' }
if (-not ('NovaLinkedListAnalysis' -as [type])) {
    Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
public sealed class NovaListIssue {
    public string Kind;
    public uint Head, Node, Value, CycleLength;
    public uint[] Witness;
}
public sealed class NovaListReport {
    public uint HeadCount, EmptyHeads, TerminatingHeads, OutOfRangeHeads, SelfLinkedHeads, CyclicHeads, UncapturedHeads;
    public uint Capacity, CapturedNodes, ReachableNodes, TerminatorNodes, MaxReachableIndex, GdsCounter;
    public uint MaxChainLength;
    public SortedDictionary<string,uint> TerminatingLengths = new SortedDictionary<string,uint>();
    public bool HasReachableNodes, CounterBeyondCapacity, ReachableAtOrBeyondCounter, AllHeadsTerminate;
    public List<NovaListIssue> Examples = new List<NovaListIssue>();
}
public static class NovaLinkedListAnalysis {
    // Memoized functional graph walk: O(heads + reachable nodes), ignoring free nodes.
    public static NovaListReport Analyze(byte[] heads, byte[] nodes, uint capacity, uint counter, int maxExamples) {
        var r = new NovaListReport { HeadCount=(uint)(heads.Length/4), Capacity=capacity,
            CapturedNodes=(uint)(nodes.Length/8), GdsCounter=counter, CounterBeyondCapacity=counter>capacity };
        uint[] state = new uint[nodes.Length/8]; // 0 unseen, MAX active, otherwise outcome+1.
        uint[] distance = new uint[nodes.Length/8]; // Memoized distance to terminator.
        var outcomes = new List<NovaListIssue>();
        outcomes.Add(new NovaListIssue {Kind="terminator"});
        var path = new List<uint>();
        for (uint h=0; h<r.HeadCount; ++h) {
            uint current=BitConverter.ToUInt32(heads, checked((int)h*4));
            if (current==uint.MaxValue) {
                ++r.EmptyHeads; ++r.TerminatingHeads;
                if (!r.TerminatingLengths.ContainsKey("0")) r.TerminatingLengths["0"]=0;
                ++r.TerminatingLengths["0"]; continue;
            }
            path.Clear();
            uint code=0;
            NovaListIssue issue=null;
            while (true) {
                if (current==uint.MaxValue) { code=1; break; }
                if (current>=capacity || current>=r.CapturedNodes) {
                    issue=new NovaListIssue {Kind=current>=capacity ? "out-of-range" : "uncaptured",
                        Node=path.Count==0 ? uint.MaxValue : path[path.Count-1], Value=current};
                    break;
                }
                if (state[current]==uint.MaxValue) {
                    int start=path.IndexOf(current);
                    int length=path.Count-start;
                    var witness=new List<uint>();
                    for (int j=start; j<path.Count && witness.Count<64; ++j) witness.Add(path[j]);
                    witness.Add(current);
                    issue=new NovaListIssue {Kind=length==1 ? "self-link" : "cycle", Node=path[path.Count-1],
                        Value=current, CycleLength=(uint)length, Witness=witness.ToArray()};
                    break;
                }
                if (state[current]!=0) { code=state[current]; break; }
                state[current]=uint.MaxValue;
                path.Add(current);
                ++r.ReachableNodes;
                r.HasReachableNodes=true;
                r.MaxReachableIndex=Math.Max(r.MaxReachableIndex,current);
                current=BitConverter.ToUInt32(nodes, checked((int)current*8+4));
                if (current==uint.MaxValue) ++r.TerminatorNodes;
            }
            if (issue!=null) {
                if (issue.Witness==null) {
                    int n=Math.Min(path.Count,64);
                    issue.Witness=path.GetRange(Math.Max(0,path.Count-n),n).ToArray();
                }
                outcomes.Add(issue); code=(uint)outcomes.Count;
            }
            foreach (uint item in path) state[item]=code;
            var outcome=outcomes[(int)code-1];
            if (outcome.Kind=="terminator") {
                uint length=current==uint.MaxValue ? 0 : distance[current];
                for (int j=path.Count-1; j>=0; --j) distance[path[j]]=++length;
                if (!r.TerminatingLengths.ContainsKey(length.ToString())) r.TerminatingLengths[length.ToString()]=0;
                ++r.TerminatingLengths[length.ToString()];
                r.MaxChainLength=Math.Max(r.MaxChainLength,length);
            }
            switch (outcome.Kind) {
                case "terminator": ++r.TerminatingHeads; break;
                case "out-of-range": ++r.OutOfRangeHeads; break;
                case "self-link": ++r.SelfLinkedHeads; break;
                case "cycle": ++r.CyclicHeads; break;
                case "uncaptured": ++r.UncapturedHeads; break;
            }
            if (outcome.Kind!="terminator" && r.Examples.Count<maxExamples)
                r.Examples.Add(new NovaListIssue {Head=h, Kind=outcome.Kind, Node=outcome.Node, Value=outcome.Value,
                    CycleLength=outcome.CycleLength, Witness=outcome.Witness});
        }
        r.ReachableAtOrBeyondCounter=r.HasReachableNodes && r.MaxReachableIndex>=counter;
        r.AllHeadsTerminate=r.TerminatingHeads==r.HeadCount;
        return r;
    }
}
'@
}
$counter = [BitConverter]::ToUInt32($gds, [int]$meta.gds.counter_offset)
$report = [NovaLinkedListAnalysis]::Analyze($heads, $nodes, [uint32]($node[0].guest_bytes / 8), $counter, $MaxExamples)
$s33 = $null
$constant = @($meta.buffers | Where-Object binding -eq $meta.s33_source.binding)
if ($constant.Count -eq 1 -and $constant[0].captured_bytes -ge ($meta.s33_source.byte_offset + 4)) {
    $data = [IO.File]::ReadAllBytes((CaptureFile $constant[0].file))
    if ($data.Length -ne $constant[0].captured_bytes) { throw 'Truncated constants file' }
    $s33 = [BitConverter]::ToUInt32($data, [int]$meta.s33_source.byte_offset)
}
$result = [ordered]@{
    manifest=$manifestPath; copy_tick=$meta.copy_tick; timeline=$meta.timeline
    analysis=$report; s33=$s33
    caveat='Reachable lists only; valid lists do not prove valid PS head-index calculation. GDS comparison assumes this counter is the current append high-water mark.'
}
$domainSource = 'explicit -ActiveHeadCount; verify this is the current PS domain'
$latest = @($meta.producer_history | Where-Object { $_.timeline -eq $meta.timeline }) | Select-Object -Last 1
if (!$ActiveHeadCount -and $latest -and $latest.indirect_address -eq '0x0' -and
    $latest.groups.Count -eq 3 -and $latest.groups[0] -eq $s33 -and $s33 -gt 0 -and
    $latest.groups[1] -gt 0 -and $latest.groups[2] -eq 1) {
    $product = [uint64]$latest.groups[0] * [uint64]$latest.groups[1]
    if ($product -le $heads.Length / 4) {
        $ActiveHeadCount = [int]$product
        $domainSource = 'candidate: latest direct producer groups X*Y, X agrees with captured PS s33; does not prove actual PS indices'
    }
}
if ($ActiveHeadCount) {
    if ($ActiveHeadCount -gt $heads.Length / 4) { throw 'ActiveHeadCount exceeds captured heads' }
    $prefix = New-Object byte[] ($ActiveHeadCount*4)
    [Buffer]::BlockCopy($heads,0,$prefix,0,$prefix.Length)
    $active = [NovaLinkedListAnalysis]::Analyze($prefix, $nodes, [uint32]($node[0].guest_bytes / 8), $counter, $MaxExamples)
    $result.active_domain = @{source=$domainSource; head_count=$ActiveHeadCount; analysis=$active}
    Write-Output "ACTIVE DOMAIN ($domainSource):"
    Write-Output ($active | Format-List | Out-String)
}
if (!$Output) { $Output = Join-Path $folder 'analysis.json' }
$result | ConvertTo-Json -Depth 12 | Set-Content -LiteralPath $Output -Encoding UTF8
Write-Output ($report | Format-List | Out-String)
Write-Output "s33=$s33; report=$Output"
if ($report.UncapturedHeads) { Write-Output 'INCOMPLETE: reachable nodes outside the captured prefix.' }
elseif (!$report.AllHeadsTerminate) { Write-Output 'INVALID: reachable OOB/self-link/cycle found.' }
else { Write-Output 'VALID: all captured heads reach a terminator.' }
