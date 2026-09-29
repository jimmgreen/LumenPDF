param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[ValidateSet('export','preview','cancel','failure','word')][string]$Case='export')
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System;using System.Text;using System.Runtime.InteropServices;
public class MergeNative {
 public delegate bool E(IntPtr h,IntPtr l);
 [StructLayout(LayoutKind.Sequential)]public struct R{public int left,top,right,bottom;}
 [DllImport("user32.dll")]public static extern bool EnumWindows(E fn,IntPtr l);
 [DllImport("user32.dll")]public static extern bool IsWindowVisible(IntPtr h);
 [DllImport("user32.dll")]public static extern uint GetWindowThreadProcessId(IntPtr h,out uint id);
 [DllImport("user32.dll",CharSet=CharSet.Unicode)]public static extern int GetClassName(IntPtr h,StringBuilder s,int n);
 [DllImport("user32.dll")]public static extern IntPtr GetDlgItem(IntPtr h,int id);
 [DllImport("user32.dll")]public static extern bool EnumChildWindows(IntPtr h,E fn,IntPtr l);
 [DllImport("user32.dll")]public static extern int GetDlgCtrlID(IntPtr h);
 public static IntPtr Filename(IntPtr dialog){
  IntPtr found=GetDlgItem(dialog,1152);
  if(found!=IntPtr.Zero)return found;
  EnumChildWindows(dialog,(h,l)=>{var s=new StringBuilder(80);GetClassName(h,s,80);int id=GetDlgCtrlID(h);
   if(s.ToString()=="ComboBoxEx32"&&id==1148){found=Send(h,0x407,IntPtr.Zero,IntPtr.Zero);if(found!=IntPtr.Zero)return false;}
   if(s.ToString()=="Edit"&&(id==1152||id==1001)){found=h;return false;}return true;},IntPtr.Zero);return found;
 }
 [DllImport("user32.dll",EntryPoint="SendMessageW")]public static extern IntPtr Send(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll",EntryPoint="SendMessageW",CharSet=CharSet.Unicode)]public static extern IntPtr Text(IntPtr h,uint m,IntPtr w,string value);
 [DllImport("user32.dll")]public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
 [DllImport("user32.dll")]public static extern bool SetWindowPos(IntPtr h,IntPtr z,int x,int y,int w,int height,uint flags);
 [DllImport("user32.dll")]public static extern bool GetWindowRect(IntPtr h,out R r);
 [DllImport("user32.dll")]public static extern uint GetDpiForWindow(IntPtr h);
 [DllImport("user32.dll")]public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
 public static IntPtr Dialog(uint pid){IntPtr found=IntPtr.Zero;EnumWindows((h,l)=>{uint owner;GetWindowThreadProcessId(h,out owner);if(owner!=pid||!IsWindowVisible(h))return true;var s=new StringBuilder(80);GetClassName(h,s,80);if(s.ToString()=="#32770"){found=h;return false;}return true;},IntPtr.Zero);return found;}
}
"@
[void][MergeNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
$artifacts=[IO.Path]::GetFullPath("$PSScriptRoot/../build/app/merge-output")
$outFolder=Join-Path $artifacts 'ui';[void](New-Item -ItemType Directory -Force -Path $outFolder)
$checks=0;$p=$null;$h=[IntPtr]::Zero;$trace=New-Object Collections.Generic.List[string]
function Check($ok,$message){$script:checks++;if(!$ok){throw $message}}
function Root {return [System.Windows.Automation.AutomationElement]::FromHandle($script:h)}
function Elements {return (Root).FindAll([System.Windows.Automation.TreeScope]::Descendants,[System.Windows.Automation.Condition]::TrueCondition)}
function Find($name,$enabled=$true){
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 foreach($e in (Root).FindAll([System.Windows.Automation.TreeScope]::Descendants,$condition)){
  $r=$e.Current.BoundingRectangle;if(!$e.Current.IsOffscreen -and $r.Width -gt 1 -and (!$enabled -or $e.Current.IsEnabled)){return $e}
 };return $null
}
function Wait-For($probe,$name,$seconds=35){
 $clock=[Diagnostics.Stopwatch]::StartNew()
 while($clock.Elapsed.TotalSeconds -lt $seconds){$value=& $probe;if($value){return $value};if($script:p -and $script:p.HasExited){throw "Application exited while waiting for $name"};Start-Sleep -Milliseconds 70}
 throw "Timeout: $name"
}
function Invoke($name){$e=Wait-For {Find $name} $name;$e.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke();Start-Sleep -Milliseconds 100}
function Post-Button($name){$e=Wait-For {Find $name} $name;$e.SetFocus();[void][MergeNative]::PostMessage($script:h,0x100,[IntPtr]32,[IntPtr]::Zero)}
function File-Dialog($value){
 $d=Wait-For {$w=[MergeNative]::Dialog([uint32]$script:p.Id);if($w -ne [IntPtr]::Zero){$w}} 'file dialog'
 $edit=Wait-For {$w=[MergeNative]::Filename($d);if($w -ne [IntPtr]::Zero){$w}} 'owned filename field'
 [void][MergeNative]::Text($edit,0xC,[IntPtr]::Zero,$value)
 [void][MergeNative]::PostMessage($d,0x111,[IntPtr]1,[IntPtr]::Zero)
 [void](Wait-For {[MergeNative]::Dialog([uint32]$script:p.Id) -eq [IntPtr]::Zero} 'file dialog dismissal')
}
function Add-Files($files){Post-Button '添加文件';File-Dialog (($files|ForEach-Object {'"'+$_+'"'}) -join ' ');[void](Wait-For {Find '生成预览'} 'queue ready')}
function Row-Value($list){return $list.GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).Current.Value}
function Select-Index($index){
 $list=Find '合并文件队列';$list.SetFocus()
 [void][MergeNative]::Send($script:h,0x100,[IntPtr]36,[IntPtr]::Zero)
 for($i=0;$i -lt $index;$i++){[void][MergeNative]::Send($script:h,0x100,[IntPtr]40,[IntPtr]::Zero)}
 Start-Sleep -Milliseconds 60
 return $list
}
function Rows {
 # This virtualized ListView exposes the selected row through ValuePattern,
 # rather than creating one HWND/UIA child for every backing data row.
 $values=@()
 for($i=0;$i -lt $script:files.Count;$i++){$list=Select-Index $i;$values+=,(Row-Value $list)}
 return $values
}
function Select-Row($needle){
 for($i=0;$i -lt $script:files.Count;$i++){$list=Select-Index $i;if((Row-Value $list).Contains($needle)){return}}
 throw ('Cannot select queue row '+$needle)
}
function Snapshot($name){
 [void][MergeNative]::SetWindowPos($script:h,[IntPtr](-1),0,0,0,0,0x13);Start-Sleep -Milliseconds 180
 $r=New-Object MergeNative+R;[void][MergeNative]::GetWindowRect($script:h,[ref]$r)
 $b=New-Object Drawing.Bitmap(($r.right-$r.left),($r.bottom-$r.top));$g=[Drawing.Graphics]::FromImage($b);$g.CopyFromScreen($r.left,$r.top,0,0,$b.Size)
 $b.Save((Join-Path $outFolder ($Case+'-'+$name+'.png')));$g.Dispose();$b.Dispose()
 [void][MergeNative]::SetWindowPos($script:h,[IntPtr](-2),0,0,0,0,0x13)
}
$names=if($Case -eq 'cancel'){@('long-conversion.txt')}elseif($Case -eq 'failure'){@('invalid-encoding.txt')}elseif($Case -eq 'word'){@('notes.TXT','work-report.docx','three-pages.pdf','site-photo.png')}else{@('notes.TXT','three-pages.pdf','site-photo.png')}
$files=@($names|ForEach-Object {Join-Path $artifacts $_});$hashes=@{};foreach($file in $files){$hashes[$file]=(Get-FileHash -LiteralPath $file).Hash}
if($Case -eq 'word' -and @(Get-Process WINWORD -ErrorAction SilentlyContinue).Count){throw 'Skipping Office UI check: an existing interactive Word process is running'}
$output=Join-Path $outFolder ($Case+'-'+[guid]::NewGuid().ToString('N').Substring(0,8)+'.pdf')
$env:LPDF_SMOKE_MODE='3';$env:LPDF_SMOKE_TIMEOUT='120';$env:LPDF_SMOKE_SIZE='1320x860'
try{
 $p=Start-Process $Exe -ArgumentList '--smoke' -PassThru;$handle=$p.Handle
 $h=Wait-For {$p.Refresh();if($p.MainWindowHandle -ne [IntPtr]::Zero){$p.MainWindowHandle}} 'main window'
 Start-Sleep -Seconds 4;$dpi=[MergeNative]::GetDpiForWindow($h)/96.0
 [void][MergeNative]::SetWindowPos($h,[IntPtr]::Zero,20,20,[int](1320*$dpi),[int](860*$dpi),0x14)
 Add-Files $files
 $rows=Rows;Check ($rows.Count -eq $files.Count -and @($rows|Select-Object -Unique).Count -eq $files.Count) 'Queue did not contain each input exactly once'
 if($Case -in @('export','preview','word')){
  Check (($rows|Where-Object {$_ -like '*PDF*'}).Count -gt 0) 'PDF kind/status missing in accessible row'
  Check (($rows|Where-Object {$_ -like '*文本*'}).Count -gt 0) 'Text kind/status missing'
  Check (($rows|Where-Object {$_ -like '*图片*'}).Count -gt 0) 'Image kind/status missing'
  if($Case -eq 'word'){Check (($rows|Where-Object {$_ -like '*Word*'}).Count -gt 0) 'Word kind missing'}
  Select-Row 'three-pages.pdf';(Find '合并文件页码范围').GetCurrentPattern([System.Windows.Automation.ValuePattern]::Pattern).SetValue('3,1')
  # Selecting another row commits programmatic/UIA drafts too.
  Select-Row 'notes.TXT';Invoke '下移';Invoke '上移'
  Check (@(Rows)[0].Contains('notes.TXT')) 'Moving a queue item changed its identity or order'
  Check ((@(Rows|Where-Object {$_.Contains('three-pages.pdf')})[0].Contains('3,1'))) 'Page range was lost on selection or reorder'
 }
 Snapshot 'queue'
 if($Case -in @('export','word')){Post-Button '合并并导出';File-Dialog ('"'+$output+'"')}
 else{Invoke '生成预览'}
 if($Case -eq 'cancel'){
  [void](Wait-For {foreach($e in (Elements)){if($e.Current.Name -match '^合并进度：文本分页'){return $e}}} 'active text conversion')
  $cancel=Wait-For {Find '取消转换与合并'} 'enabled in-panel cancel'
  Check ($cancel.Current.IsEnabled) 'The progress cancel action was disabled with queue controls'
  Snapshot 'converting'
  $cancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()
  [void](Wait-For {Find '任务已取消'} 'confirmed cancellation')
  Check ((Find '生成预览').Current.IsEnabled) 'Cancellation did not restore the queue actions'
  Snapshot 'cancelled'
 }elseif($Case -eq 'failure'){
  [void](Wait-For {Find '处理失败'} 'failed task state')
  Snapshot 'failure-dialog'
  $all=Elements;$ok=$all|Where-Object {$_.Current.ControlType -eq [System.Windows.Automation.ControlType]::Button -and $_.Current.IsEnabled -and !$_.Current.IsOffscreen -and $_.Current.Name -in @('确定','知道了','OK')}|Select-Object -First 1
  if($ok){$ok.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke()}
  else{[void][MergeNative]::PostMessage($h,0x100,[IntPtr]27,[IntPtr]::Zero);Start-Sleep -Milliseconds 180}
  [void](Wait-For {Find '处理失败'} 'failure status')
  Check ((@(Rows)[0].Contains('处理失败'))) 'The failed file was not identified in the queue'
  Snapshot 'failed'
 }else{
  $watch=[Diagnostics.Stopwatch]::StartNew();$captured=$false
  while($watch.Elapsed.TotalSeconds -lt 85){
   foreach($e in (Elements)){
    $text=$e.Current.Name
    if($text -match '^[123] / 3|^合并进度：'){if(!$trace.Contains($text)){$trace.Add($text)}}
   }
   $activeCancel=Find '取消转换与合并'
   if(!$captured -and $activeCancel){Snapshot 'working';$captured=$true}
   if($Case -eq 'preview'){
    if(Find '尚未打开 PDF' $false){Start-Sleep -Milliseconds 80;continue}
    if(!(Find '取消转换与合并') -and (Find 'PDF 文档')){break}
   }elseif(Find '合并导出完成'){break}
   Start-Sleep -Milliseconds 80
  }
  if($Case -eq 'preview'){
   Check ($null -ne (Find 'PDF 文档')) 'Preview did not open the result document'
   Invoke '合并';[void](Wait-For {Find '合并预览已就绪'} 'persistent preview result')
  }else{
   [void](Wait-For {Find '合并导出完成'} 'successful atomic export')
   Check (Test-Path -LiteralPath $output) 'Success was shown without a saved output file'
   if($Case -eq 'export'){& "$PSScriptRoot/../build/app/merge_tests.exe" --verify-merged $output;if($LASTEXITCODE -ne 0){throw 'Saved merge failed verification'}}
  }
  Check (((Rows)|Where-Object {$_.Contains('已合并')}).Count -eq $files.Count) 'Completed rows did not retain their per-file result'
  Check ($null -eq (Find '取消转换与合并')) 'Cancel remained active after completion'
  Snapshot 'complete'
  [void][MergeNative]::SetWindowPos($h,[IntPtr]::Zero,20,20,[int](1050*$dpi),[int](720*$dpi),0x14)
  Start-Sleep -Milliseconds 250;Snapshot 'compact'
 }
 foreach($file in $files){Check ((Get-FileHash -LiteralPath $file).Hash -eq $hashes[$file]) 'An original input file was modified'}
 [void][MergeNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
 if(!$p.WaitForExit(5000)){throw 'Application did not close after merge UI test'}
 Check ($p.ExitCode -eq 0) 'Application exited abnormally'
 @{passed=$true;case=$Case;checks=$checks;trace=@($trace);output=$output;dpi=$dpi} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $outFolder ($Case+'-result.json')) -Encoding UTF8
 Write-Host "PASS merge UI $Case : $checks assertions"
}catch{
 @{message=$_.Exception.Message;pid=$(if($p){$p.Id}else{0});hasExited=$(if($p){$p.HasExited}else{$true});exitCode=$(if($p -and $p.HasExited){$p.ExitCode}else{$null})}|ConvertTo-Json|Set-Content -LiteralPath (Join-Path $outFolder ($Case+'-error.json')) -Encoding utf8
 try{if($p -and !$p.HasExited){Snapshot 'failure-debug';Elements|ForEach-Object {$_.Current.Name+' | '+$_.Current.IsEnabled+' | '+$_.Current.ControlType.ProgrammaticName}|Out-File (Join-Path $outFolder ($Case+'-failure.txt')) -Encoding utf8}}catch{};throw}
finally{
 if($p -and !$p.HasExited){
  try{$cancel=Find '取消转换与合并';if($cancel){$cancel.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern).Invoke();[void](Wait-For {Find '任务已取消'} 'test cleanup cancellation' 12)}}catch{}
  [void][MergeNative]::PostMessage($h,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
  if(!$p.WaitForExit(4000)){$p.Kill();$p.WaitForExit()}
 }
 Remove-Item Env:LPDF_SMOKE_MODE,Env:LPDF_SMOKE_TIMEOUT,Env:LPDF_SMOKE_SIZE -ErrorAction SilentlyContinue
}
