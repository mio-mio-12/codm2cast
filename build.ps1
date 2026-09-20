param([string] $Target = 'all', [string] $StudioOutputName = 'codm2cast_v12')
$ErrorActionPreference = 'Stop'
$buildTemp = Join-Path $PSScriptRoot 'work\temp'
New-Item -ItemType Directory -Force -Path $buildTemp | Out-Null
$env:TEMP = $buildTemp
$env:TMP = $buildTemp
$env:MSBUILDDISABLENODEREUSE = '1'
$studio = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products '*' -property installationPath
$cmake = Join-Path $studio 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
function Invoke-BuildTool([string[]] $arguments) {
    $info = [System.Diagnostics.ProcessStartInfo]::new($cmake)
    $info.UseShellExecute = $false
    $info.CreateNoWindow = $true
    $info.RedirectStandardOutput = $true
    $info.RedirectStandardError = $true
    $info.Environment.Clear()
    foreach ($entry in [Environment]::GetEnvironmentVariables().GetEnumerator()) { $info.Environment[$entry.Key.ToUpperInvariant()] = $entry.Value }
    foreach ($argument in $arguments) { $info.ArgumentList.Add($argument) }
    $process = [System.Diagnostics.Process]::Start($info)
    $stdout = $process.StandardOutput.ReadToEndAsync()
    $stderr = $process.StandardError.ReadToEndAsync()
    $process.WaitForExit()
    Write-Output $stdout.GetAwaiter().GetResult()
    Write-Output $stderr.GetAwaiter().GetResult()
    if ($process.ExitCode) { throw "Build tool failed with exit code $($process.ExitCode)" }
}
Invoke-BuildTool @('-S', $PSScriptRoot, '-B', "$PSScriptRoot\work\build", '-G', 'Visual Studio 18 2026', '-A', 'x64', "-DCODM_STUDIO_OUTPUT_NAME=$StudioOutputName")
$buildArguments = @('--build', "$PSScriptRoot\work\build", '--config', 'Release', '--parallel', '4')
if ($Target -ne 'all') { $buildArguments += @('--target', $Target) }
Invoke-BuildTool $buildArguments
Invoke-BuildTool @('-E', 'chdir', "$PSScriptRoot\work\build", "$studio\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\ctest.exe", '-C', 'Release', '--output-on-failure')

