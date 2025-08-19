**Overview**
- Purpose: ESP32-based controller that selects antenna relays based on radio frequency (either via CAT data or MQTT) or user input (webpage).
- Hardware: Kincony KC868-A16 uses the (PCF8574 expanders) & mosfet outputs to drive a antenna switch.
- Interfaces: CAT over UART, optional MQTT, HTTP web UI, Wi‑Fi (with SmartConfig fallback).

**System Composition**
- `main/app_main`: Entry point that orchestrates startup, Wi‑Fi readiness, SmartConfig fallback, watchdog feeding, and error handling/restart.
- `SystemInitializer`: Two-phase init.
  - Basic: NVS, event loop, Wi‑Fi manager, watchdog.
  - Full: Config, CAT parser, optional MQTT client, RelayController, network readiness logging.
- `WifiManager` (singleton): STA mode Wi‑Fi bring-up, credential storage in NVS, event handling, SmartConfig orchestration, exposes IP/MAC, starts web server on `IP_EVENT_STA_GOT_IP`.
- `Webserver`: HTTP server (ESP-IDF `esp_http_server`) serving a simple UI and JSON endpoints for status, configuration, and relay control.
- `ConfigManager` (singleton): Owns `antenna_switch_config_t`, persists to NVS as a blob, publishes updates to observers, provides defaults on first boot.
- `Antenna Switch` (C API + C++ helper): Pure business logic for band/antenna mapping and auto-mode flag. Delegates actuation to `RelayController` injected via setter.
- `RelayController` (singleton): Relay selection semantics, hot-switch avoidance hooks, cooldowns, state tracking; delegates hardware I/O to `kc868_a16_hw`.
- `kc868_a16_hw`: I2C driver for two PCF8574 output expanders (active-low), plus helpers to set/get per-output and batch states.
- `CatParser` (singleton): CAT command ingestion over UART2; parses Kenwood style FA, IF, AP, TX, RX CAT commands, tracks frequency/mode/tx-state and invokes antenna selection with transmit protection.
- `MQTTClient` (singleton, optional): Connects to broker, subscribes to radio info topic, keeps frequency/tx-state in sync with CAT (configurable concurrency).
- `TCPClient`/`UDPClient`: Socket helpers for network I/O; present for potential KC868 control/diagnostics (not central to current I2C path).
- `RestartManager`: NVS-backed restart counter and last error storage to gate SmartConfig fallback.
- `html_content`: All related html / web page stuff.

**Data Flow**
- Frequency sources:
  - UART CAT (`CatParser`): Parses `FA` (frequency), `IF` (status incl. mode and TX), `TX`/`RX` (transmit state), `AP` (antenna ports) and updates internal state.
  - MQTT (`MQTTClient`): Parses JSON with `Freq` and `IsTransmitting`; updates CAT state and invokes frequency change.
- Selection pipeline:
  - New frequency → `CatParser::handle_frequency_change()` → checks transmit state → if not transmitting: `antenna_switch_set_frequency()` → consults `ConfigManager` bands/ports → calls `RelayController::set_relay_for_antenna()` for the first enabled port in-band.
  - If transmitting: frequency tracking updated but antenna change deferred until transmission ends.
- Manual control:
  - Web UI POST `/relay/control` → `RelayController::set_relay()` (turns off others first) → `kc868_a16_hw` updates PCF8574 lines.
- Status & config:
  - Web UI GET `/status` reads `CatParser` and `RelayController` state, returns JSON with frequency, active antenna, available antennas, TX state.
  - GET `/config` returns HTML generated from `ConfigManager` values; POST `/config` validates and persists new config to NVS.

**Initialization Sequence**
- NVS init → event loop → Wi‑Fi init. If saved STA credentials exist, connect; otherwise SmartConfig is primed and will start on `WIFI_EVENT_STA_START`.
- Watchdog (task WDT) is configured and main task is subscribed.
- After Wi‑Fi is connected (or SmartConfig mode entered), full init runs:
  - `antenna_switch_init()` → `ConfigManager::init()` (defaults if missing).
  - `cat_parser_init()` sets up UART2 (defaults: TX=GPIO33, RX=GPIO32, baud=9600) and handlers using config.
  - If `mqtt_enabled`, `MQTTClient` is initialized/started and subscribes to the configured topic.
  - `RelayController::instance().init()` initializes I2C and sets all relays off; injected into antenna switch.
