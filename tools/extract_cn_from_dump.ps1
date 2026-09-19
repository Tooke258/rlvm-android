# Extract Chinese script text from a RealLive Chinese-patch memory dump.
# (This file MUST stay pure ASCII: PowerShell 5.1 reads BOM-less .ps1 as ANSI
#  and mangles non-ASCII comments, which silently swallows following lines.)
#
# Context and findings: docs/LOCALIZATION.md. The patch is a runtime loader, so
# the translation only exists in process memory, stored as decompressed scene
# bytecode with GBK text inlined.
#
# Verified textout structure. There are (at least) TWO variants, so the anchor
# is the marker they share - 05 22 22 - not the opcode that precedes it:
#   character line: FF 01 00 00 00 29 40 ?? 05 22 22 ...
#   narration:      00 00 00 0A ?? 0E 40 ?? 05 22 22 ...
#   end:   22 23 00
#   text:  bytes between the marker and the end, with the 0x22 separators
#          removed, decoded as GBK. Format inside is
#            81 79 22 <speaker> 22 81 7A 22 A1B8 <line> A1B9
#          i.e. speaker and line are delimited, which post-processing splits.
#
# Strict filter (needed because "contains a CJK char" is almost no constraint:
# GBK covers most byte pairs, so statistical matching matches garbage):
#   * strict GBK decode (invalid sequences are dropped)
#   * printable only: no control chars, no U+FFFD
#   * at least 2 CJK ideographs, length 2..400
#   * global de-duplication by content (lines appear in several copies)
#
# Usage (needs elevation to read the large dump):
#   powershell -NoProfile -ExecutionPolicy Bypass -File tools/extract_cn_from_dump.ps1 `
#       -Dump <dump> -Out <tsv> [-Threads 16] [-StartMB 0] [-LengthMB 0]
# LengthMB=0 scans to the end of the file.
#
# Multiple dumps can be merged in one go (results are de-duplicated by content,
# so dumping the same scene twice costs nothing):
#   ... -Dump dump1.DMP,dump2.DMP,dump3.DMP -Out merged.tsv
# This is the intended workflow: the Chinese patch caches translated scenes in
# memory as the story advances, so dumping periodically (e.g. once per chapter)
# accumulates coverage - and dumping per chapter also avoids losing everything
# if the 32-bit process runs out of memory and dies mid-skip.

param(
    [Parameter(Mandatory = $true)][string[]]$Dump,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$Threads = 16,
    [int]$StartMB = 0,
    [int]$LengthMB = 0
)

