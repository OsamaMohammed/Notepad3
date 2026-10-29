param(
    [ValidateSet('Core','WebView','Host','Package','UI')][string]$Suite = 'Core',
    [string]$VisualStudioPath,
    [string]$PackagePath,
    [ValidateSet('Win32','x64','ARM64')][string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'
$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (!$VisualStudioPath) {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path -LiteralPath $vswhere) {
        $VisualStudioPath = & $vswhere -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
    }
}
if (!$VisualStudioPath) {
    throw 'Markdown preview tests require Visual Studio C++ Build Tools and a Windows SDK. Supply -VisualStudioPath for a custom installation.'
}
$vcvars = Join-Path $VisualStudioPath 'VC\Auxiliary\Build\vcvars64.bat'
if (!(Test-Path -LiteralPath $vcvars)) {
    throw "C++ build environment not found: $vcvars"
}
$output = Join-Path $root '.superpowers\sdd\2026-10-28-markdown-preview\core-tests'
[void](New-Item -ItemType Directory -Path $output -Force)
$source = Join-Path $root 'src\MarkdownPreview'
if ($Suite -eq 'Package') {
    if (!$PackagePath -or !(Test-Path -LiteralPath $PackagePath)) { throw 'Package suite requires -PackagePath <zip>.' }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead((Resolve-Path -LiteralPath $PackagePath).Path)
    try {
        $names = @($zip.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
        $checks = 0
        function Assert-Package([bool]$condition, [string]$message) {
            if (!$condition) { throw "Package assertion failed: $message" }
            $script:checks++
        }
        Assert-Package ($names -contains 'MarkdownPreview.dll') 'DLL at package root'
        Assert-Package ($names -contains 'MarkdownPreview/index.html') 'preview document'
        Assert-Package ($names -contains 'MarkdownPreview/preview.js') 'preview script'
        Assert-Package ($names -contains 'MarkdownPreview/preview.css') 'preview stylesheet'
        Assert-Package ($names -contains 'THIRD-PARTY-NOTICES.txt') 'license notices'
        Assert-Package (($names | Where-Object { $_ -like 'MarkdownPreview/vendor/mermaid/*' }).Count -gt 0) 'bundled Mermaid assets'
        Assert-Package (($names | Where-Object { $_ -like 'MarkdownPreview/vendor/mermaid/chunks/*' }).Count -gt 0) 'bundled Mermaid chunks'
        Assert-Package (($names | Where-Object { $_ -like 'MarkdownPreview/vendor/mermaid/LICENSE' }).Count -eq 1) 'Mermaid license'
        Assert-Package (($names | Where-Object { $_ -match '(WebView2Loader|EmbeddedBrowserWebView|msedgewebview2)\.' }).Count -eq 0) 'no bundled browser runtime'
        Assert-Package (($names | Where-Object { $_ -match '\.(pdb|ilk|exp|obj)$' }).Count -eq 0) 'no build leftovers'

        $dllEntry = $zip.Entries | Where-Object { ($_.FullName -replace '\\', '/') -eq 'MarkdownPreview.dll' }
        $memory = [IO.MemoryStream]::new()
        $stream = $dllEntry.Open()
        try {
            $stream.CopyTo($memory)
        }
        finally {
            $stream.Dispose()
        }
        $memory.Position = 0
        $reader = [IO.BinaryReader]::new($memory)
        [void]$reader.ReadUInt16()
        $memory.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $memory.Position = $peOffset + 4
        $machine = $reader.ReadUInt16()
        $memory.Dispose()
        $expected = @{ Win32 = 0x014c; x64 = 0x8664; ARM64 = 0xaa64 }[$Platform]
        Assert-Package ($machine -eq $expected) "DLL architecture matches $Platform"

        $noticesEntry = $zip.Entries | Where-Object { ($_.FullName -replace '\\', '/') -eq 'THIRD-PARTY-NOTICES.txt' }
        $noticesReader = [IO.StreamReader]::new($noticesEntry.Open())
        try { $noticesText = $noticesReader.ReadToEnd() } finally { $noticesReader.Dispose() }
        Assert-Package ($noticesText -match 'MD4C') 'MD4C notice'
        Assert-Package ($noticesText -match 'Mermaid') 'Mermaid notice'
        Assert-Package ($noticesText -match 'WebView2') 'WebView2 notice'

        "PASS: $checks package assertions"
    }
    finally {
        $zip.Dispose()
    }
    exit 0
}
if ($Suite -eq 'Host') {
    $msbuild = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $source 'MarkdownPreview.vcxproj') -m -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Preview DLL build failed.' }
    & $msbuild (Join-Path $root 'Notepad3.sln') -m -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Editor build failed.' }
    $test = Join-Path $PSScriptRoot 'TestMarkdownHost.cpp'
    $batch = Join-Path $output 'host-build.cmd'
    [IO.File]::WriteAllLines($batch, @('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$output`"", "cl /nologo /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE `"$test`" /Fe:TestMarkdownHost.exe /link user32.lib", 'exit /b %errorlevel%'), [Text.UTF8Encoding]::new($false))
    & $env:ComSpec /d /c "`"$batch`""
    if ($LASTEXITCODE -ne 0) { throw 'Host test build failed.' }
    & (Join-Path $output 'TestMarkdownHost.exe') (Join-Path $root 'Bin\Release_x64_v145\Notepad3.exe') (Join-Path $PSScriptRoot 'fixtures\host-sample.md')
    if ($LASTEXITCODE -ne 0) { throw 'Host integration tests failed.' }
    exit 0
}
if ($Suite -eq 'WebView') {
    $msbuild = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $source 'MarkdownPreview.vcxproj') /m /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Preview DLL build failed.' }
    $test = Join-Path $PSScriptRoot 'TestPreviewWindow.cpp'
    $batch = Join-Path $output 'webview-build.cmd'
    [IO.File]::WriteAllLines($batch, @('@echo off', "call `"$vcvars`" >nul", 'if errorlevel 1 exit /b 1', "cd /d `"$output`"", "cl /nologo /EHsc /std:c++17 /DUNICODE /D_UNICODE /I`"$source`" `"$test`" /Fe:TestPreviewWindow.exe /link user32.lib ole32.lib", 'exit /b %errorlevel%'), [Text.UTF8Encoding]::new($false))
    & $env:ComSpec /d /c "`"$batch`""
    if ($LASTEXITCODE -ne 0) { throw 'WebView test build failed.' }
    & (Join-Path $output 'TestPreviewWindow.exe') (Join-Path $root 'Bin\Release_x64_v145\MarkdownPreview.dll')
    if ($LASTEXITCODE -ne 0) { throw 'WebView integration tests failed.' }
    & node (Join-Path $PSScriptRoot 'TestPreviewDom.mjs') (Join-Path $output 'TestPreviewWindow.exe') (Join-Path $root 'Bin\Release_x64_v145\MarkdownPreview.dll')
    if ($LASTEXITCODE -ne 0) { throw 'Browser DOM tests failed.' }
    exit 0
}
if ($Suite -eq 'UI') {
    $installDir = (Get-ItemProperty 'HKLM:\SOFTWARE\AutoHotkey' -ErrorAction SilentlyContinue).InstallDir
    $ahk = @(
        (Join-Path $installDir 'v2\AutoHotkey64.exe'),
        (Join-Path $installDir 'v2\AutoHotkey.exe'),
        (Join-Path ${env:ProgramFiles} 'AutoHotkey\v2\AutoHotkey64.exe')
    ) | Where-Object { $installDir -and (Test-Path -LiteralPath $_) } | Select-Object -First 1
    if (!$ahk) { throw 'The Markdown preview UI matrix requires AutoHotkey v2 (https://www.autohotkey.com).' }
    $msbuild = Join-Path $VisualStudioPath 'MSBuild\Current\Bin\MSBuild.exe'
    & $msbuild (Join-Path $source 'MarkdownPreview.vcxproj') -m -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Preview DLL build failed.' }
    & $msbuild (Join-Path $root 'Notepad3.sln') -m -p:Configuration=Release -p:Platform=x64 -nologo -v:minimal
    if ($LASTEXITCODE -ne 0) { throw 'Editor build failed.' }
    $arguments = @(
        (Join-Path $PSScriptRoot 'TestMarkdownPreview.ahk'),
        (Join-Path $root 'Bin\Release_x64_v145\Notepad3.exe'),
        (Join-Path $PSScriptRoot 'fixtures'),
        (Join-Path $root '.superpowers\sdd\2026-10-28-markdown-preview\ui-matrix')
    ) | ForEach-Object { '"' + $_ + '"' }
    # Start-Process waits for GUI applications; the call operator does not.
    $ui = Start-Process -FilePath $ahk -ArgumentList ($arguments -join ' ') -Wait -PassThru -NoNewWindow
    if ($ui.ExitCode -ne 0) { throw "Markdown preview UI matrix failed (exit $($ui.ExitCode))." }
    exit 0
}
$test = Join-Path $PSScriptRoot 'TestMarkdownDocument.cpp'
$batch = Join-Path $output 'build.cmd'
# Paths are quoted for cmd, not JSON-escaped. Reject cmd expansion characters.
foreach ($path in @($vcvars, $source, $test, $output)) {
    if ($path -match '[%\r\n"]') { throw 'Unsupported build path character' }
}
$commands = @(
    '@echo off',
    "call `"$vcvars`" >nul",
    'if errorlevel 1 exit /b 1',
    "cd /d `"$output`"",
    "cl /nologo /W0 /c `"$source\vendor\md4c\md4c.c`" `"$source\vendor\md4c\entity.c`"",
    'if errorlevel 1 exit /b 1',
    "cl /nologo /EHsc /std:c++17 /utf-8 /W4 /I`"$source`" `"$test`" `"$source\MarkdownDocument.cpp`" md4c.obj entity.obj /Fe:TestMarkdownDocument.exe",
    'if errorlevel 1 exit /b 1',
    "cl /nologo /EHsc /std:c++17 /utf-8 /W4 /DUNICODE /D_UNICODE /I`"$source`" `"$PSScriptRoot\TestPreviewResources.cpp`" `"$source\PreviewResources.cpp`" /Fe:TestPreviewResources.exe /link shlwapi.lib winhttp.lib",
    'exit /b %errorlevel%'
)
[IO.File]::WriteAllLines($batch, $commands, [Text.UTF8Encoding]::new($false))
& $env:ComSpec /d /c "`"$batch`""
if ($LASTEXITCODE -ne 0) { throw 'Core test build failed; no test results are available.' }
& (Join-Path $output 'TestMarkdownDocument.exe')
if ($LASTEXITCODE -ne 0) { throw 'Markdown source-mapping tests failed.' }
& (Join-Path $output 'TestPreviewResources.exe') (Join-Path $output 'resource-fixtures')
if ($LASTEXITCODE -ne 0) { throw 'Preview resource tests failed.' }
