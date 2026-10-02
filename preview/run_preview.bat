@echo off
REM Detached launcher for the tkinter live preview window.
REM `start` spawns python in its own process tree so it survives the launching shell.
cd /d D:\ESP32-S3\preview
start "" C:\Users\fhaiy\AppData\Local\Programs\Python\Python311\python.exe live_preview.py
