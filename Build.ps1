[CmdletBinding()]
param(
    [ValidateSet('All','Build','Test','Package')][string]$Target='All',
    [switch]$Render,
    [switch]$AddressSanitizer,
    [string]$DriverInstaller,
    [string]$BuildDirectory
)
$ErrorActionPreference='Stop'
$root=$PSScriptRoot
if($AddressSanitizer -and $Target -eq 'Package'){throw 'AddressSanitizer builds are for validation only. Package the ordinary Release build.'}
if(![Environment]::Is64BitProcess){throw 'Run 64-bit Windows PowerShell.'}
if(!$BuildDirectory){$BuildDirectory=Join-Path $root $(if($AddressSanitizer){'App\.build\Asan'}else{'App\.build\Release'})}
$BuildDirectory=[IO.Path]::GetFullPath($BuildDirectory)
$out=Join-Path $BuildDirectory 'bin'
$vswhere=Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
if(!(Test-Path -LiteralPath $vswhere)){throw 'Install Visual Studio 2022 Build Tools with Desktop development with C++.'}
$vs=& $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if(!$vs){throw 'Visual Studio 2022 C++ x64 tools are missing.'}
$asanToolsetFolder=$null
$msbuildArguments=@()
if($AddressSanitizer){
    $toolsets=Join-Path $vs 'VC\Tools\MSVC'
    if($Target -eq 'Test'){
        $record=Join-Path $BuildDirectory 'asan-toolset.txt'
        if(!(Test-Path -LiteralPath $record)){throw 'Build the AddressSanitizer configuration before running its tests.'}
        $version=[IO.File]::ReadAllText($record).Trim()
        if($version -notmatch '^\d+\.\d+\.\d+$'){throw 'Invalid AddressSanitizer toolset record.'}
        $asanToolsetFolder=Get-Item -LiteralPath (Join-Path $toolsets $version)
    }else{
        $asanToolsetFolder=Get-ChildItem -LiteralPath $toolsets -Directory | Where-Object {$_.Name -match '^\d+\.\d+\.\d+$' -and [version]$_.Name -ge [version]'14.38.0' -and (Test-Path -LiteralPath (Join-Path $_.FullName 'bin\Hostx64\x64\clang_rt.asan_dynamic-x86_64.dll'))} | Sort-Object {[version]$_.Name} -Descending | Select-Object -First 1
    }
    if(!$asanToolsetFolder){throw 'Install a current Visual Studio 2022 C++ AddressSanitizer component (MSVC 14.38 or newer).'}
    $msbuildArguments=@('--',('/p:VCToolsVersion='+$asanToolsetFolder.Name),'/p:PreferredToolArchitecture=x64')
}
$cmakeCommand=Get-Command cmake.exe -ErrorAction SilentlyContinue
$cmake=if($cmakeCommand){$cmakeCommand.Source}else{Join-Path $vs 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'}
$originalPath=$env:PATH
function UseAsanRuntime {
    if(!$AddressSanitizer){return}
    $runtimeDirectory=Join-Path $asanToolsetFolder.FullName 'bin\Hostx64\x64'
    if(!(Test-Path -LiteralPath (Join-Path $runtimeDirectory 'clang_rt.asan_dynamic-x86_64.dll'))){throw 'Install the C++ AddressSanitizer component for the selected MSVC toolset.'}
    $env:PATH=$runtimeDirectory+';'+$env:PATH
}
try {
if($Target -ne 'Test'){
    & $cmake -S $root -B $BuildDirectory -G 'Visual Studio 17 2022' -A x64 ('-DCMAKE_GENERATOR_INSTANCE='+$vs) ('-DGPR_ASAN='+$(if($AddressSanitizer){'ON'}else{'OFF'}))
    if($LASTEXITCODE){throw 'CMake configuration failed.'}
    UseAsanRuntime
    if($AddressSanitizer){[IO.File]::WriteAllText((Join-Path $BuildDirectory 'asan-toolset.txt'),$asanToolsetFolder.Name)}
    & $cmake --build $BuildDirectory --config Release --parallel @msbuildArguments
    if($LASTEXITCODE){throw 'C++ build failed.'}
    $scripts=Join-Path $out 'Scripts'
    $null=New-Item -ItemType Directory -Path $scripts -Force
    Get-ChildItem -LiteralPath (Join-Path $root 'Scripts') -Filter '*.igc' -File | Copy-Item -Destination $scripts -Force
}
if($Target -eq 'Test'){UseAsanRuntime}
function RunApp([string[]]$Arguments){
    $process=Start-Process -FilePath (Join-Path $out 'GamePadRemapper.exe') -ArgumentList $Arguments -PassThru -WindowStyle Hidden
    $handle=$process.Handle
    try{
        if(!$process.WaitForExit(120000)){throw ('Application test exceeded its deadline; process '+$process.Id)}
        if($process.ExitCode){throw ('Application test failed with '+$process.ExitCode)}
    }finally{$process.Dispose()}
}
if($Target -in @('All','Test','Package')){
    & $cmake --build $BuildDirectory --config Release --target RUN_TESTS @msbuildArguments
    if($LASTEXITCODE){throw 'Capture, language or visual document tests failed.'}
    RunApp @('--ui-self-test',('"'+(Join-Path $out 'ui-tests.json')+'"'))
}
if($Render){RunApp @('--render-tests',('"'+(Join-Path $root 'docs\images')+'"'))}
if($Target -eq 'Package'){
    & (Join-Path $root 'App\Tools\Package.ps1') -BuildDirectory $BuildDirectory -DriverInstaller $DriverInstaller
}
Write-Host ('C++ build completed: '+$Target+'. Output: '+$out+'. No driver was installed or captured.')
}finally{$env:PATH=$originalPath}
