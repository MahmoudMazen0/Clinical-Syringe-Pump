# 💉 Clinical-Grade Syringe Pump

<div align="center">

![Syringe Pump](https://img.shields.io/badge/Medical-Syringe%20Pump-blue?style=for-the-badge&logo=heart&logoColor=white)
![ESP32](https://img.shields.io/badge/ESP32-Firmware-red?style=for-the-badge&logo=espressif&logoColor=white)
![Python](https://img.shields.io/badge/Python-Flet%20Dashboard-3776AB?style=for-the-badge&logo=python&logoColor=white)
![WebSocket](https://img.shields.io/badge/WebSocket-Real--Time-green?style=for-the-badge&logo=socket.io&logoColor=white)

**A high-precision, IoT-enabled syringe pump firmware and dashboard**  
*Task 03 — Medical Equipment Course | Second Term 2026*

</div>

---

The system consists of an **ESP32** controlling a stepper motor via non-blocking architecture, paired with a modern **Python/Flet** dashboard communicating over WebSockets for real-time telemetry and control.

---

## 📋 Table of Contents

- [Key Features](#-key-features)
- [Hardware Requirements](#️-hardware-requirements)
- [Project Structure](#️-project-structure)
- [Setup & Installation](#️-setup--installation)
- [Safety Warning](#️-safety-warning)
- [Team](#-team)

## 🚀 Key Features

* **High-Precision Control**: Non-blocking microsecond-level stepper motor driving (28BYJ-48) with mechanical backlash compensation.
* **Auto-Recovery (Black Box)**: Real-time EEPROM state saving allows the pump to safely recover and resume interrupted sessions after power loss.
* **Safety & Alarms**: 
  * **Occlusion Detection**: Uses an FSR (Force Sensitive Resistor) to detect blockages in the IV line.
  * **Empty Syringe**: Infrared (IR) sensor detects the plunger's black marker to stop the pump when empty.
  * **Acoustic Warnings**: Distinct buzzer patterns for different alarm states (Occlusion, Empty, Dose Completed) with user-controlled muting.
* **Real-time Telemetry**: 100ms WebSocket dispatch rate ensures the UI reflects the pump's physical state instantly without UI freezing.
* **Cross-Platform Dashboard**: A beautiful, thread-safe dashboard built with Python and Flet, capable of running on Desktop, Web, or Android.

## 🛠️ Hardware Requirements

* **Microcontroller**: ESP32
* **Actuator**: 28BYJ-48 Stepper Motor + ULN2003 Driver
* **Sensors**: 
  * Force Sensitive Resistor (FSR) connected to an analog pin.
  * IR Obstacle Avoidance Sensor (Digital).
* **Audio**: Active Buzzer.

## 🗂️ Project Structure

* `syringe_pump_esp32.ino`: The C++ firmware for the ESP32. Compilable via Arduino IDE. Contains the WebSocket server, motor stepping logic, and EEPROM layout.
* `python_dashboard/`: The Python interface application.
  * `main.py`: The main Flet UI and WebSocket client script.

## ⚙️ Setup & Installation

### 1. ESP32 Firmware
1. Open `syringe_pump_esp32.ino` in the Arduino IDE.
2. Install dependencies via the Library Manager:
   - `WebSockets` by Markus Sattler
   - `ArduinoJson` by Benoit Blanchon
3. Upload to your ESP32 board.
4. The ESP32 will host an Access Point named **`SyringePump`** (Password: `12345678`).

### 2. Python Dashboard
1. Connect your PC/Phone to the `SyringePump` WiFi network.
2. Navigate to the `python_dashboard` directory.
3. Install dependencies:
   ```bash
   pip install flet websocket-client
   ```
4. Run the application:
   ```bash
   python main.py
   ```
*(Note: To build for Android, run `flet build apk` in the directory after installing Flutter SDK).*

## 🛡️ Safety Warning

This software/hardware is a prototype developed for educational and research purposes. **Do not use on humans or animals.** Always consult medical device regulations before deploying clinical equipment.

---

## 👥 Team

| # | Name |
|---|---|
| 1 | **Mahmoud Mazen** |
| 2 | **Philopater Emad** |
| 3 | **Mohannad** |
| 4 | **Yassin Omar** |
| 5 | **Youssef Ahmed** |
| 6 | **Mohamed Hamdy** |

*Medical Equipment Course — Second Term 2026*

---

<div align="center">
  <sub>Built with ❤️ for patient safety simulation</sub>
</div>
