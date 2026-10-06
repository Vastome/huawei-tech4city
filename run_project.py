#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import signal
import socket
import shutil
import subprocess
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent
HARDWARE_DIR = ROOT / "hardware"
SOFTWARE_DIR = ROOT / "software"
EDGE_DIR = ROOT / "edge"
EDGE_BUILD_DIR = EDGE_DIR / "build"
HARDWARE_SETUP = HARDWARE_DIR / "setup_wokwi.py"
SOFTWARE_NODE_MODULES = SOFTWARE_DIR / "node_modules"
WOKWI_BRIDGE = ROOT / "wokwi_terminal_bridge.py"


def executable_name(base_name: str) -> str:
    if os.name == "nt" and base_name == "npm":
        return "npm.cmd"
    return base_name


def fail(message: str) -> int:
    print(f"Error: {message}", file=sys.stderr)
    return 1


def command_exists(name: str) -> bool:
    return shutil.which(name) is not None


def resolve_vscode_cli() -> str | None:
    candidates = ["code"]
    if os.name == "nt":
        candidates = ["code.cmd", "code.exe", "code"]

    for candidate in candidates:
        if shutil.which(candidate):
            return candidate
    return None


def run(cmd: list[str], cwd: Path | None = None, env: dict[str, str] | None = None) -> None:
    print(f"\n> {' '.join(cmd)}")
    process_env = os.environ.copy()
    if env:
        process_env.update(env)
    subprocess.run(cmd, cwd=str(cwd) if cwd else None, check=True, env=process_env)


def start_background(cmd: list[str], cwd: Path | None = None) -> subprocess.Popen[str]:
    print(f"\n> {' '.join(cmd)}")
    return subprocess.Popen(cmd, cwd=str(cwd) if cwd else None)


def ensure_paths() -> None:
    if not HARDWARE_SETUP.exists():
        raise FileNotFoundError(f"Missing hardware setup script: {HARDWARE_SETUP}")
    if not (SOFTWARE_DIR / "package.json").exists():
        raise FileNotFoundError(f"Missing software package.json: {SOFTWARE_DIR / 'package.json'}")
    if not (EDGE_DIR / "CMakeLists.txt").exists():
        raise FileNotFoundError(f"Missing edge CMake project: {EDGE_DIR / 'CMakeLists.txt'}")
    if not WOKWI_BRIDGE.exists():
        raise FileNotFoundError(f"Missing Wokwi bridge script: {WOKWI_BRIDGE}")


def ensure_npm() -> None:
    if not command_exists(executable_name("npm")):
        raise RuntimeError("npm was not found on PATH. Install Node.js 22.13 or newer.")


def ensure_software_dependencies() -> None:
    ensure_npm()
    if SOFTWARE_NODE_MODULES.exists():
        return

    print("Installing software dependencies because node_modules is missing...")
    run([executable_name("npm"), "install"], cwd=SOFTWARE_DIR)


def run_hardware(prepare_only: bool = False) -> None:
    command = [sys.executable, str(HARDWARE_SETUP)]
    if prepare_only:
        command.append("--prepare-only")
    run(command, cwd=ROOT)


def run_software_script(script_name: str, install: bool = True, env: dict[str, str] | None = None) -> None:
    if install:
        ensure_software_dependencies()
    run([executable_name("npm"), "run", script_name], cwd=SOFTWARE_DIR, env=env)


def run_edge_build(run_tests: bool = True) -> None:
    if not command_exists("cmake"):
        raise RuntimeError(
            "cmake was not found. On macOS run: brew install cmake opencv tesseract",
        )
    run(
        ["cmake", "-S", str(EDGE_DIR), "-B", str(EDGE_BUILD_DIR), "-DCMAKE_BUILD_TYPE=Release"],
        cwd=ROOT,
    )
    run(["cmake", "--build", str(EDGE_BUILD_DIR), "--parallel"], cwd=ROOT)
    if run_tests:
        run(["ctest", "--test-dir", str(EDGE_BUILD_DIR), "--output-on-failure"], cwd=ROOT)


