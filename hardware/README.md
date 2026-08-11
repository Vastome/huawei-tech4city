# Huawei Tech4City Braille Display

Built for the Huawei Tech4City competition by the Vastome team.

Canonical repository:
https://github.com/Vastome/huawei-tech4city

This folder contains the hardware half of the project: a Raspberry Pi Pico Wokwi simulation that shows an 8-dot braille pattern on a MAX7219 dot matrix and highlights the current character on an I2C LCD.

Note: this folder is maintained inside the main repository above (it is not a separate Git repository).

It now also accepts direct remote handoff frames over serial from the software demo.

## What this repo includes

- `sketch.ino` - the Pico sketch
- `diagram.json` - the Wokwi circuit
- `wokwi.toml` - local simulator configuration
- `setup_wokwi.py` - cross-platform bootstrap script

## What the setup script does

`setup_wokwi.py` prepares the project for local use on Windows, Ubuntu, or macOS:

- copies the main sketch into the build folder Wokwi uses for firmware generation
- downloads Arduino CLI if it is not already installed
- installs the Arduino RP2040 core
- installs the LCD library used by the sketch
- compiles the firmware into `simsrc/build/`
- refreshes `wokwi.toml` so it points to the generated firmware

On iOS, the script only prepares the project files. Native firmware compilation and the desktop Wokwi extension are not available there, so use the browser-based Wokwi simulator for viewing the diagram or run the setup script on a desktop OS to build the firmware.

## Quick start

1. Install Python 3.10 or newer.
2. Run the setup script from the repository root:

```bash
python hardware/setup_wokwi.py
```

3. Open `hardware/diagram.json` in VS Code.
4. Start the simulator with `Wokwi: Start Simulator`.

If you prefer working from inside this folder, `python setup_wokwi.py` still works there as well.

If you already have Arduino CLI installed and want to skip tool installation, the script will still use it when available.

## Manual simulator setup

If you want to do the build steps yourself:

1. Compile the sketch for the Raspberry Pi Pico.
2. Place the generated firmware files under `simsrc/build/`.
3. Keep `wokwi.toml` pointing to:

```toml
[wokwi]
version = 1
firmware = 'simsrc/build/simsrc.ino.uf2'
elf = 'simsrc/build/simsrc.ino.elf'
```

## Project notes

- The braille display is wired for the Vastome Huawei Tech4City build.
- The MAX7219 matrix uses the top-left 4x2 area for the 8-dot braille cell.
- The serial monitor is connected over UART on GP0 and GP1.
- The companion OCR web app lives in the sibling `software/` folder at the repository root.
- The Wokwi simulator can accept a single pasted command in the serial monitor using `BATCH:<holdMs>,<blinkMs>|<frames...>`.
- Direct software handoff uses `CONFIG:` lines such as `CONFIG:700,233`.
- Direct software handoff uses `PINS:` lines such as `PINS:10000000,11000000`.
- Each frame contains 8 dot bits in dot order `1..8`, where `1` means the pin is raised for that cell.
- The firmware replies with `ACK CONFIG ...`, `FRAME x/y`, `OK remote frames displayed`, or `ERR ...`.

## License

This project is released under the MIT License. See the repository-root `LICENSE` file.