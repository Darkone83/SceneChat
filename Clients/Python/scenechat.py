#!/usr/bin/env python3
"""
SceneChat.py -- Bootstrap launcher for the SceneChat Python client.
Double-click or run directly. Installs PySide6 + cryptography if missing,
then launches the application.
"""

import sys
import subprocess
import importlib
import os

REQUIRED = {
    'PySide6':      'PySide6',
    'cryptography': 'cryptography',
}

def _pip_install(packages):
    print(f"[SceneChat] Installing: {packages}")
    try:
        subprocess.check_call(
            [sys.executable, '-m', 'pip', 'install', '--quiet'] + packages
        )
        return True
    except subprocess.CalledProcessError as e:
        print(f"[SceneChat] pip install failed: {e}")
        return False

def bootstrap():
    missing = []
    for import_name, pip_name in REQUIRED.items():
        try:
            importlib.import_module(import_name)
        except ImportError:
            missing.append(pip_name)

    if not missing:
        return True

    print(f"[SceneChat] First run — installing dependencies: {missing}")

    # Try without --user first (venvs, most system Pythons)
    if _pip_install(missing):
        return True

    # Fallback: --user flag (some system Pythons need this)
    print("[SceneChat] Retrying with --user flag...")
    if _pip_install(['--user'] + missing):
        return True

    print("[SceneChat] ERROR: Could not install dependencies.")
    print("  Please run manually:")
    print(f"    pip install {' '.join(missing)}")
    input("Press Enter to exit...")
    return False

if __name__ == '__main__':
    if not bootstrap():
        sys.exit(1)

    # Import and run after dependencies are confirmed present
    from sc_ui import main
    main()