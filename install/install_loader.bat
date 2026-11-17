@echo off
setlocal
rem Hybrid Co-op for Total War: ROME II - installs the loader (dinput8.dll) next to Rome2.exe.
rem If hybrid_coop.pack is in this folder too (test builds), it is copied into the game's data folder.
rem The ROME II folder is found automatically (Steam registry entry, then all Steam libraries).
call :findgame
if not defined GAME (
  echo Could not find Total War: ROME II automatically.
  set /p GAME=Paste the game folder ^(the one with Rome2.exe^) and press Enter: 
)
set GAME=%GAME:"=%
if not exist "%GAME%\Rome2.exe" (echo "%GAME%" does not contain Rome2.exe & goto fail)
echo Game folder: %GAME%
copy /Y "%~dp0dinput8.dll" "%GAME%\dinput8.dll" >nul || goto fail
echo Loader installed.
if exist "%~dp0hybrid_coop.pack" (
  copy /Y "%~dp0hybrid_coop.pack" "%GAME%\data\hybrid_coop.pack" >nul || goto fail
  echo Test pack installed into the data folder.
)
echo Now enable "hybrid_coop" in the ROME II launcher.
pause
exit /b 0
:fail
echo Install failed. Is the game running? Copy dinput8.dll next to Rome2.exe by hand.
pause
exit /b 1

:findgame
for %%K in ("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 214950" "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 214950") do (
  for /f "tokens=2,*" %%a in ('reg query %%K /v InstallLocation 2^>nul ^| find "InstallLocation"') do (
    if exist "%%b\Rome2.exe" set "GAME=%%b"
  )
)
if defined GAME exit /b 0
for /f "usebackq delims=" %%g in (`powershell -NoProfile -Command "$s=(Get-ItemProperty 'HKCU:\Software\Valve\Steam' -ErrorAction SilentlyContinue).SteamPath; if(-not $s){exit}; $libs=@($s); $v=Join-Path $s 'steamapps\libraryfolders.vdf'; if(Test-Path $v){ $libs+= (Select-String -Path $v -Pattern ([char]34+'path'+[char]34+'\s+'+[char]34+'([^'+[char]34+']+)'+[char]34) | ForEach-Object { $_.Matches[0].Groups[1].Value -replace '\\\\','\' }) }; foreach($l in $libs){ $g=Join-Path $l 'steamapps\common\Total War Rome II'; if(Test-Path (Join-Path $g 'Rome2.exe')){ $g; break } }"`) do set "GAME=%%g"
if defined GAME exit /b 0
if exist "C:\Program Files (x86)\Steam\steamapps\common\Total War Rome II\Rome2.exe" set "GAME=C:\Program Files (x86)\Steam\steamapps\common\Total War Rome II"
exit /b 0
