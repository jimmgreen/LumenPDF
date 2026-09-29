param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class DragNative {
 [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h,ref P p);
 [StructLayout(LayoutKind.Sequential)] public struct P {public int x,y;}
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [StructLayout(LayoutKind.Sequential)] public struct R {public int left,top,right,bottom;}
}
"@
[void][DragNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
function Shot($h,$path){
 $rect=New-Object DragNative+R;[void][DragNative]::GetWindowRect($h,[ref]$rect)
 $bitmap=New-Object System.Drawing.Bitmap(($rect.right-$rect.left),($rect.bottom-$rect.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
 [void][DragNative]::PrintWindow($h,$dc,2);$g.ReleaseHdc($dc)
 $bitmap.Save($path);$g.Dispose();$bitmap.Dispose()
}
$env:LPDF_SMOKE_MODE='1';$env:LPDF_SMOKE_SIZE='1320x860'
$source=Join-Path $PSScriptRoot "../build/app/test-output/drag-smoke.pdf"
Copy-Item "$PSScriptRoot/../build/app/test-output/中文批注.pdf" $source
$hash=(Get-FileHash $source).Hash
$p=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -WindowStyle Hidden -PassThru
try{
 Start-Sleep -Seconds 5;$p.Refresh()
 $h=$p.MainWindowHandle
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'PDF 文档')
 $canvas=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
 if(!$canvas){throw 'No document canvas'}
 $r=$canvas.Current.BoundingRectangle
 # 文字批注位于页面坐标 (50,160)-(310,220)，页面 595x842 居中显示。按 canvas 宽度估算比例并点到批注内部。
 $scale=[Math]::Min(($r.Width-52*1.5)/595,($r.Height-40*1.5)/842)
 $pageX=$r.X+($r.Width-595*$scale)/2;$pageY=$r.Y+16*1.5
 $point=New-Object DragNative+P
 $point.x=[int]($pageX+80*$scale);$point.y=[int]($pageY+175*$scale)
 [void][DragNative]::ScreenToClient($h,[ref]$point)
 $x0=$point.x;$y0=$point.y
 $xy=[IntPtr](($y0 -shl 16)-bor $x0)
 [void][DragNative]::Send($h,0x201,[IntPtr]1,$xy)  # 选中
 [void][DragNative]::Send($h,0x202,[IntPtr]::Zero,$xy)
 Start-Sleep -Milliseconds 400
 Shot $h "$PSScriptRoot/../build/app/drag-0-selected.png"
 [void][DragNative]::Send($h,0x201,[IntPtr]1,$xy)  # 按下开始拖动
 Start-Sleep -Milliseconds 400
 for($i=1;$i -le 12;$i++){
  $xy=[IntPtr]((($y0+$i*15) -shl 16)-bor ($x0+$i*20))
  [void][DragNative]::Send($h,0x200,[IntPtr]1,$xy);Start-Sleep -Milliseconds 40
  if($i -eq 6){Start-Sleep -Milliseconds 200;Shot $h "$PSScriptRoot/../build/app/drag-1-mid.png"}
 }
 Start-Sleep -Milliseconds 200;Shot $h "$PSScriptRoot/../build/app/drag-2-before-drop.png"
 [void][DragNative]::Send($h,0x202,[IntPtr]::Zero,$xy)
 foreach($ms in 20,60,120,250,500,1000){Start-Sleep -Milliseconds $ms;Shot $h "$PSScriptRoot/../build/app/drag-3-drop-$ms.png"}
 Start-Sleep -Milliseconds 1500;Shot $h "$PSScriptRoot/../build/app/drag-4-settled.png"
 [void][DragNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 800
 # 关闭对话框：放弃修改
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 $c2=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'放弃修改')
 $b=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$c2)
 if($b){$b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
 if(!$p.WaitForExit(5000)){throw 'process did not exit'}
 if((Get-FileHash $source).Hash -ne $hash){throw 'source modified'}
 Write-Host 'DRAG SMOKE OK'
}finally{if(!$p.HasExited){$p.Kill()}}
