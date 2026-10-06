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
- Akční člen: **relé R1** na 4-relé desce (P12), viz níže.
- Logika: chlazení s hysterezí — ON při `t ≥ práh`, OFF při `t ≤ práh − 3 °C`. Default 30/27 °C, rozsah 20–45 °C.
- Práh v EEPROM blok `0x0010` (mimo topení na `0x0000`).
- MQTT: `vent/-/set-point/set` (float) + `/get`, stav na `vent/-/state` (+ `/state/get`).

| Funkce | Čidlo | Relé / GPIO | EEPROM | MQTT prefix |
|---|---|---|---|---|
| Topení baterie | Climate Module (onboard) | Power Module relé (P0) | `0x0000` | `thermostat/-/...` |
| Ventilátor garáže | ext. DS18B20 `…a428` | relé deska R1 / **P12** | `0x0010` | `vent/-/...` |

### 3. 4-relé deska (5V, active-LOW)

- Výstupy **open-drain** (jako ve `vyz-bazen`): pin jen stahuje IN k zemi, log. 1 drží pull-up desky na 5 V.
- R1 = P12 ventilátor, R2–R4 = P13–P15 rezerva. P11 nepoužit — je to TXD2 UART logu.
- MQTT pro R2–R4: `relay/N/state/set` (bool), `relay/N/pulse/set` (int ms), `relay/N/state/get`; stav na `relay/N/state`.
- Napájení cívek z 5 V Power Modulu (adaptér 3 A).

### 4. Garážová vrata a parkovací signalizace

| Vstup | Pin | Zapojení |
|---|---|---|
| Žárovka pohonu 32,5 V | P4 (Sensor A) | optočlen **PC814** (AC vstup) + 6k8/0,5 W, kolektor na P4, emitor na GND |
| Kontakt vrat | P7 (Sensor C) | jazýček k GND, sepnuto = zavřeno; nezapojeno = „otevřeno" |
| Fotobuňka v rovině vrat | P6 | reléový kontakt **NC/COM** k GND, sepnuto = paprsek přerušen |

- LED pásek (Power Module, P1): paprsek přerušen ≥ 1 s při otevřených vratech → **červená**; paprsek se uvolní → **zelená** (vrata lze zavřít) na 30 s; zavření vrat → zhasne. Parkovací barva má přednost před MQTT barvou pásku.
- Žárovka se vyhodnocuje jako „svítí, pokud byl signál v posledních 1,5 s" — funguje pro AC i DC a přemostí blikání.
- MQTT: `door/-/open`, `door-light/-/state`, `door-beam/-/blocked` (bool), `parking/-/state` (`idle`/`car`/`clear`), dotaz `parking/-/state/get`.
- **Init vstupů až po `twr_ds18b20_init_single`** — `twr_module_sensor_init` jinak P4/P7 přenastaví.

Logování a vizualizace (TimescaleDB + InfluxDB + Grafana dashboard `garaz-ventilace`) je popsané v repu `vyz-topeni` → `DOKUMENTACE.md`, sekce *Garážový ventilátor*. Build a flash **pouze přes VS Code HARDWARIO extension**.

## License

This project is licensed under the [MIT License](https://opensource.org/licenses/MIT/) - see the [LICENSE](LICENSE) file for details.

---

Made with &#x2764;&nbsp; by [**HARDWARIO a.s.**](https://www.hardwario.com/) in the heart of Europe.
