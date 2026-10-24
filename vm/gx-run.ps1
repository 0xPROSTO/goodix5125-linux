# gx-run.ps1 - runs as SYSTEM inside the throwaway Windows VM (scheduled task,
# on every boot). Installs the Goodix driver from Windows Update, waits for the
# driver to provision the sensor PSK, unseals it onto the GXSETUP stick, then
# powers the VM off. Progress goes to COM1 (host: serial.log) and the stick.

$ErrorActionPreference = 'Continue'
$stick = (Get-Volume -FileSystemLabel GXSETUP -ErrorAction SilentlyContinue).DriveLetter
$stick = if ($stick) { "${stick}:" } else { 'C:\gx' }
$cache = Join-Path $env:ProgramData 'Goodix\Goodix_Cache.bin'

$com = $null
try { $com = New-Object System.IO.Ports.SerialPort COM1, 115200; $com.Open() } catch { $com = $null }

function Log([string]$m) {
    $line = "[{0}] {1}" -f (Get-Date -Format 'HH:mm:ss'), $m
    Add-Content -Path "$stick\gx-log.txt" -Value $line -ErrorAction SilentlyContinue
    if ($com) { try { $com.WriteLine($line) } catch {} }
}

function Show-Goodix {
    Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -like 'USB\VID_27C6*' -or $_.Class -eq 'Biometric' } |
        ForEach-Object { Log ("  dev: {0} | {1} | {2} | {3}" -f $_.Status, $_.Class, $_.FriendlyName, $_.InstanceId) }
    $svc = Get-Service WbioSrvc -ErrorAction SilentlyContinue
    if ($svc) { Log "  WbioSrvc: $($svc.Status)" }
}

function Finish {
    # Stay up: the host decides when to power off (it may want more traces).
    Log 'DONE'
    if ($com) { $com.Close() }
    exit 0
}

Log "gx-run start, stick=$stick, user=$([Security.Principal.WindowsIdentity]::GetCurrent().Name)"
powercfg /change standby-timeout-ac 0 | Out-Null
powercfg /change monitor-timeout-ac 0 | Out-Null

if (Test-Path "$stick\psk.bin") { Log 'psk.bin already extracted'; Finish }

Show-Goodix
if (Test-Path C:\gx\pnputil.log) {
    Get-Content C:\gx\pnputil.log | Where-Object { $_.Trim() } | ForEach-Object { Log "  pnputil: $_" }
}
$goodixOk = [bool](Get-PnpDevice -PresentOnly -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'USB\VID_27C6*' -and $_.Status -eq 'OK' })

# 1. Network.
for ($i = 0; $i -lt 60; $i++) {
    if (Resolve-DnsName www.catalog.update.microsoft.com -ErrorAction SilentlyContinue) { break }
    Start-Sleep 10
}
Log "network: $([bool](Resolve-DnsName www.catalog.update.microsoft.com -ErrorAction SilentlyContinue))"

# 2. Drivers from Windows Update (only if the cache is not there yet).
if (-not (Test-Path $cache) -and -not $goodixOk) {
    try {
        $session = New-Object -ComObject Microsoft.Update.Session
        $searcher = $session.CreateUpdateSearcher()
        Log 'searching Windows Update for drivers...'
        $res = $searcher.Search("IsInstalled=0 and Type='Driver'")
        $todo = New-Object -ComObject Microsoft.Update.UpdateColl
        foreach ($u in $res.Updates) {
            Log "  found: $($u.Title)"
            if (-not $u.EulaAccepted) { $u.AcceptEula() }
            [void]$todo.Add($u)
        }
        if ($todo.Count -gt 0) {
            $dl = $session.CreateUpdateDownloader(); $dl.Updates = $todo; [void]$dl.Download()
            $inst = $session.CreateUpdateInstaller(); $inst.Updates = $todo
            $r = $inst.Install()
            Log "  install result=$($r.ResultCode) reboot=$($r.RebootRequired)"
            if ($r.RebootRequired) { Log 'rebooting for driver'; Restart-Computer -Force; exit 0 }
        } else {
            Log '  no pending drivers'
        }
    } catch {
        Log "WU error: $($_.Exception.Message)"
    }
    Restart-Service WbioSrvc -Force -ErrorAction SilentlyContinue
}

# 3. Wait for the driver to provision the sensor.
for ($i = 0; $i -lt 90 -and -not (Test-Path $cache); $i++) {
    if ($i % 6 -eq 0) { Show-Goodix }
    Start-Sleep 10
}

if (-not (Test-Path $cache)) {
    Log 'Goodix_Cache.bin did not appear - needs manual step (Windows Hello enrollment?)'
    Show-Goodix
    Get-ChildItem "$env:ProgramData\Goodix" -Recurse -ErrorAction SilentlyContinue |
        ForEach-Object { Log "  file: $($_.FullName) $($_.Length)" }
    Log 'WAITING'
    exit 1
}

Log "cache present: $((Get-Item $cache).Length) bytes"

# 4. Unseal. SYSTEM first, then LOCAL SERVICE (the WBF service account).
$out = "$stick\psk.bin"
& powershell -NoProfile -ExecutionPolicy Bypass -File C:\gx\extract-psk.ps1 -Out $out 2>&1 |
    ForEach-Object { Log "  [SYSTEM] $_" }

if (-not (Test-Path $out)) {
    $tmp = 'C:\gx\out'
    New-Item -ItemType Directory $tmp -Force | Out-Null
    icacls $tmp /grant '*S-1-5-19:(OI)(CI)F' | Out-Null
    $action = New-ScheduledTaskAction -Execute 'powershell.exe' -Argument "-NoProfile -ExecutionPolicy Bypass -Command `"& C:\gx\extract-psk.ps1 -Out $tmp\psk.bin *> $tmp\ls.log`""
    Register-ScheduledTask -TaskName gxls -Action $action -User 'NT AUTHORITY\LOCALSERVICE' -Force | Out-Null
    Start-ScheduledTask -TaskName gxls
    Start-Sleep 20
    Get-Content "$tmp\ls.log" -ErrorAction SilentlyContinue | ForEach-Object { Log "  [LOCALSERVICE] $_" }
    Unregister-ScheduledTask -TaskName gxls -Confirm:$false
    if (Test-Path "$tmp\psk.bin") { Move-Item "$tmp\psk.bin" $out -Force }
}

if (Test-Path $out) {
    Log 'PSK EXTRACTED'
    Finish
}
Log 'unseal failed'
Log 'WAITING'
