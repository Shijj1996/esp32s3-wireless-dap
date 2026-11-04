# ============================================================================
#  无线 DAP 启动器（STM32CubeIDE 版）
#
#  固件模式：ESP32-S3 作为 STA 连电脑热点 "ESP32-DAP"，固定 IP 192.168.137.50
#
#  作用：把「usbip 挂载 → OpenOCD(cmsis-dap)」串成一条命令。
#        OpenOCD / 脚本 / GDB 全部自动从 STM32CubeIDE 安装目录里找。
#
#  用法：
#    .\stm32cubeide-dap.ps1                     # 【一条命令】检测 → 挂载 → 启动 OpenOCD
#    .\stm32cubeide-dap.ps1 -Target stm32f1x -Speed 300
#
#    # 烧录（支持 .elf / .hex / .bin）
#    .\stm32cubeide-dap.ps1 -Mode flash -Image app.elf
#    .\stm32cubeide-dap.ps1 -Mode flash -Image app.hex
#    .\stm32cubeide-dap.ps1 -Mode flash -Image app.bin -Address 0x08000000
#    .\stm32cubeide-dap.ps1 -Mode flash -Image app.elf -Run          # 烧完立即运行
#    .\stm32cubeide-dap.ps1 -Mode flash -Image app.elf -ChipErase    # 先整片擦除
#    .\stm32cubeide-dap.ps1 -Mode flash -Image app.elf -NoVerify     # 跳过校验
#
#    # 擦除 / 回读
#    .\stm32cubeide-dap.ps1 -Mode erase                          # 整片擦除
#    .\stm32cubeide-dap.ps1 -Mode dump -DumpFile .\readback.bin  # 整片读回
#
#    .\stm32cubeide-dap.ps1 -Mode status    # 只看状态，不做任何动作
#    .\stm32cubeide-dap.ps1 -Mode attach    # 只挂载
#    .\stm32cubeide-dap.ps1 -Mode detach
#    .\stm32cubeide-dap.ps1 -Mode targets
# ============================================================================

param(
    [ValidateSet('status', 'attach', 'serve', 'flash', 'erase', 'dump', 'detach', 'targets')]
    [string]$Mode = 'serve',

    [string]$Target  = 'stm32f1x',
    [int]   $Speed   = 300,

    # ------------------------------ 烧录相关 ------------------------------
    [Alias('Elf')]
    [string]$Image,                    # 镜像文件：.elf / .hex / .bin（-Elf 仍兼容）
    [string]$Address = '0x08000000',   # .bin 的烧录基地址
    [switch]$Run,                      # 烧录后立即运行
    [switch]$NoVerify,                 # 跳过校验（默认校验）
    [switch]$ChipErase,                # 烧录前整片擦除

    # ------------------------------ 回读相关 ------------------------------
    [string]$DumpFile,                 # 回读输出路径，默认 .\flash_dump.bin
    [string]$Size,                     # 回读长度，默认自动探测

    [switch]$DryRun,                   # 只打印将要执行的 OpenOCD 命令，不碰硬件

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

    # 早失败：连不上盒子就别往下走 attach 了
    if (-not (Test-Link)) {
        Err "连不上 $HostIp:3240 —— 盒子没上电 / 没连上热点 / 电脑热点没开"
        Warn "检查顺序：热点 'ESP32-DAP' 是否开启 → 盒子是否插电 → 串口是否有"
        Warn "  'wireless DAP ready at 192.168.137.50:3240' 这行日志"
        return $false
    }

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
        Err "设备节点存在，但状态是 $($dev.Status) —— 链路已断（盒子重启过？热点关了？）"
        $dev | Select-Object Status, FriendlyName, InstanceId | Format-Table -AutoSize | Out-String |
            ForEach-Object { $_ -split "`n" | Where-Object { $_.Trim() } | ForEach-Object { "    $_" } }
        Warn "驱动端口僵死时：管理员运行 tools\reset-usbip.ps1；或重新插拔盒子 USB"
    } else {
        Err "attach 后 15 秒仍未出现 VID_C251 设备"
    }
    return $false
}

