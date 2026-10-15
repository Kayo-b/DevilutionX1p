# Diablo First-Person (a DevilutionX mod): sets the game up in this folder. Install.bat runs it.
#
# Copies your Diablo data (DIABDAT.MPQ, and Hellfire's if you want it) next to devilutionx.exe, and, if you want the
# weapons in hand, the first-person weapon art of your own copy of The Elder Scrolls II: Daggerfall into daggerfall\.
# Then makes shortcuts. The game starts in first person (F4 switches to the original view).
#
# Options, each answering its question (Install.bat passes them on):
#   -Diablo <folder>          the folder with DIABDAT.MPQ (Hellfire: hellfire.mpq and hf*.mpq beside it or in hellfire\)
#   -Game diablo|hellfire     Diablo alone, or Diablo + Hellfire (when Hellfire's data is there, the default)
#   -Daggerfall <folder>      Daggerfall's ARENA2 folder, or the folder Daggerfall is installed in
#   -NoDaggerfall             no weapons in hand
#   -NoShortcuts              no desktop or Start menu shortcuts
#   -Yes                      take every default without asking
param(
	[string]$Diablo = '',
	[string]$Game = '',
	[string]$Daggerfall = '',
	[switch]$NoDaggerfall,
	[switch]$NoShortcuts,
	[switch]$Yes
)

$ErrorActionPreference = 'Stop'
$Here = Split-Path -Parent $MyInvocation.MyCommand.Path
$GogDiablo = 'https://www.gog.com/en/game/diablo'
$GogDaggerfall = 'https://www.gog.com/en/game/the_elder_scrolls_chapter_ii_daggerfall'
$HellfireFiles = @('hellfire.mpq', 'hfmonk.mpq', 'hfmusic.mpq', 'hfvoice.mpq', 'hfbard.mpq', 'hfbarb.mpq', 'hfopt1.mpq', 'hfopt2.mpq')
$DaggerfallFiles = @('WEAPON00.CIF', 'WEAPON01.CIF', 'WEAPON02.CIF', 'WEAPON03.CIF', 'WEAPON04.CIF', 'WEAPON05.CIF',
	'WEAPON06.CIF', 'WEAPON07.CIF', 'WEAPON08.CIF', 'WEAPON09.CIF', 'WEAPON10.CIF', 'WEAPON11.CIF', 'ART_PAL.COL')

function Step($text) { Write-Host ''; Write-Host "==> $text" -ForegroundColor White }
function Info($text) { Write-Host "    $text" }
function Ok($text) { Write-Host "    $text" -ForegroundColor Green }
function Warn($text) { Write-Host "    $text" -ForegroundColor Yellow }
function Fail($text) {
	Write-Host ''
	Write-Host "error: $text" -ForegroundColor Red
	if (-not $Yes) { Read-Host 'Press Enter to close' | Out-Null }
	exit 1
}