$code = @'
using System;using System.IO;using System.Text;using System.Collections.Generic;using System.Threading.Tasks;
public class CnScan2{
 static Encoding Strict936(){
  return Encoding.GetEncoding(936, EncoderFallback.ExceptionFallback, DecoderFallback.ExceptionFallback);
 }
 static bool LooksLikeText(string s, out int cjk){
  cjk=0;
  if(s.Length<2||s.Length>400) return false;
  foreach(char c in s){
   if(c<0x20||c=='\uFFFD') return false;
   if((c>=0x4E00&&c<=0x9FFF)||(c>=0x3400&&c<=0x4DBF)) cjk++;
   else if(c>=0x3000&&c<=0x303F) {}
   else if(c>=0xFF00&&c<=0xFFEF) {}
   else if(c>=0x20&&c<0x7F) {}
   else if(c>=0x3040&&c<=0x30FF) {}
   else return false;
  }
  return cjk>=2;
 }
 public static string Run(string path,string outPath,int threads,long startOff,long length){
  var fi=new FileInfo(path);
  long total=length>0?Math.Min(length,fi.Length-startOff):fi.Length-startOff;
  long per=total/threads+1;
  var bags=new List<string>[threads];
  long candidates=0,accepted=0,rejected=0;
  var enc=Strict936();
  Parallel.For(0,threads,t=>{
   var local=new List<string>();
   long s0=startOff+t*per;
   if(s0>=startOff+total){ bags[t]=local; return; }
   long len=Math.Min(per+64,startOff+total-s0);
   var buf=new byte[len];
   using(var fs=File.OpenRead(path)){
    fs.Seek(s0,SeekOrigin.Begin);
    int got=0,rd;
    while(got<len && (rd=fs.Read(buf,got,(int)(len-got)))>0) got+=rd;
    len=got;
   }
   var sb=new StringBuilder();
   for(int i=0;i<len-8;i++){
    if(buf[i]==0x05&&buf[i+1]==0x22&&buf[i+2]==0x22){
     System.Threading.Interlocked.Increment(ref candidates);
     int st=i+3,en=st;
     while(en<len-3){ if(buf[en]==0x22&&buf[en+1]==0x23&&buf[en+2]==0x00) break; en++; }
     if(en>st&&en<len-1){
      sb.Length=0;
      for(int k=st;k<en;k++){ if(buf[k]!=0x22) sb.Append((char)buf[k]); }
      if(sb.Length>=2){
       var bytes=new byte[sb.Length];
       for(int k=0;k<sb.Length;k++) bytes[k]=(byte)sb[k];
       string txt=null;
       try{ txt=enc.GetString(bytes); }catch{ txt=null; }
       int cjk;
       if(txt!=null&&LooksLikeText(txt,out cjk)){
        System.Threading.Interlocked.Increment(ref accepted);
        local.Add((s0+st).ToString()+"\t"+txt);
       } else System.Threading.Interlocked.Increment(ref rejected);
      } else System.Threading.Interlocked.Increment(ref rejected);
     }
     i=en;
    }
   }
   bags[t]=local;
  });
  var seen=new HashSet<string>();
  var rows=new List<string>();
  for(int t=0;t<threads;t++){
   if(bags[t]==null) continue;
   foreach(var line in bags[t]){
    int tab=line.IndexOf('\t');
    string body=tab>=0?line.Substring(tab+1):line;
    if(seen.Add(body)) rows.Add(line);
   }
  }
  rows.Sort((a,b)=>{
   long x=long.Parse(a.Substring(0,a.IndexOf('\t')));
   long y=long.Parse(b.Substring(0,b.IndexOf('\t')));
   return x.CompareTo(y);
  });
  File.WriteAllLines(outPath,rows,new UTF8Encoding(false));
  return string.Format("threads={0} candidates={1} accepted={2} rejected={3} unique={4}",
   threads,candidates,accepted,rejected,rows.Count);
 }
}
'@

Add-Type -TypeDefinition $code -Language CSharp

$start = [long]$StartMB * 1MB
$len = [long]$LengthMB * 1MB
$lenText = if ($LengthMB -eq 0) { "to-end" } else { "${LengthMB}MB" }

# Merge across dumps, de-duplicated by text content.
$seen = New-Object 'System.Collections.Generic.HashSet[string]'
$merged = New-Object 'System.Collections.Generic.List[string]'
$totalUnique = 0
foreach ($one in $Dump) {
    if (-not (Test-Path -LiteralPath $one)) { Write-Host "skip (not found): $one"; continue }
    $tmp = [System.IO.Path]::Combine($env:TEMP, "cnscan-part.tsv")
    Write-Host ("Scanning {0} (start {1}MB, length {2}, threads {3}) ..." -f $one, $StartMB, $lenText, $Threads)
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $summary = [CnScan2]::Run($one, $tmp, $Threads, $start, $len)
    $sw.Stop()
    $added = 0
    foreach ($line in [System.IO.File]::ReadAllLines($tmp)) {
        $tab = $line.IndexOf([char]9)
        if ($tab -lt 0) { continue }
        $body = $line.Substring($tab + 1)
        if ($seen.Add($body)) { $merged.Add($line); $added++ }
    }
    $totalUnique += $added
    Write-Host ("{0}  new-unique={1}  total-unique={2}  elapsed {3}s" -f $summary, $added, $totalUnique, [math]::Round($sw.Elapsed.TotalSeconds, 1))
}

$merged.Sort()
[System.IO.File]::WriteAllLines($Out, $merged, (New-Object System.Text.UTF8Encoding($false)))
Write-Host ("merged unique lines: {0} -> {1}" -f $merged.Count, $Out)
Get-Content $Out -Encoding UTF8 -TotalCount 5
