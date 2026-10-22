[CmdletBinding()]
param([string]$BuildDirectory,[string]$DriverInstaller)
$ErrorActionPreference='Stop'
$root=Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
$version='2.3.2'
$driverRelease='1.0.9.0-demo'
$driverKernelVersion='1.0.5.0'
$signedKernelSHA256='2d58e5aa6fcc50d10a2b8a350174dea1183ac688d8993471ef1105c6da4da3fa'
$expectedInstaller='558738a04fa7c08ddd6c2a63943fa537fe8a4472817af586a0a61a14b983016d'
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'App\.build\Release'}
$out=Join-Path $BuildDirectory 'bin'
$dist=Join-Path $root 'App\.build\dist'
$stage=Join-Path $root ('App\.build\Package-'+[Guid]::NewGuid().ToString('N'))
$folder=Join-Path $stage ('GamePad-Remapper-'+$version+'-win-x64')
$null=New-Item -ItemType Directory -Path $dist,$folder -Force
if(!$DriverInstaller){
    throw 'Supply the verified installer for Microsoft-signed controller kernel 1.0.5.0.'
}
if((Get-FileHash -LiteralPath $DriverInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expectedInstaller){throw 'The driver installer must match the pinned Microsoft-signed controller kernel 1.0.5.0 release.'}
$installerInfo=[Diagnostics.FileVersionInfo]::GetVersionInfo($DriverInstaller)
if($installerInfo.ProductName -cne 'InterceptorGamepadController'){throw 'The installer must identify the Interceptor Gamepad Controller product, not classic Interceptor.'}
$application=Join-Path $out 'GamePadRemapper.exe'
if(!(Test-Path -LiteralPath $application)){throw 'Build GamePadRemapper.exe first.'}
# A native Windows EXE must have an empty CLR runtime directory (PE32+).
$binary=[IO.File]::ReadAllBytes($application)
if($binary.Length -lt 64 -or [BitConverter]::ToUInt16($binary,0) -ne 0x5a4d){throw 'Invalid application DOS header.'}
$pe=[BitConverter]::ToInt32($binary,0x3c)
if($pe -lt 64 -or $pe -gt $binary.Length-264 -or [BitConverter]::ToUInt32($binary,$pe) -ne 0x4550 -or [BitConverter]::ToUInt16($binary,$pe+4) -ne 0x8664 -or [BitConverter]::ToUInt16($binary,$pe+20) -lt 240 -or [BitConverter]::ToUInt16($binary,$pe+24) -ne 0x20b -or [BitConverter]::ToUInt32($binary,$pe+24+108) -lt 15 -or [BitConverter]::ToUInt32($binary,$pe+24+112+14*8) -ne 0 -or [BitConverter]::ToUInt32($binary,$pe+24+112+14*8+4) -ne 0){throw 'Expected a native AMD64 EXE without a CLR directory.'}
if([Text.Encoding]::ASCII.GetString($binary).IndexOf('clang_rt.asan',[StringComparison]::OrdinalIgnoreCase) -ge 0){throw 'AddressSanitizer-instrumented binaries cannot be packaged as a portable production release.'}
Copy-Item -LiteralPath $application -Destination $folder
foreach($name in @('LICENSE','README.md','THIRD-PARTY-NOTICES.md','DRIVER-BINARY-LICENSE.txt')){Copy-Item -LiteralPath (Join-Path $root $name) -Destination $folder}
# Keep the dependency's full MIT license available in binary distributions.
Copy-Item -LiteralPath (Join-Path $root 'App\ThirdParty\json.LICENSE.MIT') -Destination (Join-Path $folder 'JSON-LICENSE.txt')
$scriptDestination=Join-Path $folder 'Scripts'
New-Item -ItemType Directory -Path $scriptDestination|Out-Null
Get-ChildItem -LiteralPath (Join-Path $root 'Scripts') -Filter '*.igc' -File|Copy-Item -Destination $scriptDestination
Copy-Item -LiteralPath (Join-Path $root 'docs') -Destination $folder -Recurse
Copy-Item -LiteralPath $DriverInstaller -Destination (Join-Path $folder 'InterceptorSetup.exe')
Copy-Item -LiteralPath $DriverInstaller -Destination (Join-Path $dist 'InterceptorSetup.exe') -Force
# Remove the obsolete duplicate from a previously populated output directory.
$obsoleteInstaller=Join-Path $dist 'InterceptorGamepadControllerSetup.exe'
if(Test-Path -LiteralPath $obsoleteInstaller -PathType Leaf){Remove-Item -LiteralPath $obsoleteInstaller -Force}
Copy-Item -LiteralPath $application -Destination (Join-Path $dist 'GamePadRemapper.exe') -Force
$zipPath=Join-Path $dist ('GamePad-Remapper-'+$version+'-win-x64.zip')
if(Test-Path -LiteralPath $zipPath){Remove-Item -LiteralPath $zipPath -Force}
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::Open($zipPath,[IO.Compression.ZipArchiveMode]::Create)
try{
    foreach($file in (Get-ChildItem -LiteralPath $folder -Recurse -File)){
        $entryName=$file.FullName.Substring($folder.Length+1).Replace('\','/')
        $null=[IO.Compression.ZipFileExtensions]::CreateEntryFromFile($archive,$file.FullName,$entryName,[IO.Compression.CompressionLevel]::Optimal)
    }
}finally{$archive.Dispose()}
$assets=@($zipPath,(Join-Path $dist 'GamePadRemapper.exe'),(Join-Path $dist 'InterceptorSetup.exe'))
$lines=@(foreach($path in $assets){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+[IO.Path]::GetFileName($path)})
[IO.File]::WriteAllText((Join-Path $dist 'SHA256SUMS.txt'),(($lines -join [char]10)+[char]10),(New-Object Text.UTF8Encoding($false)))
$manifest=[ordered]@{Version=$version;CreatedUtc=[DateTime]::UtcNow.ToString('o');DriverSource='https://github.com/TitanCSA/Interceptor-Gamepad-Controller';DriverReleaseURL=('https://github.com/TitanCSA/Interceptor-Gamepad-Controller/releases/tag/v'+$driverRelease);DriverRelease=$driverRelease;DriverKernelVersion=$driverKernelVersion;SignedKernelSHA256=$signedKernelSHA256;SdkVersion='1.0.4.2';InstallerProductVersion=$installerInfo.ProductVersion;InstallerFileVersion=$installerInfo.FileVersion;InstallerSHA256=$expectedInstaller;Assets=@(foreach($path in $assets){[ordered]@{Name=[IO.Path]::GetFileName($path);Path=$path;SHA256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();Size=(Get-Item -LiteralPath $path).Length}})}
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $dist 'package-manifest.json') -Encoding UTF8
Write-Host ('Native portable application and unchanged signed driver installer ready in '+$dist)
