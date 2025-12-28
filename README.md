# ESPWM-32 - ESP32 Inverter

![MCU](https://img.shields.io/badge/MCU-ESP32-blue)
![Framework](https://img.shields.io/badge/Framework-ESP--IDF_5.4.x-green)
![RTOS](https://img.shields.io/badge/RTOS-FreeRTOS-orange)
![Protocol](https://img.shields.io/badge/Protocol-MQTT-yellow)
![Build](https://img.shields.io/badge/Build-CMake-lightgrey)

This project provides firmware for an **ESP32-based inverter controller** and its integration with home appliances using the MQTT protocol.
The device exposes a structured topic tree for controlling inverter operation and reporting its runtime status.

> ⚠️ This project is in **staging phase** – MQTT communication layer is being developed, the inverter driver is present but **not well tested**, and **fuzzy logic auto-frequency control is not yet implemented**.

---

## 📌 Project Status

| Feature                           | Status                                        |
| --------------------------------- | --------------------------------------------- |
| Inverter driver (SPWM generation) | 🟡 Implemented – *staging / not fully tested* |
| MQTT communication                | 🟡 In progress                                |
| Manual frequency control          | 🟡 Partial                                    |
| Status manipulation via MQTT      | 🟡 Partial                                    |
| Status reporting via MQTT         | 🟡 In progress                                |
| Fuzzy logic auto-frequency mode   | 🔴 Not implemented                            |
| Silent mode                       | 🔴 Not implemented                            |
| Electrical protection logic       | 🔴 Not implemented                            |

---

## 🔧 MQTT Topic Structure

### Control Topics

| Topic                                         | Payload               | Description                                              |
| --------------------------------------------- | --------------------- | -------------------------------------------------------- |
| `home/inverter/<device_id>/control/state`     | `"ON"` / `"OFF"`      | Enable or disable inverter                               |
| `home/inverter/<device_id>/control/frequency` | `float` (e.g. `50.0`) | Target output frequency in Hz                            |
| `home/inverter/<device_id>/control/auto_freq` | `"ON"` / `"OFF"`      | Enable fuzzy logic frequency control *(not implemented)* |
| `home/inverter/<device_id>/control/silent`    | `"ON"` / `"OFF"`      | Enable silent mode *(not implemented)*                   |

---

### Status Topics

| Topic                                        | Payload          | Description                                |
| -------------------------------------------- | ---------------- | ------------------------------------------ |
| `home/inverter/<device_id>/status/state`     | `"ON"` / `"OFF"` | Current inverter state                     |
| `home/inverter/<device_id>/status/frequency` | `float`          | Actual output frequency                    |
| `home/inverter/<device_id>/status/mod_index` | `float`          | PWM modulation index (duty multiplier)     |
| `home/inverter/<device_id>/status/diff_step` | `int`            | Step used for smooth frequency transitions |
| `home/inverter/<device_id>/status/auto_freq` | `"ON"` / `"OFF"` | Fuzzy logic mode state                     |
| `home/inverter/<device_id>/status/silent`    | `"ON"` / `"OFF"` | Silent mode state                          |

---

## ⚙️ Capabilities

* ESP32-based inverter control firmware
* MCPWM generation driver for inverter stage
* MQTT interface for:

  * ON / OFF switching
  * Frequency setpoint control
  * Runtime status reporting

* Prepared API for fuzzy-logic based auto-frequency mode *(not implemented yet)*

---

## 🛠 Build Requirements

| Component | Version                                                |
| --------- | ------------------------------------------------------ |
| ESP-IDF   | **v5.4.x**                                             |
| Hardware  | Any **ESP32** variant (ESP32, ESP32-S3, ESP32-C3 etc.) |
| OS        | Linux / macOS / Windows                                |

---

## 📦 Dependencies

This project uses only **ESP-IDF built-in components**.

No external libraries required.

---

## 🔐 Required Configuration

> ⚠️ **TODO**

This section will describe required configuration options such as:

* Wi-Fi credentials
* MQTT broker address and credentials
* Device ID mapping
* PWM pin assignments

---

## ⚡ Electrical Schematics

> ⚠️ **TODO**

This section will contain:

* Inverter power stage schematic
* ESP32 pin mapping
* Gate driver topology
* Safety and isolation notes

---

## 🏗 Disclaimer

This project controls **high-power electrical hardware**.
Use at your own risk. No responsibility is taken for hardware damage or personal injury.
