$py = "C:\Users\fhaiy\AppData\Local\Programs\Python\Python311\python.exe"
if (-not (Test-Path $py)) { $py = "C:\Users\fhaiy\AppData\Roaming\Python\Python311\python.exe" }
Start-Process -FilePath $py -ArgumentList "D:\ESP32-S3\scripts\flashcap13.py 180" -WindowStyle Hidden
Write-Output "launched flashcap13"
