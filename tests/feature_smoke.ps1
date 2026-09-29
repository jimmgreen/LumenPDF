param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Runtime.InteropServices;using System.Text;
public class AnnotationNative {
 [StructLayout(LayoutKind.Sequential)] public struct P {public int x,y;}
 [StructLayout(LayoutKind.Sequential)] public struct R {public int left,top,right,bottom;}
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc callback,IntPtr l);
 [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h,out uint id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")] public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)] public static extern bool SetDlgItemText(IntPtr h,int id,string value);
 [DllImport("user32.dll")] public static extern bool ScreenToClient(IntPtr h,ref P p);
 [DllImport("user32.dll",EntryPoint="SendMessageW")] public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)] public static extern IntPtr SendText(IntPtr h,uint m,IntPtr w,string text);
 [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
 [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")] public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 public static IntPtr FileDialog(uint process) {
  IntPtr found=IntPtr.Zero;
  EnumWindows((h,l)=>{uint owner;GetWindowThreadProcessId(h,out owner);var name=new StringBuilder(128);GetClassName(h,name,128);if(owner==process&&name.ToString()=="#32770"){found=h;return false;}return true;},IntPtr.Zero);
  return found;
 }
}
"@
[void][AnnotationNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$artifacts=[IO.Path]::GetFullPath("$PSScriptRoot/../build/app/annotation-output")
New-Item -ItemType Directory -Force $artifacts | Out-Null
$source=Join-Path $artifacts 'feature-ui.pdf'
$original=Join-Path $artifacts 'annotation-source.pdf'
Copy-Item $original $source -Force
$hash=(Get-FileHash $original).Hash
function Find-Element($name){
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($script:h)
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 return $root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
}
function Wait-For($fn,$label){
 $until=[DateTime]::UtcNow.AddSeconds(15)
 do{try{$value=& $fn;if($value){return $value}}catch{};Start-Sleep -Milliseconds 100}while([DateTime]::UtcNow -lt $until)
 throw "Timeout: $label"
}
function Invoke-Button($name){
 $b=Wait-For { $item=Find-Element $name;if($item -and $item.Current.IsEnabled){$item} } $name
 $b.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
 Start-Sleep -Milliseconds 180
}
function Ready { [void](Wait-For { $b=Find-Element '矩形';$b -and $b.Current.IsEnabled } 'annotation operation');Start-Sleep -Milliseconds 250 }
function Set-Value($name,$value){
 $e=Find-Element $name;if(!$e){throw "Missing field: $name"}
 $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue([string]$value)
}
function Shot($name){
 Start-Sleep -Milliseconds 180
 $r=New-Object AnnotationNative+R;[void][AnnotationNative]::GetWindowRect($script:h,[ref]$r)
 $bitmap=New-Object System.Drawing.Bitmap(($r.right-$r.left),($r.bottom-$r.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
 [void][AnnotationNative]::PrintWindow($script:h,$dc,2);$g.ReleaseHdc($dc)
 $bitmap.Save((Join-Path $artifacts ($name+'.png')));$g.Dispose();$bitmap.Dispose()
}
function Position($x,$y){
 $canvas=Find-Element 'PDF 文档';if(!$canvas){throw 'Missing canvas'}
 $r=$canvas.Current.BoundingRectangle;$dpi=[AnnotationNative]::GetDpiForWindow($script:h)/96.0
 $scale=[Math]::Min(($r.Width-52*$dpi)/595.28,($r.Height-40*$dpi)/841.89)
 $p=New-Object AnnotationNative+P;$p.x=[int]($r.X+($r.Width-595.28*$scale)/2+$x*$scale);$p.y=[int]($r.Y+16*$dpi+$y*$scale)
 [void][AnnotationNative]::ScreenToClient($script:h,[ref]$p)
 return [IntPtr](($p.y -shl 16)-bor ($p.x -band 65535))
}
function Drag($points,$shot=''){
 $first=Position $points[0][0] $points[0][1]
 [void][AnnotationNative]::Send($script:h,0x201,[IntPtr]1,$first)
 Start-Sleep -Milliseconds 180
 foreach($point in $points){$last=Position $point[0] $point[1];[void][AnnotationNative]::Send($script:h,0x200,[IntPtr]1,$last);Start-Sleep -Milliseconds 25}
 if($shot){Shot $shot}
 [void][AnnotationNative]::PostMessage($script:h,0x202,[IntPtr]::Zero,$last)
 Start-Sleep -Milliseconds 350
}
function Find-Any($name){
 # 菜单与对话框可能是独立的弹出窗口：在桌面范围按进程查找。
 $c1=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 $c2=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::ProcessIdProperty,$p.Id)
 $windows=[System.Windows.Automation.AutomationElement]::RootElement.FindAll([System.Windows.Automation.TreeScope]::Children,$c2)
 foreach($w in $windows){$e=$w.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$c1);if($e){return $e}}
 return $null
}
function Invoke-Any($name){
 $e=Wait-For { $item=Find-Any $name;if($item -and $item.Current.IsEnabled){$item} } $name
 $pattern=$null
 if($e.TryGetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern,[ref]$pattern)){$pattern.Invoke()}
 elseif($e.TryGetCurrentPattern([System.Windows.Automation.SelectionItemPattern]::Pattern,[ref]$pattern)){$pattern.Select()}
 else{throw "Not invokable: $name"}
 Start-Sleep -Milliseconds 250
}
function Set-Any($name,$value){
 $e=Wait-For {Find-Any $name} $name
 $e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue([string]$value)
}
Add-Type @"
using System;using System.Runtime.InteropServices;using System.Text;
namespace LumenSmoke{public static class SaveBox{
 public delegate bool E(IntPtr h,IntPtr l);
 [DllImport("user32.dll")]public static extern bool EnumChildWindows(IntPtr h,E fn,IntPtr l);
 [DllImport("user32.dll")]public static extern int GetDlgCtrlID(IntPtr h);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",EntryPoint="SendMessageW")]public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 public static IntPtr Filename(IntPtr dialog){
  IntPtr found=IntPtr.Zero;
  EnumChildWindows(dialog,(h,l)=>{var s=new StringBuilder(80);GetClassName(h,s,80);int id=GetDlgCtrlID(h);
   if(s.ToString()=="ComboBoxEx32"&&id==1148){found=Send(h,0x407,IntPtr.Zero,IntPtr.Zero);if(found!=IntPtr.Zero)return false;}
   if(s.ToString()=="Edit"&&(id==1152||id==1001)){found=h;return false;}return true;},IntPtr.Zero);return found;
 }
}}
"@
function Save-Dialog($path){
 $dialog=Wait-For {$d=[AnnotationNative]::FileDialog([uint32]$p.Id);if($d -ne [IntPtr]::Zero){$d}} 'save dialog'
 Start-Sleep -Milliseconds 600
 $filename=Wait-For {$f=[LumenSmoke.SaveBox]::Filename($dialog);if($f -ne [IntPtr]::Zero){$f}} 'save filename box'
 [void][AnnotationNative]::SendText($filename,0xC,[IntPtr]::Zero,$path)
 [void][AnnotationNative]::PostMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
 [void](Wait-For { [AnnotationNative]::FileDialog([uint32]$p.Id) -eq [IntPtr]::Zero } 'save dialog closed')
}
function Click-Element($name){
 # 菜单按钮会进入嵌套消息循环，UIA Invoke 会阻塞；改为投递鼠标消息。
 $e=Wait-For { $item=Find-Element $name;if($item -and $item.Current.IsEnabled){$item} } $name
 $r=$e.Current.BoundingRectangle
 $pt=New-Object AnnotationNative+P;$pt.x=[int]($r.X+$r.Width/2);$pt.y=[int]($r.Y+$r.Height/2)
 [void][AnnotationNative]::ScreenToClient($script:h,[ref]$pt)
 $l=[IntPtr](($pt.y -shl 16)-bor ($pt.x -band 65535))
 [void][AnnotationNative]::PostMessage($script:h,0x200,[IntPtr]::Zero,$l)
 [void][AnnotationNative]::PostMessage($script:h,0x201,[IntPtr]1,$l)
 [void][AnnotationNative]::PostMessage($script:h,0x202,[IntPtr]::Zero,$l)
 Start-Sleep -Milliseconds 500
}
Add-Type -Namespace LumenSmoke -Name FeatureCursor -MemberDefinition '[DllImport("user32.dll")] public static extern bool SetCursorPos(int x,int y);'
function Menu-Item($key){
 # 弹出菜单不暴露 UIA；它的模态循环从线程队列读取字符，按助记符提交菜单项。
 [void][LumenSmoke.FeatureCursor]::SetCursorPos(0,0)
 Click-Element '更多'
 [void][AnnotationNative]::PostMessage($script:h,0x102,[IntPtr][int][char]$key,[IntPtr]::Zero)
 Start-Sleep -Milliseconds 500
}
$compressed=Join-Path $artifacts 'feature-compressed.pdf'
Remove-Item $compressed -ErrorAction SilentlyContinue
$p=Start-Process $Exe -ArgumentList @(([char]34+$source+[char]34)) -PassThru
try{
 $script:h=Wait-For {$p.Refresh();if($p.MainWindowHandle -ne [IntPtr]::Zero){$p.MainWindowHandle}} 'main window'
 Start-Sleep -Seconds 2
 $dpi=[AnnotationNative]::GetDpiForWindow($h)/96.0
 [void][AnnotationNative]::SetWindowPos($h,[IntPtr]::Zero,20,20,[int](1320*$dpi),[int](860*$dpi),4)
 Invoke-Button '批注';Ready
 foreach($tool in @('下划线','删除线','椭圆','直线','印章')){if(!(Find-Element $tool).Current.IsEnabled){throw "Disabled tool: $tool"}}
 Invoke-Button '椭圆';Drag @(@(70,160),@(150,200),@(240,240)) 'f01-ellipse-preview';Ready
 Invoke-Button '直线';Drag @(@(70,300),@(160,320),@(250,340));Ready
 Invoke-Button '印章';Drag @(@(400,300),@(400,300))
 [void](Wait-For {Find-Any '印章文字'} 'stamp dialog')
 Set-Any '印章文字' '机密';Shot 'f02-stamp-dialog';Invoke-Any '放置印章';Ready
 Shot 'f03-new-annotations'
 Menu-Item 'b'
 [void](Wait-For {Find-Any '名称'} 'bookmark dialog')
 Set-Any '名称' '冒烟书签';Invoke-Any '添加'
 [void](Wait-For {Find-Element '书签已更新  ·  Ctrl+Z 撤销'} 'bookmark committed')
 Shot 'f04-bookmark';Invoke-Button '批注';Ready
 Menu-Item 'h'
 [void](Wait-For {Find-Any '页脚文字'} 'decorate dialog')
 Set-Any '页脚文字' '冒烟页脚 {page}';Set-Any '水印文字' '内部资料';Shot 'f05-decorate-dialog'
 Invoke-Any '应用';Ready;Shot 'f06-decorated'
 Menu-Item 's'
 [void](Wait-For {Find-Element '确定'} 'single-page split refused');Shot 'f07-split-refused';Invoke-Button '确定';Start-Sleep -Milliseconds 400
 Menu-Item 'c'
 [void](Wait-For {Find-Any '压缩级别'} 'compress dialog');Shot 'f08-compress-dialog';Invoke-Any '另存副本…'
 Save-Dialog $compressed;Ready
 [void](Wait-For {Test-Path $compressed} 'compressed copy')
 Shot 'f09-compressed'
 [void][AnnotationNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 Invoke-Button '保存'
 if(!$p.WaitForExit(15000)){throw 'Save and close did not finish'}
 if($p.ExitCode -ne 0){throw "App exit code $($p.ExitCode)"}
 if((Get-FileHash $original).Hash -ne $hash){throw 'Original fixture was modified'}
 & "$PSScriptRoot/../build/app/annotation_tests.exe" --verify-features $source
 if($LASTEXITCODE -ne 0){throw 'Saved feature smoke failed verification'}
 & "$PSScriptRoot/../build/app/annotation_tests.exe" --verify-features $compressed
 if($LASTEXITCODE -ne 0){throw 'Compressed copy must include the unsaved edits and stay editable'}
 Write-Host 'PASS feature UI: ellipse, line, stamp dialog, bookmark add, header/footer + watermark dialog, split dialog, compress to copy, save and verify.'
}catch{
 try{
  Shot '99-feature-failure'
  $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
  $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object {$_.Current.Name+" | "+$_.Current.IsEnabled+" | "+$_.Current.ControlType.ProgrammaticName} | Out-File (Join-Path $artifacts 'feature-failure.txt') -Encoding utf8
 }catch{}
 throw
}finally{if(!$p.HasExited){$p.Kill()}}
