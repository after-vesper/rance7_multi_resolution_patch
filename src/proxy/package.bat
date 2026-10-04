@echo off
rem Build the proxy DLL and assemble the distributable patch package
rem under dist\SengokuRance_MultiResPatch (plus a .zip next to it).
setlocal
cd /d %~dp0
call build.bat || exit /b 1

set DIST=..\..\dist\SengokuRance_MultiResPatch
if exist "%DIST%" rmdir /s /q "%DIST%"
mkdir "%DIST%" || exit /b 1
copy /y dinput.dll "%DIST%\" >nul
copy /y MultiRes.ini "%DIST%\" >nul
copy /y ..\..\docs\README_patch.txt "%DIST%\README.txt" >nul || echo WARN: README missing

powershell -NoProfile -Command "Compress-Archive -Force -Path '%DIST%' -DestinationPath '..\..\dist\SengokuRance_MultiResPatch.zip'" || exit /b 1
echo Packaged to dist\SengokuRance_MultiResPatch\
endlocal
