# usbip-win2 connects Apollo's built-in virtual Wacom tablet to Windows as a USB device.
# Installed silently, unless this version (or a newer one) is already there.
$required = [System.Version]"0.9.8.0"
try {
    $installed = Get-ItemProperty "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\*" -ErrorAction SilentlyContinue |
        Where-Object { $_.Publisher -eq "usbip-win2" } |
        Select-Object -First 1
    if ($installed -and [System.Version]$installed.DisplayVersion -ge $required) {
        Write-Information "usbip-win2 $($installed.DisplayVersion) is installed, nothing to do."
        exit 0
    }
}
catch {
    Write-Information "usbip-win2 not found, installing it."
}

$installer = Join-Path (Split-Path -Parent $MyInvocation.MyCommand.Path) "usbip_installer.exe"
Start-Process -FilePath $installer -ArgumentList "/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/SP-" -Wait
