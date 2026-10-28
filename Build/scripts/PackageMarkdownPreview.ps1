# Packs the optional Markdown preview add-on (DLL, assets, notices) for one architecture.
[CmdletBinding()]
param(
    [ValidateSet('Win32', 'x64', 'ARM64')][string]$Platform = 'x64',
    [ValidateSet('Release', 'Release_AVX2')][string]$Configuration = 'Release',
    [Parameter(Mandatory = $true)][string]$OutputDirectory
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$platformShortName = @{ Win32 = 'x86'; x64 = 'x64'; ARM64 = 'ARM64' }[$Platform]
$expectedMachine = @{ Win32 = 0x014c; x64 = 0x8664; ARM64 = 0xaa64 }[$Platform]

$bin = Join-Path $root "Bin\$($Configuration)_$($platformShortName)_v145"
$dll = Join-Path $bin 'MarkdownPreview.dll'
$assets = Join-Path $bin 'MarkdownPreview'
$notices = Join-Path $root 'src\MarkdownPreview\THIRD-PARTY-NOTICES.txt'

foreach ($required in @($dll, (Join-Path $assets 'index.html'), (Join-Path $assets 'preview.js'), (Join-Path $assets 'preview.css'), $notices)) {
    if (!(Test-Path -LiteralPath $required)) {
        throw "Missing preview artifact: $required. Build src\MarkdownPreview\MarkdownPreview.vcxproj for $Configuration|$Platform first."
    }
}

function Get-PeMachine([string]$path) {
    $stream = [IO.File]::OpenRead($path)
    try {
        $reader = [IO.BinaryReader]::new($stream)
        if ($reader.ReadUInt16() -ne 0x5a4d) { return -1 }          # 'MZ'
        $stream.Position = 0x3c
        $peOffset = $reader.ReadInt32()
        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) { return -1 }      # 'PE\0\0'
        return $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
}

$machine = Get-PeMachine $dll
if ($machine -ne $expectedMachine) {
    throw ("MarkdownPreview.dll architecture mismatch: found 0x{0:x4}, expected 0x{1:x4} for {2}." -f $machine, $expectedMachine, $Platform)
}

if (!(Test-Path -LiteralPath $OutputDirectory)) {
    [void](New-Item -ItemType Directory -Path $OutputDirectory -Force)
}
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path
$package = Join-Path $OutputDirectory "MarkdownPreview-$Configuration-$Platform.zip"

$staging = Join-Path ([IO.Path]::GetTempPath()) ("np3-markdown-package-" + [Guid]::NewGuid().ToString('N'))
[void](New-Item -ItemType Directory -Path $staging -Force)

try {
    Copy-Item -LiteralPath $dll -Destination $staging
    Copy-Item -LiteralPath $assets -Destination (Join-Path $staging 'MarkdownPreview') -Recurse
    Copy-Item -LiteralPath $notices -Destination $staging

    # No browser runtime and no build leftovers belong in the add-on.
    $forbidden = Get-ChildItem -LiteralPath $staging -Recurse -File |
        Where-Object { $_.Name -match '^(WebView2Loader\.dll|EmbeddedBrowserWebView\.dll)$' -or $_.Extension -in @('.pdb', '.ilk', '.exp', '.lib', '.obj') }
    if ($forbidden) {
        throw ("Unexpected files in the preview package: {0}" -f (($forbidden | ForEach-Object { $_.Name }) -join ', '))
    }

    if (Test-Path -LiteralPath $package) {
        Remove-Item -LiteralPath $package -Force
    }
    Compress-Archive -Path (Join-Path $staging '*') -DestinationPath $package -CompressionLevel Optimal

    $package
}
finally {
    # Only remove the staging directory this script created.
    if ($staging -and (Test-Path -LiteralPath $staging) -and $staging.StartsWith([IO.Path]::GetTempPath()) -and $staging -match 'np3-markdown-package-[0-9a-f]{32}$') {
        Remove-Item -LiteralPath $staging -Recurse -Force
    }
}
