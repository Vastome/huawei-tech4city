#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import platform
import shutil
import subprocess
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import urlparse

ALLOWED_ORIGIN_HOSTS = {"127.0.0.1", "localhost"}


def is_allowed_origin(origin: str | None) -> bool:
    if not origin:
        return False

    try:
        parsed = urlparse(origin)
    except ValueError:
        return False

    if parsed.scheme not in {"http", "https"}:
        return False

    return (parsed.hostname or "") in ALLOWED_ORIGIN_HOSTS


def command_exists(name: str) -> bool:
    return shutil.which(name) is not None


def write_clipboard_windows(text: str) -> None:
    subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "Set-Clipboard -Value ([Console]::In.ReadToEnd())",
        ],
        input=text,
        text=True,
        check=True,
    )


def paste_windows() -> None:
    subprocess.run(
        [
            "powershell",
            "-NoProfile",
            "-NonInteractive",
            "-Command",
            "$shell = New-Object -ComObject WScript.Shell; "
            "Start-Sleep -Milliseconds 120; "
            "$shell.SendKeys('^v'); "
            "Start-Sleep -Milliseconds 50; "
            "$shell.SendKeys('~')",
        ],
        check=True,
    )


def write_clipboard_macos(text: str) -> None:
    subprocess.run(["pbcopy"], input=text, text=True, check=True)


def paste_macos() -> None:
    subprocess.run(
        [
            "osascript",
            "-e",
            'tell application "System Events" to keystroke "v" using command down',
            "-e",
            'tell application "System Events" to key code 36',
        ],
        check=True,
    )


def write_clipboard_linux(text: str) -> None:
    if command_exists("xclip"):
        subprocess.run(["xclip", "-selection", "clipboard"], input=text, text=True, check=True)
        return

    if command_exists("wl-copy"):
        subprocess.run(["wl-copy"], input=text, text=True, check=True)
        return

    raise RuntimeError("Linux clipboard support requires xclip or wl-copy.")


def paste_linux() -> None:
    if command_exists("xdotool"):
        subprocess.run(["xdotool", "key", "--clearmodifiers", "ctrl+v", "Return"], check=True)
        return

    if command_exists("wtype"):
        subprocess.run(["wtype", "-M", "ctrl", "v", "-m", "ctrl", "Return"], check=True)
        return

    raise RuntimeError("Linux paste automation requires xdotool or wtype.")


class WokwiTerminalBridge:
    def __init__(self) -> None:
        self.system_name = platform.system().lower()

    def describe_backend(self) -> str:
        if self.system_name == "windows":
            return "PowerShell clipboard + SendKeys"
        if self.system_name == "darwin":
            return "pbcopy + osascript"
        if self.system_name == "linux":
            return "clipboard tool + window typing helper"
        raise RuntimeError(f"Unsupported operating system: {platform.system()}")

    def paste_text(self, text: str) -> None:
        if self.system_name == "windows":
            write_clipboard_windows(text)
            paste_windows()
            return

        if self.system_name == "darwin":
            write_clipboard_macos(text)
            paste_macos()
            return

        if self.system_name == "linux":
            write_clipboard_linux(text)
            paste_linux()
            return

        raise RuntimeError(f"Unsupported operating system: {platform.system()}")


def make_handler(bridge: WokwiTerminalBridge):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, format: str, *args: object) -> None:
            return None

        def _write_json(self, status: int, payload: dict[str, object]) -> None:
            self.send_response(status)
            origin = self.headers.get("Origin")
            if is_allowed_origin(origin):
                self.send_header("Access-Control-Allow-Origin", origin)
                self.send_header("Vary", "Origin")
            self.send_header("Access-Control-Allow-Headers", "Content-Type")
            self.send_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS")
            self.send_header("Content-Type", "application/json")
            self.end_headers()
            self.wfile.write(json.dumps(payload).encode("utf-8"))

        def do_OPTIONS(self) -> None:
            self._write_json(204, {})

        def do_GET(self) -> None:
            if self.path != "/health":
                self._write_json(404, {"error": "Not found"})
                return

            self._write_json(
                200,
                {
                    "ok": True,
                    "backend": bridge.describe_backend(),
                    "platform": platform.system(),
                },
            )

        def do_POST(self) -> None:
            if self.path != "/paste":
                self._write_json(404, {"error": "Not found"})
                return

            try:
                content_length = int(self.headers.get("Content-Length", "0"))
                payload = json.loads(self.rfile.read(content_length).decode("utf-8"))
                text = payload.get("text", "")
                if not isinstance(text, str) or not text.strip():
                    raise ValueError("text is required")

                bridge.paste_text(text)
                self._write_json(
                    200,
                    {
                        "ok": True,
                        "message": "Command pasted into the focused Wokwi Serial Monitor window.",
                    },
                )
            except Exception as error:  # noqa: BLE001
                self._write_json(500, {"error": str(error)})

    return Handler


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Local bridge that pastes Wokwi serial commands into the focused simulator window.",
    )
    parser.add_argument("--host", default="127.0.0.1", help="Host interface to bind. Defaults to 127.0.0.1")
    parser.add_argument("--port", type=int, default=8765, help="Port to bind. Defaults to 8765")
    args = parser.parse_args()

    bridge = WokwiTerminalBridge()
    server = ThreadingHTTPServer((args.host, args.port), make_handler(bridge))
    print(f"Wokwi terminal bridge listening on http://{args.host}:{args.port}")
    print(f"Backend: {bridge.describe_backend()}")
    print("Focus the Wokwi Serial Monitor window before sending from the OCR app.")

    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nBridge stopped.")
    finally:
        server.server_close()

    return 0


if __name__ == "__main__":
    raise SystemExit(main())