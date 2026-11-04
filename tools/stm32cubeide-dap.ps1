# ============================================================================
#  无线 DAP 启动器（STM32CubeIDE 版）
#
#  固件模式：ESP32-S3 作为 STA 连电脑热点 "ESP32-DAP"，固定 IP 192.168.137.50
#
#  作用：把「usbip 挂载 → OpenOCD(cmsis-dap)」串成一条命令。
#        OpenOCD / 脚本 / GDB 全部自动从 STM32CubeIDE 安装目录里找。
#
#  用法：
#    .\stm32cubeide-dap.ps1                 # 【一条命令】检测 → 挂载 → 启动 OpenOCD
#    .\stm32cubeide-dap.ps1 -Target stm32f1x -Speed 300
#    .\stm32cubeide-dap.ps1 -Mode status    # 只看状态，不做任何动作
#    .\stm32cubeide-dap.ps1 -Mode attach    # 只挂载
#    .\stm32cubeide-dap.ps1 -Mode flash -Elf .\Debug\app.elf
#    .\stm32cubeide-dap.ps1 -Mode detach
#    .\stm32cubeide-dap.ps1 -Mode targets
# ============================================================================

param(
    [ValidateSet('status', 'attach', 'serve', 'flash', 'detach', 'targets')]
    [string]$Mode = 'serve',

    [string]$Target  = 'stm32f1x',
    [int]   $Speed   = 300,
    [string]$Elf,
    [string]$HostIp   = '192.168.137.50',
    [string]$BusId    = '1-1',
    [string]$CubeRoot = 'D:\STM32CubeIDE_1.19.0\STM32CubeIDE',
    [string]$UsbipCli = 'C:\Program Files\USBip\usbip.exe'
)

$ErrorActionPreference = 'Continue'
$DapVid = '0xc251'
$DapPid = '0xf00a'

function Info($m) { Write-Host "[*] $m" -ForegroundColor Cyan }
function Ok($m)   { Write-Host "[+] $m" -ForegroundColor Green }
function Warn($m) { Write-Host "[!] $m" -ForegroundColor Yellow }
function Err($m)  { Write-Host "[x] $m" -ForegroundColor Red }

# ---------------------------------------------------------------- 工具定位 ---
function Find-CubeTool([string]$pattern, [string]$exeName) {
    $dir = Get-ChildItem (Join-Path $CubeRoot 'plugins') -Directory -ErrorAction SilentlyContinue |
           Where-Object { $_.Name -like $pattern } | Select-Object -First 1
    if (-not $dir) { return $null }
    return (Get-ChildItem $dir.FullName -Filter $exeName -Recurse -ErrorAction SilentlyContinue |
            Select-Object -First 1).FullName
}

$OpenOcd = Find-CubeTool '*openocd.win32*' 'openocd.exe'
$Scripts = $null
$dbg = Get-ChildItem (Join-Path $CubeRoot 'plugins') -Directory -ErrorAction SilentlyContinue |
       Where-Object { $_.Name -like '*debug.openocd*' } | Select-Object -First 1
if ($dbg) {
    $cand = Join-Path $dbg.FullName 'resources\openocd\st_scripts'
    if (Test-Path (Join-Path $cand 'interface\cmsis-dap.cfg')) { $Scripts = $cand }
}
$Gdb = Find-CubeTool '*gnu-tools-for-stm32*' 'arm-none-eabi-gdb.exe'

# ---------------------------------------------------------------- 状态检查 ---
function Get-DapDevice {
    # Windows 用 %04X 格式化 VID：0xC251 -> "VID_C251"（没有前导 0）
    Get-PnpDevice -ErrorAction SilentlyContinue |
        Where-Object { $_.InstanceId -match '(?i)VID_0?C251' }
}

function Test-Link {
    return (Test-NetConnection -ComputerName $HostIp -Port 3240 -InformationLevel Quiet -WarningAction SilentlyContinue)
}

function Show-Status {
    Info "探测 $HostIp : 3240 ..."
    if (-not (Test-Link)) {
        Warn "连不上 $HostIp:3240 —— 确认电脑热点 'ESP32-DAP' 已开启、盒子已上电并连上热点"
    }
    Info "远端 $HostIp ..."
    if (-not (Test-Path $UsbipCli)) { Err "找不到 $UsbipCli"; return $false }
    & $UsbipCli list -r $HostIp 2>&1 | ForEach-Object { "    $_" }
    if ($LASTEXITCODE -ne 0) { return $false }

    Info "本地已挂载端口："
    & $UsbipCli port 2>&1 | ForEach-Object { "    $_" }

    $dev = Get-DapDevice
    if ($dev) {
        $dev | Select-Object Status, Class, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String |
            ForEach-Object { $_ -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { "    $_" } }
    } else {
        Warn "尚未看到 VID_C251 设备节点"
    }
    return $true
}

function Ensure-Attached {
    $dev = Get-DapDevice | Where-Object { $_.Status -eq 'OK' }
    if ($dev) { Ok "DAP 已挂载（$($dev.InstanceId)）"; return $true }

    if (-not (Show-Status)) { return $false }

    Info "执行 attach ..."
    & $UsbipCli attach -r $HostIp -b $BusId --receive-mode low-latency 2>&1 | ForEach-Object { "    $_" }

    for ($i = 0; $i -lt 15; $i++) {
        Start-Sleep -Seconds 1
        $dev = Get-DapDevice | Where-Object { $_.Status -eq 'OK' }
        if ($dev) { Ok "挂载成功（$($dev.InstanceId)）"; return $true }
    }

    $dev = Get-DapDevice
    if ($dev) {
        Err "设备节点存在，但状态是 $($dev.Status) —— 链路已断（热点关了？）"
        $dev | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String |
            ForEach-Object { $_ -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { "    $_" } }
    } else {
        Err "attach 后 15 秒仍未出现 VID_C251 设备"
    }
    return $false
}