# --------------------------------------------------- OpenOCD 调用封装 ---
function To-OcdPath([string]$p) {
    # OpenOCD 的 Tcl 会把反斜杠当转义（\f \n \t 会被吃掉），统一转正斜杠
    return ($p -replace '\\', '/')
}

function Get-OcdArgs {
    return @(
        '-s', $Scripts,
        '-f', 'interface/cmsis-dap.cfg',
        '-c', "cmsis-dap vid_pid $DapVid $DapPid",
        '-f', "target/$Target.cfg",
        '-c', "adapter speed $Speed"
    )
}

function Invoke-Ocd {
    param([string[]]$Commands)
    $argv = Get-OcdArgs
    foreach ($c in $Commands) { $argv += @('-c', $c) }
    return (& $OpenOcd @argv 2>&1 | Out-String)
}

function Show-OcdResult {
    param([string]$Out, [string]$Filter)
    ($Out -split "`r?`n") |
        Where-Object { $_ -match $Filter } |
        ForEach-Object { "    $($_.Trim())" }
}

function Show-OcdPlan {
    param([string[]]$Commands)
    Info "将执行以下 OpenOCD 命令（-DryRun，不接触硬件）："
    $argv = Get-OcdArgs
    Write-Host "  $OpenOcd"
    Write-Host "  $($argv -join ' ') \\"
    foreach ($c in $Commands) { Write-Host "  -c `"$c`"" }
}

function Get-FlashInfo {
    # 解析 "#0 : stm32f1x.flash (stm32f1x) at 0x08000000, size 0x00040000, ..."
    $out = Invoke-Ocd @('init', 'flash banks', 'shutdown')
    $m = [regex]::Match($out, 'at\s+(0x[0-9A-Fa-f]+),\s*size\s+(0x[0-9A-Fa-f]+)')
    if ($m.Success) {
        return @{
            Base = [Convert]::ToUInt32($m.Groups[1].Value, 16)
            Size = [Convert]::ToUInt32($m.Groups[2].Value, 16)
        }
    }
    return $null
}

function Get-MassEraseCmd {
    # 各系列的全片擦除命令不同
    $map = [ordered]@{
        'stm32f1' = 'stm32f1x mass_erase 0'
        'stm32f2' = 'stm32f2x mass_erase 0'
        'stm32f4' = 'stm32f4x mass_erase 0'
        'stm32f7' = 'stm32f7x mass_erase 0'
        'stm32l0' = 'stm32lx mass_erase 0'
        'stm32l1' = 'stm32lx mass_erase 0'
        'stm32l4' = 'stm32l4x mass_erase 0'
        'stm32g0' = 'stm32g0x mass_erase 0'
        'stm32g4' = 'stm32g4x mass_erase 0'
        'stm32wb' = 'stm32wbx mass_erase 0'
        'stm32h7' = 'stm32h7x mass_erase 0'
    }
    foreach ($k in $map.Keys) {
        if ($Target.StartsWith($k)) { return $map[$k] }
    }
    return $null
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
        if (-not $Image) { Err "flash 模式需要 -Image <.elf|.hex|.bin>（-Elf 也可）"; exit 1 }
        if (-not (Test-Path $Image)) { Err "文件不存在：$Image"; exit 1 }
        if (-not $OpenOcd -or -not $Scripts) { Err "OpenOCD 不完整"; exit 1 }

        $full = (Resolve-Path $Image).Path
        $ocdPath = To-OcdPath $full
        $ext = [IO.Path]::GetExtension($full).ToLowerInvariant()

        switch ($ext) {
            '.elf' { $kind = 'elf' }
            '.hex' { $kind = 'ihex' }
            '.bin' { $kind = 'bin' }
            default {
                Err "不支持的扩展名：$ext（只支持 .elf / .hex / .bin）"
                exit 1
            }
        }
        if ($kind -eq 'bin' -and -not $Address) { Err ".bin 必须用 -Address 指定基地址（默认 0x08000000）"; exit 1 }

        # 不用 program：它内部的 reset-init 会在这条链路上把目标搞失联
        # （Error connecting DP: cannot read IDR）。init + halt + write_image 稳定。
        $cmds = @('init; halt')

        if ($ChipErase) {
            $me = Get-MassEraseCmd
            if (-not $me) {
                Err "不知道 $Target 的整片擦除命令；可去掉 -ChipErase，或先用 -Mode erase"
                exit 1
            }
            Info "整片擦除（$me）"
            $cmds += $me
        }

        Info "烧录 $full"
        if ($kind -eq 'bin') {
            Info "  格式 bin，基地址 $Address"
            $cmds += "flash write_image erase {$ocdPath} $Address bin"
        } else {
            Info "  格式 $($ext.TrimStart('.'))"
            $cmds += "flash write_image erase {$ocdPath}"
        }

        if (-not $NoVerify) {
            Info "写入后校验"
            if ($kind -eq 'bin') { $cmds += "verify_image {$ocdPath} $Address bin" }
            else { $cmds += "verify_image {$ocdPath}" }
        }

        if ($Run) { Info "烧录后复位并运行"; $cmds += 'reset run' }
        else { Info "烧录后停在 halt"; $cmds += 'reset halt' }
        $cmds += 'shutdown'

        if ($DryRun) { Show-OcdPlan $cmds; exit 0 }
        if (-not (Ensure-Attached)) { exit 1 }

        $out = Invoke-Ocd $cmds
        Show-OcdResult $out 'wrote |verified|erased|halted|Running|Error|error:'

        $bad = ($out -match '(?m)^Error') -or ($out -match 'failed')
        if ($bad) { Err "烧录失败（见上方 OpenOCD 输出）"; exit 1 }
        if ($Run) { Ok "烧录完成，目标已运行" } else { Ok "烧录完成，目标停在复位后 halt 状态" }
        exit 0
    }

    'erase' {
        if (-not $OpenOcd -or -not $Scripts) { Err "OpenOCD 不完整"; exit 1 }
        $me = Get-MassEraseCmd
        if (-not $me) {
            Err "不知道 $Target 的整片擦除命令"
            exit 1
        }
        if ($DryRun) { Show-OcdPlan @('init; halt', $me, 'shutdown'); exit 0 }
        if (-not (Ensure-Attached)) { exit 1 }

        Info "整片擦除（$me）..."
        $out = Invoke-Ocd @('init; halt', $me, 'shutdown')
        Show-OcdResult $out 'mass erase|erased|Error|error:'
        if ($out -match 'mass erase complete|erased') { Ok "整片擦除完成"; exit 0 }
        Err "整片擦除失败"; exit 1
    }

    'dump' {
        if (-not $OpenOcd -or -not $Scripts) { Err "OpenOCD 不完整"; exit 1 }
        if (-not $DumpFile) { $DumpFile = Join-Path (Get-Location) 'flash_dump.bin' }
        $DumpFile = [IO.Path]::GetFullPath($DumpFile)
        $dir = Split-Path $DumpFile -Parent
        if ($dir -and -not (Test-Path $dir)) { New-Item -ItemType Directory -Force -Path $dir | Out-Null }

        if ($DryRun) {
            $b = if ($Size) { $Size } else { '<自动探测>' }
            Show-OcdPlan @('init; halt', "dump_image {$(To-OcdPath $DumpFile)} 0x08000000 $b", 'shutdown')
            exit 0
        }
        if (-not (Ensure-Attached)) { exit 1 }

        Info "探测 Flash 信息 ..."
        $info = Get-FlashInfo
        $base = if ($info) { '0x{0:X8}' -f $info.Base } else { '0x08000000' }
        $len  = if ($Size) { $Size } elseif ($info) { '0x{0:X}' -f $info.Size } else { '0x40000' }
        Info "  基地址 $base，长度 $len"

        $ocdDump = To-OcdPath $DumpFile
        Info "回读整片 Flash → $DumpFile"
        $out = Invoke-Ocd @('init; halt', "dump_image {$ocdDump} $base $len", 'shutdown')
        Show-OcdResult $out 'dumped|Error|error:'

        if ((Test-Path $DumpFile) -and (Get-Item $DumpFile).Length -gt 0) {
            $sz = (Get-Item $DumpFile).Length
            Ok ("回读完成：{0} 字节 ({1:N0} KB)" -f $sz, ($sz / 1KB))
            exit 0
        }
        Err "回读失败"; exit 1
    }
}
