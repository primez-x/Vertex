function Set-NativeProcessEnvironment {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.ProcessStartInfo]$StartInfo,
        [Parameter(Mandatory = $true)][System.Collections.IDictionary]$SourceEnvironment,
        [string[]]$PathPrefixes = @(),
        [string]$BasePath = ((@(
            [Environment]::GetEnvironmentVariable('Path', 'Machine'),
            [Environment]::GetEnvironmentVariable('Path', 'User')) |
            Where-Object { ![string]::IsNullOrWhiteSpace($_) }) -join ';')
    )

    $StartInfo.Environment.Clear()
    foreach ($entry in $SourceEnvironment.GetEnumerator()) {
        $name = [string]$entry.Key
        if ($name -ieq 'Path') { continue }
        $StartInfo.Environment[$name] = [string]$entry.Value
    }
    $pathParts = @($PathPrefixes | Where-Object { ![string]::IsNullOrWhiteSpace($_) })
    if (![string]::IsNullOrWhiteSpace($BasePath)) { $pathParts += $BasePath }
    $StartInfo.Environment['Path'] = $pathParts -join ';'
}

function New-NativeProcessGuardResult {
    param(
        [Parameter(Mandatory = $true)][string]$State,
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [object]$ExitCode,
        [Parameter(Mandatory = $true)][bool]$TimedOut,
        [Parameter(Mandatory = $true)][bool]$TerminationConfirmed,
        [string]$Error
    )
    [pscustomobject]@{
        State = $State
        ProcessId = $ProcessId
        ExitCode = $ExitCode
        TimedOut = $TimedOut
        TerminationConfirmed = $TerminationConfirmed
        Error = $Error
    }
}

