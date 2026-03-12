# Mishabot: ESP32 Discord Image Bot

A lightweight proof-of-concept Discord image bot written for the ESP32 using the ESP-IDF framework. It queries the Danbooru API for random images and posts them to Discord. It also has a fishing minigame with stats tracking.

## Usage

Configure the project using `menuconfig` to set your WiFi and API credentials:

1. Run the configuration tool:
   ```bash
   idf.py menuconfig
   ```
2. Navigate to `Project Configuration`.
3. Set the following:
   - **WiFi SSID/Password**
   - **Discord Bot Token**
   - **Danbooru Login & API Key**

4. Build and flash the project to your ESP32:
   ```bash
   idf.py flash monitor
   ```

## Commands

- /fish
- .misha
- .furina
- .karen
- .kokomi
- .reisen
- .ika
- .amber
- .venti

## License

This project is licensed under the GNU Affero General Public License v3.0 (AGPL-3.0). See the [LICENSE](LICENSE) file for more details.
