# Ставлю или убираю плагин MagerCam для OBS. Запускается из install-obs-plugin.bat и uninstall-obs-plugin.bat.
#
# Куда кладу, чтобы плагин нашли все версии OBS:
#   обычная установка OBS (и Steam): C:\ProgramData\obs-studio\plugins\magercam\
#     bin\64bit\magercam.dll — так ищут OBS 30–32, в 33 это «старая» раскладка, она пока работает;
#     magercam.dll           — новая раскладка OBS 33; если найдены обе, OBS 33 берёт эту, а вторую пропускает;
#   портативная OBS (из zip) в ProgramData не смотрит вообще, поэтому кладу в её папку:
#     obs-plugins\64bit\magercam.dll и data\obs-plugins\magercam — для 30–32;
#     plugins\magercam\magercam.dll  — для 33 и новее.
# Портативную OBS нахожу, если она сейчас запущена, или по пути, который передали параметром
# (можно просто перетащить папку OBS на install-obs-plugin.bat).
param(
	[string]$ObsPath = '',
	[switch]$Uninstall,
	[switch]$Elevated
)
$ErrorActionPreference = 'Stop'
[Console]::OutputEncoding = [Text.Encoding]::UTF8

$ProgramDataDir = Join-Path $env:ProgramData 'obs-studio\plugins\magercam'
$MinVersion = [version]'30.0'

function Say([string]$text, [string]$color = '') {
	if ($color) { Write-Host $text -ForegroundColor $color } else { Write-Host $text }
}

# Архитектура exe по заголовку PE: x64 или ARM64. Плагин другой архитектуры OBS просто не загрузит.
function Get-PeArch([string]$file) {
	try {
		$fs = [IO.File]::OpenRead($file)
		try {
			$br = New-Object IO.BinaryReader($fs)
			$fs.Position = 0x3C
			$pe = $br.ReadInt32()
			$fs.Position = $pe + 4
			switch ($br.ReadUInt16()) {
				0x8664 { return 'x64' }
				0xAA64 { return 'arm64' }
				0x014C { return 'x86' }
				default { return 'unknown' }
			}
		} finally { $fs.Close() }
	} catch { return 'unknown' }
}

function Test-Portable([string]$root) {
	foreach ($n in 'portable_mode', 'obs_portable_mode', 'portable_mode.txt', 'obs_portable_mode.txt') {
		if (Test-Path -LiteralPath (Join-Path $root $n)) { return $true }
	}
	return $false
}

