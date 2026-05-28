#!/usr/bin/env bash
cd "$(dirname "$0")"
if command -v python3 &>/dev/null; then
    python3 SceneChat.py
elif command -v python &>/dev/null; then
    python SceneChat.py
else
    echo "Python 3 not found. Please install Python 3.8+ from https://python.org"
    read -p "Press Enter to exit..."
fi