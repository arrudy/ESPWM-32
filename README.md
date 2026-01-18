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
| Status manipulation via MQTT      | 🟡 Implemented – *staging / not fully tested* |
| Status reporting via MQTT         | 🟡 Implemented - *staging / not fully tested* |
| Fuzzy logic auto-frequency mode   | 🔴 Not implemented                            |
| Silent mode                       | 🔴 Not implemented                            |

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

Before building and flashing the firmware, you must create and configure a file named
`credentials.h` in the project src dir (./main directory, where the rest of the sources are located).

This file contains all network and broker credentials and is **not** tracked in version control.

---

### 1️⃣ Create `credentials.h`

Copy the template below and replace the placeholder values with your real credentials.

```c
#ifndef CREDENTIALS_H
#define CREDENTIALS_H

#define WIFI_SSID      "your_wifi_ssid"
#define WIFI_PASS      "your_wifi_password"

#define MQTT_BROKER_URI "your.broker.ip.address"
#define MQTT_USER "your_mqtt_user"
#define MQTT_PASS "your_mqtt_password"

//#define MQTT_USE_TLS

#ifdef MQTT_USE_TLS
  #define MQTT_PORT    8883
  #define MQTT_SCHEME  "mqtts"

  #define STRICT
  #ifdef STRICT // strict certificate policy
    #define CACERTPEM \
"-----BEGIN CERTIFICATE-----\n\
PUT_YOUR_CA_CERTIFICATE_HERE\n\
-----END CERTIFICATE-----\n"

  #else
      // --- PERMISSIVE MODE ---
      // Encrypted, but skips certificate validation.
      #define MQTT_SKIP_CERT_CHECK
      static const char *server_cert_pem = NULL; 
  #endif
#else
  #define MQTT_PORT       1883
  #define MQTT_SCHEME     "mqtt"   // Plain TCP scheme
  static const char *server_cert_pem = NULL;
#endif

#endif
```

---

### 2️⃣ TLS / Non-TLS configuration

By default the firmware uses **unencrypted MQTT (port 1883)**.

To enable **TLS encryption**:

1. Uncomment this line:

   ```c
   #define MQTT_USE_TLS
   ```
2. Paste your broker CA certificate into `CACERTPEM`.

**Modes**

| Mode       | Behavior                                                        |
| ---------- | --------------------------------------------------------------- |
| `STRICT`   | Full certificate validation                                     |
| Permissive | Encrypted but skips certificate verification (requires kconfig changes) |

---

### 3️⃣ PWM Pin Assignments

Default PWM pin mapping, as well as the rest of inverter config, is located in `driver.c`:

```c
#define SPWM_LEG1_LOW_PIN       12
#define SPWM_LEG1_HIGH_PIN      13
#define SPWM_LEG2_LOW_PIN       14
#define SPWM_LEG2_HIGH_PIN      27
```

⚠️ **Important:**
Most of these GPIOs (12, 13, 14) are also used by the ESP32 JTAG interface.
If you plan to use hardware debugging, you may need to reassign these pins to avoid conflicts.

---

Once `credentials.h` is created and your pin mapping is verified, the firmware is ready to build and flash.

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
