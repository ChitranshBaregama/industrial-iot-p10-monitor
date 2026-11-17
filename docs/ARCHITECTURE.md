# Architecture and operational flow

The main application uses a `loop()`-driven state-machine architecture rather than an RTOS. `millis()` schedules sensor, LED, RTC, and telemetry tasks.

## Start-up

1. Initialize debug UART, `Serial2`, I2C, DHT22, output pins, and P10 display.
2. Use `modemInitProcess()` to configure AT, echo-off, SMS text mode, character set, and SMS indications.
3. Use `netBringupProcess()` to query SIM/modem status, configure IPv4v6 PDP, and attempt cellular attachment.
4. Each iteration calls the RX parser and then advances modem, network, SMS, and HTTP state machines.

## Sampling and display

- `updateDHT()` runs nominally every **2,000 ms**.
- `showTimeDateFull()` and `showTempHumi()` alternate nominally every **4,000 ms**.
- `updateTimeOnly()` refreshes the clock view nominally every **1,000 ms**.
- `checkAlarm()` checks 40°C or 80% RH thresholds after a **6,000 ms** boot guard; it raises/clears the buzzer and queues an SMS.

## Telemetry

At nominal 60-second intervals, `httpStartLogging()` builds JSON including DeviceId, Temperature, Humidity, and RTC Timestamp. `httpV6TxProcess()`:

1. Checks cellular attach, registration and IPv6 assignment.
2. Applies an IPv6 endpoint and port via AT commands.
3. Opens a per-POST HTTP session.
4. Sends a JSON payload using a modem data prompt and Ctrl+Z.
5. Processes the modem response, closes the HTTP session, or backs off on errors.

## SMS control

The UART parser recognizes unsolicited messages, reads the SMS body and sender, and invokes `handleSMS()` if the sender exactly matches `ADMIN_NUMBER`. Commands 1–6 control two relays, request status, or silence the buzzer. `smsTxProcess()` sends queued responses and notifications using an explicit finite-state machine.

## Data path

```text
DHT22 + DS3231  → ATmega2560 main loop → P10 2-panel display
                               |
                               +→ threshold check → buzzer / SMS alert
                               +→ JSON encoder → Neo I58 cellular → HTTP endpoint
                               +← authorized-number SMS → relays and status response
```

The remote backend and dashboard implementation were not included in the supplied source.
