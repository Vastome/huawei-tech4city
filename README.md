# Huawei Tech4City Reader

Unified hardware + software workspace for the Tech4City demo.

This repository includes:

- `hardware/`: Raspberry Pi Pico + Wokwi hardware simulation (MAX7219 + LCD)
- `software/`: browser OCR app that captures printed text and sends hardware frames
- `run_project.py`: cross-platform launcher for install, demo, and verification
- `wokwi_terminal_bridge.py`: local bridge for Wokwi Serial Monitor handoff

## Requirements

- Python 3.10+
- Node.js 22.13+
- VS Code with Wokwi extension (for simulator workflow)
- Chrome or Edge desktop (required for Web Serial to real Pico)

## Quick Start (Default Demo)

From repository root:

```bash
python run_project.py
```

Default behavior with no arguments:

1. Compiles/prepares hardware firmware via `hardware/setup_wokwi.py`
2. Opens hardware and software files in VS Code when `code` CLI is available
3. Starts a fresh local bridge automatically
4. Starts software dev server automatically
5. If bridge port `8765` is busy, chooses a free port automatically and injects it into the app

## Command Reference

```bash
python run_project.py install
python run_project.py demo
python run_project.py demo --prepare-only
python run_project.py hardware --prepare-only
python run_project.py software dev
python run_project.py bridge
python run_project.py verify
```

- `install`: install software dependencies and prepare hardware files
- `demo`: full integrated run (hardware prep + bridge + software dev)
- `hardware`: run only hardware setup/compile path
- `software <script>`: run software script (`dev`, `build`, `start`, `test`, `lint`)
- `bridge`: run the local Wokwi bridge directly
- `verify`: hardware compile + software tests + lint

## End-to-End Demo Flow

1. Run `python run_project.py`.
2. In the browser app, click `Start camera`, grant permission, and scan one printed line.
3. In VS Code, open `hardware/diagram.json` and start Wokwi simulator.
4. Keep Wokwi Serial Monitor focused while scanning.
5. The app shows recognized OCR text and send state (`Waiting to send`, `Sending`, `Sent`, `Send failed`).
6. OCR output is sent to selected hardware target:
	 - Wokwi: through local bridge as one `BATCH:` command
	 - Real Pico: via Web Serial (`CONFIG:` then `PINS:`)

## Hardware Protocol

Software to hardware commands:

- `CONFIG:<holdMs>,<blinkMs>`
- `PINS:<8bit>,<8bit>,...`
- `BATCH:<holdMs>,<blinkMs>|<8bit>,<8bit>,...`

Hardware replies include:

- `ACK CONFIG ...`
- `FRAME x/y`
- `OK remote frames displayed`
- `ERR ...`

## Troubleshooting

- `code` not found:
	- Demo continues without auto-opening files.
	- Optionally enable VS Code PATH integration.
- Bridge send fails:
	- Ensure Wokwi Serial Monitor is focused.
	- Use `Copy Wokwi command` fallback in the app.
- Port conflict:
	- Demo auto-selects a free bridge port and passes it to software.
- Camera or Web Serial unavailable:
	- Use a modern desktop browser and grant permissions.

## Repository Structure

- `hardware/README.md`: hardware-specific setup and notes
- `software/README.md`: software-specific setup and tests

## License

See `LICENSE`.