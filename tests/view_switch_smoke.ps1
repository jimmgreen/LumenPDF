# 阅读布局切换（单页 / 双页 / 页面网格）与网格双击打开的原生界面冒烟。只操作自己启动的测试实例和测试副本。
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
$out=Join-Path $PSScriptRoot "../build/app/view-switch-output";New-Item -ItemType Directory -Force $out|Out-Null
function Shot($h,$name){
 $rect=New-Object PageDragNative+R;[void][PageDragNative]::GetWindowRect($h,[ref]$rect)
 $bitmap=New-Object System.Drawing.Bitmap(($rect.right-$rect.left),($rect.bottom-$rect.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
 [void][PageDragNative]::PrintWindow($h,$dc,2);$g.ReleaseHdc($dc)
 $bitmap.Save((Join-Path $out $name));$g.Dispose();$bitmap.Dispose()
}
function MkL($x,$y){[IntPtr]((([int]$y) -shl 16)-bor ([int]$x -band 0xffff))}
function Visible($name){
 $c=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 foreach($e in $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$c)){$b=$e.Current.BoundingRectangle;if(!$e.Current.IsOffscreen -and $b.Width -gt 0 -and $b.Height -gt 0){return $e}}
 return $null
}
function Press($name){$e=Visible $name;if(!$e){Shot $h 'fail.png';throw "button not visible: $name"};$e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
function Status($pattern){($root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)|ForEach-Object{$_.Current.Name}|Where-Object{$_ -like $pattern}|Select-Object -First 1)}
$env:LPDF_SMOKE_MODE='0';$env:LPDF_LAYOUT='0';$env:LPDF_SMOKE_SIZE='1320x860';$env:LPDF_SMOKE_TIMEOUT='60'
$source=Join-Path $out "view-switch.pdf"
Copy-Item "$PSScriptRoot/../build/app/test-output/长文档100页.pdf" $source -Force
$hash=(Get-FileHash $source).Hash
$p=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -WindowStyle Hidden -PassThru
$result=@()
try{
 Start-Sleep -Seconds 5;$p.Refresh();$h=$p.MainWindowHandle
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 if(!(Visible '单页阅读')){Start-Sleep -Seconds 3}
 foreach($n in '单页阅读','双页阅读','页面网格'){if(!(Visible $n)){Shot $h 'fail.png';throw "reader toolbar missing $n"}}
 Start-Sleep -Milliseconds 600;Shot $h 'v0-reader-single.png'
 Press '双页阅读';Start-Sleep -Milliseconds 900;Shot $h 'v1-reader-double.png'
 $st=Status '双页阅读（封面单独）*Ctrl+Shift+D*';if(!$st){throw 'double layout status missing'};$result+="double=$st"
 Press '单页阅读';Start-Sleep -Milliseconds 900;Shot $h 'v2-reader-single.png'
 $st=Status '单页连续阅读';if(!$st){throw 'single layout status missing'};$result+="single=$st"
 Press '页面网格';Start-Sleep -Milliseconds 1000;Shot $h 'v3-grid.png'
 $st=Status '页面网格  ·*';if(!$st){throw 'grid status missing'};$result+="grid=$st"
 if(Visible '手型工具'){throw 'reader toolbar still visible in grid'}
 foreach($n in '单页阅读','双页阅读','页面网格'){if(!(Visible $n)){throw "page toolbar missing $n"}}
 Press '双页阅读';Start-Sleep -Milliseconds 1000;Shot $h 'v4-back-to-double.png'
 if(!(Visible '手型工具')){throw 'grid -> double did not return to reader'};$result+="grid->double ok"
 # 网格中双击第 6 页：回到阅读视图并定位到第 6 页（当前仍是双页布局）。
 Press '页面网格';Start-Sleep -Milliseconds 1000
 $cond=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'PDF 文档')
 $canvas=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$cond)
 $r=$canvas.Current.BoundingRectangle;$s=[PageDragNative]::GetDpiForWindow($h)/96.0
 $w=$r.Width/$s;$columns=[Math]::Max(1,[Math]::Floor($w/220));$cell=($w-24)/$columns
 $scale=[Math]::Min(($cell-32)/$PageW,0.28);$pw=$PageW*$scale;$ph=$PageH*$scale;$pitch=$ph+48
 $i=5;$col=$i%$columns;$row=[Math]::Floor($i/$columns)
 $pt=New-Object PageDragNative+P;$pt.x=[int]($r.X+(12+$col*$cell+$cell/2)*$s);$pt.y=[int]($r.Y+(16+$row*$pitch+$ph/2)*$s)
 [void][PageDragNative]::ScreenToClient($h,[ref]$pt);$l=MkL $pt.x $pt.y
 [void][PageDragNative]::Send($h,0x201,[IntPtr]1,$l);[void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,$l)
 Start-Sleep -Milliseconds 60
 [void][PageDragNative]::Send($h,0x203,[IntPtr]1,$l);[void][PageDragNative]::Send($h,0x202,[IntPtr]::Zero,$l)
 Start-Sleep -Milliseconds 1200;Shot $h 'v5-double-click-page6.png'
 if(!(Visible '手型工具')){throw 'double-click did not open reader'};$result+="dblclick->reader ok"
 [void][PageDragNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 800
 $c2=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,'放弃修改')
 $btn=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$c2)
 if($btn){$btn.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
 if(!$p.WaitForExit(5000)){throw 'process did not exit'}
 if((Get-FileHash $source).Hash -ne $hash){throw 'test copy was modified'}
 $result+='VIEW SWITCH SMOKE OK'
}finally{if(!$p.HasExited){$p.Kill()};Remove-Item Env:LPDF_LAYOUT -ErrorAction SilentlyContinue;$result|ForEach-Object{Write-Host $_}}
