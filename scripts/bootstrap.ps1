param([switch]$SkipDownload)
$ErrorActionPreference = 'Stop'
$project = Split-Path $PSScriptRoot -Parent
$deps = Join-Path $project 'build/deps'
$source = Join-Path $deps 'mupdf-1.28.4-source'
$archive = Join-Path $deps 'mupdf-1.28.4-source.tar.gz'
New-Item -ItemType Directory -Force $deps | Out-Null
if (!(Test-Path $archive)) {
    if ($SkipDownload) { throw 'MuPDF source archive missing.' }
    Invoke-WebRequest 'https://github.com/ArtifexSoftware/mupdf-downloads/releases/download/1.28.4/mupdf-1.28.4-source.tar.gz' -OutFile $archive
}
if ((Get-FileHash $archive -Algorithm SHA256).Hash -ne '2D97E043A616F96B148657C9C3D81AD71C4BD2052C59A2A3315AD842599340F9') { throw 'MuPDF source checksum mismatch.' }
if (!(Test-Path "$source/include/mupdf/fitz.h")) {
    & tar -xzf $archive -C $deps --exclude='*/thirdparty/freeglut/progs/test-shapes-gles1/android_toolchain.cmake' --exclude='*/thirdparty/zxing-cpp/wrappers/python/core' --exclude='*/thirdparty/zxing-cpp/wrappers/python/zxing.cmake' --exclude='*/thirdparty/zxing-cpp/wrappers/rust/core'
    if ($LASTEXITCODE) { throw 'MuPDF extraction failed.' }
}
$projectFile = Join-Path $source 'platform/win32/libmupdf.vcxproj'
$xml = [IO.File]::ReadAllText($projectFile)
$originalXml = $xml
foreach ($name in @('libtesseract','libextract','libmubarcode')) {
    $xml = [regex]::Replace($xml, '(?s)\s*<ProjectReference Include="' + $name + '\.vcxproj">.*?</ProjectReference>', '')
}
$xml = $xml.Replace('HAVE_TESSERACT;', '').Replace('HAVE_LEPTONICA;', '')
$flags = 'LUMEN_PDF_BUILD;FZ_ENABLE_JS=0;FZ_ENABLE_BARCODE=0;FZ_ENABLE_OCR_OUTPUT=0;FZ_ENABLE_DOCX_OUTPUT=0;FZ_ENABLE_ODT_OUTPUT=0;FZ_ENABLE_XPS=0;FZ_ENABLE_SVG=0;FZ_ENABLE_CBZ=0;FZ_ENABLE_IMG=0;FZ_ENABLE_HTML=0;FZ_ENABLE_FB2=0;FZ_ENABLE_MOBI=0;FZ_ENABLE_EPUB=0;FZ_ENABLE_OFFICE=0;FZ_ENABLE_TXT=0;FZ_ENABLE_HTML_ENGINE=1;TOFU;'
if (!$xml.Contains('LUMEN_PDF_BUILD;')) { $xml = $xml.Replace('<PreprocessorDefinitions>', '<PreprocessorDefinitions>' + $flags) }
if ($xml -ne $originalXml) { [IO.File]::WriteAllText($projectFile, $xml) }
$vswhere = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles(x86)')) 'Microsoft Visual Studio/Installer/vswhere.exe'
$vs = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (!$vs) { throw 'Visual Studio C++ tools are required.' }
$msbuild = Join-Path $vs 'MSBuild/Current/Bin/MSBuild.exe'
$toolset = if (Test-Path "$vs/VC/Tools/MSVC/14.5*") { 'v145' } else { 'v143' }
$env:CL = "$env:CL /utf-8"
& $msbuild "$source/platform/win32/mupdf.sln" /t:libmupdf /m:4 /p:Configuration=Release /p:Platform=x64 "/p:PlatformToolset=$toolset" /p:WindowsTargetPlatformVersion=10.0 /p:WholeProgramOptimization=false /verbosity:minimal /nologo "/flp:logfile=$project/build/mupdf-build.log;verbosity=normal"
if ($LASTEXITCODE) { throw "MuPDF build failed. See build/mupdf-build.log." }
Write-Host "MuPDF core built: $source/platform/win32/x64/Release"


