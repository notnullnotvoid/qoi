@echo off
pushd "%~dp0"
    clang qoiconv.c -std=c99 -D_CRT_SECURE_NO_WARNINGS -Os -o qoiconv.exe
    if %errorlevel% neq 0 popd && exit /b %errorlevel%
    clang qoibench.cpp -D_CRT_SECURE_NO_WARNINGS -Os -o qoibench.exe
    if %errorlevel% neq 0 popd && exit /b %errorlevel%
    qoibench.exe 10 images/lance
    if %errorlevel% neq 0 popd && exit /b %errorlevel%
popd
