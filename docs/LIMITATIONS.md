# Known limitations and next steps

This repository preserves the author's state-machine firmware snapshot, with secrets removed and small portability fixes. It has **not** been compiled or tested on target hardware as part of this publication package.

## Before production deployment

- **Authentication:** sender-number equality is not robust SMS authentication. Implement authenticated commands, replay protection, and authorization policy before controlling equipment remotely.
- **Transport security:** the existing implementation uses HTTP over a cellular link without TLS. Use HTTPS/TLS with server certificate validation and device credentials for sensitive or actionable telemetry.
- **Modem responses:** SMS and HTTP state machines currently share some `OK`, `ERROR` and `>` parser flags, which may misattribute responses when multiple commands overlap. Use a single serialized AT-command transaction manager with response ownership and line parsing.
- **Retry reliability:** a 60-second HTTPSETUP timeout currently proceeds to HTTPACTION; verify session readiness and use explicit retry/reset states. Queue unsent readings if data cannot be dropped.
- **Timestamp validity:** DS3231 RTC time does not encode timezone. Preserve explicit timezone metadata and handle `rtc.begin()`/lost-power failures.
- **Sensor validity:** initially cached temperature/humidity values are placeholders; track sensor-valid flags and prevent alerts/telemetry from claiming fake measurements before the first valid sample.
- **Electrical safety:** relays require suitable isolation, transient suppression, safe default state and independent interlocks as appropriate for the actual connected loads.
- **Alert behavior:** consider alarm hysteresis, dwell intervals and notification rate limits to avoid repeated toggling at boundaries.
- **Message handling:** storage deletion and string parsing should be revised for concurrent messages, unsolicited notifications and unexpected modem errors.
- **Scaling:** current firmware does not implement manufacturing production counts, multi-site provisioning, OTA updates, fleet identity, or dashboard frontend.
- **Testing:** add unit/integration tests, UART modem transcript tests, packet-level testing, and soak tests before production claims.

## Minor changes made for GitHub packaging

1. Extracted contact number, APN, and server address to untracked `config.h`, with `config.example.h` containing only dummy values.
2. Replaced `%f` formatting in alarm SMS `snprintf()` with `dtostrf`-based formatting already used for JSON, avoiding unsupported float formatting on typical AVR libc builds.
3. Removed unconditional UTC `Z` suffix from RTC timestamps; timezone must be defined by the deployment.

4. **Reconstructed tail section.** The original `IndustrialMonitor.ino` was damaged during file transfer after the `H6_W_RESULT` case of `httpV6TxProcess()`. The rest of the file was rewritten to match the surviving code, the README and the architecture notes: the HTTP result/close states, `smsTxProcess()`, `handleSMS()`, `checkAlarm()`, `httpStartLogging()`, `setup()` and `loop()`. It passes a host-side syntax check against stub headers but has **not** been built for AVR or run on hardware. Treat it as the part most in need of bench validation.

This is not a wholesale firmware redesign; all other limitations of the supplied code remain relevant.
