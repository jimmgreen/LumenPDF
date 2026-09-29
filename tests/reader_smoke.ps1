# 阅读器端到端冒烟：拖选文字并复制、单击内部链接跳转、打印到 PDF 文件、单实例转发。
param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[string]$Output="$PSScriptRoot/../build/reader")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing,System.Windows.Forms
Add-Type @"
using System;using System.Runtime.InteropServices;
public class RNative {
 [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h,ref P p);
 [StructLayout(LayoutKind.Sequential)] public struct P {public int x,y;}
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
 [DllImport("user32.dll")] public static extern void keybd_event(byte k,byte s,uint f,UIntPtr e);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [StructLayout(LayoutKind.Sequential)] public struct R {public int left,top,right,bottom;}
}
"@
[void][RNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
New-Item -ItemType Directory -Force $Output | Out-Null
$results=@()
function Shot($h,$name){
 $rect=New-Object RNative+R;[void][RNative]::GetWindowRect($h,[ref]$rect)
 $bitmap=New-Object System.Drawing.Bitmap(($rect.right-$rect.left),($rect.bottom-$rect.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
 [void][RNative]::PrintWindow($h,$dc,2);$g.ReleaseHdc($dc)
 $bitmap.Save((Join-Path $Output $name));$g.Dispose();$bitmap.Dispose()
}
function Front($h){for($i=0;$i -lt 10 -and [RNative]::GetForegroundWindow() -ne $h;$i++){[RNative]::keybd_event(0x12,0,0,[UIntPtr]::Zero);[RNative]::keybd_event(0x12,0,2,[UIntPtr]::Zero);[void][RNative]::SetForegroundWindow($h);Start-Sleep -Milliseconds 150};[RNative]::GetForegroundWindow() -eq $h}
function Find($root,$name){$c=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name);$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$c)}
function Client($h,$x,$y){$p=New-Object RNative+P;$p.x=[int]$x;$p.y=[int]$y;[void][RNative]::ScreenToClient($h,[ref]$p);[IntPtr](($p.y -shl 16)-bor ($p.x -band 0xffff))}
$fixture=(Resolve-Path "$PSScriptRoot/fixtures/links.pdf").Path
$source=Join-Path $Output "links-copy.pdf";Copy-Item $fixture $source -Force
$env:LPDF_SMOKE_MODE='0';$env:LPDF_SMOKE_SIZE='1320x860';$env:LPDF_SMOKE_TIMEOUT='40'
$p=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -PassThru
try{
 Start-Sleep -Seconds 5;$p.Refresh();$h=$p.MainWindowHandle
 $scale=[RNative]::GetDpiForWindow($h)/96.0
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
 $canvas=Find $root 'PDF 文档';if(!$canvas){throw 'No document canvas'}
 $r=$canvas.Current.BoundingRectangle
 # 适合页面：612x792 页面在画布中水平居中，顶部留 16 DIP。
 $k=[Math]::Min(($r.Width-52*$scale)/612,($r.Height-40*$scale)/792)
 $px=$r.X+($r.Width-612*$k)/2;$py=$r.Y+16*$scale
 function PagePt($x,$y){Client $h ($px+$x*$k) ($py+$y*$k)}
 # 1) 拖选前两行文字。
 [void][RNative]::Send($h,0x201,[IntPtr]1,(PagePt 70 60))
 for($i=1;$i -le 10;$i++){[void][RNative]::Send($h,0x200,[IntPtr]1,(PagePt (70+$i*33) (60+$i*3.6)));Start-Sleep -Milliseconds 30}
 [void][RNative]::Send($h,0x202,[IntPtr]::Zero,(PagePt 400 96))
 Start-Sleep -Milliseconds 800;Shot $h 'reader-1-selection.png'
 $status=(Find $root '文件仅在本机处理')
 $front=Front $h;Start-Sleep -Milliseconds 300
 [System.Windows.Forms.Clipboard]::Clear()
 [RNative]::keybd_event(0x11,0,0,[UIntPtr]::Zero);[RNative]::keybd_event(0x43,0,0,[UIntPtr]::Zero)
 [RNative]::keybd_event(0x43,0,2,[UIntPtr]::Zero);[RNative]::keybd_event(0x11,0,2,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 1200
 $clip=[System.Windows.Forms.Clipboard]::GetText()
 $results+=[pscustomobject]@{check='drag-select + Ctrl+C';ok=($clip -like '*Links fixture first line*' -and $clip -like '*Second line*');detail="front=$front "+($clip -replace "`r?`n",' | ')}
 # 2) 单击内部链接（第 1 页 "Go to page three"，页面坐标 70..260 x 118..138）。
 [void][RNative]::Send($h,0x200,[IntPtr]::Zero,(PagePt 120 128));Start-Sleep -Milliseconds 300
 Shot $h 'reader-2-link-hover.png'
 [void][RNative]::Send($h,0x201,[IntPtr]1,(PagePt 120 128));[void][RNative]::Send($h,0x202,[IntPtr]::Zero,(PagePt 120 128))
 Start-Sleep -Milliseconds 1200;Shot $h 'reader-3-after-link.png'
 $pageBox=Find $root '当前页码，输入后回车跳转'
 $page=$null;if($pageBox){try{$page=$pageBox.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value}catch{}}
 $results+=[pscustomobject]@{check='internal link jumps to page 3';ok=($page -eq '3');detail="page box=$page"}
 # 3) Alt+← 返回。
 [void](Front $h)
 [RNative]::keybd_event(0x12,0,0,[UIntPtr]::Zero);[RNative]::keybd_event(0x25,0,0,[UIntPtr]::Zero)
 [RNative]::keybd_event(0x25,0,2,[UIntPtr]::Zero);[RNative]::keybd_event(0x12,0,2,[UIntPtr]::Zero)
 Start-Sleep -Milliseconds 1000
 $page2=$null;if($pageBox){try{$page2=$pageBox.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value}catch{}}
 $results+=[pscustomobject]@{check='Alt+Left returns to page 1';ok=($page2 -eq '1');detail="page box=$page2"}
}finally{if(!$p.HasExited){$p.Kill()}}
# 4) 打印到 PDF 文件（Microsoft Print to PDF）。
$printed=Join-Path $Output 'printed.pdf';Remove-Item $printed -ErrorAction SilentlyContinue
$env:LPDF_PRINT_TO=$printed;$env:LPDF_SMOKE_TIMEOUT='25'
$p=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -WindowStyle Hidden -PassThru
for($i=0;$i -lt 40 -and !(Test-Path $printed);$i++){Start-Sleep -Milliseconds 500}
Start-Sleep -Seconds 3
if(!$p.HasExited){$p.Kill()}
Remove-Item Env:LPDF_PRINT_TO
$size=if(Test-Path $printed){(Get-Item $printed).Length}else{0}
$header=if($size -gt 8){[System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($printed)[0..4])}else{''}
$pages=if($size -gt 0){([regex]::Matches([System.Text.Encoding]::ASCII.GetString([System.IO.File]::ReadAllBytes($printed)),'/Type\s*/Page[^s]')).Count}else{0}
$results+=[pscustomobject]@{check='print to PDF file';ok=($header -eq '%PDF-' -and $pages -eq 3);detail="bytes=$size pages=$pages"}
# 5) 单实例：第二次启动把文件交给第一个窗口后立即退出。
$store=Join-Path $Output 'recent-readonly.txt';'lumen-recent 2' | Set-Content $store
$env:LPDF_RECENT_STORE=$store;$env:LPDF_NO_DEFAULT_PROMPT='1';Remove-Item Env:LPDF_SMOKE_MODE,Env:LPDF_SMOKE_SIZE,Env:LPDF_SMOKE_TIMEOUT -ErrorAction SilentlyContinue
$first=Start-Process $Exe -PassThru
try{
 Start-Sleep -Seconds 4
 $second=Start-Process $Exe -ArgumentList @(([char]34+$source+[char]34)) -PassThru
 $exited=$second.WaitForExit(8000)
 Start-Sleep -Seconds 3;$first.Refresh()
 Shot $first.MainWindowHandle 'reader-4-forwarded.png'
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($first.MainWindowHandle)
 $name=$null;if($first.MainWindowTitle -like 'links-copy.pdf*LumenPDF'){$name=$first.MainWindowTitle}
 $results+=[pscustomobject]@{check='second launch forwards file to first window';ok=($exited -and $second.ExitCode -eq 0 -and $name -ne $null -and !$first.HasExited);detail="second exited=$exited code=$($second.ExitCode) first alive=$(!$first.HasExited) opened=$($name -ne $null)"}
}finally{if(!$first.HasExited){$first.Kill()};[void]$first.WaitForExit(5000)}
Start-Sleep -Milliseconds 500
# 6) 右键“使用 LumenPDF 合并”：同时启动的多个 --merge 进程汇总到同一个窗口的合并列表。
$log=Join-Path $env:LOCALAPPDATA 'LumenPDF\logs\lumenpdf.log'
$before=if(Test-Path $log){(Get-Content $log -Encoding UTF8).Count}else{0}
$second2=Join-Path $Output 'merge-b.pdf';Copy-Item $source $second2 -Force
$first=Start-Process $Exe -ArgumentList @('--merge',([char]34+$source+[char]34)) -PassThru
try{
 $other=Start-Process $Exe -ArgumentList @('--merge',([char]34+$second2+[char]34)) -PassThru
 $otherExited=$other.WaitForExit(10000)
 Start-Sleep -Seconds 4;$first.Refresh()
 Shot $first.MainWindowHandle 'reader-5-merge-menu.png'
 $lines=if(Test-Path $log){Get-Content $log -Encoding UTF8 | Select-Object -Skip $before}else{@()}
 $added=($lines | Select-String -Pattern ([regex]::Escape('右键合并加入 2 个文件'))) -ne $null
 $results+=[pscustomobject]@{check='context-menu merge collects files in one window';ok=($otherExited -and $other.ExitCode -eq 0 -and $added -and !$first.HasExited);detail="second exited=$otherExited code=$($other.ExitCode) log=$added"}
 # 7) 生成合并预览 → 预览条出现 → “返回合并列表”结束预览。
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($first.MainWindowHandle)
 function Press($name){$b=Find $root $name;if($b){try{$b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke();$true}catch{$false}}else{$false}}
 $made=Press '生成预览';Start-Sleep -Seconds 5;$first.Refresh()
 $previewTitle=$first.MainWindowTitle
 Shot $first.MainWindowHandle 'reader-6-merge-preview.png'
 $bar=(Find $root '返回合并列表') -ne $null
 $closed=Press '返回合并列表';Start-Sleep -Seconds 2;$first.Refresh()
 Shot $first.MainWindowHandle 'reader-7-preview-closed.png'
 $lines=Get-Content $log -Encoding UTF8 | Select-Object -Skip $before
 $ended=($lines | Select-String -Pattern ([regex]::Escape('结束合并预览，返回合并列表'))) -ne $null
 $results+=[pscustomobject]@{check='merge preview can be ended';ok=($made -and $previewTitle -like '合并预览.pdf*' -and $bar -and $closed -and $ended -and $first.MainWindowTitle -eq 'LumenPDF');detail="preview=$made title=$previewTitle bar=$bar ended=$ended after=$($first.MainWindowTitle)"}
}finally{if(!$first.HasExited){$first.Kill()};if($other -and !$other.HasExited){$other.Kill()};Remove-Item Env:LPDF_RECENT_STORE,Env:LPDF_NO_DEFAULT_PROMPT -ErrorAction SilentlyContinue}
$results | Format-Table -AutoSize | Out-String -Width 220
if($results | Where-Object {!$_.ok}){'READER SMOKE: FAIL';exit 1}else{'READER SMOKE: PASS'}