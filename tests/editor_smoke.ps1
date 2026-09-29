param([string]$Exe="$PSScriptRoot/../build/app/LumenPDF.exe",[ValidateSet('cancel','commit','unchanged')][string]$Case='cancel')
# The editor is a LUMEN control now, not a RICHEDIT50W child window.
& "$PSScriptRoot/text_smoke.ps1" -Exe $Exe -Case $Case
if(!$?){exit 1}

