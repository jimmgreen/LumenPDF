# 页面视图拖动重排的原生界面冒烟：在真实窗口中发送鼠标消息，截取拖动中、松手后各时刻与稳定后的画面。
# 只操作自己启动的测试实例和测试副本；结束时放弃修改并确认源文件未被改动。
param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[double]$PageW=612,[double]$PageH=792)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;
public class PageDragNative {
 [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h,ref P p);
 [StructLayout(LayoutKind.Sequential)] public struct P {public int x,y;}
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [StructLayout(LayoutKind.Sequential)] public struct R {public int left,top,right,bottom;}
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,IntPtr pid);
 [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
 [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a,uint b,bool attach);
 [DllImport("user32.dll")] public static extern bool GetKeyboardState(byte[] k);
 [DllImport("user32.dll")] public static extern bool SetKeyboardState(byte[] k);
 // 程序用 GetKeyState 判断 Ctrl：点击期间共享目标线程的输入状态并设置键盘状态，不向系统注入真实按键。
 public static void CtrlClick(IntPtr h,IntPtr lp){
  uint me=GetCurrentThreadId(),them=GetWindowThreadProcessId(h,IntPtr.Zero);
  AttachThreadInput(me,them,true);byte[] k=new byte[256];GetKeyboardState(k);
  k[0x11]=0x80;k[0xA2]=0x80;SetKeyboardState(k);
  Send(h,0x201,(IntPtr)9,lp);Send(h,0x202,(IntPtr)8,lp);
  k[0x11]=0;k[0xA2]=0;SetKeyboardState(k);AttachThreadInput(me,them,false);
 }
}
"@
[void][PageDragNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$out=Join-Path $PSScriptRoot "../build/app/page-drag-output";New-Item -ItemType Directory -Force $out|Out-Null
function Shot($h,$name){
 $rect=New-Object PageDragNative+R;[void][PageDragNative]::GetWindowRect($h,[ref]$rect)
 $bitmap=New-Object System.Drawing.Bitmap(($rect.right-$rect.left),($rect.bottom-$rect.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
 [void][PageDragNative]::PrintWindow($h,$dc,2);$g.ReleaseHdc($dc)
 $bitmap.Save((Join-Path $out $name));$g.Dispose();$bitmap.Dispose()
}
function MkL($x,$y){[IntPtr]((([int]$y) -shl 16)-bor ([int]$x -band 0xffff))}
$env:LPDF_SMOKE_MODE='2';$env:LPDF_SMOKE_SIZE='1320x860';$env:LPDF_SMOKE_TIMEOUT='60'
$source=Join-Path $out "page-drag.pdf"
Copy-Item "$PSScriptRoot/../build/app/test-output/长文档100页.pdf" $source -Force
$hash=(Get-FileHash $source).Hash
$p=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -WindowStyle Hidden -PassThru
$result=@()
try{
 Start-Sleep -Seconds 5;$p.Refresh();$h=$p.MainWindowHandle
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 $cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'PDF 文档')
 $canvas=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$cond)
 if(!$canvas){Start-Sleep -Seconds 3;$canvas=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$cond)}
 if(!$canvas){Shot $h 'fail.png';throw "No document canvas (hwnd=$h)"}
 $r=$canvas.Current.BoundingRectangle;$s=[PageDragNative]::GetDpiForWindow($h)/96.0
 # 与 PdfCanvas::LayoutPages 相同的网格几何（DIP）。
 $w=$r.Width/$s;$columns=[Math]::Max(1,[Math]::Floor($w/220));$cell=($w-24)/$columns
 $scale=[Math]::Min(($cell-32)/$PageW,0.28);$pw=$PageW*$scale;$ph=$PageH*$scale;$pitch=$ph+48
 function Cell($i,$fx=0.5,$fy=0.5){
  $col=$i%$columns;$row=[Math]::Floor($i/$columns)
  $x=12+$col*$cell+($cell-$pw)/2+$pw*$fx;$y=16+$row*$pitch+$ph*$fy
  $pt=New-Object PageDragNative+P;$pt.x=[int]($r.X+$x*$s);$pt.y=[int]($r.Y+$y*$s)
  [void][PageDragNative]::ScreenToClient($h,[ref]$pt);return $pt
 }
 $result+="columns=$columns cell=$([Math]::Round($cell,1)) thumb=$([Math]::Round($pw,1))x$([Math]::Round($ph,1))"
 Start-Sleep -Milliseconds 800;Shot $h '0-grid.png'
 # 1) 把第 1 页拖到第 4 页之后（第 4 页右半边）。
 $a=Cell 0;$b=Cell 3 0.8 0.5
 [void][PageDragNative]::Send($h,0x201,[IntPtr]1,(MkL $a.x $a.y));Start-Sleep -Milliseconds 120
 for($i=1;$i -le 24;$i++){
  $x=$a.x+($b.x-$a.x)*$i/24;$y=$a.y+($b.y-$a.y)*$i/24+[Math]::Sin($i/24*3.14159)*30
  [void][PageDragNative]::Send($h,0x200,[IntPtr]1,(MkL $x $y));Start-Sleep -Milliseconds 16
  if($i -eq 10){Shot $h '1-drag-mid.png'}
 }
 Start-Sleep -Milliseconds 60;Shot $h '2-drag-reflow-60ms.png'
 Start-Sleep -Milliseconds 400;Shot $h '3-drag-before-drop.png'
 [void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,(MkL $b.x $b.y))
 foreach($ms in 30,90,180,400){Start-Sleep -Milliseconds $ms;Shot $h "4-drop-$ms.png"}
 Start-Sleep -Milliseconds 1200;Shot $h '5-settled.png'
 $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
 $status=($all|ForEach-Object{$_.Current.Name}|Where-Object{$_ -like '*页已移到*'}|Select-Object -First 1)
 $result+="status=$status"
 if(!$status){throw 'reorder status not shown'}
 # 2) Ctrl 多选第 6、8 页，整组拖到第 2 页前面；拖到一半截图（数量徽标 + 两个空位）。
 $c6=Cell 5;$c8=Cell 7
 [void][PageDragNative]::Send($h,0x201,[IntPtr]1,(MkL $c6.x $c6.y));[void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,(MkL $c6.x $c6.y))
 [PageDragNative]::CtrlClick($h,(MkL $c8.x $c8.y))
 Start-Sleep -Milliseconds 200
 $t=Cell 1 0.15 0.5
 [void][PageDragNative]::Send($h,0x201,[IntPtr]1,(MkL $c8.x $c8.y));Start-Sleep -Milliseconds 100
 for($i=1;$i -le 24;$i++){$x=$c8.x+($t.x-$c8.x)*$i/24;$y=$c8.y+($t.y-$c8.y)*$i/24;[void][PageDragNative]::Send($h,0x200,[IntPtr]1,(MkL $x $y));Start-Sleep -Milliseconds 16}
 Start-Sleep -Milliseconds 450;Shot $h '6-group-drag.png'
 [void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,(MkL $t.x $t.y))
 Start-Sleep -Milliseconds 1500;Shot $h '7-group-settled.png'
 $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
 $status=($all|ForEach-Object{$_.Current.Name}|Where-Object{$_ -like '已移动*页*'}|Select-Object -First 1)
 $result+="group=$status"
 if(!$status){throw 'group reorder status not shown'}
 # 3) Esc 取消：拖动中按 Esc，页面退回原位。
 $c3=Cell 2;$e=Cell 4 0.5 0.3
 [void][PageDragNative]::Send($h,0x201,[IntPtr]1,(MkL $c3.x $c3.y));Start-Sleep -Milliseconds 80
 for($i=1;$i -le 12;$i++){$x=$c3.x+($e.x-$c3.x)*$i/12;$y=$c3.y+($e.y-$c3.y)*$i/12;[void][PageDragNative]::Send($h,0x200,[IntPtr]1,(MkL $x $y));Start-Sleep -Milliseconds 16}
 Start-Sleep -Milliseconds 300;Shot $h '8-before-escape.png'
 [void][PageDragNative]::Send($h,0x100,[IntPtr]0x1B,[IntPtr]1);[void][PageDragNative]::Send($h,0x101,[IntPtr]0x1B,[IntPtr]0xC0000001)
 Start-Sleep -Milliseconds 120;Shot $h '9-escape-120ms.png'
 [void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,(MkL $e.x $e.y))
 Start-Sleep -Milliseconds 1200;Shot $h '10-escape-settled.png'
 # 4) 侧栏缩略图：先单击网格第 1 页让侧栏回到顶部，再把侧栏第 1 页拖到第 2 页下半部分。
 $g0=Cell 0;[void][PageDragNative]::Send($h,0x201,[IntPtr]1,(MkL $g0.x $g0.y));[void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,(MkL $g0.x $g0.y))
 Start-Sleep -Milliseconds 900
 $tc=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'页面缩略图')
 $side=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$tc);if(!$side){throw 'No thumbnail sidebar'}
 $sr=$side.Current.BoundingRectangle;$sw=$sr.Width/$s;$scell=$sw-24;$ss=[Math]::Min(($scell-32)/$PageW,0.19);$tw=$PageW*$ss;$th=$PageH*$ss
 function Thumb($i,$fy){$pt=New-Object PageDragNative+P;$pt.x=[int]($sr.X+(12+($scell-$tw)/2+$tw/2)*$s);$pt.y=[int]($sr.Y+(16+$i*($th+48)+$th*$fy)*$s);[void][PageDragNative]::ScreenToClient($h,[ref]$pt);return $pt}
 $t0=Thumb 0 0.5;$t1=Thumb 1 0.8
 [void][PageDragNative]::Send($h,0x201,[IntPtr]1,(MkL $t0.x $t0.y));Start-Sleep -Milliseconds 100
 for($i=1;$i -le 16;$i++){$x=$t0.x+($t1.x-$t0.x)*$i/16;$y=$t0.y+($t1.y-$t0.y)*$i/16;[void][PageDragNative]::Send($h,0x200,[IntPtr]1,(MkL $x $y));Start-Sleep -Milliseconds 16}
 Start-Sleep -Milliseconds 350;Shot $h '11-sidebar-drag.png'
 [void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,(MkL $t1.x $t1.y))
 Start-Sleep -Milliseconds 1200;Shot $h '12-sidebar-settled.png'
 $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)
 $status=($all|ForEach-Object{$_.Current.Name}|Where-Object{$_ -like '*已移到第 2 位*'}|Select-Object -First 1)
 $result+="sidebar=$status"
 if(!$status){throw 'sidebar reorder status not shown'}
 [void][PageDragNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 800
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 $c2=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'放弃修改')
 $btn=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$c2)
 if($btn){$btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
 if(!$p.WaitForExit(5000)){throw 'process did not exit'}
 if((Get-FileHash $source).Hash -ne $hash){throw 'source modified'}
 $result+='PAGE DRAG SMOKE OK'
}finally{if(!$p.HasExited){$p.Kill()};$result|ForEach-Object{Write-Host $_}}
