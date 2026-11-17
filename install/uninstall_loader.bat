@echo off
setlocal
rem Removes the Hybrid Co-op loader (dinput8.dll) from the ROME II folder.
for %%K in ("HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 214950" "HKLM\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\Steam App 214950") do (
  for /f "tokens=2,*" %%a in ('reg query %%K /v InstallLocation 2^>nul ^| find "InstallLocation"') do (
    if exist "%%b\Rome2.exe" set "GAME=%%b"
  )
)
if not defined GAME set /p GAME=Paste the game folder ^(the one with Rome2.exe^) and press Enter: 
set GAME=%GAME:"=%
del "%GAME%\dinput8.dll" && echo Loader removed from %GAME%.
pause