function Get-ObsInfo([string]$root) {
	$exe = Join-Path $root 'bin\64bit\obs64.exe'
	if (-not (Test-Path -LiteralPath $exe)) { return $null }
	$ver = $null
	try {
		$fv = (Get-Item -LiteralPath $exe).VersionInfo
		$ver = [version]('{0}.{1}.{2}' -f $fv.FileMajorPart, $fv.FileMinorPart, $fv.FileBuildPart)
	} catch {}
	$bin = Split-Path $exe
	$ff = Get-ChildItem -LiteralPath $bin -Filter 'avcodec-*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
	[pscustomobject]@{
		Root     = (Resolve-Path -LiteralPath $root).Path.TrimEnd('\')
		Version  = $ver
		Arch     = Get-PeArch $exe
		Portable = Test-Portable $root
		FFmpeg   = if ($ff) { $ff.BaseName } else { '' }
	}
}

# Все OBS, которые удаётся найти: реестр установщика, Steam, папка по умолчанию, запущенные процессы, путь из параметра.
function Find-Obs {
	$roots = New-Object System.Collections.Generic.List[string]
	$add = {
		param($p)
		if (-not $p) { return }
		$p = $p.Trim('"').TrimEnd('\')
		# путь могли дать к самому exe или к bin\64bit
		if ($p -match '\\obs64\.exe$') { $p = Split-Path (Split-Path (Split-Path $p)) }
		elseif ($p -match '\\bin\\64bit$') { $p = Split-Path (Split-Path $p) }
		if ((Test-Path -LiteralPath (Join-Path $p 'bin\64bit\obs64.exe')) -and -not ($roots -contains $p)) { $roots.Add($p) }
	}
	& $add $ObsPath
	foreach ($k in 'HKLM:\SOFTWARE\OBS Studio', 'HKLM:\SOFTWARE\WOW6432Node\OBS Studio') {
		try { & $add ((Get-ItemProperty -LiteralPath $k -ErrorAction Stop).'(default)') } catch {}
	}
	foreach ($k in 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
		'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
		'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*') {
		Get-ItemProperty $k -ErrorAction SilentlyContinue |
			Where-Object { $_.DisplayName -like 'OBS Studio*' -or $_.PSChildName -eq 'Steam App 1905180' } |
			ForEach-Object { & $add $_.InstallLocation }
	}
	& $add (Join-Path $env:ProgramFiles 'obs-studio')
	Get-Process obs64 -ErrorAction SilentlyContinue | ForEach-Object { try { & $add $_.Path } catch {} }
	foreach ($r in $roots) { Get-ObsInfo $r }
}

function Copy-Plugin([string]$dll, [string]$binDir, [string]$dataDir) {
	New-Item -ItemType Directory -Force -Path $binDir, $dataDir | Out-Null
	Copy-Item -LiteralPath $dll -Destination (Join-Path $binDir 'magercam.dll') -Force
}

function Test-NeedsAdmin([string]$dir) {
	try {
		New-Item -ItemType Directory -Force -Path $dir | Out-Null
		$probe = Join-Path $dir ('.write-test-' + [guid]::NewGuid())
		[IO.File]::WriteAllText($probe, '')
		Remove-Item -LiteralPath $probe -Force
		return $false
	} catch { return $true }
}

function Restart-Elevated {
	$argList = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$PSCommandPath`"", '-Elevated')
	if ($ObsPath) { $argList += @('-ObsPath', "`"$ObsPath`"") }
	if ($Uninstall) { $argList += '-Uninstall' }
	Say 'Нужны права администратора, Windows сейчас спросит разрешение…' Yellow
	try {
		Start-Process powershell -Verb RunAs -ArgumentList $argList -Wait
	} catch {
		Say '[ОШИБКА] Без прав администратора поставить плагин в C:\ProgramData не получится.' Red
	}
	exit
}

# adb нужен только для USB; скачиваю один раз с официального сайта Google и раскладываю по папкам данных плагина
function Install-Adb([string[]]$dataDirs) {
	if (Get-Command adb -ErrorAction SilentlyContinue) { return }
	if (Test-Path (Join-Path $env:ProgramFiles 'MagerCam\adb\adb.exe')) { return }
	$have = $dataDirs | Where-Object { Test-Path (Join-Path $_ 'adb\adb.exe') } | Select-Object -First 1
	if (-not $have) {
		Say 'Скачиваю adb (Android platform-tools) для подключения по USB…'
		try {
			[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
			$z = Join-Path $env:TEMP 'mc-pt.zip'
			$t = Join-Path $env:TEMP 'mc-pt'
			Invoke-WebRequest -UseBasicParsing 'https://dl.google.com/android/repository/platform-tools-latest-windows.zip' -OutFile $z
			Expand-Archive -Force $z $t
			$have = Join-Path $env:TEMP 'mc-adb'
			New-Item -ItemType Directory -Force (Join-Path $have 'adb') | Out-Null
			foreach ($f in 'adb.exe', 'AdbWinApi.dll', 'AdbWinUsbApi.dll') {
				Copy-Item -Force (Join-Path $t "platform-tools\$f") (Join-Path $have 'adb')
			}
			Remove-Item -Recurse -Force $t, $z
		} catch {
			Say '[!] adb не скачался. По Wi-Fi всё работает и без него.' Yellow
			return
		}
	}
	foreach ($d in $dataDirs) {
		if ($d -ne $have) {
			New-Item -ItemType Directory -Force (Join-Path $d 'adb') | Out-Null
			Copy-Item -Force (Join-Path $have 'adb\*') (Join-Path $d 'adb')
		}
	}
}

# --- что ставить ---
$here = $PSScriptRoot
$src = Join-Path $here 'magercam'
if (-not (Test-Path $src)) { $src = Join-Path $here 'dist\obs-plugin\magercam' }
$dllX64 = Join-Path $src 'bin\64bit\magercam.dll'
$dllArm = Join-Path (Split-Path $src) 'magercam-arm64\magercam.dll'

$found = @(Find-Obs)
$osArm = $env:PROCESSOR_ARCHITECTURE -eq 'ARM64' -or $env:PROCESSOR_ARCHITEW6432 -eq 'ARM64'

# --- удаление ---
if ($Uninstall) {
	$targets = @($ProgramDataDir)
	foreach ($o in $found | Where-Object Portable) {
		$targets += Join-Path $o.Root 'plugins\magercam'
		$targets += Join-Path $o.Root 'data\obs-plugins\magercam'
		$targets += Join-Path $o.Root 'obs-plugins\64bit\magercam.dll'
	}
	if ((Test-Path $ProgramDataDir) -and -not $Elevated -and (Test-NeedsAdmin $ProgramDataDir)) { Restart-Elevated }
	foreach ($t in $targets) {
		if (Test-Path -LiteralPath $t) {
			try { Remove-Item -LiteralPath $t -Recurse -Force; Say "Удалено: $t" }
			catch { Say "Не удалось удалить $t (закройте OBS): $($_.Exception.Message)" Red }
		}
	}
	Say 'Плагин MagerCam удалён.' Green
	if ($Elevated) { Read-Host 'Нажмите Enter' }
	exit
}

# --- установка ---
if (-not (Test-Path $dllX64)) {
	Say '[ОШИБКА] Рядом нет папки magercam с плагином.' Red
	Say 'Распакуйте архив целиком (правой кнопкой → «Извлечь все…») и запустите install-obs-plugin.bat из распакованной папки.'
	if ($Elevated) { Read-Host 'Нажмите Enter' }
	exit 1
}

Say 'Установка плагина MagerCam для OBS'
Say ''

$installed = $false
$dataDirs = @()
$warnings = @()

# обычные установки делят одну папку в ProgramData; архитектуру беру от первой найденной, иначе от системы
$regular = @($found | Where-Object { -not $_.Portable })
$arch = if ($regular.Count -gt 0) { $regular[0].Arch } elseif ($osArm) { 'arm64' } else { 'x64' }
$needProgramData = $regular.Count -gt 0 -or $found.Count -eq 0
if ($needProgramData) {
	if (-not $Elevated -and (Test-NeedsAdmin $ProgramDataDir)) { Restart-Elevated }
	$dll = if ($arch -eq 'arm64' -and (Test-Path $dllArm)) { $dllArm } else { $dllX64 }
	try {
		Copy-Plugin $dll (Join-Path $ProgramDataDir 'bin\64bit') (Join-Path $ProgramDataDir 'data')
		Copy-Item -LiteralPath $dll -Destination (Join-Path $ProgramDataDir 'magercam.dll') -Force
		$dataDirs += Join-Path $ProgramDataDir 'data'
		$installed = $true
		Say "Плагин ($arch) установлен: $ProgramDataDir" Green
	} catch {
		Say "[ОШИБКА] Не удалось скопировать в $ProgramDataDir`: $($_.Exception.Message)" Red
		Say 'Если OBS открыт, закройте его и запустите установку снова.'
	}
}

foreach ($o in $found | Where-Object Portable) {
	$dll = if ($o.Arch -eq 'arm64' -and (Test-Path $dllArm)) { $dllArm } else { $dllX64 }
	try {
		Copy-Plugin $dll (Join-Path $o.Root 'obs-plugins\64bit') (Join-Path $o.Root 'data\obs-plugins\magercam')
		Copy-Plugin $dll (Join-Path $o.Root 'plugins\magercam') (Join-Path $o.Root 'plugins\magercam\data')
		$dataDirs += Join-Path $o.Root 'data\obs-plugins\magercam'
		$dataDirs += Join-Path $o.Root 'plugins\magercam\data'
		$installed = $true
		Say "Плагин установлен в портативную OBS: $($o.Root)" Green
	} catch {
		Say "[ОШИБКА] Не удалось скопировать в $($o.Root): $($_.Exception.Message)" Red
	}
}

foreach ($o in $found) {
	if ($o.Version -and $o.Version -lt $MinVersion) {
		$warnings += "OBS $($o.Version) в $($o.Root) слишком старый: плагину нужен OBS 30 или новее. Обновите OBS с obsproject.com."
	}
	if ($o.Arch -eq 'arm64' -and -not (Test-Path $dllArm)) {
		$warnings += "OBS в $($o.Root) собран для ARM64, а в этом архиве нет сборки под ARM64."
	}
	if (-not $o.FFmpeg) {
		$warnings += "В $($o.Root) не нашлось avcodec-*.dll: похоже, OBS повреждён. Переустановите его."
	}
}
if ($found.Count -eq 0) {
	$warnings += 'OBS не найден. Плагин положен туда, где его ищет обычная установка OBS. Если у вас портативная OBS из zip, запустите её и повторите установку, или перетащите папку OBS на install-obs-plugin.bat.'
}

if ($installed) { Install-Adb $dataDirs }

Say ''
foreach ($w in $warnings) { Say "[!] $w" Yellow }
if (Get-Process obs64 -ErrorAction SilentlyContinue) {
	Say '[!] OBS сейчас запущен. Плагин загрузится после перезапуска OBS.' Yellow
}
if ($installed) {
	Say ''
	Say 'Готово. Перезапустите OBS и добавьте источник «MagerCam (телефон)».' Green
}

# сводка, которую можно прислать, если что-то не так
$os = try { (Get-CimInstance Win32_OperatingSystem) } catch { $null }
Say ''
Say '--- Если источник не появился, пришлите разработчику этот блок ---'
Say ("Windows: {0} {1} ({2}), процессор {3}" -f $(if ($os) { $os.Caption } else { '?' }), $(if ($os) { $os.Version } else { '' }),
	$(if ($osArm) { 'ARM64' } else { 'x64' }), $env:PROCESSOR_ARCHITECTURE)
if ($found.Count -eq 0) { Say 'OBS: не найден' }
foreach ($o in $found) {
	Say ("OBS {0} {1}{2}, FFmpeg {3}: {4}" -f $o.Version, $o.Arch, $(if ($o.Portable) { ', портативная' } else { '' }),
		$(if ($o.FFmpeg) { $o.FFmpeg } else { 'нет' }), $o.Root)
}
Say ("Плагин: {0}" -f $(if ($installed) { 'установлен' } else { 'НЕ установлен' }))
Say '-------------------------------------------------------------------'
if ($Elevated) { Read-Host 'Нажмите Enter' }
