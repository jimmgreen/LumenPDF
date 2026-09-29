param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[string]$Output="$PSScriptRoot/../build/ui",[int[]]$Modes=@(0,1,2,3),[switch]$Empty,[string]$Size="1320x860")
$ErrorActionPreference='Stop'
New-Item -ItemType Directory -Force -Path $Output | Out-Null
Add-Type -AssemblyName System.Drawing
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class PdfCapture {
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr context);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr hwnd,IntPtr hdc,uint flags);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr hwnd,out Rect r);
 [StructLayout(LayoutKind.Sequential)] public struct Rect{public int left,top,right,bottom;}
}
'@
[void][PdfCapture]::SetThreadDpiAwarenessContext([IntPtr](-4))
foreach($mode in $Modes){
    $env:LPDF_SMOKE_MODE="$mode"
    $env:LPDF_SMOKE_SIZE=$Size
    $filename=if($mode -eq 1){"中文批注.pdf"}else{"合并.pdf"}
    $source=Join-Path $PSScriptRoot "../build/app/test-output/$filename"
    $arguments=if($Empty){@("--smoke")}else{@("--smoke",([char]34 + $source + [char]34))}
    $p=Start-Process -FilePath $Exe -ArgumentList $arguments -WindowStyle Hidden -PassThru
    Start-Sleep -Seconds 6
    $p.Refresh()
    if($p.HasExited){throw "Application exited early: $($p.ExitCode)"}
    $r=New-Object PdfCapture+Rect
    if(-not [PdfCapture]::GetWindowRect($p.MainWindowHandle,[ref]$r)){throw 'No application window'}
    $bitmap=New-Object System.Drawing.Bitmap(($r.right-$r.left),($r.bottom-$r.top))
    $graphics=[System.Drawing.Graphics]::FromImage($bitmap)
    $dc=$graphics.GetHdc()
    $ok=[PdfCapture]::PrintWindow($p.MainWindowHandle,$dc,2)
    $graphics.ReleaseHdc($dc)
    $bitmap.Save((Join-Path ([IO.Path]::GetFullPath($Output)) "mode-$mode-$Size-empty-$Empty.png"))
    $graphics.Dispose();$bitmap.Dispose()
    if(-not $ok){throw 'Window capture failed'}
    if(-not $p.WaitForExit(20000)){throw 'Smoke application did not close on time'}
    if($p.ExitCode -ne 0){throw "Application failed: $($p.ExitCode)"}
    "PASS native mode $mode"
}
Remove-Item Env:LPDF_SMOKE_MODE,Env:LPDF_SMOKE_SIZE -ErrorAction SilentlyContinue