# The answer, or the default on Enter (and with -Yes).
function Ask($question, $default) {
	if ($Yes) { return $default }
	$answer = Read-Host "    $question [$default]"
	if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
	return $answer.Trim().Trim('"').Trim("'")
}
function YesNo($question, [bool]$default) {
	if ($Yes) { return $default }
	$hint = 'y/N'
	if ($default) { $hint = 'Y/n' }
	$answer = Read-Host "    $question [$hint]"
	if ([string]::IsNullOrWhiteSpace($answer)) { return $default }
	return $answer.Trim().ToLower().StartsWith('y')
}
# A folder picked in Windows' folder dialog ($null if cancelled).
function Browse($description) {
	try {
		Add-Type -AssemblyName System.Windows.Forms
		$dialog = New-Object System.Windows.Forms.FolderBrowserDialog
		$dialog.Description = $description
		if ($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { return $dialog.SelectedPath }
	} catch {
		Warn 'The folder dialog could not open: type the folder instead.'
	}
	return $null
}
# A numbered choice from a list, a typed folder, or B to browse: the folder picked.
function PickFolder($found, $question, $description) {
	$answer = Ask "$question (a number, a folder, or B to browse)" '1'
	$number = 0
	if ([int]::TryParse($answer, [ref]$number) -and $number -ge 1 -and $number -le $found.Count) { return $found[$number - 1] }
	if ($answer -match '^[bB]') { return (Browse $description) }
	return $answer
}

# --- Diablo -----------------------------------------------------------------------------------------------------------

# Install folders the GOG registry knows for games whose name matches.
function GogFolders($pattern) {
	$folders = @()
	foreach ($key in @('HKLM:\SOFTWARE\WOW6432Node\GOG.com\Games', 'HKLM:\SOFTWARE\GOG.com\Games')) {
		if (-not (Test-Path $key)) { continue }
		foreach ($game in Get-ChildItem $key -ErrorAction SilentlyContinue) {
			$p = Get-ItemProperty $game.PSPath -ErrorAction SilentlyContinue
			if ($p -and $p.gameName -like $pattern -and $p.path) { $folders += $p.path }
		}
	}
	return $folders
}

# The MPQs of a Diablo folder worth copying: the game's, Hellfire's (also in a hellfire\ folder, as GOG installs them)
# and the extra fonts. Never devilutionx.mpq: another DevilutionX's own files would hide this one's.
function DiabloFiles($dir) {
	$files = @()
	foreach ($sub in @($dir, (Join-Path $dir 'hellfire'))) {
		if (-not (Test-Path -LiteralPath $sub -PathType Container)) { continue }
		foreach ($f in Get-ChildItem -LiteralPath $sub -File -ErrorAction SilentlyContinue) {
			$n = $f.Name.ToLower()
			if ($n -eq 'diabdat.mpq' -or $n -eq 'spawn.mpq' -or $n -eq 'fonts.mpq' -or $HellfireFiles -contains $n) { $files += $f.FullName }
		}
	}
	return $files
}
function HasFile($files, $name) {
	foreach ($f in $files) { if ((Split-Path -Leaf $f).ToLower() -eq $name) { return $true } }
	return $false
}
function Describe($dir) {
	$files = DiabloFiles $dir
	if (HasFile $files 'diabdat.mpq') {
		if (HasFile $files 'hellfire.mpq') { return 'Diablo + Hellfire' }
		return 'Diablo'
	}
	if (HasFile $files 'spawn.mpq') { return 'the shareware Diablo (spawn.mpq)' }
	return ''
}
function DetectDiablo() {
	# Paths as strings: Join-Path fails on a drive that isn't there (no D:), Test-Path just says no.
	$candidates = @($Here, "$env:APPDATA\diasurgical\devilution")
	$candidates += GogFolders '*Diablo*'
	foreach ($root in @('C:\GOG Games', 'D:\GOG Games', "${env:ProgramFiles(x86)}\GOG Galaxy\Games", "$env:ProgramFiles\GOG Galaxy\Games")) {
		$candidates += "$root\Diablo"
	}
	$candidates += @("${env:ProgramFiles(x86)}\Diablo", 'C:\Diablo', "$env:USERPROFILE\Downloads", "$env:USERPROFILE\Desktop")
	$found = @()
	foreach ($c in $candidates) {
		if (-not $c -or -not (Test-Path -LiteralPath $c -PathType Container)) { continue }
		$full = (Resolve-Path -LiteralPath $c).Path.TrimEnd('\')
		if ($found -notcontains $full -and (Describe $full)) { $found += $full }
	}
	return $found
}

Write-Host 'Diablo First-Person: setup' -ForegroundColor White
Info "Sets the game up in this folder: $Here"

Step "Diablo's data"
if (-not $Diablo) {
	$found = @(DetectDiablo)
	if ($found.Count -gt 0) {
		Info "Found the game's data (DIABDAT.MPQ) in:"
		for ($i = 0; $i -lt $found.Count; $i++) {
			$where = $found[$i]
			if ($where -eq $Here.TrimEnd('\')) { $where = "$where (this folder)" }
			Info "  $($i + 1)) $where ($(Describe $found[$i]))"
		}
		$Diablo = PickFolder $found 'Which one?' 'The folder with DIABDAT.MPQ'
	} else {
		Info "DIABDAT.MPQ wasn't found. It's the data of the original game, which you need to own:"
		Info "GOG.com sells Diablo with its expansion Hellfire: $GogDiablo"
		Info 'Install it with GOG Galaxy or GOG''s offline installer, then run Install.bat again,'
		Info 'or give the folder that holds DIABDAT.MPQ (Hellfire''s hellfire.mpq and hf*.mpq too, if you have them).'
		if ($Yes) { Fail "no Diablo data found; get the game at $GogDiablo" }
		if (YesNo 'Open the GOG page in your browser?' $true) { Start-Process $GogDiablo }
		$Diablo = Ask 'Folder (or B to browse; Enter to stop here)' ''
		if ($Diablo -match '^[bB]$') { $Diablo = Browse 'The folder with DIABDAT.MPQ' }
		if (-not $Diablo) { Fail 'no Diablo data given' }
	}
}
if (-not $Diablo) { Fail 'no folder picked' }
if (Test-Path -LiteralPath $Diablo -PathType Leaf) { $Diablo = Split-Path -Parent $Diablo }
if (-not (Test-Path -LiteralPath $Diablo -PathType Container)) { Fail "no such folder: $Diablo" }
$Diablo = (Resolve-Path -LiteralPath $Diablo).Path.TrimEnd('\')
$contents = Describe $Diablo
if (-not $contents) { Fail "no DIABDAT.MPQ (or the shareware spawn.mpq) in $Diablo" }
Info "$Diablo ($contents)"

Step 'Which game'
$Game = $Game.ToLower()
if ($Game -and $Game -ne 'diablo' -and $Game -ne 'hellfire') { Fail '-Game takes diablo or hellfire' }
if ($contents -eq 'Diablo + Hellfire') {
	if (-not $Game) {
		Info 'Hellfire''s data is there too. Set up:'
		Info '  1) Diablo'
		Info '  2) Diablo + Hellfire (a shortcut for each; started on its own, the game asks which)'
		$answer = Ask 'Pick' '2'
		if ($answer -match '^(1|[dD])') { $Game = 'diablo' } elseif ($answer -match '^(2|[hH])') { $Game = 'hellfire' } else { Fail 'pick 1 or 2' }
	}
} else {
	if ($Game -eq 'hellfire') { Fail "no Hellfire data (hellfire.mpq, hfmonk.mpq, hfmusic.mpq, hfvoice.mpq) in $Diablo" }
	$Game = 'diablo'
	if ($contents -eq 'Diablo') { Info "Hellfire's data (hellfire.mpq and hf*.mpq) isn't there; GOG's Diablo comes with it: $GogDiablo" }
}
if ($Game -eq 'hellfire') { Info 'Diablo + Hellfire' } else { Info 'Diablo' }

# --- Daggerfall -------------------------------------------------------------------------------------------------------

# The folder (ARENA2) with WEAPON00.CIF in it, in or under a folder.
function Arena2In($dir) {
	if (-not $dir -or -not (Test-Path -LiteralPath $dir -PathType Container)) { return $null }
	$hit = Get-ChildItem -LiteralPath $dir -Filter 'WEAPON00.CIF' -File -Recurse -Depth 4 -ErrorAction SilentlyContinue | Select-Object -First 1
	if ($hit) { return $hit.DirectoryName }
	return $null
}
function SteamLibraries() {
	$libraries = @()
	$steam = $null
	try { $steam = (Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction Stop).SteamPath } catch { }
	if (-not $steam) { $steam = "${env:ProgramFiles(x86)}\Steam" }
	$libraries += $steam
	$vdf = "$steam\steamapps\libraryfolders.vdf"
	if (Test-Path -LiteralPath $vdf) {
		foreach ($line in Get-Content -LiteralPath $vdf) {
			if ($line -match '^\s*"path"\s*"(.+)"') { $libraries += $Matches[1].Replace('\\', '\') }
		}
	}
	return $libraries
}
function DetectDaggerfall() {
	$candidates = @((Join-Path $Here 'daggerfall'))  # the others as strings, as in DetectDiablo
	$candidates += GogFolders '*Daggerfall*'
	foreach ($lib in SteamLibraries) { $candidates += "$lib\steamapps\common\The Elder Scrolls II Daggerfall" }
	$candidates += @('C:\GOG Games\Daggerfall', 'D:\GOG Games\Daggerfall', "${env:ProgramFiles(x86)}\GOG Galaxy\Games\Daggerfall",
		'C:\dosgames\DAGGER', 'C:\DAGGER', "${env:ProgramFiles(x86)}\Bethesda Softworks\Daggerfall")
	$found = @()
	foreach ($c in $candidates) {
		$arena2 = Arena2In $c
		if ($arena2 -and $found -notcontains $arena2) { $found += $arena2 }
	}
	return $found
}

Step 'Weapons in hand (optional)'
$arena2 = $null
if ($NoDaggerfall) {
	Info 'None.'
} elseif ($Daggerfall) {
	$arena2 = Arena2In $Daggerfall
	if (-not $arena2) { Fail "no Daggerfall weapon art (WEAPON00.CIF) in $Daggerfall" }
	Info $arena2
} else {
	Info 'First person can show the weapon you hold, using the first-person weapon art of'
	Info "The Elder Scrolls II: Daggerfall, which is free: $GogDaggerfall"
	Info 'It''s read from your own copy of Daggerfall: this mod includes none of it.'
	$found = @(DetectDaggerfall)
	if ($found.Count -gt 0) {
		Info 'Found Daggerfall''s art in:'
		for ($i = 0; $i -lt $found.Count; $i++) { Info "  $($i + 1)) $($found[$i])" }
		if (YesNo 'Use Daggerfall''s weapons?' $true) {
			$picked = PickFolder $found 'Which one?' 'Daggerfall''s folder'
			$arena2 = Arena2In $picked
			if (-not $arena2) { Fail "no Daggerfall weapon art (WEAPON00.CIF) in $picked" }
		}
	} elseif (-not $Yes) {
		Info 'Daggerfall wasn''t found. Install it (free on GOG) and run Install.bat again,'
		if (YesNo 'or give the folder it''s in now?' $false) {
			$picked = Ask 'Daggerfall''s folder (or B to browse)' 'B'
			if ($picked -match '^[bB]$') { $picked = Browse 'Daggerfall''s folder (the one with ARENA2 in it)' }
			$arena2 = Arena2In $picked
			if (-not $arena2) { Fail "no Daggerfall weapon art (WEAPON00.CIF) in $picked" }
		} elseif (YesNo 'Open Daggerfall''s GOG page in your browser?' $false) {
			Start-Process $GogDaggerfall
		}
	}
	if ($arena2) { Info "Using $arena2" } else { Info 'No weapons in hand (run Install.bat again to add them).' }
}

# --- Copy -------------------------------------------------------------------------------------------------------------

Step "Setting up $Here"
if (-not (Test-Path -LiteralPath (Join-Path $Here 'devilutionx.exe'))) { Fail "devilutionx.exe isn't in ${Here}: run Install.bat from the unpacked zip" }

function CopyAs($from, $to) {
	$source = (Resolve-Path -LiteralPath $from).Path
	if ((Test-Path -LiteralPath $to) -and ((Resolve-Path -LiteralPath $to).Path -eq $source)) { return }
	Copy-Item -LiteralPath $source -Destination $to -Force
}

$copying = @(DiabloFiles $Diablo | Where-Object { $Game -eq 'hellfire' -or $HellfireFiles -notcontains (Split-Path -Leaf $_).ToLower() })
foreach ($f in $copying) {
	$name = (Split-Path -Leaf $f).ToLower()
	if ($name -eq 'diabdat.mpq') { $name = 'DIABDAT.MPQ' }
	Info "Copying $name..."
	CopyAs $f (Join-Path $Here $name)
}
# Diablo alone: Hellfire's data from an earlier setup goes, or the game would still offer it.
if ($Game -eq 'diablo') {
	foreach ($name in $HellfireFiles) {
		$f = Join-Path $Here $name
		if (Test-Path -LiteralPath $f) { Remove-Item -LiteralPath $f -Force }
	}
}
Ok (Describe $Here)

$daggerfallDir = Join-Path $Here 'daggerfall'
if ($arena2) {
	New-Item -ItemType Directory -Force -Path $daggerfallDir | Out-Null
	$count = 0
	foreach ($name in $DaggerfallFiles) {
		$f = Join-Path $arena2 $name
		if (Test-Path -LiteralPath $f) {
			CopyAs $f (Join-Path $daggerfallDir $name)
			$count++
		} else {
			Warn "Not in Daggerfall's folder: $name"
		}
	}
	Ok "Daggerfall's weapons ($count of $($DaggerfallFiles.Count) files)"
} elseif ($NoDaggerfall -and (Test-Path -LiteralPath $daggerfallDir)) {
	Remove-Item -LiteralPath $daggerfallDir -Recurse -Force
}

# --- Shortcuts --------------------------------------------------------------------------------------------------------

$exe = Join-Path $Here 'devilutionx.exe'
$madeShortcuts = $false
if (-not $NoShortcuts -and (YesNo 'Make shortcuts on the desktop and in the Start menu?' $true)) {
	try {
		$shell = New-Object -ComObject WScript.Shell
		$places = @([Environment]::GetFolderPath('Desktop'), (Join-Path ([Environment]::GetFolderPath('Programs')) 'Diablo First-Person'))
		New-Item -ItemType Directory -Force -Path $places[1] | Out-Null
		$links = @(@{ Name = 'Diablo First-Person'; Args = '' })
		if ($Game -eq 'hellfire') {
			$links = @(@{ Name = 'Diablo First-Person'; Args = '--diablo' }, @{ Name = 'Hellfire First-Person'; Args = '--hellfire' })
		}
		foreach ($place in $places) {
			$old = Join-Path $place 'Hellfire First-Person.lnk'
			if ($Game -eq 'diablo' -and (Test-Path -LiteralPath $old)) { Remove-Item -LiteralPath $old -Force }
			foreach ($link in $links) {
				$shortcut = $shell.CreateShortcut((Join-Path $place "$($link.Name).lnk"))
				$shortcut.TargetPath = $exe
				$shortcut.Arguments = $link.Args
				$shortcut.WorkingDirectory = $Here
				$shortcut.IconLocation = "$exe,0"
				$shortcut.Description = 'Diablo in first person (DevilutionX)'
				$shortcut.Save()
			}
		}
		Ok 'Shortcuts on the desktop and in the Start menu'
		$madeShortcuts = $true
	} catch {
		Warn "The shortcuts could not be made: $($_.Exception.Message)"
	}
}

Step 'Done'
if ($madeShortcuts) { Info 'Start the game with the shortcuts, or devilutionx.exe in this folder.' } else { Info 'Start the game with devilutionx.exe in this folder.' }
if ($Game -eq 'hellfire') { Info 'Started on its own, it asks whether to play Diablo or Hellfire.' }
Info 'It starts in first person: WASD walks, the mouse looks, F4 switches to the original view.'
Info 'To start in the original view instead: Settings > Gameplay > Start in First Person.'
Info 'Your heroes and settings are kept in %APPDATA%\diasurgical\devilution.'
if (-not $Yes -and (YesNo 'Start the game now?' $true)) { Start-Process -FilePath $exe -WorkingDirectory $Here }
