[CmdletBinding()]
param(
    [ValidateSet('All','Native','Managed','Test','Package')][string]$Target='All',
    [switch]$Render,
    [string]$DriverInstaller,
    [string]$BuildDirectory
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if(![Environment]::Is64BitProcess){throw 'Run 64-bit Windows PowerShell.'}
if(!$BuildDirectory){$BuildDirectory=Join-Path $root 'obj\Build'}
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
$out=Join-Path $root 'bin'
New-Item -ItemType Directory -Path $BuildDirectory,$out -Force | Out-Null
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere)){throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++ and the .NET Framework 4.8 targeting pack.'}
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio 2022 C++ x64 tools are missing.'}
$compiler=Join-Path $vs 'MSBuild\Current\Bin\Roslyn\csc.exe'
$refs=Join-Path ${env:ProgramFiles(x86)} 'Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8'
if(!(Test-Path -LiteralPath $compiler) -or !(Test-Path -LiteralPath (Join-Path $refs 'mscorlib.dll'))){throw 'Install the .NET Framework 4.8 targeting pack and C# build tools.'}

function Compile([string]$Name,[string]$Kind,[string[]]$Sources,[string[]]$ExtraReferences,[string[]]$ExtraArguments=@()){
    $arguments=@('/nologo','/noconfig','/nostdlib+','/langversion:7.3','/platform:x64','/optimize+','/deterministic+','/warn:4','/warnaserror+','/unsafe+','/utf8output','/codepage:65001',('/pathmap:'+$root+'=.'),('/target:'+$Kind),('/out:'+(Join-Path $out $Name)))
    foreach($reference in @('mscorlib.dll','System.dll','System.Core.dll','System.Xml.dll','System.Web.Extensions.dll')+$ExtraReferences){
        $path=if([IO.Path]::IsPathRooted($reference)){$reference}else{Join-Path $refs $reference}
        if(!(Test-Path -LiteralPath $path)){$path=Join-Path (Join-Path $refs 'WPF') $reference}
        if(!(Test-Path -LiteralPath $path)){throw ('Missing build reference: '+$reference)}
        $arguments+=('/reference:'+$path)
    }
    & $compiler @arguments @ExtraArguments @Sources
    if($LASTEXITCODE){throw ('C# build failed: '+$Name)}
}
function RunApp([string[]]$Arguments){
    $process=Start-Process -FilePath (Join-Path $out 'GamePadRemapper.exe') -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    $handle=$process.Handle
    try{
        if(!$process.WaitForExit(120000)){throw ('Application self-test exceeded its deadline; process '+$process.Id)}
        if($process.ExitCode){throw ('Application self-test failed with '+$process.ExitCode)}
    }finally{$process.Dispose()}
}

if($Target -in @('All','Native','Package')){
    $cmakeCommand=Get-Command cmake.exe -ErrorAction SilentlyContinue
    $cmake=if($cmakeCommand){$cmakeCommand.Source}else{Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'}
    & $cmake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 ('-DCMAKE_GENERATOR_INSTANCE='+$vs)
    if($LASTEXITCODE){throw 'CMake configuration failed.'}
    & $cmake --build $BuildDirectory --config Release --parallel
    if($LASTEXITCODE){throw 'Native build failed.'}
}
if($Target -in @('All','Managed','Package')){
    $core=@(Get-ChildItem -LiteralPath (Join-Path $root 'Core') -Filter '*.cs' -File | ForEach-Object FullName)
    $app=@(Get-ChildItem -LiteralPath (Join-Path $root 'App') -Filter '*.cs' -File | ForEach-Object FullName)
    $tests=@(Get-ChildItem -LiteralPath (Join-Path $root 'Tests') -Filter '*.cs' -File | ForEach-Object FullName)
    if(!$core.Count -or !$app.Count -or !$tests.Count){throw 'Application, core or test source is missing.'}
    Compile 'GamePadRemapper.Core.dll' 'library' $core @()
    $coreReference=Join-Path $out 'GamePadRemapper.Core.dll'
    Compile 'RemapperCoreTests.exe' 'exe' $tests @($coreReference)
    $wpf=@('System.Xaml.dll','WindowsBase.dll','PresentationCore.dll','PresentationFramework.dll','System.ServiceProcess.dll','System.Drawing.dll','Microsoft.CSharp.dll',$coreReference)
    Compile 'GamePadRemapper.exe' 'winexe' $app $wpf @(('/win32manifest:'+(Join-Path $root 'App\App.manifest')))
    $scripts=Join-Path $out 'Scripts'
    New-Item -ItemType Directory -Path $scripts -Force | Out-Null
    Get-ChildItem -LiteralPath (Join-Path $root 'Scripts') -Filter '*.igc' -File | Copy-Item -Destination $scripts -Force
}
if($Target -in @('All','Test','Package')){
    & (Join-Path $out 'RemapperNativeTests.exe')
    if($LASTEXITCODE){throw 'Native pure tests failed.'}
    & (Join-Path $out 'RemapperCoreTests.exe') --json (Join-Path $out 'core-tests.json')
    if($LASTEXITCODE){throw 'Language, visual roundtrip or stress tests failed.'}
    RunApp @('--ui-self-test',('"'+(Join-Path $out 'ui-tests.json')+'"'))
}
if($Render){RunApp @('--render-tests',('"'+(Join-Path $root 'docs\images')+'"'))}
if($Target -eq 'Package'){
    & (Join-Path $root 'Tools\Package.ps1') -DriverInstaller $DriverInstaller
}
Write-Host ('Build completed: '+$Target+'. No driver was installed, captured or modified on this computer.')
