# optimize.ps1 - Lean Windows 11 ARM64 Post-Installation Optimization Script
# Author: Ambarella Edge Virtualization

Write-Output "=== Starting Windows 11 ARM64 Lean Optimization ==="

# 1. Disable Hibernation (removes ~3 GB hiberfil.sys)
Write-Output "[1/4] Disabling hibernation..."
powercfg /h off

# 2. Configure Fixed Pagefile (1024 MB)
Write-Output "[2/4] Sizing pagefile to 1024 MB..."
try {
    $sys = Get-CimInstance Win32_ComputerSystem
    $sys.AutomaticManagedPagefile = $False
    Set-CimInstance -CimInstance $sys
    Set-CimInstance -Query "SELECT * FROM Win32_PageFileSetting" -Property @{InitialSize = 1024; MaximumSize = 1024} -ErrorAction SilentlyContinue
} catch {
    Write-Warning "Pagefile tuning warning: $_"
}

# 3. Disable Background Resource Hogs (SysMain, WSearch, DiagTrack, Spooler)
Write-Output "[3/4] Disabling background services (SysMain, WSearch, DiagTrack, Spooler)..."
$services = @("SysMain", "WSearch", "DiagTrack", "Spooler", "MapsBroker", "dmwappushservice")
foreach ($svc in $services) {
    if (Get-Service -Name $svc -ErrorAction SilentlyContinue) {
        Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
        Set-Service -Name $svc -StartupType Disabled -ErrorAction SilentlyContinue
    }
}

# 4. Adjust Visual Effects for Performance while retaining Crisp RDP
Write-Output "[4/4] Optimizing visual effects..."
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects" -Name "VisualFXSetting" -Value 2 -Type DWord -ErrorAction SilentlyContinue
Set-ItemProperty -Path "HKCU:\Control Panel\Desktop" -Name "FontSmoothing" -Value "2" -ErrorAction SilentlyContinue

Write-Output "=== Lean Optimization Complete. Windows 11 is online and ready for RDP connections. ==="

