param(
    [ValidateSet('Core')][string]$Suite = 'Core',
    [string]$VisualStudioPath
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