function Complete-NativeProcessGuard {
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][int]$ProcessId,
        [Parameter(Mandatory = $true)][int]$DeadlineMilliseconds,
        [Parameter(Mandatory = $true)][int]$TerminationWaitMilliseconds,
        [bool]$TimedOut,
        [object]$WaitError
    )

    $reason = if ($TimedOut) {
        "deadline of $DeadlineMilliseconds ms expired without a confirmed exit"
    }
    else {
        "WaitForExit failed: $($WaitError.Message)"
    }
    $inspectionErrors = [System.Collections.Generic.List[string]]::new()

    $alreadyExited = $false
    try { $alreadyExited = $Process.HasExited }
    catch { [void]$inspectionErrors.Add("initial HasExited check failed: $($_.Exception.Message)") }
    if ($alreadyExited) {
        $exitCode = $null
        try { $exitCode = $Process.ExitCode }
        catch { [void]$inspectionErrors.Add("ExitCode read failed: $($_.Exception.Message)") }
        $state = if ($TimedOut) { 'AlreadyExited' } else { 'WaitFailed' }
        $detail = if ($TimedOut) {
            "$reason; the process exited during timeout cleanup"
        }
        else {
            "$reason; process exit was observed while handling the wait failure"
        }
        if ($inspectionErrors.Count -gt 0) { $detail += "; " + ($inspectionErrors -join '; ') }
        return New-NativeProcessGuardResult -State $state -ProcessId $ProcessId -ExitCode $exitCode `
            -TimedOut:$TimedOut -TerminationConfirmed:$true -Error $detail
    }

    $killError = $null
    try { $Process.Kill() }
    catch { $killError = $_.Exception }

    $confirmed = $false
    $confirmationError = $null
    if ($null -eq $killError) {
        try { $confirmed = $Process.WaitForExit($TerminationWaitMilliseconds) }
        catch { $confirmationError = $_.Exception }
    }

    # A child can exit just after the bounded wait or between Kill and the
    # final state check. Read the retained handle once more to resolve that
    # race, but never claim confirmation without this check or WaitForExit.
    $exitedAfterCleanup = $false
    try { $exitedAfterCleanup = $Process.HasExited }
    catch { [void]$inspectionErrors.Add("final HasExited check failed: $($_.Exception.Message)") }
    if ($confirmed -or $exitedAfterCleanup) {
        $exitCode = $null
        try { $exitCode = $Process.ExitCode }
        catch { [void]$inspectionErrors.Add("ExitCode read failed: $($_.Exception.Message)") }
        $state = if (!$TimedOut) { 'WaitFailed' }
        elseif ($null -ne $killError) { 'AlreadyExited' }
        else { 'Killed' }
        $detail = if ($TimedOut) {
            "$reason; retained-handle termination was confirmed"
        }
        else {
            "$reason; retained-handle cleanup termination was confirmed"
        }
        if ($confirmationError) { $detail += "; bounded termination wait failed: $($confirmationError.Message)" }
        if ($inspectionErrors.Count -gt 0) { $detail += "; " + ($inspectionErrors -join '; ') }
        return New-NativeProcessGuardResult -State $state -ProcessId $ProcessId -ExitCode $exitCode `
            -TimedOut:$TimedOut -TerminationConfirmed:$true -Error $detail
    }

    # Kill itself can lose a race with natural exit. If the final check still
    # cannot confirm exit, report the deadline separately from the inability to
    # terminate this exact retained process handle.
    $errorParts = [System.Collections.Generic.List[string]]::new()
    [void]$errorParts.Add($reason)
    if ($killError) { [void]$errorParts.Add("retained-handle Kill failed: $($killError.Message)") }
    if ($confirmationError) { [void]$errorParts.Add("bounded termination wait failed: $($confirmationError.Message)") }
    if ($null -eq $killError -and $null -eq $confirmationError) {
        [void]$errorParts.Add("termination was not confirmed within $TerminationWaitMilliseconds ms")
    }
    if ($inspectionErrors.Count -gt 0) { [void]$errorParts.Add(($inspectionErrors -join '; ')) }
    $state = if ($TimedOut) { 'TerminationFailed' } else { 'WaitFailed' }
    return New-NativeProcessGuardResult -State $state -ProcessId $ProcessId -ExitCode $null `
        -TimedOut:$TimedOut -TerminationConfirmed:$false -Error ($errorParts -join '; ')
}

function Wait-NativeProcess {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)][System.Diagnostics.Process]$Process,
        [Parameter(Mandatory = $true)][ValidateNotNullOrEmpty()][string]$Description,
        [ValidateRange(1, 2147483647)][int]$DeadlineMilliseconds = 15000,
        [ValidateRange(1, 2147483647)][int]$TerminationWaitMilliseconds = 5000
    )

    $processId = 0
    try { $processId = $Process.Id }
    catch {
        return New-NativeProcessGuardResult -State 'HandleFailed' -ProcessId $processId -ExitCode $null `
            -TimedOut:$false -TerminationConfirmed:$false `
            -Error "$Description process handle has no usable process ID: $($_.Exception.Message)"
    }

    $waitError = $null
    $exited = $false
    try { $exited = $Process.WaitForExit($DeadlineMilliseconds) }
    catch { $waitError = $_.Exception }

    if ($null -eq $waitError -and $exited) {
        $exitCode = $null
        try { $exitCode = $Process.ExitCode }
        catch {
            return New-NativeProcessGuardResult -State 'HandleFailed' -ProcessId $processId -ExitCode $null `
                -TimedOut:$false -TerminationConfirmed:$true `
                -Error "$Description exited, but its exit code could not be read: $($_.Exception.Message)"
        }
        return New-NativeProcessGuardResult -State 'Exited' -ProcessId $processId -ExitCode $exitCode `
            -TimedOut:$false -TerminationConfirmed:$true -Error $null
    }

    if ($null -ne $waitError) {
        # A wait API failure is not a deadline. Keep the failure reason even if
        # the retained handle can be used to clean up the child successfully.
        return Complete-NativeProcessGuard -Process $Process -ProcessId $processId `
            -DeadlineMilliseconds $DeadlineMilliseconds -TerminationWaitMilliseconds $TerminationWaitMilliseconds `
            -TimedOut:$false -WaitError $waitError
    }

    # WaitForExit returned false, so the real deadline expired. Cleanup remains
    # bounded and operates only through the exact Process handle supplied by
    # the caller.
    return Complete-NativeProcessGuard -Process $Process -ProcessId $processId `
        -DeadlineMilliseconds $DeadlineMilliseconds -TerminationWaitMilliseconds $TerminationWaitMilliseconds `
        -TimedOut:$true
}
