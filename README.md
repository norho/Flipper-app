# Flipper Zero UFUID/FUID Sealer & Writer

A custom Flipper Zero application (FAP) developed to write MIFARE Classic `.bin` dumps directly to UFUID and FUID magic tags, followed by an automatic and irreversible sealing process. 

This tool is highly optimized for modern custom firmware architectures (such as Momentum) and utilizes low-level Poller APIs with manual CRC-A calculation for maximum stability and reliability.

## 🌟 Features
- **Direct `.bin` Flashing:** Reads a raw dump file (`bambu_tag_dump.bin`) from the Flipper's SD card and writes it block-by-block.
- **UFUID Permanent Sealing:** Automatically sends the raw backdoor lock commands (`0x40` at 7-bits, `0x43`, `0xE1 0x00`, `0x85...`) to permanently lock the UID.
- **FUID Support:** Writes flawlessly to FUID tags (which auto-lock upon the first Block 0 write).
- **Modern API Compatibility:** Built using `furi_hal_nfc_poller_tx` and `furi_hal_nfc_poller_rx`, completely bypassing outdated HAL functions that cause compilation errors on newer firmware versions.

## 📂 Prerequisites & File Structure
Before launching the application, you must place your target `.bin` dump on the Flipper Zero's SD card at the exact following path:
`SD Card/nfc/bambu_tag_dump.bin`

## 🛠️ Build Instructions
This project is built using the official `ufbt` (micro Flipper Build Tool).
1. Clone this repository to your local machine.
2. Open your terminal inside the project directory.
3. Run `ufbt` to compile the `.fap` application.
4. The compiled application will be generated in the `dist` folder.
5. Copy the `.fap` file to `SD Card/apps/NFC` on your Flipper Zero (via qFlipper or directly to the SD).

## 🚀 Usage
1. Open the **UFUID Sealer** app from the `Applications -> NFC` menu on your Flipper Zero.
2. Place your blank UFUID or FUID tag on the back of the Flipper.
3. Wait a few seconds for the read/write/seal process to complete. You can monitor the detailed progress and block-by-block status via the `qFlipper` CLI logs.
4. Once sealed, the tag will behave exactly like a standard, read-only MIFARE Classic tag.

## ⚠️ Disclaimer
This software is provided for educational purposes and legitimate hardware prototyping only. Use it strictly on devices, tags, and systems you own or have explicit authorization to interact with.
