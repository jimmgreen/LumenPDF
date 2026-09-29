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
$source=Join-Path $artifacts 'annotation-ui.pdf'
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
$p=Start-Process $Exe -ArgumentList @(([char]34+$source+[char]34)) -PassThru
try{
 $script:h=Wait-For {$p.Refresh();if($p.MainWindowHandle -ne [IntPtr]::Zero){$p.MainWindowHandle}} 'main window'
 Start-Sleep -Seconds 2
 $dpi=[AnnotationNative]::GetDpiForWindow($h)/96.0
 [void][AnnotationNative]::SetWindowPos($h,[IntPtr]::Zero,20,20,[int](1320*$dpi),[int](860*$dpi),4)
 Invoke-Button '批注';Ready
 foreach($tool in @('便签','高亮','矩形','箭头','手绘','图片')){if(!(Find-Element $tool).Current.IsEnabled){throw "Disabled tool: $tool"}}
 Invoke-Button '矩形';Drag @(@(65,155),@(110,174),@(195,207),@(260,237)) '01-rectangle-preview';Ready
 Set-Value '批注线宽' '2.25';Set-Value '批注不透明度' '82'
 (Find-Element '填充矩形').GetCurrentPattern([System.Windows.Automation.TogglePattern]::Pattern).Toggle()
 Invoke-Button '应用修改';Ready;Shot '02-rectangle-properties'
 Invoke-Button '箭头';Set-Value '批注颜色 HEX' '#2864DC';Set-Value '批注线宽' '2';Invoke-Button '设为新建属性'
 Drag @(@(65,355),@(125,340),@(190,312),@(260,290)) '03-arrow-preview';Ready
 Drag @(@(260,290),@(275,300),@(286,312)) '04-arrow-endpoint';Ready
 Invoke-Button '手绘';Drag @(@(330,333),@(335,309),@(347,284),@(363,282),@(376,299),@(360,325),@(353,345),@(369,350),@(390,330),@(409,302),@(429,287),@(443,300),@(454,316),@(481,325)) '05-ink-preview';Ready
 Invoke-Button '高亮';Drag @(@(52,60),@(120,60),@(180,64),@(120,78)) '06-highlight-preview';Ready
 Invoke-Button '便签';Drag @(@(430,410),@(430,410))
 [void](Wait-For {Find-Element '便签编辑内容'} 'note dialog');Invoke-Button '取消';Ready
 Invoke-Button '便签';Drag @(@(465,54),@(465,54))
 [void](Wait-For {Find-Element '便签编辑内容'} 'note dialog')
 Set-Value '便签编辑内容' ("请确认高亮段落。"+[Environment]::NewLine+"第二行：多行中文便签。")
 Shot '07-note-editor';Invoke-Button '保存便签';Ready
 Invoke-Button '图片';Drag @(@(330,154),@(485,240))
 $dialog=Wait-For {$d=[AnnotationNative]::FileDialog([uint32]$p.Id);if($d -ne [IntPtr]::Zero){$d}} 'image file dialog'
 [void][AnnotationNative]::PostMessage($dialog,0x111,[IntPtr]2,[IntPtr]::Zero);[void](Wait-For { [AnnotationNative]::FileDialog([uint32]$p.Id) -eq [IntPtr]::Zero } 'image cancel');Ready
 Invoke-Button '图片';Drag @(@(330,154),@(485,240))
 $dialog=Wait-For {$d=[AnnotationNative]::FileDialog([uint32]$p.Id);if($d -ne [IntPtr]::Zero){$d}} 'image file dialog'
 $image=[IO.Path]::GetFullPath("$PSScriptRoot/fixtures/transparent.png")
 $combo=[AnnotationNative]::GetDlgItem($dialog,1148)
 $filename=[AnnotationNative]::Send($combo,0x407,[IntPtr]::Zero,[IntPtr]::Zero)
 if($filename -eq [IntPtr]::Zero){$filename=[AnnotationNative]::GetDlgItem($dialog,1152)}
 if($filename -eq [IntPtr]::Zero){throw 'Cannot locate image filename editor'}
 [void][AnnotationNative]::SendText($filename,0xC,[IntPtr]::Zero,$image)
 [void][AnnotationNative]::PostMessage($dialog,0x111,[IntPtr]1,[IntPtr]::Zero)
 [void](Wait-For { [AnnotationNative]::FileDialog([uint32]$p.Id) -eq [IntPtr]::Zero } 'image open');Ready
 Invoke-Button '图片旋转 90°';Ready;Shot '08-image-properties'
 Invoke-Button '删除批注';Ready;Invoke-Button '撤销';Ready;Invoke-Button '重做';Ready;Invoke-Button '撤销';Ready
 Shot '09-all-tools'
 [void][AnnotationNative]::SetWindowPos($h,[IntPtr]::Zero,20,20,[int](1050*$dpi),[int](720*$dpi),4)
 Start-Sleep -Milliseconds 400;Shot '10-compact-layout'
 [void][AnnotationNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 Invoke-Button '保存'
 if(!$p.WaitForExit(15000)){throw 'Save and close did not finish'}
 if($p.ExitCode -ne 0){throw "App exit code $($p.ExitCode)"}
 if((Get-FileHash $original).Hash -ne $hash){throw 'Original fixture was modified'}
 & "$PSScriptRoot/../build/app/annotation_tests.exe" --verify-ui $source
 if($LASTEXITCODE -ne 0){throw 'Saved UI annotations failed verification'}
 Write-Host 'PASS six annotation tools UI: native drawing, properties, note/image cancellation, multiline note, image rotation, delete/undo/redo, editable save and screenshots.'
}catch{
 try{
  Shot '99-failure'
  $root=[System.Windows.Automation.AutomationElement]::FromHandle($h)
  $root.FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition) | ForEach-Object {$_.Current.Name+" | "+$_.Current.IsEnabled+" | "+$_.Current.ControlType.ProgrammaticName} | Out-File (Join-Path $artifacts 'ui-failure.txt') -Encoding utf8
  $dialog=[AnnotationNative]::FileDialog([uint32]$p.Id)
  if($dialog -ne [IntPtr]::Zero){$old=$h;$h=$dialog;Shot '99-file-dialog';$h=$old}
 }catch{}
 throw
}finally{
 if(!$p.HasExited){$p.Kill();$p.WaitForExit()}
}
