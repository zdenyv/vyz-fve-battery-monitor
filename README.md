<a href="https://www.hardwario.com/"><img src="https://www.hardwario.com/ci/assets/hw-logo.svg" width="200" alt="HARDWARIO Logo" align="right"></a>

# Firmware for HARDWARIO Power Controller Kit

[![build](https://github.com/hardwario/twr-radio-power-controller/actions/workflows/main.yml/badge.svg)](https://github.com/hardwario/twr-radio-power-controller/actions/workflows/main.yml)
[![Release](https://img.shields.io/github/release/bigclownlabs/bcf-radio-power-controller.svg)](https://github.com/bigclownlabs/bcf-radio-power-controller/releases)
[![License](https://img.shields.io/github/license/bigclownlabs/bcf-radio-power-controller.svg)](https://github.com/bigclownlabs/bcf-radio-power-controller/blob/master/LICENSE)
[![Twitter](https://img.shields.io/twitter/follow/hardwario_en.svg?style=social&label=Follow)](https://twitter.com/hardwario_en)

See the project documentation on this link:

**https://developers.hardwario.com/projects/radio-smart-led-strip/**

## Fork: FVE battery monitor + garážový ventilátor

Tento fork běží na HARDWARIO nodu **`fve-baterie`** a obsahuje **dva nezávislé termostaty**:

### 1. Topení baterie (onboard Climate Module)

- Čidlo: onboard **Climate Module** termometr.
- Akční člen: **Power Module relé** (topí, když je baterie pod prahem).
- Logika: ON při `t < práh`. Default práh 10 °C, rozsah 2–25 °C.
- Práh v EEPROM blok `0x0000`.
- MQTT: `thermostat/-/set-point/set` (float) + `/get`, publikuje `thermostat/-/set-point`.

### 2. Garážový ventilátor (chlazení FVE střídače)

- Čidlo: **externí DS18B20 v garáži** `0x3200000ceb33a428` (`VENT_SENSOR_ADDRESS`).
- Akční člen: 5V relé deska na **Sensor Module kanál C = GPIO P7**, deska **active-high** (`VENT_RELAY_ACTIVE_HIGH 1`).
- Logika: chlazení s hysterezí — ON při `t ≥ práh`, OFF při `t ≤ práh − 3 °C`. Default 30/27 °C, rozsah 20–45 °C.
- Práh v EEPROM blok `0x0010` (mimo topení na `0x0000`).
- MQTT: `vent/-/set-point/set` (float) + `/get`, stav na `vent/-/state` (+ `/state/get`).
- **Init P7 až po `twr_ds18b20_init_single`** — `twr_module_sensor_init` jinak P7 přepíše zpět na vstup.

| Funkce | Čidlo | Relé / GPIO | EEPROM | MQTT prefix |
|---|---|---|---|---|
| Topení baterie | Climate Module (onboard) | Power Module relé | `0x0000` | `thermostat/-/...` |
| Ventilátor garáže | ext. DS18B20 `…a428` | Sensor kanál C / **P7** | `0x0010` | `vent/-/...` |

Logování a vizualizace (TimescaleDB + InfluxDB + Grafana dashboard `garaz-ventilace`) je popsané v repu `vyz-topeni` → `DOKUMENTACE.md`, sekce *Garážový ventilátor*. Build a flash **pouze přes VS Code HARDWARIO extension**.

## License

This project is licensed under the [MIT License](https://opensource.org/licenses/MIT/) - see the [LICENSE](LICENSE) file for details.

---

Made with &#x2764;&nbsp; by [**HARDWARIO a.s.**](https://www.hardwario.com/) in the heart of Europe.
