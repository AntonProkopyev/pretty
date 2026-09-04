[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$source = Split-Path -Parent $PSCommandPath
$install = Join-Path $env:LOCALAPPDATA "Programs\Shitty"
$executable = Join-Path $install "st.exe"
$config = Join-Path $install "config\shitty.toml"
$desktop = [Environment]::GetFolderPath("Desktop")
$shortcutPath = Join-Path $desktop "Shitty.lnk"

New-Item -ItemType Directory -Path $install -Force | Out-Null
Copy-Item -Path (Join-Path $source "*") -Destination $install -Recurse -Force

$shortcutShell = New-Object -ComObject WScript.Shell
$shortcut = $shortcutShell.CreateShortcut($shortcutPath)
$shortcut.TargetPath = $executable
$shortcut.Arguments = "-config `"$config`""
$shortcut.WorkingDirectory = $env:USERPROFILE
$shortcut.IconLocation = "$executable,0"
$shortcut.Save()
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcut) | Out-Null
[Runtime.InteropServices.Marshal]::FinalReleaseComObject($shortcutShell) | Out-Null

$locations = @(
    @{
        Key = "HKCU:\Software\Classes\Directory\Background\shell\Shitty"
        Directory = "%V"
    },
    @{
        Key = "HKCU:\Software\Classes\Directory\shell\Shitty"
        Directory = "%1"
    }
)

foreach ($location in $locations) {
    New-Item -Path $location.Key -Force | Out-Null
    New-ItemProperty -Path $location.Key -Name "MUIVerb" -Value "Open in Shitty" -Force | Out-Null
    New-ItemProperty -Path $location.Key -Name "Icon" -Value $executable -Force | Out-Null
    New-ItemProperty -Path $location.Key -Name "Position" -Value "Top" -Force | Out-Null
    $commandKey = Join-Path $location.Key "command"
    $command = "`"$executable`" -config `"$config`" -e `"$env:ComSpec`" /K cd /D `"$($location.Directory)`""
    New-Item -Path $commandKey -Force | Out-Null
    Set-Item -Path $commandKey -Value $command
}

Write-Output "Installed Shitty in $install"
Write-Output "Created $shortcutPath"
Write-Output "Registered Open in Shitty for folders and folder backgrounds"
