# Industrial IoT Monitoring & Control with P10 LED Display

**ATmega2560 | Cellular (Neo I58) | IPv6 HTTP | SMS Control | P10 LED | DHT22 | DS3231**

An embedded firmware prototype for environmental monitoring in manufacturing settings. It displays time, date, temperature, and humidity on a two-panel P10 LED matrix; sends timestamped telemetry to a remote HTTP endpoint over cellular connectivity; and accepts SMS commands for basic equipment control.

> **Scope note:** The shared firmware monitors **temperature and humidity**. It does **not** implement manufacturing production counts, targets, or a web dashboard frontend. It publishes telemetry that a separately hosted dashboard can ingest. Deployment scale and measured business impact have not been provided.

## Features

- **ATmega2560 firmware** with periodic sensing, display updates, and communication handling.
- **P10 display:** two 32×16 panels configured as a 2×1 layout (`SoftDMD dmd(2, 1)`). Alternates time/date and temperature/humidity views every **4 seconds**.
- **DHT22 sensor:** temperature/humidity sampling nominally every **2 seconds**; last valid readings are preserved when a sensor read fails.
- **DS3231 RTC:** timestamp generation and one-second time-screen refresh.
- **Cellular modem:** UART (`Serial2`, 9600 baud), registration and PDP activation, SMS send/receive, IPv6 HTTP session configuration.
- **Telemetry:** JSON payload sent by HTTP POST nominally every **60 seconds**, when the modem/network are ready.
- **Remote actuation:** six SMS commands for fan/heater relays, status, and buzzer silence.
- **Alarms:** local buzzer and queued SMS notifications when temperature reaches **40°C** or humidity reaches **80% RH**.
- **State-machine design:** modem init, network bring-up, SMS TX, and HTTP TX are represented by explicit state machines.

## System overview

```text
 DHT22 ────────┐
 DS3231 RTC ───┤
               v
        +---------------+           +--------------------------+
        |  ATmega2560   | --UART--> | Neo I58 cellular modem  |
        |  monitoring   |           +------------+-------------+
        |  + control    |                        |
        +---+---+---+---+           +------------+-------------+
            |   |   |               | Cellular IP/IPv6 & SMS   |
           P10  |  Relay + buzzer   +------+------------+------+
          LED   |                         |            |
          x2    +-------------------------+            |
                               HTTP POST JSON     SMS commands/alerts
                                     |                 |
                                  /sensor         Authorized phone
                                     |
                        Backend / dashboard (external)
```

## Firmware organization

```text
firmware/IndustrialMonitor/
  IndustrialMonitor.ino  # Main application source
  config.example.h       # Sanitized configuration template
  config.h               # Local copy; deliberately ignored by Git

docs/
  ARCHITECTURE.md
  LIMITATIONS.md
  RESUME_DESCRIPTION.md
```

## Getting started

1. Use an **ATmega2560-based target** with the necessary P10 display wiring, UART cellular modem, RTC, sensor, and output drivers.
2. Install the Arduino-compatible libraries used by the sketch: `RTClib` (DS3231), `DHT sensor library`, `DMD2` (including `SystemFont5x7`), and their dependencies; `Wire` is provided by the toolchain.
3. Copy `config.example.h` to `config.h` in `firmware/IndustrialMonitor/` and fill in the authorized SMS number, APN, device ID, and server details. **Do not commit `config.h`.**
4. Open `firmware/IndustrialMonitor/IndustrialMonitor.ino` with a compatible toolchain, select an ATmega2560 target, and configure the physical pinout for your hardware before flashing.
5. Set the DS3231 RTC clock appropriately; the sample now emits timestamps without a falsely asserted UTC `Z` suffix.
6. Validate cellular AT command compatibility against the actual Neo I58 firmware documentation. HTTP commands and PDP support vary by modem firmware.

**Pin constants in current firmware (verify before applying power):** DHT22 data → D4; fan relay → D23; heater relay → D22; buzzer → D24; modem on `Serial2`. The display and I2C wiring depend on the chosen interface/adapter and board design.

## Example telemetry

```json
{
  "DeviceId": "MONITOR_001",
  "Temperature": 28.3,
  "Humidity": 62,
  "Timestamp": "2026-11-17T10:30:00"
}
```

`Timestamp` is generated from RTC contents and has no timezone offset in this version; the receiver must know its timezone policy.

## SMS command set

| Command | Action |
|---|---|
| `1` | Fan ON |
| `2` | Fan OFF |
| `3` | Heater ON |
| `4` | Heater OFF |
| `5` | Return status |
| `6` | Silence buzzer |

Incoming commands are compared against a configured admin number. **Caller ID alone is not strong authentication**; use additional authentication and a secure communications architecture before deploying safety-critical remote actuation.

## Design notes and limitations

See [Architecture](docs/ARCHITECTURE.md) and [Known limitations](docs/LIMITATIONS.md). The current implementation is a working-code snapshot, **not a security-audited or verified production release**. Claims of factory-wide deployment, dashboard features, and measured efficiency improvements require separate evidence.

## Status

Prototype snapshot. The second half of the main sketch (SMS TX, command handling, alarms, telemetry scheduling, `setup()`/`loop()`) was reconstructed after the original file was damaged; see [Known limitations](docs/LIMITATIONS.md#minor-changes-made-for-github-packaging).

## License

[MIT](LICENSE) © 2026 Chitransh. Third-party libraries (RTClib, DHT sensor library, DMD2) keep their own licenses.
