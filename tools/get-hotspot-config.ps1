# 读取 Windows 移动热点的当前配置（SSID / 密码 / 频段 / 状态）
# 用 Windows PowerShell 5.1 运行（PS7 没有内置 WinRT 投影）

$ErrorActionPreference = 'Stop'

try {
    [void][Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager, Windows.Networking.NetworkOperators, ContentType = WindowsRuntime]
    [void][Windows.Networking.Connectivity.NetworkInformation, Windows.Networking.Connectivity, ContentType = WindowsRuntime]
} catch {
    Write-Host "WinRT 类型加载失败: $($_.Exception.Message)" -ForegroundColor Red
    exit 1
}

$profile = [Windows.Networking.Connectivity.NetworkInformation]::GetInternetConnectionProfile()
if (-not $profile) {
    Write-Host "没有可用的 Internet 连接配置（热点无法共享）" -ForegroundColor Yellow
} else {
    Write-Host "共享源连接: $($profile.ProfileName)"
}

$mgr = [Windows.Networking.NetworkOperators.NetworkOperatorTetheringManager]::CreateFromConnectionProfile($profile)

Write-Host "热点状态: $($mgr.TetheringOperationalState)"
Write-Host "客户端数: $($mgr.ClientCount) / $($mgr.MaxClientCount)"

$cfg = $mgr.GetCurrentAccessPointConfiguration()
Write-Host ""
Write-Host "---- 当前热点配置 ----"
Write-Host "SSID     : [$($cfg.Ssid)]"
Write-Host "密码     : [$($cfg.Passphrase)]"
Write-Host "频段     : $($cfg.Band)"
Write-Host "认证     : $($cfg.AuthenticationKind)"
Write-Host "----------------------"