def bridge_health_ok(host: str, port: int, timeout_seconds: float = 0.8) -> bool:
    try:
        with urllib.request.urlopen(
            f"http://{host}:{port}/health",
            timeout=timeout_seconds,
        ) as response:
            if response.status != 200:
                return False
            payload = json.loads(response.read().decode("utf-8"))
            return bool(payload.get("ok"))
    except Exception:  # noqa: BLE001
        return False


def find_free_port(host: str) -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.bind((host, 0))
        return int(sock.getsockname()[1])


def start_bridge_for_demo(host: str, preferred_port: int) -> tuple[subprocess.Popen[str] | None, int]:
    chosen_port = preferred_port
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as sock:
        sock.settimeout(0.3)
        if sock.connect_ex((host, preferred_port)) == 0:
            chosen_port = find_free_port(host)
            print(
                f"\nBridge port {preferred_port} is occupied by another process. "
                f"Starting bridge on port {chosen_port} instead.",
            )

    bridge_process = start_background(
        [sys.executable, str(WOKWI_BRIDGE), "--host", host, "--port", str(chosen_port)],
        cwd=ROOT,
    )

    for _ in range(20):
        if bridge_health_ok(host, chosen_port):
            print(f"\nBridge started on http://{host}:{chosen_port}")
            return bridge_process, chosen_port
        if bridge_process.poll() is not None:
            raise RuntimeError("Bridge process exited before becoming ready.")
        time.sleep(0.15)

    raise RuntimeError("Bridge did not become ready in time.")


def print_hardware_next_step() -> None:
    print("\nHardware is prepared. Open hardware/diagram.json in VS Code and start the Wokwi simulator.")


def open_demo_views_in_vscode() -> None:
    code_cli = resolve_vscode_cli()
    if not code_cli:
        print("\nVS Code CLI ('code') was not found on PATH; skipping automatic editor opening.")
        return

    try:
        run(
            [
                code_cli,
                "-r",
                str(HARDWARE_DIR / "diagram.json"),
                str(SOFTWARE_DIR / "app" / "page.tsx"),
            ],
            cwd=ROOT,
        )
    except (FileNotFoundError, subprocess.CalledProcessError):
        print("\nUnable to auto-open VS Code files; continuing demo startup.")


def terminate_process(process: subprocess.Popen[str]) -> None:
    if process.poll() is not None:
        return

    process.terminate()
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        if os.name == "nt":
            process.send_signal(signal.CTRL_BREAK_EVENT)
        process.kill()


def demo_command(args: argparse.Namespace) -> int:
    run_hardware(prepare_only=args.prepare_only)
    if args.open_vscode:
        open_demo_views_in_vscode()

    bridge_process: subprocess.Popen[str] | None = None
    bridge_endpoint: str | None = None
    if args.bridge:
        bridge_process, resolved_port = start_bridge_for_demo(args.bridge_host, int(args.bridge_port))
        bridge_endpoint = f"http://{args.bridge_host}:{resolved_port}/paste"

    print_hardware_next_step()
    try:
        software_env = (
            {"NEXT_PUBLIC_WOKWI_BRIDGE_ENDPOINT": bridge_endpoint}
            if bridge_endpoint
            else None
        )
        run_software_script("dev", env=software_env)
    finally:
        if bridge_process is not None:
            terminate_process(bridge_process)
    return 0


def verify_command(_: argparse.Namespace) -> int:
    run_hardware(prepare_only=False)
    run_software_script("test")
    run_software_script("lint", install=False)
    run_edge_build(run_tests=True)
    print(
        "\nVerification complete: hardware compiled; web build, tests and lint passed; "
        "C++ edge build and tests passed.",
    )
    return 0


def hardware_command(args: argparse.Namespace) -> int:
    run_hardware(prepare_only=args.prepare_only)
    print_hardware_next_step()
    return 0


def software_command(args: argparse.Namespace) -> int:
    run_software_script(args.script)
    return 0


