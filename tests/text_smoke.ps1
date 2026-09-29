param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[ValidateSet('full','cancel','commit','unchanged')][string]$Case='full')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class InlineNative {
 public delegate bool EnumProc(IntPtr h,IntPtr l);
 [StructLayout(LayoutKind.Sequential)]public struct P {public int x,y;}
 [StructLayout(LayoutKind.Sequential)]public struct R {public int left,top,right,bottom;}
 [DllImport("user32.dll")]public static extern bool EnumWindows(EnumProc fn,IntPtr l);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]public static extern IntPtr ReadText(IntPtr h,uint m,IntPtr cap,StringBuilder s);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool ScreenToClient(IntPtr h,ref P p);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr after,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 public static IntPtr Find(uint pid,string cls,string title){
  IntPtr found=IntPtr.Zero;EnumWindows((h,l)=>{uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid||!IsWindowVisible(h))return true;
   var s=new StringBuilder(256);GetClassName(h,s,256);if(!String.IsNullOrEmpty(cls)&&s.ToString()!=cls)return true;
   if(!String.IsNullOrEmpty(title)){s.Clear();ReadText(h,0xD,(IntPtr)256,s);if(s.ToString()!=title)return true;}
   found=h;return false;},IntPtr.Zero);return found;
 }
}
"@
[void][InlineNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$artifacts=[IO.Path]::GetFullPath("$PSScriptRoot/../build/app/inline-luma-output")
[void](New-Item -ItemType Directory -Force -Path $artifacts)
$fixture=[IO.Path]::GetFullPath("$PSScriptRoot/../build/app/text-output/text-auto-fixture.pdf")
$source=Join-Path $artifacts ("inline-"+$Case+'.pdf')
Copy-Item -LiteralPath $fixture -Destination $source -Force
$originalHash=(Get-FileHash -LiteralPath $fixture).Hash
$sourceHash=(Get-FileHash -LiteralPath $source).Hash
$checks=0;$p=$null;$h=[IntPtr]::Zero;$bar=[IntPtr]::Zero
function Check($value,$label){$script:checks++;if(!$value){throw $label}}
function Wait-For($probe,$label){
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while($clock.Elapsed.TotalSeconds -lt 16){
  $result=& $probe;if($result){return $result}
  if($script:p -and $script:p.HasExited){throw "Application exited while waiting for $label"}
  Start-Sleep -Milliseconds 80
 }
 throw "Timeout: $label"
}
function Find-Element($name,$window=$script:h){
 if($window -eq [IntPtr]::Zero){return $null}
 $root=[System.Windows.Automation.AutomationElement]::FromHandle($window)
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 $all=$root.FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition)
 foreach($e in $all){$r=$e.Current.BoundingRectangle;if($e.Current.IsEnabled -and !$e.Current.IsOffscreen -and $r.Width -gt 1 -and $r.Height -gt 1){return $e}}
 return $null
}
function Invoke-Button($name,$window=$script:h){
 $element=Wait-For {Find-Element $name $window} $name
 $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
 Start-Sleep -Milliseconds 150
}
function Value($name,$window=$script:h){return (Find-Element $name $window).GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value}
function Set-Value($name,$value,$window=$script:h){
 $e=Find-Element $name $window;$e.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue($value)
 Start-Sleep -Milliseconds 180
}
function Editor {return Find-Element '页内文字编辑器'}
function Bar {return [InlineNative]::Find([uint32]$script:p.Id,$null,'文字格式 · 整条批注')}
function Page-Point($x,$y){
 $r=(Find-Element 'PDF 文档').Current.BoundingRectangle
 $scale=[Math]::Min(($r.Width-52*$script:dpi)/595.28,($r.Height-40*$script:dpi)/841.89)
 $point=New-Object InlineNative+P;$point.x=[int][Math]::Round($r.X+($r.Width-595.28*$scale)/2+$x*$scale)
 $point.y=[int][Math]::Round($r.Y+16*$script:dpi+$y*$scale)
 [void][InlineNative]::ScreenToClient($script:h,[ref]$point)
 return [IntPtr](($point.y -shl 16)-bor ($point.x -band 65535))
}
function Shot($name){
 [void][InlineNative]::SetWindowPos($script:h,[IntPtr](-1),0,0,0,0,0x13)
 if($script:bar -ne [IntPtr]::Zero){[void][InlineNative]::SetWindowPos($script:bar,[IntPtr](-1),0,0,0,0,0x13)}
 Start-Sleep -Milliseconds 300
 $r=New-Object InlineNative+R;[void][InlineNative]::GetWindowRect($script:h,[ref]$r)
 $bitmap=New-Object System.Drawing.Bitmap(($r.right-$r.left),($r.bottom-$r.top))
 $g=[System.Drawing.Graphics]::FromImage($bitmap);$g.CopyFromScreen($r.left,$r.top,0,0,$bitmap.Size)
 $bitmap.Save((Join-Path $artifacts ($name+'.png')));$g.Dispose();$bitmap.Dispose()
 if($script:bar -ne [IntPtr]::Zero){[void][InlineNative]::SetWindowPos($script:bar,[IntPtr](-2),0,0,0,0,0x13)}
 [void][InlineNative]::SetWindowPos($script:h,[IntPtr](-2),0,0,0,0,0x13)
}
$env:LPDF_SMOKE_MODE='1';$env:LPDF_SMOKE_SIZE='1320x860';$env:LPDF_SMOKE_TIMEOUT='90'
try{
 $p=Start-Process $Exe -ArgumentList @('--smoke',('"'+$source+'"')) -PassThru
 $retainedHandle=$p.Handle
 $h=Wait-For {$p.Refresh();if($p.MainWindowHandle -ne [IntPtr]::Zero){$p.MainWindowHandle}} 'application'
 $dpi=[InlineNative]::GetDpiForWindow($h)/96.0
 [void][InlineNative]::SetWindowPos($h,[IntPtr]::Zero,20,20,[int](1320*$dpi),[int](860*$dpi),0x14)
 Start-Sleep -Seconds 4
 [void](Wait-For {Find-Element '编辑文字'} 'annotation selection')
 Check (!(Editor)) 'Text entered edit mode without a double click'
 $point=Page-Point 70 255
 # Targeted process messages exercise the real window/control route without
 # moving the user's cursor, changing global keyboard state or using SendInput.
 [void][InlineNative]::PostMessage($h,0x201,[IntPtr]1,$point)
 [void][InlineNative]::PostMessage($h,0x202,[IntPtr]::Zero,$point)
 Start-Sleep -Milliseconds 200
 Check (!(Editor)) 'Single click should select text, not enter editing'
 if($Case -eq 'full'){Shot '01-short-text-selected'}
 [void][InlineNative]::PostMessage($h,0x203,[IntPtr]1,$point)
 $field=Wait-For {Editor} 'double-click page editor'
 $bar=Wait-For {$w=Bar;if($w -ne [IntPtr]::Zero){$w}} 'format window'
 Check ($field.Current.ControlType -eq [System.Windows.Automation.ControlType]::Edit) 'Page editor is not a LUMEN edit control'
 Check ([InlineNative]::Find([uint32]$p.Id,'RICHEDIT50W',$null) -eq [IntPtr]::Zero) 'A RichEdit window is still rendering the draft'
 Check ((Value '页内文字编辑器') -eq '水电费') 'Double click selected the wrong text'
 Check ([double](Value '字号' $bar) -eq 12) 'Original 12pt size was silently changed'
 $initial=$field.Current.BoundingRectangle
 Check ($initial.Width -lt 180*$dpi -and $initial.Height -lt 60*$dpi) 'Short text still has an oversized editing frame'
 if($Case -eq 'full'){
  Shot '02-lumatext-inline-fit'
  Invoke-Button '页面实际大小'
  [void](Wait-For {Find-Element '页面缩放 100%'} 'actual 100 percent zoom label')
  $actual=(Editor).Current.BoundingRectangle
  Check ($actual.Width -gt $initial.Width*1.1) '100% view did not increase physical page scale'
  Check ([double](Value '字号' $bar) -eq 12) 'Zoom changed PDF font size'
  Check ($actual.Height/$dpi -gt 22 -and $actual.Height/$dpi -lt 30) '12pt actual-size line metrics are wrong'
  Shot '03-12pt-at-100-percent'
  Set-Value '页内文字编辑器' '水电费与建筑设计说明'
  $long=(Editor).Current.BoundingRectangle
  Check ($long.Width -gt $actual.Width*1.8) 'Typing more characters did not grow the text width'
  Set-Value '页内文字编辑器' "水电费`n第二行"
  $multi=(Editor).Current.BoundingRectangle
  Check ($multi.Height -gt $actual.Height*1.4) 'Hard newline did not grow the text height'
  Set-Value '页内文字编辑器' '水电费'
  $shrunk=(Editor).Current.BoundingRectangle
  Check ([Math]::Abs($shrunk.Height-$actual.Height) -lt 3 -and [Math]::Abs($shrunk.Width-$actual.Width) -lt 3) 'Deleting text did not shrink the frame back'
  Set-Value '字号' '24' $bar
  $larger=(Editor).Current.BoundingRectangle
  Check ($larger.Width -gt $shrunk.Width*1.5 -and $larger.Height -gt $shrunk.Height*1.5) 'Font size did not update the same text layout and frame'
  Set-Value '字号' '12' $bar
  Set-Value '字体' 'Arial' $bar
  Check ((Value '字体' $bar) -eq 'Arial') 'Installed font selection failed'
  Set-Value '字体' 'Microsoft YaHei' $bar
  Set-Value '页内文字编辑器' "水电费`n第二行"
  Shot '04-content-sized-two-lines'
 }elseif($Case -ne 'unchanged'){Set-Value '页内文字编辑器' "水电费`n第二行"}
 if($Case -eq 'cancel') {Invoke-Button '取消文字编辑' $bar}
 else {Invoke-Button '完成文字编辑' $bar}
 [void](Wait-For {!(Editor)} 'editor dismissal')
 Start-Sleep -Milliseconds 700
 $bar=[IntPtr]::Zero
 if($Case -eq 'full'){Shot '05-same-renderer-after-commit'}
 [void][InlineNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 if($Case -in @('full','commit')){Invoke-Button '保存'}
 if(!$p.WaitForExit(5000)){throw 'Application did not close after the test'}
 Check ($p.ExitCode -eq 0) 'Application exited abnormally'
 $savedHash=(Get-FileHash -LiteralPath $source).Hash
 if($Case -in @('cancel','unchanged')){Check ($savedHash -eq $sourceHash) 'Cancelling/unchanged editing modified the source PDF'}
 else{
  Check ($savedHash -ne $sourceHash) 'Completed text editing was not saved'
  & "$PSScriptRoot/../build/app/text_tests.exe" --verify-inline $source
  if($LASTEXITCODE -ne 0){throw 'Saved PDF failed the core verification'}
 }
 Check ((Get-FileHash -LiteralPath $fixture).Hash -eq $originalHash) 'The original fixture was modified'
 @{passed=$true;case=$Case;checks=$checks;dpi=$dpi;source=$source;originalHash=$originalHash;initialFrame=@{width=$initial.Width;height=$initial.Height};noGlobalInput=$true} |
  ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $artifacts ("result-"+$Case+'.json')) -Encoding UTF8
 Write-Host "PASS LumaText page editing $Case : $checks UI assertions; targeted events only, no global mouse/keyboard input."
}catch{
 try{if($p -and !$p.HasExited){Shot '99-inline-failure'}}catch{}
 throw
}finally{
 if($p -and !$p.HasExited){$p.Kill();$p.WaitForExit()}
 Remove-Item Env:LPDF_SMOKE_MODE,Env:LPDF_SMOKE_SIZE,Env:LPDF_SMOKE_TIMEOUT -ErrorAction SilentlyContinue
}
