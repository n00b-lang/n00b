#Requires -Version 7.0

param(
    [string]$N00b = '.\n00b.exe',

    [string]$Transcript,
    [ValidateRange(1, 86400)]
    [int]$StepTimeoutSeconds = 180,
    [switch]$SkipRun
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$BundleDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$N00bPath = (Resolve-Path -LiteralPath $N00b).Path

if ($Transcript) {
    if ([System.IO.Path]::IsPathRooted($Transcript)) {
        $TranscriptPath = $Transcript
    }
    else {
        $TranscriptPath = Join-Path $BundleDir $Transcript
    }
}
else {
    $TranscriptPath = Join-Path $BundleDir 'windows-smoke-transcript.txt'
}

$env:N00B_LIB_DIR = $BundleDir
$env:N00B_RESHARP_TEST_DIR = Join-Path $BundleDir 'test-data\resharp\tests'
$env:PATH = "$BundleDir;$env:PATH"

$Results = [System.Collections.Generic.List[object]]::new()

function Invoke-Step {
    [CmdletBinding(PositionalBinding = $false)]
    param(
        [Parameter(Mandatory = $true, Position = 0)]
        [string]$Exe,

        [ValidateRange(1, 86400)]
        [int]$TimeoutSeconds = $StepTimeoutSeconds,

        [Parameter(Position = 1, ValueFromRemainingArguments = $true)]
        [string[]]$Args
    )

    $Display = @($Exe) + $Args
    $Name = $Display -join ' '
    Write-Host ''
    Write-Host ('>> ' + $Name)

    $Output = $null
    $ExitCode = 0
    $ErrorMessage = $null

    $TimedOut = $false
    $Process = $null

    try {
        $Process = [System.Diagnostics.Process]::new()
        $Process.StartInfo.FileName = $Exe
        $Process.StartInfo.UseShellExecute = $false
        $Process.StartInfo.RedirectStandardOutput = $true
        $Process.StartInfo.RedirectStandardError = $true
        foreach ($Arg in $Args) {
            $Process.StartInfo.ArgumentList.Add($Arg) | Out-Null
        }

        $Process.Start() | Out-Null
        $StdoutTask = $Process.StandardOutput.ReadToEndAsync()
        $StderrTask = $Process.StandardError.ReadToEndAsync()
        if (-not $Process.WaitForExit($TimeoutSeconds * 1000)) {
            $TimedOut = $true
            $ExitCode = -1
            $ErrorMessage = "Timed out after $TimeoutSeconds seconds"
            Write-Host "TIMEOUT: $Name exceeded $TimeoutSeconds seconds"
            try {
                $Process.Kill($true)
            }
            catch {
                $Process.Kill()
            }
            $Process.WaitForExit()
        }
        else {
            $ExitCode = $Process.ExitCode
        }

        $Stdout = $StdoutTask.GetAwaiter().GetResult()
        $Stderr = $StderrTask.GetAwaiter().GetResult()
        foreach ($Line in ($Stdout -split "`r?`n")) {
            if ($Line.Length -gt 0) {
                Write-Host $Line
            }
        }
        foreach ($Line in ($Stderr -split "`r?`n")) {
            if ($Line.Length -gt 0) {
                Write-Host $Line
            }
        }
    }
    catch {
        $ExitCode = -1
        $ErrorMessage = $_.Exception.Message
        Write-Host $ErrorMessage
    }
    finally {
        if ($Process) {
            $Process.Dispose()
        }
    }

    $Passed = (($ExitCode -eq 0) -and (-not $TimedOut))
    $Results.Add([pscustomobject]@{
        Name = $Name
        ExitCode = $ExitCode
        Passed = $Passed
        Error = $ErrorMessage
    }) | Out-Null

    if (-not $Passed) {
        Write-Host "FAILED: $Name exited with $ExitCode"
    }
}

function Invoke-StepExpectFailure {
    param(
        [Parameter(Mandatory = $true)]
        [string]$Name,

        [Parameter(Mandatory = $true)]
        [string]$Exe,

        [Parameter(Mandatory = $true)]
        [string[]]$Args,

        [Parameter(Mandatory = $true)]
        [string]$ExpectedPattern
    )

    $Display = @($Exe) + $Args
    Write-Host ''
    Write-Host ('>> ' + $Name)
    Write-Host ('   ' + ($Display -join ' '))

    $Output = $null
    $ExitCode = 0
    $ErrorMessage = $null

    try {
        $Output = & $Exe @Args 2>&1
        $ExitCode = $LASTEXITCODE

        foreach ($Line in $Output) {
            Write-Host $Line
        }
    }
    catch {
        $ExitCode = -1
        $ErrorMessage = $_.Exception.Message
        Write-Host $ErrorMessage
    }

    $OutputText = (($Output | ForEach-Object { $_.ToString() }) -join "`n")
    if ($ErrorMessage) {
        $OutputText = ($OutputText + "`n" + $ErrorMessage)
    }

    $Passed = (($ExitCode -ne 0) -and ($OutputText -match $ExpectedPattern))
    $Results.Add([pscustomobject]@{
        Name = $Name
        ExitCode = $ExitCode
        Passed = $Passed
        Error = $ErrorMessage
    }) | Out-Null

    if (-not $Passed) {
        Write-Host "FAILED: $Name did not fail with the expected unsupported message"
    }
}

function Write-Summary {
    $Failures = @($Results | Where-Object { -not $_.Passed })

    Write-Host ''
    Write-Host 'Smoke summary:'
    Write-Host "  Passed: $($Results.Count - $Failures.Count)"
    Write-Host "  Failed: $($Failures.Count)"

    if ($Failures.Count -gt 0) {
        Write-Host ''
        Write-Host 'Failures:'
        foreach ($Failure in $Failures) {
            Write-Host "  [$($Failure.ExitCode)] $($Failure.Name)"
            if ($Failure.Error) {
                Write-Host "      $($Failure.Error)"
            }
        }

        throw "$($Failures.Count) smoke step(s) failed"
    }
}

$TestNames = @(
    'test_array.exe',
    'test_list.exe',
    'test_stack.exe',
    'test_result.exe',
    'test_variant.exe',
    'test_tuple.exe',
    'test_tree.exe',
    'test_buffer.exe',
    'test_dict.exe',
    'test_type_registry.exe',
    'test_unicode_bidi.exe',
    'test_unicode_casemap.exe',
    'test_unicode_collation.exe',
    'test_unicode_emoji.exe',
    'test_unicode_encoding.exe',
    'test_unicode_identifiers.exe',
    'test_unicode_idna.exe',
    'test_unicode_iter.exe',
    'test_unicode_linebreak.exe',
    'test_unicode_normalization.exe',
    'test_unicode_properties.exe',
    'test_unicode_security.exe',
    'test_unicode_segmentation.exe',
    'test_regex_charset.exe',
    'test_regex_parse.exe',
    'test_regex_match.exe',
    'test_regex_api.exe',
    'test_regex_resharp.exe',
    'test_io.exe',
    'test_io_windows.exe',
    'test_signal.exe',
    'test_proc_lifecycle.exe',
    'test_file_change.exe',
    'test_subproc.exe',
    'test_objfile_pe.exe',
    'test_hexdump.exe',
    'test_print.exe',
    'test_fd_managed.exe',
    'test_socket.exe',
    'test_vfs_local.exe',
    'test_vfs_journal.exe'
)

$WorkDir = Join-Path $BundleDir '.windows-smoke'
$TestsDir = Join-Path $BundleDir 'tests'
$Hello = Join-Path $BundleDir 'hello.n'

$TranscriptStarted = $false
$LocationPushed = $false
if (Test-Path -LiteralPath $WorkDir) {
    Remove-Item -LiteralPath $WorkDir -Recurse -Force
}
New-Item -ItemType Directory -Path $WorkDir | Out-Null

try {
    Start-Transcript -Path $TranscriptPath -Force | Out-Null
    $TranscriptStarted = $true

    Push-Location -LiteralPath $BundleDir
    $LocationPushed = $true

    Write-Host "Bundle directory: $BundleDir"
    Write-Host "Transcript path: $TranscriptPath"
    Write-Host "Working directory: $((Get-Location).Path)"
    Write-Host "Using N00B_LIB_DIR=$env:N00B_LIB_DIR"

    Invoke-Step $N00bPath '--help'

    foreach ($Name in $TestNames) {
        Invoke-Step (Join-Path $TestsDir $Name)
    }

    if (-not $SkipRun) {
        Invoke-Step $N00bPath 'run' $Hello
    }

    $UnsupportedCompileExe = Join-Path $WorkDir 'unsupported-compile.exe'
    Invoke-StepExpectFailure `
        -Name 'compile unsupported smoke' `
        -Exe $N00bPath `
        -Args @('compile', '-o', $UnsupportedCompileExe, $Hello) `
        -ExpectedPattern 'compile mode is not supported on Windows yet|Windows linking is not supported'

    Write-Summary
}
finally {
    if ($LocationPushed) {
        Pop-Location
    }

    if (Test-Path -LiteralPath $WorkDir) {
        Remove-Item -LiteralPath $WorkDir -Recurse -Force
    }

    if ($TranscriptStarted) {
        Stop-Transcript | Out-Null
        Write-Host "Transcript saved to $TranscriptPath"
    }
}
