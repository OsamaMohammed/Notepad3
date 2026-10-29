# Packages Notepad3 for redistribution: a portable ZIP and an Inno Setup installer.
# The layout follows Build\make_portable_rel(.zip).cmd, plus the optional Markdown
# preview add-on (MarkdownPreview.dll and its bundled assets).
[CmdletBinding()]
param(
    [ValidateSet('x64', 'x86', 'both')][string]$Platform = 'both',
    [string]$Configuration = 'Release',
    [string]$OutputDirectory,
    [string]$SevenZipPath,
    [string]$IsccPath,
    [switch]$SkipZip,
    [switch]$SkipInstaller
)

$ErrorActionPreference = 'Stop'

$root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
$build = Join-Path $root 'Build'
$iss = Join-Path $build 'Notepad3.iss'

if (!$OutputDirectory) {
    $OutputDirectory = Join-Path $root 'out'
}
if (!(Test-Path -LiteralPath $OutputDirectory)) {
    [void](New-Item -ItemType Directory -Path $OutputDirectory -Force)
}
$OutputDirectory = (Resolve-Path -LiteralPath $OutputDirectory).Path

# Version: 7.26.1028.1
$version = $null
$defines = @{}
foreach ($line in Get-Content -LiteralPath (Join-Path $root 'src\VersionEx.h')) {
    if ($line -match '^\s*#define\s+(VERSION_(?:MAJOR|MINOR|REV|BUILD))\s+(\d+)') {
        $defines[$Matches[1]] = [int]$Matches[2]
    }
}
if ($defines.Count -ne 4) {
    throw 'Could not read VERSION_MAJOR/MINOR/REV/BUILD from src\VersionEx.h.'
}
$version = '{0}.{1}.{2}.{3}' -f $defines['VERSION_MAJOR'], $defines['VERSION_MINOR'], $defines['VERSION_REV'], $defines['VERSION_BUILD']

function Resolve-SevenZip {
    param([string]$Hint)
    if ($Hint) {
        if (!(Test-Path -LiteralPath $Hint)) { throw "7-Zip not found at '$Hint'." }
        return (Resolve-Path -LiteralPath $Hint).Path
    }
    $candidates = @(
        (Join-Path $env:ProgramFiles '7-Zip\7z.exe'),
        (Join-Path ${env:ProgramFiles(x86)} '7-Zip\7z.exe')
    )
    $onPath = Get-Command '7z.exe' -ErrorAction SilentlyContinue
    if ($onPath) { $candidates = @($onPath.Source) + $candidates }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    throw '7-Zip (7z.exe) is required to write the portable ZIP. Pass -SevenZipPath.'
}

function Resolve-Iscc {
    param([string]$Hint)
    if ($Hint) {
        if (!(Test-Path -LiteralPath $Hint)) { throw "ISCC.exe not found at '$Hint'." }
        return (Resolve-Path -LiteralPath $Hint).Path
    }
    $candidates = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe')
    )
    $onPath = Get-Command 'ISCC.exe' -ErrorAction SilentlyContinue
    if ($onPath) { $candidates = @($onPath.Source) + $candidates }
    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate)) { return $candidate }
    }
    throw 'Inno Setup 6 (ISCC.exe) is required for the installer. Install it, or pass -SkipInstaller.'
}

function Get-ArchName([string]$platform) {
    if ($platform -eq 'x64') { return 'x64' }
    return 'x86'
}

