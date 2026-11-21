@echo off
setlocal
cd /d "%~dp0"
echo X68KTab5 R8T5 PPA Direct -- APP-ONLY, preserve HumanXDF
echo Requires CI build artifact and Tab5 on COM9.
python "tools\flash_r8t5_app_only.py" --port COM9
exit /b %ERRORLEVEL%