def edge_command(args: argparse.Namespace) -> int:
    run_edge_build(run_tests=not args.no_test)
    return 0


def install_command(_: argparse.Namespace) -> int:
    ensure_software_dependencies()
    run_hardware(prepare_only=True)
    print("\nInstallation complete. Run 'python run_project.py demo' to launch the software side and prepare hardware.")
    return 0


def bridge_command(args: argparse.Namespace) -> int:
    run([sys.executable, str(WOKWI_BRIDGE), "--host", args.host, "--port", str(args.port)], cwd=ROOT)
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Cross-platform launcher for the Huawei Tech4City hardware and software demos.",
    )
    subparsers = parser.add_subparsers(dest="command")

    install_parser = subparsers.add_parser(
        "install",
        help="Install software dependencies and prepare the hardware project files.",
    )
    install_parser.set_defaults(handler=install_command)

    hardware_parser = subparsers.add_parser(
        "hardware",
        help="Prepare or compile the hardware project.",
    )
    hardware_parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="Only prepare hardware project files without native compilation.",
    )
    hardware_parser.set_defaults(handler=hardware_command)

    software_parser = subparsers.add_parser(
        "software",
        help="Run a software npm script from the repository root.",
    )
    software_parser.add_argument(
        "script",
        choices=["dev", "build", "start", "test", "lint"],
        help="The npm script to run inside software/.",
    )
    software_parser.set_defaults(handler=software_command)

    edge_parser = subparsers.add_parser(
        "edge",
        help="Build and test the C++ Raspberry Pi/ESP32 edge simulator.",
    )
    edge_parser.add_argument(
        "--no-test",
        action="store_true",
        help="Build without running the C++ test suite.",
    )
    edge_parser.set_defaults(handler=edge_command)

    demo_parser = subparsers.add_parser(
        "demo",
        help="Compile hardware, then start the software development server.",
    )
    demo_parser.add_argument(
        "--prepare-only",
        action="store_true",
        help="Prepare hardware files without native compilation before starting the software server.",
    )
    demo_parser.add_argument(
        "--no-bridge",
        dest="bridge",
        action="store_false",
        help="Do not auto-start the local Wokwi bridge.",
    )
    demo_parser.add_argument(
        "--bridge-host",
        default="127.0.0.1",
        help="Bridge host interface for demo mode.",
    )
    demo_parser.add_argument(
        "--bridge-port",
        default="8765",
        help="Bridge port for demo mode.",
    )
    demo_parser.add_argument(
        "--no-open-vscode",
        dest="open_vscode",
        action="store_false",
        help="Do not auto-open hardware and software files in VS Code.",
    )
    demo_parser.set_defaults(bridge=True, open_vscode=True)
    demo_parser.set_defaults(handler=demo_command)

    bridge_parser = subparsers.add_parser(
        "bridge",
        help="Start the local Wokwi terminal bridge for simulator handoff automation.",
    )
    bridge_parser.add_argument("--host", default="127.0.0.1", help="Bridge host interface.")
    bridge_parser.add_argument("--port", default="8765", help="Bridge port.")
    bridge_parser.set_defaults(handler=bridge_command)

    verify_parser = subparsers.add_parser(
        "verify",
        help="Run the full validation pass for hardware, web software and C++ edge software.",
    )
    verify_parser.set_defaults(handler=verify_command)

    return parser


def main() -> int:
    try:
        ensure_paths()
        parser = build_parser()
        if len(sys.argv) == 1:
            print("No command provided. Running default demo workflow...")
            args = parser.parse_args(["demo"])
            return args.handler(args)
        args = parser.parse_args()
        if not hasattr(args, "handler"):
            parser.print_help()
            return 2
        return args.handler(args)
    except subprocess.CalledProcessError as error:
        return fail(f"Command failed with exit code {error.returncode}.")
    except (FileNotFoundError, RuntimeError) as error:
        return fail(str(error))
    except KeyboardInterrupt:
        return fail("Interrupted.")


if __name__ == "__main__":
    raise SystemExit(main())