# ------------------------------------------------------------------ 主流程 ---
if (-not $OpenOcd) { Warn "没找到 CubeIDE 的 openocd.exe（$CubeRoot）" }
if (-not $Scripts) { Warn "没找到 CubeIDE 的 OpenOCD 脚本目录" }

switch ($Mode) {

    'status' { Show-Status | Out-Null }

    'attach' { Ensure-Attached | Out-Null }

    'targets' {
        if (-not $Scripts) { Err '没找到 OpenOCD 脚本目录'; exit 1 }
        Info '可用 target 配置：'
        (Get-ChildItem (Join-Path $Scripts 'target') -Filter 'stm32*.cfg' |
         ForEach-Object { $_.BaseName }) -join ', '
    }

    'detach' {
        # usbip 的 -p 取值是 1..255，不能写 0；从 "Port 01:" 里解析真实端口号
        $num = 1
        $line = & $UsbipCli port 2>&1 | Select-String -Pattern 'Port\s+(\d+)' | Select-Object -First 1
        if ($line -and $line.Matches.Count -gt 0) { $num = [int]$line.Matches[0].Groups[1].Value }
        Info "detach 端口 $num ..."
        & $UsbipCli detach -p $num 2>&1 | ForEach-Object { "    $_" }
        if ($LASTEXITCODE -eq 0) { Ok "已卸载" } else { Err "卸载失败（退出码 $LASTEXITCODE）" }
    }

    'serve' {
        if (-not (Ensure-Attached)) { exit 1 }
        if (-not $OpenOcd -or -not $Scripts) { Err "OpenOCD 不完整"; exit 1 }

        $targetCfg = Join-Path $Scripts "target\$Target.cfg"
        if (-not (Test-Path $targetCfg)) {
            Err "没有 target 配置：$targetCfg"
            (Get-ChildItem (Join-Path $Scripts 'target') -Filter 'stm32*.cfg' |
             ForEach-Object { $_.BaseName }) -join ', '
            exit 1
        }

        # 防重复启动：端口被占说明已经有一个 OpenOCD 在跑
        foreach ($p in 3333, 4444) {
            if (Test-NetConnection -ComputerName 127.0.0.1 -Port $p -InformationLevel Quiet -WarningAction SilentlyContinue) {
                Err "端口 $p 已被占用 —— 可能已经有一个 OpenOCD 在运行"
                Info "先关掉那个窗口；或换端口：-c gdb_port 3334（需要手改脚本）"
                exit 1
            }
        }

        $dev = Get-DapDevice | Where-Object { $_.Status -eq 'OK' } | Select-Object -First 1
        Write-Host ""
        Write-Host "==================================================" -ForegroundColor Cyan
        Write-Host "  无线 DAP 已就绪" -ForegroundColor Green
        Write-Host "--------------------------------------------------" -ForegroundColor Cyan
        Write-Host "  设备   : $($dev.FriendlyName)"
        Write-Host "  GDB    : localhost:3333"
        Write-Host "  Telnet : localhost:4444"
        Write-Host "  目标   : $Target @ ${Speed} kHz"
        Write-Host "  退出   : Ctrl+C"
        Write-Host "==================================================" -ForegroundColor Cyan
        Write-Host ""

        & $OpenOcd -s $Scripts `
            -f "interface/cmsis-dap.cfg" `
            -c "cmsis-dap vid_pid $DapVid $DapPid" `
            -f "target/$Target.cfg" `
            -c "adapter speed $Speed" `
            -c "gdb_port 3333" -c "telnet_port 4444" -c "tcl_port 6666"
        exit $LASTEXITCODE
    }

    'flash' {
        if (-not $Elf) { Err "flash 模式需要 -Elf <文件.elf>"; exit 1 }
        if (-not (Test-Path $Elf)) { Err "文件不存在：$Elf"; exit 1 }
        if (-not (Ensure-Attached)) { exit 1 }
        if (-not $OpenOcd -or -not $Scripts) { Err "OpenOCD 不完整"; exit 1 }

        $full = (Resolve-Path $Elf).Path
        Info "烧录 $full ..."
        # 不用 `program`：它内部会执行 reset init，而这个 reset-init 事件在我们
        # 这条链路上会把目标搞失联（Error connecting DP: cannot read IDR）。
        # init + halt + write_image 不触发 reset-init，实测稳定。
        & $OpenOcd -s $Scripts `
            -f "interface/cmsis-dap.cfg" `
            -c "cmsis-dap vid_pid $DapVid $DapPid" `
            -f "target/$Target.cfg" `
            -c "adapter speed $Speed" `
            -c "init; halt" `
            -c "flash write_image erase {$full}" `
            -c "verify_image {$full}" `
            -c "reset halt" `
            -c "shutdown"
        $rc = $LASTEXITCODE
        if ($rc -eq 0) { Ok "烧录完成" } else { Err "OpenOCD 退出码 $rc" }
        exit $rc
    }
}
