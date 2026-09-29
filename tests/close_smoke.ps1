param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe")
$ErrorActionPreference='Stop'
Add-Type -AssemblyName UIAutomationClient,UIAutomationTypes,System.Drawing
Add-Type @"
using System; using System.Runtime.InteropServices;
public class CloseTestNative {
[DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h,uint m,IntPtr w,IntPtr l);
[DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h,IntPtr dc,uint f);
[DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h,out R r);
[DllImport("user32.dll")] public static extern IntPtr SetThreadDpiAwarenessContext(IntPtr c);
[StructLayout(LayoutKind.Sequential)] public struct R { public int left,top,right,bottom; }
}
"@
[void][CloseTestNative]::SetThreadDpiAwarenessContext([IntPtr](-4))
function Button($root,$name) {
 $condition=New-Object System.Windows.Automation.PropertyCondition([System.Windows.Automation.AutomationElement]::NameProperty,$name)
 $element=$root.FindFirst([System.Windows.Automation.TreeScope]::Descendants,$condition)
 if(!$element){throw "Missing button: $name"}
 return $element.GetCurrentPattern([System.Windows.Automation.InvokePattern]::Pattern)
}
$env:LPDF_SMOKE_CLOSE='1';$env:LPDF_SMOKE_MODE='0'
try{
 foreach($case in @('discard','cancel','save')){
  $source=Join-Path $PSScriptRoot "../build/app/test-output/close-$case.pdf"
  Copy-Item "$PSScriptRoot/../build/app/test-output/中文批注.pdf" $source
  $before=(Get-FileHash $source).Hash
  $proc=Start-Process $Exe -ArgumentList @('--smoke',([char]34+$source+[char]34)) -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 6
  $proc.Refresh()
  if($proc.HasExited){throw 'Exited before choice'}
  $module=$proc.Modules | Where-Object ModuleName -eq 'lumatext.dll'
  if(!$module){throw 'LumaText not loaded'}
  $root=[System.Windows.Automation.AutomationElement]::FromHandle($proc.MainWindowHandle)
  if($case -eq 'discard'){
   $r=New-Object CloseTestNative+R
   [void][CloseTestNative]::GetWindowRect($proc.MainWindowHandle,[ref]$r)
   $bitmap=New-Object System.Drawing.Bitmap(($r.right-$r.left),($r.bottom-$r.top))
   $g=[System.Drawing.Graphics]::FromImage($bitmap);$dc=$g.GetHdc()
   [void][CloseTestNative]::PrintWindow($proc.MainWindowHandle,$dc,2)
   $g.ReleaseHdc($dc);$bitmap.Save("$PSScriptRoot/../build/app/close-dialog.png");$g.Dispose();$bitmap.Dispose()
   (Button $root '放弃修改').Invoke()
  }elseif($case -eq 'cancel'){
   (Button $root '取消').Invoke()
   Start-Sleep -Milliseconds 600
   $proc.Refresh();if($proc.HasExited){throw 'Cancel closed app'}
   [void][CloseTestNative]::PostMessage($proc.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
   Start-Sleep -Milliseconds 600
   # Esc must dismiss the second close prompt without exiting.
   [void][CloseTestNative]::PostMessage($proc.MainWindowHandle,0x100,[IntPtr]27,[IntPtr]::Zero)
   Start-Sleep -Milliseconds 600
   $proc.Refresh();if($proc.HasExited){throw 'Esc closed app'}
   [void][CloseTestNative]::PostMessage($proc.MainWindowHandle,0x10,[IntPtr]::Zero,[IntPtr]::Zero)
   Start-Sleep -Milliseconds 600
   (Button $root '放弃修改').Invoke()
  }else{(Button $root '保存').Invoke()}
  if(!$proc.WaitForExit(2500)){throw "Choice did not close promptly: $case"}
  if($proc.ExitCode -ne 0){throw "Exit failed: $case"}
  $after=(Get-FileHash $source).Hash
  if($case -ne 'save' -and $before -ne $after){throw 'Discard changed source'}
  "PASS $case; LumaText loaded; source unchanged=$($before -eq $after)"
 }
}finally{Remove-Item Env:LPDF_SMOKE_CLOSE,Env:LPDF_SMOKE_MODE -ErrorAction SilentlyContinue}
