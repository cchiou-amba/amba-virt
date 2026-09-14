@echo off
echo ======================================================== > C:\firstboot.log
echo  Ambarella Edge Virtualization - Windows 11 ARM64 Init  >> C:\firstboot.log
echo ======================================================== >> C:\firstboot.log

echo [*] Setting unlimited password age... >> C:\firstboot.log
net accounts /maxpwage:unlimited /minpwlen:0 >> C:\firstboot.log 2>&1

echo [*] Enabling Administrator account... >> C:\firstboot.log
net user Administrator windows /active:yes >> C:\firstboot.log 2>&1

echo [*] Creating windows user account... >> C:\firstboot.log
net user windows windows /add /passwordchg:no /active:yes >> C:\firstboot.log 2>&1
net localgroup Administrators windows /add >> C:\firstboot.log 2>&1
net localgroup "Remote Desktop Users" windows /add >> C:\firstboot.log 2>&1

echo [*] Disabling Windows Firewall across all profiles... >> C:\firstboot.log
netsh advfirewall set allprofiles state off >> C:\firstboot.log 2>&1

echo [*] Enabling Remote Desktop (Terminal Server)... >> C:\firstboot.log
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server" /v fDenyTSConnections /t REG_DWORD /d 0 /f >> C:\firstboot.log 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp" /v UserAuthentication /t REG_DWORD /d 0 /f >> C:\firstboot.log 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp" /v SecurityLayer /t REG_DWORD /d 1 /f >> C:\firstboot.log 2>&1
reg add "HKLM\SYSTEM\CurrentControlSet\Control\Terminal Server\WinStations\RDP-Tcp" /v fEnableWinStation /t REG_DWORD /d 1 /f >> C:\firstboot.log 2>&1
sc config TermService start= auto >> C:\firstboot.log 2>&1
net start TermService >> C:\firstboot.log 2>&1

echo [*] Installing additional VirtIO drivers via pnputil... >> C:\firstboot.log
if exist C:\Drivers\VirtIO pnputil.exe /add-driver C:\Drivers\VirtIO\*.inf /subdirs /install >> C:\firstboot.log 2>&1

echo [*] Running lean optimization script... >> C:\firstboot.log
if exist C:\Windows\Setup\Scripts\optimize.ps1 (
    powershell.exe -ExecutionPolicy Bypass -File C:\Windows\Setup\Scripts\optimize.ps1 >> C:\firstboot.log 2>&1
)

echo [*] Resetting Setup state to normal boot... >> C:\firstboot.log
reg add "HKLM\SYSTEM\Setup" /v SetupType /t REG_DWORD /d 0 /f >> C:\firstboot.log 2>&1
reg add "HKLM\SYSTEM\Setup" /v SystemSetupInProgress /t REG_DWORD /d 0 /f >> C:\firstboot.log 2>&1
reg add "HKLM\SYSTEM\Setup" /v OOBEInProgress /t REG_DWORD /d 0 /f >> C:\firstboot.log 2>&1
echo [*] Removing FirstBoot from RunOnce and Startup... >> C:\firstboot.log
reg delete "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\RunOnce" /v FirstBoot /f >> C:\firstboot.log 2>&1
if exist "C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\firstboot.cmd" del /f /q "C:\ProgramData\Microsoft\Windows\Start Menu\Programs\StartUp\firstboot.cmd" >> C:\firstboot.log 2>&1

echo [*] FirstBoot provisioning complete! Windows is ready. >> C:\firstboot.log

