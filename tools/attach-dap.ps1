# ============================================================================
#  一键把无线 DAP 挂到 Windows（usbip-win2 0.9.8.1）
#  用法：先连上 Wi-Fi "DAP" / "12345678"，然后在普通 PowerShell 里执行本脚本
# ============================================================================

param(
    [string]$HostIp = '192.168.137.50',
    [string]$BusId  = '1-1'
)

$ErrorActionPreference = 'Continue'
$cli = 'C:\Program Files\USBip\usbip.exe'

if (-not (Test-Path $cli)) {
    Write-Host "找不到 $cli ，请先安装 usbip-win2" -ForegroundColor Red
    exit 1
}

Write-Host "== 1. 查询远端导出设备 ($HostIp) ==" -ForegroundColor Cyan
& $cli list -r $HostIp
if ($LASTEXITCODE -ne 0) {
    Write-Host ""
    Write-Host "连不上 $HostIp —— 确认电脑热点 'ESP32-DAP' 已开启且盒子已连上" -ForegroundColor Yellow
    exit 1
}

Write-Host ""
Write-Host "== 2. 导入设备 $BusId ==" -ForegroundColor Cyan
& $cli attach -r $HostIp -b $BusId --receive-mode low-latency
if ($LASTEXITCODE -ne 0) {
    Write-Host "attach 失败（可能已经挂载过，先看下面的 port 输出）" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "== 3. 当前已挂载端口 ==" -ForegroundColor Cyan
& $cli port

Write-Host ""
Write-Host "== 4. Windows 设备节点 ==" -ForegroundColor Cyan
Start-Sleep -Seconds 2
$dev = Get-PnpDevice -ErrorAction SilentlyContinue |
       Where-Object { $_.InstanceId -match '(?i)VID_0?C251' -or $_.FriendlyName -match 'CMSIS|DAP' }
if ($dev) {
    $dev | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize
    Write-Host "看到 OK 的 CMSIS-DAP / WinUSB 设备就说明挂载成功。" -ForegroundColor Green
} else {
    Write-Host "暂时没看到 VID_C251 设备，等 3~5 秒再跑一次本脚本的 4 步骤检查。" -ForegroundColor Yellow
}

Write-Host ""
Write-Host "提示：卸载用  & '$cli' detach -p 0" -ForegroundColor DarkGray
