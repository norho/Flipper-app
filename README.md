Flipper Zero UFUID/FUID Sealer & Writer
​A custom Flipper Zero application (FAP) designed for the precise writing and sealing of MIFARE Classic data to magic tags.
​Release 1.4 introduces a robust, crash-proof architecture that separates the high-speed radio engine (for UFUIDs) from the secure JIT compilation engine (for FUIDs). This ensures total system stability and hardware safety.
​🌟 Features
​Optimized UFUID Engine: Direct, high-speed flashing and automatic sealing for Gen2 UFUID magic tags.
​JIT FUID Converter: An integrated "Just-In-Time" tool to convert any .bin dump into a native .nfc file. This allows FUID cards to be written securely via the Flipper's official NFC application, bypassing hardware-level risks for locked-door tags.
​Crash-Proof Architecture: Asynchronous state machine that eliminates "ViewPort lockups" and protects the NFC radio from kernel panics.
​Modern API Compatibility: Built using current furi_hal_nfc poller APIs for maximum compatibility with modern firmware (Momentum, Unleashed, Official).
​📂 Prerequisites & File Structure
​Place your source .bin dumps in the following directory:
SD Card/nfc/
​🚀 Writing Procedures
​1. Writing to UFUID Cards (Gen2)
​UFUID tags feature a back-door for direct block access.
​Select "Scrivi tag UFUID" from the menu.
​Select your .bin file.
​The app will automatically unlock the tag, write the data block-by-block, and complete the process.
​(Optional) Use "Sigilla tag UFUID" to perform the irreversible locking sequence (0xE1, 0x85).
​2. Writing to FUID Cards
​FUID cards are cryptographically locked and do not support magic commands.
​Select "Prepara FUID (.bin > .nfc)" from the menu.
​Select your .bin file. The app will generate a .nfc file in the same folder.
​Exit our app and open the native Flipper NFC application.
​Select the newly created .nfc file and use the native "Write" function to deploy the data securely.
​🛠️ Build Instructions
​This project is built using the official ufbt (micro Flipper Build Tool).
​Clone this repository to your local machine.
​Open your terminal inside the project directory.
​Run ufbt to compile the .fap application.
​Copy the resulting .fap file from the dist folder to SD Card/apps/NFC/ on your Flipper Zero.
​⚠️ Disclaimer
​This software is provided for educational purposes and legitimate hardware prototyping only. Use it strictly on devices, tags, and systems you own or have explicit authorization to interact with. 
