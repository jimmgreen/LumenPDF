# 生成便携目录 dist/LumenPDF-<版本>-win-x64、便携 zip，以及（找到 Inno Setup 时）安装程序。
# 版本默认取自 CMakeLists.txt。用法：scripts/package.ps1 [-Version x.y.z] [-NoInstaller] [-Iscc <ISCC.exe>]
param([string]$Version='',[switch]$NoInstaller,[string]$Iscc='')
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
if(!$Version){$Version=(Select-String -LiteralPath "$root/CMakeLists.txt" -Pattern 'project\(LumenPDF VERSION ([0-9.]+)').Matches[0].Groups[1].Value}
$package=Join-Path $root "dist/LumenPDF-$Version-win-x64"
if(Test-Path -LiteralPath $package){Remove-Item -LiteralPath $package -Recurse -Force}
New-Item -ItemType Directory -Force -Path $package | Out-Null
Copy-Item -LiteralPath "$root/build/app/LumenPDF.exe","$root/build/app/lumatext.dll" -Destination $package
Copy-Item -LiteralPath "$root/build/app/licenses" -Destination $package -Recurse -Force
Copy-Item -LiteralPath "$root/README.md","$root/LICENSE" -Destination $package
$vswhere=Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs=& $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio runtime source not found'}
$crt=Get-ChildItem -LiteralPath "$vs/VC/Redist/MSVC" -Directory | Where-Object Name -Match '^14\.' | Sort-Object Name -Descending | Select-Object -First 1
$crtDir=Get-ChildItem -Path "$($crt.FullName)/x64/Microsoft.VC*.CRT" -Directory | Select-Object -First 1
if(!$crtDir){throw 'C++ x64 redistributable runtime not found'}
Get-ChildItem -LiteralPath $crtDir.FullName -Filter '*.dll' | Copy-Item -Destination $package
$notices=Join-Path $package 'notices'
New-Item -ItemType Directory -Force -Path $notices | Out-Null
$lumenLicense=if(Test-Path -LiteralPath "$root/third_party/LUMENUI/LICENSE"){"$root/third_party/LUMENUI/LICENSE"}else{"$root/../LUMENUI/LICENSE"}
if(Test-Path -LiteralPath $lumenLicense){Copy-Item -LiteralPath $lumenLicense -Destination "$notices/LUMEN-MIT.txt"}else{Write-Warning "LUMENUI 缺少 LICENSE 文件，便携包未包含 LUMEN 许可声明"}
$lumenRoot=Split-Path $lumenLicense -Parent
$paper=Join-Path $lumenRoot 'third_party/paper-shaders'
if(Test-Path -LiteralPath "$paper/LICENSE"){New-Item -ItemType Directory -Force -Path "$notices/PaperShaders" | Out-Null;Copy-Item -LiteralPath "$paper/LICENSE","$paper/NOTICE" -Destination "$notices/PaperShaders" -ErrorAction SilentlyContinue}
$mu=Join-Path $root 'build/deps/mupdf-1.28.4-source'
$licenseFiles=Get-ChildItem -LiteralPath $mu -Recurse -File | Where-Object { $_.Name -match '^(LICENSE|COPYING|NOTICE|OFL|copyright)([._-]|$)' }
foreach($file in $licenseFiles){
    $relative=$file.FullName.Substring($mu.Length).TrimStart('\','/')
    $target=Join-Path "$notices/MuPDF" $relative
    New-Item -ItemType Directory -Force -Path (Split-Path $target -Parent) | Out-Null
    Copy-Item -LiteralPath $file.FullName -Destination $target
}
$files=Get-ChildItem -LiteralPath $package -Recurse -File
$bytes=($files | Measure-Object Length -Sum).Sum
$report=[ordered]@{version=$Version;platform='Windows x64';files=$files.Count;bytes=$bytes;MiB=[math]::Round($bytes/1MB,2);externalOptional=@('Microsoft Word','LibreOffice');exeSha256=(Get-FileHash "$package/LumenPDF.exe" -Algorithm SHA256).Hash}
$report | ConvertTo-Json | Set-Content -LiteralPath "$root/dist/package-report.json" -Encoding utf8
$report | ConvertTo-Json
# 便携 zip
$zip=Join-Path $root "dist/LumenPDF-$Version-portable-x64.zip"
if(Test-Path -LiteralPath $zip){Remove-Item -LiteralPath $zip -Force}
Compress-Archive -Path "$package/*" -DestinationPath $zip
"PORTABLE $zip $((Get-Item -LiteralPath $zip).Length)"
# Inno Setup 安装程序（按当前用户安装，无需管理员）
if($NoInstaller){exit 0}
if(!$Iscc){
    foreach($candidate in "$env:LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe","${env:ProgramFiles(x86)}/Inno Setup 6/ISCC.exe","$env:ProgramFiles/Inno Setup 6/ISCC.exe"){
        if(Test-Path -LiteralPath $candidate){$Iscc=$candidate;break}
    }
}
if(!$Iscc){Write-Warning '未找到 Inno Setup 6（ISCC.exe），只生成便携包';exit 0}
& $Iscc /Q "/DAppVersion=$Version" "/DSourceDir=$package" "/DOutputDir=$(Join-Path $root 'dist')" (Join-Path $root 'installer/LumenPDF.iss')
if($LASTEXITCODE){throw "Inno Setup 编译失败：$LASTEXITCODE"}
$setup=Join-Path $root "dist/LumenPDF-$Version-setup.exe"
"SETUP $setup $((Get-Item -LiteralPath $setup).Length)"
