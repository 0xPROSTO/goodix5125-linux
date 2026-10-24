# stage2.ps1 - run elevated inside the VM (fetched from the host over the
# QEMU user network). Installs the Goodix driver from the Microsoft Update
# Catalog package with firmware auto-update disabled and verbose logging on,
# then re-runs gx-run.ps1 (as SYSTEM) to wait for and extract the PSK.

$ErrorActionPreference = 'Continue'
$host_url = 'http://10.0.2.2:8765'

schtasks /end /tn gxrun | Out-Null
Start-Sleep 2

$fp = 'HKLM:\SOFTWARE\Goodix\FP'
New-Item -Path $fp -Force | Out-Null
New-Item -Path "$fp\LogOutput" -Force | Out-Null
New-Item -Path "$fp\DataDump" -Force | Out-Null
# Keep the sensor on its current firmware (12509); the driver carries 12512.
New-ItemProperty -Path $fp -Name FirmwareUpdateSwitch -PropertyType DWord -Value 0 -Force | Out-Null
New-ItemProperty -Path $fp -Name AlgorithmLogSwitch -PropertyType DWord -Value 1 -Force | Out-Null
New-ItemProperty -Path "$fp\LogOutput" -Name LogLevel -PropertyType DWord -Value 9 -Force | Out-Null
New-ItemProperty -Path "$fp\LogOutput" -Name LogTarget -PropertyType DWord -Value 1 -Force | Out-Null
New-ItemProperty -Path "$fp\DataDump" -Name DumpSwitch -PropertyType DWord -Value 1 -Force | Out-Null

$drv = 'C:\gx\drv'
New-Item -ItemType Directory $drv -Force | Out-Null
foreach ($f in 'gfusb.inf', 'gfusb.cat', 'gfusb.dll', 'EngineAdapter.dll', 'AlgoMilan.dll',
               'AlgoChicago.dll', 'AlgoChicagoT.dll', 'GoodixEventLog.dll', 'SessionService.exe') {
    Invoke-WebRequest -UseBasicParsing "$host_url/drv/$f" -OutFile "$drv\$f"
}
Invoke-WebRequest -UseBasicParsing "$host_url/gx-run.ps1" -OutFile 'C:\gx\gx-run.ps1'

pnputil /add-driver "$drv\gfusb.inf" /install | Out-File C:\gx\pnputil.log
schtasks /run /tn gxrun | Out-Null
