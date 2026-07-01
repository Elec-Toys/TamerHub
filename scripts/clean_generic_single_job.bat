@echo off
cd /d "D:\Coding\_Arduino\Openshock Revised Hub"
C:\Users\leven\.platformio\penv\Scripts\platformio.exe run -e Generic-ESP32-Dev -t clean
echo CLEAN_EXIT=%ERRORLEVEL%