# Stages the files a user gets from the portable ZIP. The installed build is
# assembled by the Inno script from the same sources; it deliberately leaves out
# Notepad3.ini so settings live in %APPDATA% instead of Program Files.
function New-Stage {
    param([string]$platform, [string]$stage)

    $arch = Get-ArchName $platform
    $bin = Join-Path $root "Bin\$($Configuration)_$($arch)_v145"

    foreach ($required in @('Notepad3.exe', 'minipath.exe', 'np3encrypt.exe', 'MarkdownPreview.dll')) {
        if (!(Test-Path -LiteralPath (Join-Path $bin $required))) {
            throw "Missing build output: $bin\$required. Build $Configuration|$platform first."
        }
    }

    [void](New-Item -ItemType Directory -Path $stage -Force)

    foreach ($name in @('Notepad3.exe', 'minipath.exe', 'np3encrypt.exe', 'MarkdownPreview.dll')) {
        Copy-Item -LiteralPath (Join-Path $bin $name) -Destination $stage
    }
    Copy-Item -LiteralPath (Join-Path $bin "MarkdownPreview") -Destination (Join-Path $stage 'MarkdownPreview') -Recurse

    # Portable settings: the per-architecture template becomes Notepad3.ini.
    Copy-Item -LiteralPath (Join-Path $build "Notepad3_ptb_$arch.ini") -Destination (Join-Path $stage 'Notepad3.ini')
    Copy-Item -LiteralPath (Join-Path $build 'minipath.ini') -Destination $stage

    # Translation DLLs, without the intermediate objects and the built-in English.
    $lngSource = Join-Path $bin 'lng'
    $lngDest = Join-Path $stage 'lng'
    [void](New-Item -ItemType Directory -Path $lngDest -Force)
    Get-ChildItem -LiteralPath $lngSource -Directory |
        Where-Object { $_.Name -notin @('obj', 'en-US') } |
        ForEach-Object { Copy-Item -LiteralPath $_.FullName -Destination $lngDest -Recurse }

    Copy-Item -LiteralPath (Join-Path $build 'Themes') -Destination (Join-Path $stage 'Themes') -Recurse

    $docs = Join-Path $stage 'Docs'
    Copy-Item -LiteralPath (Join-Path $build 'Docs') -Destination $docs -Recurse
    Copy-Item -LiteralPath (Join-Path $build 'Changes.txt') -Destination $docs
    Copy-Item -LiteralPath (Join-Path $root 'src\MarkdownPreview\THIRD-PARTY-NOTICES.txt') -Destination $docs

    $grepwin = Join-Path $stage 'grepWin'
    [void](New-Item -ItemType Directory -Path $grepwin -Force)
    Copy-Item -LiteralPath (Join-Path $root "grepWin\portables\grepWin-${arch}_portable.exe") -Destination $grepwin
    Copy-Item -LiteralPath (Join-Path $root 'grepWin\portables\LICENSE.txt') -Destination $grepwin
    Copy-Item -LiteralPath (Join-Path $root 'grepWin\translations\*.lang') -Destination $grepwin

    # MiniPath's default favorites folder ships empty.
    [void](New-Item -ItemType Directory -Path (Join-Path $stage 'Favorites') -Force)

    $stage
}

function New-PortableZip {
    param([string]$arch, [string]$sevenZip, [string]$stage)

    $zip = Join-Path $OutputDirectory "Notepad3_${version}_${arch}_Portable.zip"
    if (Test-Path -LiteralPath $zip) { Remove-Item -LiteralPath $zip -Force }

    Push-Location $stage
    try {
        & $sevenZip a -tzip -mcu=on -mx=9 $zip '*' | Out-Null
        if ($LASTEXITCODE -ne 0) { throw "7-Zip failed with exit code $LASTEXITCODE." }
    }
    finally {
        Pop-Location
    }

    # Nothing but runtime files belong in the package.
    $forbidden = Get-ChildItem -LiteralPath $stage -Recurse -File |
        Where-Object { $_.Extension -in @('.pdb', '.ilk', '.exp', '.lib', '.obj') -or $_.Name -in @('WebView2Loader.dll', 'EmbeddedBrowserWebView.dll') }
    if ($forbidden) {
        throw ('Unexpected files in the portable package: {0}' -f (($forbidden | ForEach-Object { $_.Name }) -join ', '))
    }

    $zip
}

$platforms = if ($Platform -eq 'both') { @('x64', 'x86') } else { @($Platform) }

$sevenZip = $null
if (!$SkipZip) { $sevenZip = Resolve-SevenZip $SevenZipPath }
$iscc = $null
if (!$SkipInstaller) { $iscc = Resolve-Iscc $IsccPath }

$artifacts = New-Object System.Collections.Generic.List[string]

foreach ($platform in $platforms) {
    $arch = Get-ArchName $platform
    $stage = Join-Path ([IO.Path]::GetTempPath()) ("np3-package-" + [Guid]::NewGuid().ToString('N'))

    try {
        [void](New-Stage -platform $platform -stage $stage)

        if (!$SkipZip) {
            $artifacts.Add((New-PortableZip -arch $arch -sevenZip $sevenZip -stage $stage))
        }

        if (!$SkipInstaller) {
            $installer = Join-Path $OutputDirectory "Notepad3_${version}_${arch}_Setup.exe"
            if (Test-Path -LiteralPath $installer) { Remove-Item -LiteralPath $installer -Force }
            # Inno Setup cannot wildcard directory names, so hand it the staged lng tree.
            & $iscc "/DAppArch=$arch" "/DAppVersion=$version" "/DSourceLng=$(Join-Path $stage 'lng')" "/O$OutputDirectory" $iss
            if ($LASTEXITCODE -ne 0) { throw "Inno Setup failed with exit code $LASTEXITCODE." }
            $artifacts.Add($installer)
        }
    }
    finally {
        if ((Test-Path -LiteralPath $stage) -and $stage.StartsWith([IO.Path]::GetTempPath()) -and $stage -match 'np3-package-[0-9a-f]{32}$') {
            Remove-Item -LiteralPath $stage -Recurse -Force
        }
    }
}

foreach ($artifact in $artifacts) {
    $item = Get-Item -LiteralPath $artifact
    '{0,10:N1} MB  {1}  {2}' -f ($item.Length / 1MB), (Get-FileHash -LiteralPath $artifact -Algorithm SHA256).Hash, $item.FullName
}
