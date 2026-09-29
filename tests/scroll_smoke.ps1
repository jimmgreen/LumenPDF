param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class ScrollNative {
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
[DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h,ref P p);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
[StructLayout(LayoutKind.Sequential)] public struct P { public int x,y; }
[StructLayout(LayoutKind.Sequential)] public struct R {public int left,top,right,bottom;}
}
"@
[void][ScrollNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$env:LPDF_SMOKE_SIZE='1050x720';$env:LPDF_SMOKE_MODE='0'
$source=Join-Path $PSScriptRoot '../build/app/test-output/长文档100页.pdf'
$p=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -WindowStyle Hidden -PassThru
try {
 Start-Sleep -Seconds 5
 $p.Refresh()
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($p.MainWindowHandle)
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'PDF 文档')
 $canvas=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
 if(!$canvas){throw 'PDF canvas missing'}
 $r=$canvas.Current.BoundingRectangle
 $x=[int]($r.X+$r.Width/2);$y=[int]($r.Y+$r.Height/2)
 for($i=0;$i -lt 24;$i++){
  [void][ScrollNative]::PostMessage($p.MainWindowHandle,0x20A,[IntPtr](-120*65536),[IntPtr](($y -shl 16)-bor $x))
 }
 Start-Sleep -Milliseconds 900
 $labels=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
 $page=@($labels | ForEach-Object {$_.Current.Name} | Where-Object {$_ -match '^\d+ / 100$'})[-1]
 if(!$page -or $page -eq '1 / 100'){throw "Wheel did not advance long PDF: $page"}
 "PASS native wheel: $page"
 $point=New-Object ScrollNative+P
 $point.x=[int]($r.Right-12);$point.y=[int]($r.Bottom-8)
 [void][ScrollNative]::ScreenToClient($p.MainWindowHandle,[ref]$point)
 $coords=[IntPtr](($point.y -shl 16)-bor $point.x)
 [void][ScrollNative]::PostMessage($p.MainWindowHandle,0x201,[IntPtr]1,$coords)
 [void][ScrollNative]::PostMessage($p.MainWindowHandle,0x202,[IntPtr]::Zero,$coords)
 Start-Sleep -Milliseconds 1200
 $labels=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
 $page=@($labels | ForEach-Object {$_.Current.Name} | Where-Object {$_ -match '^\d+ / 100$'})[-1]
 if($page -ne '100 / 100'){throw "Scrollbar did not reach end: $page"}
 $rect=New-Object ScrollNative+R;[void][ScrollNative]::GetWindowRect($p.MainWindowHandle,[ref]$rect)
 $bitmap=New-Object System.Drawing.Bitmap(($rect.right-$rect.left),($rect.bottom-$rect.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
 [void][ScrollNative]::PrintWindow($p.MainWindowHandle,$dc,2);$g.ReleaseHdc($dc)
 $bitmap.Save("$PSScriptRoot/../build/ui-interactions/long-document-end.png");$g.Dispose();$bitmap.Dispose()
 "PASS native scrollbar: $page"
 [void][ScrollNative]::PostMessage($p.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 if(!$p.WaitForExit(3000)){throw 'Close failed'}
}finally{
 Remove-Item Env:LPDF_SMOKE_MODE,Env:LPDF_SMOKE_SIZE -ErrorAction SilentlyContinue
}
