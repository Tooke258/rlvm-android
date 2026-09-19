# 从 RealLive 汉化补丁的内存转储里提取中文剧本文本。
#
# 背景与结论见 docs/LOCALIZATION.md：Kud Wafter 的汉化补丁是运行时加载器，
# 译文只存在于进程内存里；内存中的形态是「解压后的场景字节码 + GBK 内联文本」。
#
# 本脚本按已验证的指令结构定位文本：
#   起始：29 40 ?? 05 22 22       （说明：?? 是变化的参数）
#   结束：22 23 00
#   文本：起始之后到结束之间、剔除分隔用的 0x22 字节，按 GBK 解码
#
# 用法（PowerShell，需提权读大文件）：
#   powershell -File tools/extract_cn_from_dump.ps1 -Dump <转储路径> -Out <输出 tsv>
#
# 注意：脚本刻意只扫描**指定范围**（-StartMB/-LengthMB）。全量扫描 300MB 时
# 该模式命中量极大（每次命中都要组串），实测会把内存和时间吃光——先用
# tools 里记录的切片范围做定点提取，再按需要扩大。

param(
    [Parameter(Mandatory = $true)][string]$Dump,
    [Parameter(Mandatory = $true)][string]$Out,
    [int]$StartMB = 90,
    [int]$LengthMB = 12
)

$code = @'
using System;using System.IO;using System.Text;using System.Collections.Generic;
public class CnScan{
 public static List<string> Run(string path,string outPath,long startOff,long length){
  var list=new List<string>();
  var enc=Encoding.GetEncoding(936);
  using(var fs=File.OpenRead(path)){
   fs.Seek(startOff,SeekOrigin.Begin);
   int chunk=8*1024*1024, overlap=256;
   var buf=new byte[chunk+overlap];
   long baseOff=startOff; long remaining=length; int read;
   var sb=new StringBuilder();
   while(remaining>0){
    int want=(int)Math.Min(buf.Length,remaining);
    read=fs.Read(buf,0,want);
    if(read<=0) break;
    for(int i=0;i<read-8;i++){
     if(buf[i]==0x29&&buf[i+1]==0x40&&buf[i+3]==0x05&&buf[i+4]==0x22&&buf[i+5]==0x22){
      int st=i+6,en=st;
      while(en<read-3){ if(buf[en]==0x22&&buf[en+1]==0x23&&buf[en+2]==0x00) break; en++; }
      if(en>st&&en<read-1){
       sb.Length=0;
       for(int k=st;k<en;k++){ if(buf[k]!=0x22) sb.Append((char)buf[k]); }
       if(sb.Length>=2&&sb.Length<4000){
        var bytes=new byte[sb.Length];
        for(int k=0;k<sb.Length;k++) bytes[k]=(byte)sb[k];
        list.Add((baseOff+st).ToString()+"\t"+enc.GetString(bytes));
       }
      }
      i=en;
     }
    }
    if(read<want) break;
    baseOff+=read-overlap;
    remaining-=(read-overlap);
    fs.Seek(baseOff,SeekOrigin.Begin);
   }
  }
  File.WriteAllLines(outPath,list,new UTF8Encoding(false));
  return list;
 }
}
'@

Add-Type -TypeDefinition $code -Language CSharp

$start = [long]$StartMB * 1MB
$len = [long]$LengthMB * 1MB
Write-Host "扫描 $Dump 的 $StartMB MB ~ $($StartMB + $LengthMB) MB ..."
$rows = [CnScan]::Run($Dump, $Out, $start, $len)
Write-Host "提取 $($rows.Count) 条 -> $Out"
$rows | Select-Object -First 5
