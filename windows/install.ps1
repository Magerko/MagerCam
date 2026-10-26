# Installs the MagerCam Windows client and registers the virtual camera (run elevated).
#   -Src        folder with MagerCam.exe, magercam-vcam.dll and adb\ (default: dist\windows next to this repo)
#   -Uninstall  remove everything
param(
	[string]$Src = (Join-Path $PSScriptRoot '..\dist\windows'),
	[switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
Start-Transcript -Path (Join-Path $env:TEMP 'magercam-install.log') -Force | Out-Null

$dest = Join-Path $env:ProgramFiles 'MagerCam'
$clsid = '{6B1E3C7A-4D2F-4A8B-9C5E-2F7A1D3B8E41}'
$key = "HKLM:\Software\Classes\CLSID\$clsid"
$startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\MagerCam.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'MagerCam.lnk'

function Copy-Locked([string]$from, [string]$to) {
	try {
		Copy-Item -Force $from $to
	} catch {
		# the Frame Server keeps the DLL loaded while a camera app is open
		Write-Host 'Файл занят службой камер Windows, перезапускаю её...'
		Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue
		Stop-Service FrameServer -Force -ErrorAction SilentlyContinue
		Start-Sleep -Seconds 1
		Copy-Item -Force $from $to
	}
}

# only an installed copy blocks the files; a copy started from elsewhere (e.g. the one that
# launched this installer) keeps running
function Stop-Installed {
	Get-Process MagerCam -ErrorAction SilentlyContinue |
		Where-Object { $_.Path -and $_.Path.StartsWith($dest, [StringComparison]::OrdinalIgnoreCase) } |
		Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($Uninstall) {
	Stop-Installed
	Remove-Item -Recurse -Force $key -ErrorAction SilentlyContinue
	Remove-Item -Force $startMenu, $desktop -ErrorAction SilentlyContinue
	Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue
	Stop-Service FrameServer -Force -ErrorAction SilentlyContinue
	Start-Sleep -Seconds 1
	Remove-Item -Recurse -Force $dest -ErrorAction SilentlyContinue
	Write-Host 'MagerCam удалён.'
	Stop-Transcript | Out-Null
	exit 0
}

$Src = (Resolve-Path $Src).Path.TrimEnd('\')
New-Item -ItemType Directory -Force $dest | Out-Null
if ($Src -ne $dest.TrimEnd('\')) {
	Stop-Installed
	Copy-Locked (Join-Path $Src 'magercam-vcam.dll') (Join-Path $dest 'magercam-vcam.dll')
	foreach ($f in 'MagerCam.exe', 'install.ps1') {
		if (Test-Path (Join-Path $Src $f)) { Copy-Item -Force (Join-Path $Src $f) $dest }
	}
	if (Test-Path (Join-Path $Src 'adb')) {
		Copy-Item -Recurse -Force (Join-Path $Src 'adb') $dest
	}
}

# adb for USB mode: not redistributed with MagerCam, fetched from Google's official platform-tools
$adbDir = Join-Path $dest 'adb'
if (-not (Test-Path (Join-Path $adbDir 'adb.exe'))) {
	try {
		Write-Host 'Скачиваю adb (Android platform-tools) для режима USB...'
		[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
		$zip = Join-Path $env:TEMP 'magercam-platform-tools.zip'
		$tmp = Join-Path $env:TEMP 'magercam-platform-tools'
		Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $zip
		Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
		Expand-Archive -Force $zip $tmp
		New-Item -ItemType Directory -Force $adbDir | Out-Null
		foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') {
			Copy-Item -Force (Join-Path $tmp "platform-tools\$f") $adbDir
		}
		Remove-Item -Recurse -Force $tmp, $zip -ErrorAction SilentlyContinue
	} catch {
		Write-Host "adb не скачан ($($_.Exception.Message)). Wi-Fi работает и без него; для USB установите Android platform-tools."
	}
}

# COM registration of the media source (HKLM: the Frame Server service cannot see HKCU)
New-Item -Force $key -Value 'MagerCam Virtual Camera' | Out-Null
New-Item -Force "$key\InprocServer32" -Value (Join-Path $dest 'magercam-vcam.dll') | Out-Null
New-ItemProperty -Force -Path "$key\InprocServer32" -Name ThreadingModel -Value 'Both' | Out-Null

if (Test-Path (Join-Path $dest 'MagerCam.exe')) {
	$sh = New-Object -ComObject WScript.Shell
	foreach ($lnk in @($startMenu, $desktop)) {
		$s = $sh.CreateShortcut($lnk)
		$s.TargetPath = Join-Path $dest 'MagerCam.exe'
		$s.WorkingDirectory = $dest
		$s.Description = 'MagerCam: телефон как веб-камера'
		$s.Save()
	}
}
Write-Host "MagerCam установлен в $dest"
Stop-Transcript | Out-Null
