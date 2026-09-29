param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[switch]$Observe)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;using System.Threading;
public class DoubleNative {
 [StructLayout(LayoutKind.Sequential)]public struct P {public int x,y;}
 [StructLayout(LayoutKind.Sequential)]public struct R {public int left,top,right,bottom;}
 [StructLayout(LayoutKind.Sequential)]public struct MI {public int dx,dy;public uint data,flags,time;public UIntPtr extra;}
 [StructLayout(LayoutKind.Sequential)]public struct INPUT {public uint type;public MI mouse;}
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]public static extern uint SendInput(uint count,INPUT[] input,int size);
 [DllImport("user32.dll")]public static extern bool GetCursorPos(out P p);
 [DllImport("user32.dll")]public static extern bool SetCursorPos(int x,int y);
 [DllImport("user32.dll")]public static extern uint GetDoubleClickTime();
 [DllImport("user32.dll")]public static extern int GetSystemMetrics(int id);
 [DllImport("user32.dll")]public static extern short GetAsyncKeyState(int key);
 [DllImport("user32.dll")]public static extern IntPtr WindowFromPoint(P point);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint p);
 [DllImport("user32.dll")]public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")]public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr z,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern bool EnumWindows(EnumProc fn,IntPtr l);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]public static extern IntPtr ReadText(IntPtr h,uint m,IntPtr n,StringBuilder s);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 public static IntPtr Named(uint pid,string name){IntPtr found=IntPtr.Zero;EnumWindows((h,l)=>{uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid||!IsWindowVisible(h))return true;var s=new StringBuilder(256);ReadText(h,0xD,(IntPtr)256,s);if(s.ToString()==name){found=h;return false;}return true;},IntPtr.Zero);return found;}
 public static void Move(int x,int y){
  int left=GetSystemMetrics(76),top=GetSystemMetrics(77),width=GetSystemMetrics(78),height=GetSystemMetrics(79);
  INPUT input=new INPUT();input.mouse.dx=(int)Math.Floor((x-left+0.5)*65536.0/width);input.mouse.dy=(int)Math.Floor((y-top+0.5)*65536.0/height);input.mouse.flags=0xC001;
  if(SendInput(1,new[]{input},Marshal.SizeOf(typeof(INPUT)))!=1)throw new Exception("Mouse move injection failed");
 }
 public static void Click(bool twice){
  foreach(int key in new[]{16,17,18,91,92})if((GetAsyncKeyState(key)&0x8000)!=0)throw new Exception("Refusing input while a modifier key is held");
  INPUT down=new INPUT(),up=new INPUT();down.mouse.flags=2;up.mouse.flags=4;
  if(SendInput(2,new[]{down,up},Marshal.SizeOf(typeof(INPUT)))!=2)throw new Exception("Mouse click injection failed");
  if(twice){Thread.Sleep((int)Math.Max(10,Math.Min(70,GetDoubleClickTime()/4)));if(SendInput(2,new[]{down,up},Marshal.SizeOf(typeof(INPUT)))!=2)throw new Exception("Second click injection failed");}
 }
 public static void Release(){INPUT input=new INPUT();input.mouse.flags=4;SendInput(1,new[]{input},Marshal.SizeOf(typeof(INPUT)));}

}
"@
[void][DoubleNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$root=Split-Path -Parent $PSScriptRoot
if(!(Test-Path -LiteralPath (Join-Path $root 'app'))){$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))}
$Exe=[IO.Path]::GetFullPath($Exe)
$fixture=Join-Path $root 'build/app/text-output/text-auto-fixture.pdf'
$folder=Join-Path $root 'build/app/doubleclick-output';[void](New-Item -ItemType Directory -Force -Path $folder)
$hash=(Get-FileHash -LiteralPath $fixture).Hash
$originalCursor=New-Object DoubleNative+P;[void][DoubleNative]::GetCursorPos([ref]$originalCursor)
$script:lastPointer=$null;$results=@();$checks=0
function Check($ok,$message){$script:checks++;if(!$ok){throw $message}}
function Find-Element($name,$window=$script:h){
 if($window -eq [IntPtr]::Zero){return $null};$tree=[System.Windows.Automation.AutomationElement]::FromHandle($window)
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 foreach($e in $tree.FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition)){
  $r=$e.Current.BoundingRectangle;if($e.Current.IsEnabled -and !$e.Current.IsOffscreen -and $r.Width -gt 1 -and $r.Height -gt 1){return $e}
 };return $null
}
function Wait-For($probe,$name,$seconds=10){$clock=[Diagnostics.Stopwatch]::StartNew();while($clock.Elapsed.TotalSeconds -lt $seconds){$v=& $probe;if($v){return $v};Start-Sleep -Milliseconds 70};throw "Timeout: $name"}
function Invoke-Button($name,$window=$script:h){$e=Wait-For {Find-Element $name $window} $name;$e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke();Start-Sleep -Milliseconds 160}
function Editor {return Find-Element '页内文字编辑器'}
function Canvas-Rect {
 $r=(Find-Element 'PDF 文档').Current.BoundingRectangle;$q=New-Object DoubleNative+R
 $q.left=[int][Math]::Ceiling($r.Left);$q.top=[int][Math]::Ceiling($r.Top);$q.right=[int][Math]::Floor($r.Right);$q.bottom=[int][Math]::Floor($r.Bottom);return $q
}
function Move-Owned($x,$y){
 [void][DoubleNative]::SetForegroundWindow($script:h)
 [DoubleNative]::Move($x,$y);Start-Sleep -Milliseconds 100
 $actual=New-Object DoubleNative+P;[void][DoubleNative]::GetCursorPos([ref]$actual)
 if([Math]::Abs($actual.x-$x) -gt 1 -or [Math]::Abs($actual.y-$y) -gt 1){throw 'Pointer moved during test; refusing to click'}
 [uint32]$ownerId=0;$under=[DoubleNative]::WindowFromPoint($actual);[void][DoubleNative]::GetWindowThreadProcessId($under,[ref]$ownerId)
 if($ownerId -ne $script:p.Id){throw 'Refusing mouse input outside the test application'}
 $script:lastPointer=$actual
}
function Screenshot($name){
 $r=New-Object DoubleNative+R;[void][DoubleNative]::GetWindowRect($script:h,[ref]$r)
 $image=New-Object System.Drawing.Bitmap(($r.right-$r.left),($r.bottom-$r.top));$g=[System.Drawing.Graphics]::FromImage($image)
 $g.CopyFromScreen($r.left,$r.top,0,0,$image.Size);$image.Save((Join-Path $folder ($name+'.png')));$g.Dispose();$image.Dispose()
}
$cases=@(@{name='select-fit';tool='选择';read=$false;actual=$false},@{name='text-fit';tool='文字';read=$false;actual=$false},@{name='select-100';tool='选择';read=$false;actual=$true},@{name='text-100';tool='文字';read=$false;actual=$true},@{name='reading-select-100';tool='选择';read=$true;actual=$true})
if($Observe){$cases=@($cases[0])}
$env:LPDF_SMOKE_MODE='1';$env:LPDF_SMOKE_SIZE='1320x860';$env:LPDF_SMOKE_TIMEOUT='90'
try{
 foreach($case in $cases){
  $source=Join-Path $folder ($case.name+'.pdf');Copy-Item -LiteralPath $fixture -Destination $source -Force
  $script:p=Start-Process $Exe -ArgumentList @('--smoke',('"'+$source+'"')) -PassThru;$retained=$p.Handle
  try{
   $script:h=Wait-For {$p.Refresh();if($p.MainWindowHandle -ne [IntPtr]::Zero){$p.MainWindowHandle}} 'main window'
   $dpi=[DoubleNative]::GetDpiForWindow($h)/96.0
   [void][DoubleNative]::SetWindowPos($h,[IntPtr](-1),20,20,[int](1320*$dpi),[int](860*$dpi),0x10)
   [void][DoubleNative]::SetForegroundWindow($h);Start-Sleep -Seconds 4
   [void](Wait-For {Find-Element '编辑文字'} 'annotation ready')
   # Select using a real click at the fixture's known page position first.
   # A late page-data reply may have cleared the initial smoke selection.
   $cr=(Find-Element 'PDF 文档').Current.BoundingRectangle
   $fit=[Math]::Min(($cr.Width-52*$dpi)/595.28,($cr.Height-40*$dpi)/841.89)
   $sx=[int][Math]::Round($cr.X+($cr.Width-595.28*$fit)/2+80*$fit)
   $sy=[int][Math]::Round($cr.Y+16*$dpi+259*$fit)
   Move-Owned $sx $sy;[DoubleNative]::Click($false);Start-Sleep -Milliseconds 180
   if($case.read){Invoke-Button '阅读'}
   $area=(Find-Element 'PDF 文档').Current.BoundingRectangle
   $fit=[Math]::Min(($area.Width-52*$dpi)/595.28,($area.Height-40*$dpi)/841.89)
   $scale=$fit;$scroll=0.0
   if($case.actual){
    # ZoomTo preserves the viewport-center page point. Derive its new scroll
    # from the same documented page/DIP transform, not from an image scan.
    $anchor=($area.Height/2-16*$dpi)/$fit
    $scale=(96.0/72.0)*$dpi
    $scroll=[Math]::Max(0,[Math]::Min(16*$dpi+841.89*$scale+24*$dpi-$area.Height,16*$dpi+$anchor*$scale-$area.Height/2))
    Invoke-Button '页面实际大小'
   }
   Start-Sleep -Milliseconds 300
   $canvasBefore=Canvas-Rect
   $pageLeft=$area.Left+[Math]::Max(12*$dpi,($area.Width-595.28*$scale)/2)
   $before=@{left=$pageLeft+65*$scale;top=$area.Top+16*$dpi+250*$scale-$scroll}
   $x=[int][Math]::Round($pageLeft+80*$scale);$y=[int][Math]::Round($area.Top+16*$dpi+259*$scale-$scroll)
   if(!$case.read){Invoke-Button $case.tool}
   Start-Sleep -Milliseconds ([DoubleNative]::GetDoubleClickTime()+100)
   Move-Owned $x $y;[DoubleNative]::Click($false);Start-Sleep -Milliseconds 160
   Check (!(Editor)) 'A single click unexpectedly entered edit mode'
   Start-Sleep -Milliseconds ([DoubleNative]::GetDoubleClickTime()+100)
   Move-Owned $x $y;[DoubleNative]::Click($true)
   $entered=$null
   $clock=[Diagnostics.Stopwatch]::StartNew();while($clock.Elapsed.TotalSeconds -lt 5){$entered=Editor;if($entered){break};Start-Sleep -Milliseconds 80}
   if($Observe){
    $results+=@{case=$case.name;entered=[bool]$entered;realInput=$true;before=$before}
    Screenshot 'baseline-real-double-click'
    Write-Host ('BASELINE real double-click entered='+[bool]$entered)
   }else{
    Check ($null -ne $entered) ("Real double click did not enter editing: "+$case.name)
    $bar=Wait-For {$b=[DoubleNative]::Named([uint32]$p.Id,'文字格式 · 整条批注');if($b -ne [IntPtr]::Zero){$b}} 'format window'
    $value=$entered.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value
    Check ($value -eq '水电费') 'Double click entered the wrong annotation'
    $canvasAfter=Canvas-Rect
    Check ($canvasBefore.left -eq $canvasAfter.left -and $canvasBefore.top -eq $canvasAfter.top -and $canvasBefore.right -eq $canvasAfter.right -and $canvasBefore.bottom -eq $canvasAfter.bottom) 'Entering editing changed the workspace/canvas viewport'
    $r=$entered.Current.BoundingRectangle;$after=@{left=$r.Left;top=$r.Top;right=$r.Right;bottom=$r.Bottom}
    Check ([Math]::Abs($after.left-$before.left) -le 2 -and [Math]::Abs($after.top-$before.top) -le 2) 'Double click scrolled, zoomed or moved the text frame'
    Check ($r.Contains($x,$y)) 'The clicked text point is not inside the in-place editor' 
    Screenshot ($case.name+'-editing')
    $results+=@{case=$case.name;entered=$true;realInput=$true;before=$before;after=$after;dpi=$dpi}
    Invoke-Button '取消文字编辑' $bar
    [void](Wait-For {!(Editor)} 'editor cancellation')
   }
   if(Editor){$bar=[DoubleNative]::Named([uint32]$p.Id,'文字格式 · 整条批注');Invoke-Button '取消文字编辑' $bar}
   [void][DoubleNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
   if(!$p.WaitForExit(4000)){throw 'Test application did not close'}
   Check ($p.ExitCode -eq 0) 'Abnormal application exit'
   Check ((Get-FileHash -LiteralPath $source).Hash -eq $hash) 'Selecting/editing/cancelling modified the source PDF'
  }catch{
   try{if(!$p.HasExited){Screenshot ($case.name+'-failure')}}catch{}
   throw
  }finally{if(!$p.HasExited){$p.Kill();$p.WaitForExit()}}
 }
 Check ((Get-FileHash -LiteralPath $fixture).Hash -eq $hash) 'Original fixture changed'
 @{observe=[bool]$Observe;checks=$checks;cases=$results;sourceHash=$hash} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $folder $(if($Observe){'baseline.json'}else{'result.json'})) -Encoding UTF8
 if(!$Observe){Write-Host "PASS real mouse double click: $checks assertions across Select, Text and reading-mode in-place entry."}
}finally{
 Remove-Item Env:LPDF_SMOKE_MODE,Env:LPDF_SMOKE_SIZE,Env:LPDF_SMOKE_TIMEOUT -ErrorAction SilentlyContinue
 if($script:lastPointer){$now=New-Object DoubleNative+P;[void][DoubleNative]::GetCursorPos([ref]$now);if([Math]::Abs($now.x-$script:lastPointer.x) -le 1 -and [Math]::Abs($now.y-$script:lastPointer.y) -le 1){[void][DoubleNative]::SetCursorPos($originalCursor.x,$originalCursor.y)}}
}
