#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent
PROJECT_NAME = "Huawei Tech4City Braille Display"
BOARD_FQBN = "arduino:mbed_rp2040:pico"
ARDUINO_CORE = "arduino:mbed_rp2040"
LIBRARIES = ["LiquidCrystal I2C"]
SOURCE_SKETCH = ROOT / "sketch.ino"
WRAPPER_DIR = ROOT / "simsrc"
WRAPPER_SKETCH = WRAPPER_DIR / "simsrc.ino"
BUILD_DIR = WRAPPER_DIR / "build"
WOKWI_TOML = ROOT / "wokwi.toml"
TOOLS_DIR = ROOT / ".tools" / "arduino-cli"
GITHUB_RELEASE_API = "https://api.github.com/repos/arduino/arduino-cli/releases/latest"


def run(cmd: list[str], cwd: Path | None = None) -> None:
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True)


def command_output(cmd: list[str]) -> str:
    completed = subprocess.run(cmd, capture_output=True, text=True, check=True)
    return completed.stdout


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def write_text(path: Path, content: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")


def is_ios() -> bool:
    platform_blob = f"{platform.platform()} {platform.system()} {platform.machine()}".lower()
    return any(token in platform_blob for token in ("ios", "iphone", "ipad"))


def cli_on_path() -> str | None:
    existing = shutil.which("arduino-cli")
    if existing:
        return existing

    common_paths = [
        Path("C:/Program Files/Arduino CLI/arduino-cli.exe"),
        Path("C:/Program Files (x86)/Arduino CLI/arduino-cli.exe"),
        Path.home() / ".local" / "bin" / "arduino-cli",
        Path("/usr/local/bin/arduino-cli"),
        Path("/opt/homebrew/bin/arduino-cli"),
    ]
    for path in common_paths:
        if path.exists():
            return str(path)

    return None


def download_file(url: str, dest: Path) -> None:
    request = urllib.request.Request(url, headers={"User-Agent": "tech4city-setup/1.0"})
    with urllib.request.urlopen(request) as response, dest.open("wb") as file_handle:
        shutil.copyfileobj(response, file_handle)


def latest_release_asset_name() -> str:
    system_name = platform.system().lower()
    machine_name = platform.machine().lower()

    if system_name == "windows":
        return "arduino-cli_1.5.1_Windows_64bit.zip"
    if system_name == "linux":
        if "aarch64" in machine_name or "arm64" in machine_name:
            return "arduino-cli_1.5.1_Linux_ARM64.tar.gz"
        return "arduino-cli_1.5.1_Linux_64bit.tar.gz"
    if system_name == "darwin":
        if "arm" in machine_name:
            return "arduino-cli_1.5.1_macOS_ARM64.tar.gz"
        return "arduino-cli_1.5.1_macOS_64bit.tar.gz"

    raise RuntimeError(f"Unsupported operating system: {platform.system()}")


def ensure_arduino_cli() -> str | None:
    existing = cli_on_path()
    if existing:
        return existing

    if is_ios():
        return None

    asset_name = latest_release_asset_name()
    TOOLS_DIR.mkdir(parents=True, exist_ok=True)
    cache_dir = TOOLS_DIR / "downloads"
    cache_dir.mkdir(parents=True, exist_ok=True)
    archive_path = cache_dir / asset_name

    if not archive_path.exists():
        release = json.loads(urllib.request.urlopen(urllib.request.Request(GITHUB_RELEASE_API, headers={"User-Agent": "tech4city-setup/1.0"})).read().decode("utf-8"))
        asset = next((item for item in release["assets"] if item["name"] == asset_name), None)
        if asset is None:
            raise RuntimeError(f"Could not find Arduino CLI asset: {asset_name}")
        download_file(asset["browser_download_url"], archive_path)

    extract_dir = TOOLS_DIR / "install"
    if extract_dir.exists():
        shutil.rmtree(extract_dir)
    extract_dir.mkdir(parents=True, exist_ok=True)

    if archive_path.suffix == ".zip":
        with zipfile.ZipFile(archive_path) as archive:
            archive.extractall(extract_dir)
    else:
        with tarfile.open(archive_path, "r:gz") as archive:
            archive.extractall(extract_dir)

    candidates = list(extract_dir.rglob("arduino-cli.exe")) + list(extract_dir.rglob("arduino-cli"))
    if not candidates:
        raise RuntimeError("Arduino CLI binary was not found after extraction.")

    cli_path = candidates[0]
    if os.name != "nt":
        cli_path.chmod(cli_path.stat().st_mode | 0o111)
    return str(cli_path)


def ensure_wokwi_toml() -> None:
    desired = """[wokwi]
version = 1
firmware = 'simsrc/build/simsrc.ino.uf2'
elf = 'simsrc/build/simsrc.ino.elf'
"""
    write_text(WOKWI_TOML, desired)


def mirror_sketch_to_wrapper() -> None:
    if not SOURCE_SKETCH.exists():
        raise FileNotFoundError(f"Missing source sketch: {SOURCE_SKETCH}")
    WRAPPER_DIR.mkdir(parents=True, exist_ok=True)
    shutil.copy2(SOURCE_SKETCH, WRAPPER_SKETCH)


def install_dependencies(cli_path: str) -> None:
    core_list = command_output([cli_path, "core", "list"])
    lib_list = command_output([cli_path, "lib", "list"])

    core_installed = ARDUINO_CORE in core_list
    libraries_installed = all(library_name in lib_list for library_name in LIBRARIES)

    if core_installed and libraries_installed:
        print("Arduino core and libraries are already installed.")
        return

    try:
        run([cli_path, "core", "update-index"])
    except subprocess.CalledProcessError:
        print("Warning: could not update the Arduino package index. Continuing with cached packages when possible.")

    if not core_installed:
        run([cli_path, "core", "install", ARDUINO_CORE])

    for library_name in LIBRARIES:
        if library_name not in lib_list:
            run([cli_path, "lib", "install", library_name])


def compile_firmware(cli_path: str) -> None:
    BUILD_DIR.mkdir(parents=True, exist_ok=True)
    run([
        cli_path,
        "compile",
        "--fqbn",
        BOARD_FQBN,
        "--build-path",
        str(BUILD_DIR),
        str(WRAPPER_DIR),
    ])


def main() -> int:
    parser = argparse.ArgumentParser(description=f"Set up {PROJECT_NAME} for Wokwi")
    parser.add_argument("--prepare-only", action="store_true", help="Skip native compilation and only generate project files")
    args = parser.parse_args()

    ensure_wokwi_toml()
    mirror_sketch_to_wrapper()

    if args.prepare_only:
        print("Project files are ready.")
        print("Native tool installation and firmware compilation were skipped because --prepare-only was requested.")
        print("Run this script without --prepare-only on Windows, Ubuntu, or macOS to build firmware locally.")
        return 0

    if is_ios():
        print("Project files are ready.")
        print("On iOS, native tool installation and firmware compilation are skipped.")
        print("Use the browser-based Wokwi simulator, or run this script on Windows, Ubuntu, or macOS to build firmware locally.")
        return 0

    cli_path = ensure_arduino_cli()
    if cli_path is None:
        print("Arduino CLI could not be installed automatically on this platform.")
        return 1

    install_dependencies(cli_path)
    compile_firmware(cli_path)

    print("Setup complete.")
    print(f"Firmware: {BUILD_DIR / 'simsrc.ino.uf2'}")
    print(f"ELF: {BUILD_DIR / 'simsrc.ino.elf'}")
    print("Open diagram.json in VS Code and start Wokwi Simulator.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())