if not exist build md build
cd build
cmake -G "Visual Studio 16 2019" -A Win32 .. 
devenv media_sdk.sln /Build "Debug|Win32"
pause