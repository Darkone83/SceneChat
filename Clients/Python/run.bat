@echo off
title SceneChat
python SceneChat.py
if %ERRORLEVEL% neq 0 (
    echo.
    echo Python not found. Please install Python 3.8+ from https://python.org
    pause
)