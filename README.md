# Huawei Tech4City Braille Display

Built for the Huawei Tech4City competition by the Vastome team.

This repository contains a Raspberry Pi Pico Wokwi simulation that shows an 8-dot braille pattern on a MAX7219 dot matrix and highlights the current character on an I2C LCD.

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
python setup_wokwi.py
```

3. Open `diagram.json` in VS Code.
4. Start the simulator with `Wokwi: Start Simulator`.

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

## License

This project is released under the MIT License. See [LICENSE](LICENSE).