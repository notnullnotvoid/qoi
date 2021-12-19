@echo off
pushd "%~dp0"
    rem clang qoiconv.c -std=c99 -D_CRT_SECURE_NO_WARNINGS -Os -o qoiconv.exe
    rem if %errorlevel% neq 0 popd && exit /b %errorlevel%
    clang qoibench.cpp miniz.c spng.c -D_CRT_SECURE_NO_WARNINGS -DSPNG_USE_MINIZ -I. -Os -fuse-ld=lld -o qoibench.exe
    if %errorlevel% neq 0 popd && exit /b %errorlevel%
    qoibench.exe 3 images --nopng --onlytotals
    if %errorlevel% neq 0 popd && exit /b %errorlevel%
popd
