# optimize.ps1 - Lean Windows 11 IoT Enterprise ARM64 Optimization Script
# Author: Ambarella Edge Virtualization

Write-Output "=== Starting Windows 11 IoT Enterprise ARM64 Lean Optimization ==="

# 1. Disable Hibernation (removes ~3 GB hiberfil.sys)
Write-Output "[1/5] Disabling hibernation..."
powercfg /h off

# 2. Configure High-Performance Power Scheme
Write-Output "[2/5] Setting High Performance power scheme..."
powercfg /setactive 8c5e7fda-e8bf-4a96-9a85-a6e23a8c635c 2>$null

# 3. Configure Fixed Pagefile (1024 MB)
Write-Output "[3/5] Sizing pagefile to 1024 MB..."
try {
    $sys = Get-CimInstance Win32_ComputerSystem
    $sys.AutomaticManagedPagefile = $False
    Set-CimInstance -CimInstance $sys
    Set-CimInstance -Query "SELECT * FROM Win32_PageFileSetting" -Property @{InitialSize = 1024; MaximumSize = 1024} -ErrorAction SilentlyContinue
} catch {
    Write-Warning "Pagefile tuning warning: $_"
}

# 4. Disable Background Resource Hogs & Telemetry
Write-Output "[4/5] Disabling background services (SysMain, WSearch, DiagTrack, Spooler)..."
$services = @("SysMain", "WSearch", "DiagTrack", "Spooler", "MapsBroker", "dmwappushservice", "WerSvc")
foreach ($svc in $services) {
    if (Get-Service -Name $svc -ErrorAction SilentlyContinue) {
        Stop-Service -Name $svc -Force -ErrorAction SilentlyContinue
        Set-Service -Name $svc -StartupType Disabled -ErrorAction SilentlyContinue
    }
}

# 5. Adjust Visual Effects for Performance while retaining Crisp RDP
Write-Output "[5/5] Optimizing visual effects..."
Set-ItemProperty -Path "HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\VisualEffects" -Name "VisualFXSetting" -Value 2 -Type DWord -ErrorAction SilentlyContinue
Set-ItemProperty -Path "HKCU:\Control Panel\Desktop" -Name "FontSmoothing" -Value "2" -ErrorAction SilentlyContinue

# ==============================================================================
# IoT Enterprise Edge Appliance Utilities (Call on-demand or during provisioning)
# ==============================================================================

function Enable-UnifiedWriteFilter {
    <#
    .SYNOPSIS
        Enables Unified Write Filter (UWF) for eMMC/NVMe flash wear protection.
    .PARAMETER OverlaySizeMB
        Size of the RAM overlay in MB (default: 512).
    #>
    param([int]$OverlaySizeMB = 512)

    Write-Output "[UWF] Configuring Unified Write Filter (RAM overlay: ${OverlaySizeMB} MB)..."
    if (Get-Command uwfmgr.exe -ErrorAction SilentlyContinue) {
        uwfmgr.exe overlay set-type RAM
        uwfmgr.exe overlay set-size $OverlaySizeMB
        uwfmgr.exe overlay set-warningthreshold ($OverlaySizeMB - 64)
        uwfmgr.exe volume protect C:
        uwfmgr.exe filter enable
        Write-Output "[UWF] Unified Write Filter enabled. Reboot required to seal volume C:."
    } else {
        Write-Warning "[UWF] uwfmgr.exe not found. Ensure the UWF optional feature is installed."
    }
}

function Enable-ShellLauncher {
    <#
    .SYNOPSIS
        Configures Custom Shell Launcher to run a dedicated Edge AI UI instead of Explorer.
    .PARAMETER AppPath
        Absolute path to the custom Edge AI application executable.
    #>
    param([string]$AppPath = "C:\Program Files\EdgeAI\App.exe")

    Write-Output "[ShellLauncher] Setting default shell to: $AppPath..."
    Set-ItemProperty -Path "HKLM:\SOFTWARE\Microsoft\Windows NT\CurrentVersion\Winlogon" -Name "Shell" -Value "$AppPath" -ErrorAction SilentlyContinue
}

Write-Output "=== Lean Optimization Complete. Windows 11 IoT Enterprise is online and ready for RDP. ==="


