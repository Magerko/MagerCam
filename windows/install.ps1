# Ставлю клиент MagerCam и регистрирую виртуальную камеру, нужны права администратора.
#   -Src        папка с MagerCam.exe, magercam-vcam.dll и adb\ (по умолчанию dist\windows рядом с репозиторием)
#   -Uninstall  удалить всё
param(
	[string]$Src = (Join-Path $PSScriptRoot '..\dist\windows'),
	[switch]$Uninstall
)
$ErrorActionPreference = 'Stop'
Start-Transcript -Path (Join-Path $env:TEMP 'magercam-install.log') -Force | Out-Null

$dest = Join-Path $env:ProgramFiles 'MagerCam'
# по CLSID на каждую из четырёх камер: «MagerCam», «MagerCam 2»… отличаются последним байтом
$keys = 0x41..0x44 | ForEach-Object { 'HKLM:\Software\Classes\CLSID\{6B1E3C7A-4D2F-4A8B-9C5E-2F7A1D3B8E' + $_.ToString('X2') + '}' }
$fwRule = 'MagerCam: сопряжение по QR'
$startMenu = Join-Path $env:ProgramData 'Microsoft\Windows\Start Menu\Programs\MagerCam.lnk'
$desktop = Join-Path ([Environment]::GetFolderPath('CommonDesktopDirectory')) 'MagerCam.lnk'

function Copy-Locked([string]$from, [string]$to) {
	try {
		Copy-Item -Force $from $to
	} catch {
		# Frame Server держит DLL загруженной, пока открыто любое приложение с камерой, поэтому перезапускаю службу
		Write-Host 'Файл занят службой камер Windows, перезапускаю её...'
		Stop-Service FrameServerMonitor -Force -ErrorAction SilentlyContinue
		Stop-Service FrameServer -Force -ErrorAction SilentlyContinue
		Start-Sleep -Seconds 1
		Copy-Item -Force $from $to
	}
}

# закрываю только установленную копию, она держит файлы; копия из другого места
# (например, та, что запустила установщик) пусть работает
function Stop-Installed {
	Get-Process MagerCam -ErrorAction SilentlyContinue |
		Where-Object { $_.Path -and $_.Path.StartsWith($dest, [StringComparison]::OrdinalIgnoreCase) } |
		Stop-Process -Force -ErrorAction SilentlyContinue
}

if ($Uninstall) {
	Stop-Installed
	foreach ($key in $keys) { Remove-Item -Recurse -Force $key -ErrorAction SilentlyContinue }
	Remove-NetFirewallRule -DisplayName $fwRule -ErrorAction SilentlyContinue
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

# adb для USB я не распространяю вместе с MagerCam, а скачиваю официальные platform-tools с сайта Google
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

# регистрирую COM-объекты именно в HKLM: служба Frame Server не видит HKCU пользователя
foreach ($key in $keys) {
	New-Item -Force $key -Value 'MagerCam Virtual Camera' | Out-Null
	New-Item -Force "$key\InprocServer32" -Value (Join-Path $dest 'magercam-vcam.dll') | Out-Null
	New-ItemProperty -Force -Path "$key\InprocServer32" -Name ThreadingModel -Value 'Both' | Out-Null
}

# для QR-сопряжения телефон сам подключается к ПК. Домашнюю сеть Windows часто считает общественной,
# поэтому разрешаю во всех профилях: порт слушается, только пока открыто окно с QR, и ждёт одноразовый nonce
try {
	Remove-NetFirewallRule -DisplayName $fwRule -ErrorAction SilentlyContinue
	New-NetFirewallRule -DisplayName $fwRule -Direction Inbound -Action Allow -Protocol TCP -LocalPort 4850-4859 `
		-Program (Join-Path $dest 'MagerCam.exe') -Profile Any | Out-Null
} catch {
	Write-Host "Правило брандмауэра не добавлено: $($_.Exception.Message)"
}

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
