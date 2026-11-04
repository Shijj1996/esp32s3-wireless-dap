# 彻底重置 USBIP 虚拟主机控制器：禁用再启用设备节点（清除僵尸端口）
$ErrorActionPreference = 'Continue'

$dev = Get-PnpDevice -ErrorAction SilentlyContinue |
       Where-Object { $_.InstanceId -like 'ROOT\USB\*' -and $_.FriendlyName -match 'usbip|USBip' }

Write-Host "=== 目标设备 ==="
$dev | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize

foreach ($d in $dev) {
    Write-Host "禁用 $($d.InstanceId) ..."
    Disable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction Continue
    Start-Sleep -Seconds 2
    Write-Host "启用 $($d.InstanceId) ..."
    Enable-PnpDevice -InstanceId $d.InstanceId -Confirm:$false -ErrorAction Continue
}

Start-Sleep -Seconds 4

Write-Host ""
Write-Host "=== 结果 ==="
Get-PnpDevice -ErrorAction SilentlyContinue |
    Where-Object { $_.InstanceId -like 'ROOT\USB\*' -or $_.InstanceId -match '(?i)VID_0?C251' } |
    Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize

Write-Host "=== usbip port ==="
& 'C:\Program Files\USBip\usbip.exe' port 2>&1

Write-Host ""
Write-Host "=== usbip list ==="
& 'C:\Program Files\USBip\usbip.exe' list -r 192.168.137.50 2>&1