- `WifiManager` event handler starts the HTTP server on IP acquisition.

**Hardware Control Layer**
- Bus: I2C0 on GPIO4 (SDA), GPIO5 (SCL) at 100kHz.
- Expanders: PCF8574 at `0x24` and `0x25` for outputs; active-low lines (logical ON → drive low).
- API:
  - `kc868_a16_set_output(output, state)` toggles a single relay.
  - `kc868_a16_set_all_outputs(mask)` writes both expanders in one go; mask is logical (before inversion).
  - `kc868_a16_get_all_outputs()` returns logical states (after inversion) for convenient bitmask introspection.

**Safety and Reliability**
- TX protection: Multi-layered approach with `CatParser` and `MQTTClient` transmit state tracking. `RelayController` and `CatParser` both consult transmit status and refuse/defer relay changes while transmitting (double-checked with small delays).
- Deferred switching: When transmitting, `CatParser::handle_frequency_change()` updates frequency tracking but defers antenna changes until transmission ends, preventing hot switching.
- Cooldown: `RelayController` enforces a minimum cooldown between relay changes to avoid hot switching and reduce wear.
- Error handling: ESP_ERR_INVALID_STATE errors are handled gracefully without propagating to prevent cascade failures.
- Watchdog: Task WDT configured; main loop feeds WDT and auto-resubscribes if needed.
- Restart gating: `RestartManager` tracks restart count in NVS; on repeated failures, system enters SmartConfig wait instead of reboot loop.

**Configuration Model (`antenna_switch_config_t`)**
- Bands: Up to `MAX_BANDS` ranges with descriptions and per-band enabled antenna ports (`MAX_ANTENNA_PORTS`).
- IO: UART (baud/parity/stop/flow, TX/RX pins) for CAT; Wi‑Fi managed separately.
- MQTT: Enabled flag, broker URI/port, client/username/password, topic, rig id.
- Behavior: `auto_mode` (auto-select), `allow_concurrent_data_sources` (prefer CAT vs MQTT when both present).
- Persistence: Stored as an NVS blob under `"antenna_switch"/"config"` with sensible defaults on first boot.

**Web API Surface**
- `GET /` HTML dashboard with device info (IP/MAC) and config links.
- `GET /config` HTML form based on current config.
- `POST /config` JSON to update full config; validated and persisted, with server-side bounds checks.
- `GET /status` JSON: `{ frequency, antenna, transmitting, available_antennas }`.
- `GET /relay/status` JSON: relay states and current configuration.
- `POST /relay/control` JSON: `{ relay: <1-16>, state: true|false }`; turns off other relays when enabling one.
- `POST /toggle-auto-mode` toggles auto/manual selection.
- `POST /reset-config`, `POST /reset-wifi`, `POST /restart` utility endpoints.

**Concurrency Model**
- FreeRTOS tasks: main task loop, UART task in `CatParser` (8KB stack, tskIDLE_PRIORITY+1), HTTP server task(s) via ESP-IDF (8KB stack).
- Synchronization: `RelayController` uses std::mutex around multi-relay operations; internal state tracked in maps and cached bitmasks.
- Memory management: Webserver uses malloc/free for request content with proper cleanup on all error paths.
- Event-driven Wi‑Fi: ESP event loop drives `WifiManager` state machine and web server start.

**Build & Layout**
- Framework: ESP-IDF (CMake project).
- Modules: Each component under `components/<name>` with its own `CMakeLists.txt` and headers; app code in `main/`.
- Artifacts: `build/` contains generated outputs and configuration (`sdkconfig.h/json`).

**Extensibility Notes**
- KC868 transport: Current hardware path is I2C to PCF8574; `TCPClient/UDPClient` exist for alternate/remote control and could back a different `kc868_*` layer.
- Additional rigs: CAT parser is handler-based; new commands can be added to `command_handlers`.
- Portability: `Antenna Switch` is logic-only and relay-agnostic once a `RelayController` implementation is provided.
