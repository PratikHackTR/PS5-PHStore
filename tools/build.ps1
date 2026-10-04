param(
    [ValidateSet('Debug', 'Release')]
    [string]$Configuration = 'Release',
    [string]$BootstrapUrl = 'https://phstore.invalid/bootstrap.json',
    [string]$SdkRoot = $env:PS5_PAYLOAD_SDK,
    [string]$LlvmBin = $(if ($env:LLVM_BIN) { $env:LLVM_BIN } else { 'C:\Program Files\LLVM\bin' })
)
$ErrorActionPreference = 'Stop'
if (-not $SdkRoot) { throw 'Set PS5_PAYLOAD_SDK or pass -SdkRoot to your external homebrew SDK.' }
if (-not $BootstrapUrl.StartsWith('https://', [StringComparison]::OrdinalIgnoreCase) -or
    $BootstrapUrl.Contains('"') -or $BootstrapUrl.Contains('\') -or
    $BootstrapUrl.Contains("`r") -or $BootstrapUrl.Contains("`n")) {
    throw 'BootstrapUrl must be a plain HTTPS URL without quotes or control characters.'
}
$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
Push-Location $root
try {
$clang = Join-Path $LlvmBin 'clang.exe'
$strip = Join-Path $LlvmBin 'llvm-strip.exe'
$linker = Join-Path $SdkRoot 'win\prospero-lld.exe'
foreach ($file in @($clang, $strip, $linker, (Join-Path $SdkRoot 'target\lib\crt1.o'))) {
    if (-not (Test-Path -LiteralPath $file)) { throw "Required SDK tool missing: $file" }
}
$llvmPath = (& cmd.exe /d /c ('for %I in ("{0}") do @echo %~sI' -f $LlvmBin) | Out-String).Trim()
if (-not $llvmPath) { throw "Could not resolve LLVM path: $LlvmBin" }
$env:PATH = "$llvmPath;$env:PATH"
$build = Join-Path $root 'build'
New-Item -ItemType Directory -Force -Path $build | Out-Null
Push-Location $root
try {
    & python tools/prepare_embedded_catalog.py
    if ($LASTEXITCODE -ne 0) { throw "Embedded catalogue preparation failed" }
    & python tools/build_assets.py --build-type $Configuration `
        --header-output (Join-Path $build "$Configuration\phstore_assets.h") `
        --source-output (Join-Path $build "$Configuration\phstore_assets.c")
    if ($LASTEXITCODE -ne 0) { throw "Frontend/icon build failed with exit code $LASTEXITCODE" }
} finally { Pop-Location }
$obj = Join-Path $build 'phstr_main.o'
$elf = Join-Path $build 'phstr.elf'
$defines = '-DPHSTORE_BOOTSTRAP_URL="' + $BootstrapUrl + '"'
$opt = if ($Configuration -eq 'Release') { @('-O2', '-D_FORTIFY_SOURCE=2') } else { @('-O0', '-g') }
$debugDefine = if ($Configuration -eq 'Debug') { @('-DPHSTORE_DEBUG_BUILD=1') } else { @() }
$targetArgs = @('-target', 'x86_64-sie-ps5', '-DPS5_BUILD=1',
    '-include', (Join-Path $root 'include\phstore_build_guard.h'), '-isysroot', $SdkRoot,
    '-isystem', (Join-Path $SdkRoot 'target\include'), '-Iinclude',
    '-Ithird_party/ps5-pkg-manager/include', "-I$(Join-Path $build $Configuration)")
$commonCompile = @('-Wall', '-Wextra', '-Werror', '-fPIE', '-fstack-usage') + $opt
$gdriveRoot = Join-Path $root 'third_party/gdrive-native'
$gdriveObjects = @()
foreach ($source in @('src/phstore_gdrive.c','src/phstore_gdrive_protocol.c',
                      'third_party/gdrive-native/src/range_buffer.c','third_party/gdrive-native/src/entropy.c',
                      'third_party/gdrive-native/src/crypto_control.c')) {
    $object = Join-Path $build ('gdrive_' + [IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    & $clang @targetArgs @commonCompile '-DWOLFSSL_USER_SETTINGS' "-I$gdriveRoot/config" "-I$gdriveRoot/include" "-I$gdriveRoot/src" -c $source -o $object
    if ($LASTEXITCODE -ne 0) { throw "Native Google Drive compile failed: $source" }
    $gdriveObjects += $object
}

$helperElf = Join-Path $build 'install-helper.elf'
$helperObjects = @()
foreach ($source in @('third_party/ps5-pkg-manager/src/install_helper.c', 'third_party/ps5-pkg-manager/src/install_ipc.c')) {
    $object = Join-Path $build ([IO.Path]::GetFileNameWithoutExtension($source) + '_upstream.o')
    $compile = $targetArgs + $debugDefine + $commonCompile + @('-c', $source, '-o', $object)
    & $clang @compile
    if ($LASTEXITCODE -ne 0) { throw "Upstream helper compile failed for $source ($LASTEXITCODE)" }
    $helperObjects += $object
}
$sdkLib = Join-Path $SdkRoot 'target\lib'
$helperLink = @('-m', 'elf_x86_64', '-pie', '-T', (Join-Path $SdkRoot 'ldscripts\elf_x86_64.x'),
    '--eh-frame-hdr', '-z', 'max-page-size=0x4000', '-mllvm', '-emulated-tls', '-o', $helperElf,
    (Join-Path $SdkRoot 'target\lib\crt1.o')) + $helperObjects + @("-L$sdkLib", '-lpthread', '-lc',
    '-lSceLibcInternal', '-lkernel_web', '-lSceNetCtl', '-lSceUserService', '-lSceSystemService',
    '-lSceAppInstUtil', '-lSceNet', '--hash-style=gnu')
& $linker @helperLink
if ($LASTEXITCODE -ne 0) { throw "Upstream install helper link failed with exit code $LASTEXITCODE" }
& $strip --strip-unneeded $helperElf
if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed for upstream installer helper with exit code $LASTEXITCODE" }

$spectrumBlob = Join-Path $build 'spectrum_blob.o'
& $clang @targetArgs -c src/spectrum_blob.S -o $spectrumBlob
if ($LASTEXITCODE -ne 0) { throw 'Spectrum helper blob compilation failed' }
$blobObject = Join-Path $build 'install_helper_blob.o' 
$blobCompile = $targetArgs + @('-c', 'third_party/ps5-pkg-manager/src/install_helper_blob.S', '-o', $blobObject)
& $clang @blobCompile
if ($LASTEXITCODE -ne 0) { throw "clang failed for embedded helper blob with exit code $LASTEXITCODE" }

$compileArgs = $targetArgs + @($defines) + $debugDefine + $commonCompile + @('-c', 'src/main.c', '-o', $obj)
foreach ($source in @('src/phstore_spectrum.c', 'src/phstore_shortcut.c', 'src/phstore_notification.c', 'src/phstore_catalog.c',
                      'src/phstore_url.c', 'src/phstore_upstream.c', 'src/phstore_progress.c',
                      'src/phstore_range.c', 'src/phstore_image_cache.c', 'src/phstore_raw_http.c', 'src/phstore_pkg_source.c', 'src/phstore_install.c',
                      'src/phstore_install_request.c',
                      'src/phstore_pkgmgr_compat.c',
                      'third_party/ps5-pkg-manager/src/installer.c', 'third_party/ps5-pkg-manager/src/install_service.c',
                      'third_party/ps5-pkg-manager/src/install_process.c', 'third_party/ps5-pkg-manager/src/install_ipc.c',
                      'third_party/ps5-pkg-manager/src/stream_server.c', 'third_party/ps5-pkg-manager/src/multipart.c',
                      'third_party/ps5-pkg-manager/src/pkg_parser.c', 'third_party/ps5-pkg-manager/src/miniz.c',
                      'third_party/ps5-pkg-manager/src/stream_debug_log.c',
                      'third_party/ps5-pkg-manager/src/app_info.c', 'third_party/ps5-pkg-manager/src/sqlite3.c',
                      'third_party/ps5-pkg-manager/src/debug_log_retention.c')) {
    $object = Join-Path $build ([IO.Path]::GetFileNameWithoutExtension($source) + '.o')
    $sourceArgs = $targetArgs + @($defines) + $debugDefine + $commonCompile + @('-c', $source, '-o', $object)
    & $clang @sourceArgs
    if ($LASTEXITCODE -ne 0) { throw "clang failed for $source with exit code $LASTEXITCODE" }
}
$assetsObject = Join-Path $build 'phstore_assets.o'
$assetArgs = $targetArgs + $debugDefine + $commonCompile + @('-c', (Join-Path $build "$Configuration\phstore_assets.c"), '-o', $assetsObject)
& $clang @assetArgs
if ($LASTEXITCODE -ne 0) { throw "clang failed for generated assets with exit code $LASTEXITCODE" }
Push-Location $root
try {
    & $clang @compileArgs
    if ($LASTEXITCODE -ne 0) { throw "clang failed with exit code $LASTEXITCODE" }
    $mainShortcutObject = Join-Path $build 'phstore_shortcut.o'
    $mainNotificationObject = Join-Path $build 'phstore_notification.o'
    $mainCatalogObject = Join-Path $build 'phstore_catalog.o'
    $mainUrlObject = Join-Path $build 'phstore_url.o'
    $mainUpstreamObject = Join-Path $build 'phstore_upstream.o'
    $mainProgressObject = Join-Path $build 'phstore_progress.o'
    $mainRangeObject = Join-Path $build 'phstore_range.o'
    $mainRawHttpObject = Join-Path $build 'phstore_raw_http.o'
    $mainInstallObject = Join-Path $build 'phstore_install.o'
    $linkArgs = @('-m', 'elf_x86_64', '-pie', '-T', (Join-Path $SdkRoot 'ldscripts\elf_x86_64.x'),
        '--eh-frame-hdr', '-z', 'max-page-size=0x4000', '-mllvm', '-emulated-tls',
        '-o', $elf, (Join-Path $SdkRoot 'target\lib\crt1.o'), $obj,
        $mainShortcutObject, $mainNotificationObject, $mainCatalogObject, $mainUrlObject,
        $mainUpstreamObject, $mainProgressObject, $mainRangeObject, $mainRawHttpObject, $mainInstallObject,
        (Join-Path $build 'phstore_image_cache.o'), (Join-Path $build 'phstore_pkg_source.o'),
        (Join-Path $build 'phstore_install_request.o'),
        (Join-Path $build 'phstore_pkgmgr_compat.o'),
        (Join-Path $build 'installer.o'), (Join-Path $build 'install_service.o'),
        (Join-Path $build 'install_process.o'), (Join-Path $build 'install_ipc.o'),
        (Join-Path $build 'stream_server.o'), (Join-Path $build 'multipart.o'),
        (Join-Path $build 'pkg_parser.o'), (Join-Path $build 'miniz.o'), (Join-Path $build 'stream_debug_log.o'),
        (Join-Path $build 'app_info.o'), (Join-Path $build 'sqlite3.o'), (Join-Path $build 'debug_log_retention.o'),
        $assetsObject, $blobObject, $spectrumBlob, (Join-Path $build 'phstore_spectrum.o'),
        "-L$sdkLib", '-lpthread', '-lc', '-lSceLibcInternal', '-lSceNet', '-lSceNetCtl',
        '-lSceUserService', '-lSceSystemService', '-lSceAppInstUtil', '-lSceNotification',
        '-lSceSsl', '-lSceHttp2', '-lkernel_web', '--hash-style=gnu')
    $linkArgs += @('--gc-sections') + $gdriveObjects + @("$gdriveRoot/lib/libcurl.a", "$gdriveRoot/lib/libwolfssl.a")
    & $linker @linkArgs
    if ($LASTEXITCODE -ne 0) { throw "PS5 linker failed with exit code $LASTEXITCODE" }
    if ($Configuration -eq 'Release') {
        & $strip --strip-unneeded $elf
        if ($LASTEXITCODE -ne 0) { throw "llvm-strip failed with exit code $LASTEXITCODE" }
    }
    & python (Join-Path $root 'tools/verify_ps5_installer_build.py') --build-dir $build --llvm-bin $LlvmBin
    if ($LASTEXITCODE -ne 0) { throw 'PS5 native installer artifact verification failed' }
    Get-Item -LiteralPath $elf,$helperElf | Select-Object FullName, Length
} finally {
    Pop-Location
}

} finally { Pop-Location }
