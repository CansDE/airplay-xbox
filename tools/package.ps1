# Erzeugt aus dem CMake-Build ein signiertes MSIX-Paket (build\AirPlayXbox.msix) plus Zertifikat (build\AirPlayXboxDev.cer).
# Aufruf: powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1
$ErrorActionPreference = 'Stop'
$root   = Split-Path -Parent $PSScriptRoot
$build  = Join-Path $root 'build'
$layout = Join-Path $build 'layout'
$sdkBin = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.26100.0\x64'
$vcpkgBin = Join-Path $root 'vcpkg_installed\x64-uwp\bin'
$pfx = Join-Path $root 'AirPlayXboxDev.pfx'
$cer = Join-Path $build 'AirPlayXboxDev.cer'
$pfxPassword = 'airplay-dev'
$subject = 'CN=AirPlayXboxDev'   # muss mit Publisher in package\AppxManifest.xml uebereinstimmen

if (-not (Test-Path (Join-Path $build 'AirPlayXbox.exe'))) { throw 'build\AirPlayXbox.exe fehlt - zuerst tools\build.bat ausfuehren.' }

# --- Layout aufbauen -------------------------------------------------------------------------------------------
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Path $layout | Out-Null
Copy-Item (Join-Path $build 'AirPlayXbox.exe') $layout
Copy-Item (Join-Path $root 'package\AppxManifest.xml') $layout
Copy-Item (Join-Path $root 'package\Assets') $layout -Recurse

# FFmpeg / OpenSSL (vcpkg, dynamisch gelinkt)
foreach ($dll in 'avcodec-*.dll','avutil-*.dll','swresample-*.dll','libcrypto-*.dll','libssl-*.dll') {
    Get-ChildItem (Join-Path $vcpkgBin $dll) | ForEach-Object { Copy-Item $_.FullName $layout }
}

# C++-Laufzeit app-lokal (aus dem installierten Microsoft.VCLibs.140.00-Framework dieses PCs)
$vclibs = Get-AppxPackage -Name Microsoft.VCLibs.140.00 | Where-Object { $_.Architecture -eq 'X64' } | Select-Object -First 1
if (-not $vclibs) { throw 'Microsoft.VCLibs.140.00 (x64) ist auf diesem PC nicht installiert.' }
Get-ChildItem $vclibs.InstallLocation -Filter '*_app.dll' | ForEach-Object { Copy-Item $_.FullName $layout }
Write-Host "VCLibs-Laufzeit $($vclibs.Version) app-lokal kopiert"

# --- Zertifikat (einmalig) -------------------------------------------------------------------------------------
if (-not (Test-Path $pfx)) {
    # Direkt per .NET erzeugen: kein Eintrag im Windows-Zertifikatspeicher noetig.
    $rsa = [System.Security.Cryptography.RSA]::Create(2048)
    $req = New-Object System.Security.Cryptography.X509Certificates.CertificateRequest(
        (New-Object System.Security.Cryptography.X509Certificates.X500DistinguishedName($subject)),
        $rsa, [System.Security.Cryptography.HashAlgorithmName]::SHA256, [System.Security.Cryptography.RSASignaturePadding]::Pkcs1)
    $req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509BasicConstraintsExtension($false, $false, 0, $true)))
    $req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509KeyUsageExtension([System.Security.Cryptography.X509Certificates.X509KeyUsageFlags]::DigitalSignature, $true)))
    $eku = New-Object System.Security.Cryptography.OidCollection
    [void]$eku.Add((New-Object System.Security.Cryptography.Oid('1.3.6.1.5.5.7.3.3')))   # Code Signing
    $req.CertificateExtensions.Add((New-Object System.Security.Cryptography.X509Certificates.X509EnhancedKeyUsageExtension($eku, $true)))
    $cert = $req.CreateSelfSigned([DateTimeOffset]::UtcNow.AddDays(-1), [DateTimeOffset]::UtcNow.AddYears(5))
    [IO.File]::WriteAllBytes($pfx, $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Pfx, $pfxPassword))
    Write-Host "Neues Entwicklerzertifikat erzeugt: $pfx"
}
$pfxCert = New-Object System.Security.Cryptography.X509Certificates.X509Certificate2($pfx, $pfxPassword)
[IO.File]::WriteAllBytes($cer, $pfxCert.Export('Cert'))

# --- Packen und signieren --------------------------------------------------------------------------------------
$msix = Join-Path $build 'AirPlayXbox.msix'
& "$sdkBin\makeappx.exe" pack /d $layout /p $msix /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'makeappx fehlgeschlagen' }
& "$sdkBin\signtool.exe" sign /fd SHA256 /f $pfx /p $pfxPassword $msix | Out-Host
if ($LASTEXITCODE -ne 0) { throw 'signtool fehlgeschlagen' }
Write-Host "Fertig: $msix"
Write-Host "Zertifikat fuer das Device Portal: $cer"
