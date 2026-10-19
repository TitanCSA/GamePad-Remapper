[CmdletBinding()]
param([string]$DriverInstaller)
$ErrorActionPreference='Stop'
$root=Split-Path $PSScriptRoot -Parent
$version='1.1.2'
$out=Join-Path $root 'bin'
$dist=Join-Path $root 'dist'
$stage=Join-Path $root ('obj\Package-'+[Guid]::NewGuid().ToString('N'))
$folder=Join-Path $stage ('GamePad-Remapper-'+$version+'-win-x64')
New-Item -ItemType Directory -Path $dist,$folder -Force | Out-Null
$expectedInstaller='3fe4671322bf248946cf52ea6ec08d2f3a70e0d400ae125b7b70e425e92749ad'
if(!$DriverInstaller){
    $DriverInstaller=Join-Path $stage 'InterceptorSetup.exe'
    Invoke-WebRequest -UseBasicParsing -Uri 'https://github.com/TitanCSA/Interceptor-Gamepad-Controller/releases/download/v1.0.0.0-demo/InterceptorSetup.exe' -OutFile $DriverInstaller
}
if((Get-FileHash -LiteralPath $DriverInstaller -Algorithm SHA256).Hash.ToLowerInvariant() -cne $expectedInstaller){throw 'The installer must match the pinned Microsoft-signed Interceptor Gamepad Controller 1.0.0.0 release.'}
foreach($name in @('GamePadRemapper.exe','GamePadRemapper.Core.dll','GamePadRemapper.Native.dll')){
    $path=Join-Path $out $name
    if(!(Test-Path -LiteralPath $path)){throw ('Build artifact missing: '+$name)}
    Copy-Item -LiteralPath $path -Destination $folder
}
foreach($name in @('LICENSE','README.md','THIRD-PARTY-NOTICES.md','DRIVER-BINARY-LICENSE.txt')){Copy-Item -LiteralPath (Join-Path $root $name) -Destination $folder}
Copy-Item -LiteralPath (Join-Path $root 'Scripts'),(Join-Path $root 'docs') -Destination $folder -Recurse
Copy-Item -LiteralPath $DriverInstaller -Destination (Join-Path $folder 'InterceptorSetup.exe')
Copy-Item -LiteralPath $DriverInstaller -Destination (Join-Path $dist 'InterceptorSetup.exe') -Force
$zipPath=Join-Path $dist ('GamePad-Remapper-'+$version+'-win-x64.zip')
# Only the single known generated archive is replaced; source and user scripts
# are never removed or traversed for cleanup.
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
$assets=@($zipPath,(Join-Path $dist 'InterceptorSetup.exe'))
$lines=@(foreach($path in $assets){(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()+'  '+[IO.Path]::GetFileName($path)})
[IO.File]::WriteAllLines((Join-Path $dist 'SHA256SUMS.txt'),$lines,(New-Object Text.UTF8Encoding($false)))
$manifest=[ordered]@{Version=$version;CreatedUtc=[DateTime]::UtcNow.ToString('o');DriverSource='https://github.com/TitanCSA/Interceptor-Gamepad-Controller/releases/tag/v1.0.0.0-demo';InstallerSHA256=$expectedInstaller;Assets=@(foreach($path in $assets){[ordered]@{Name=[IO.Path]::GetFileName($path);Path=$path;SHA256=(Get-FileHash -LiteralPath $path).Hash.ToLowerInvariant();Size=(Get-Item -LiteralPath $path).Length}})}
$manifest|ConvertTo-Json -Depth 6|Set-Content -LiteralPath (Join-Path $dist 'package-manifest.json') -Encoding UTF8
Write-Host ('Portable application and unchanged signed driver installer ready in '+$dist)
