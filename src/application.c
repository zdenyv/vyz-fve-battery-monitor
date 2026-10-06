#include <application.h>
#include <twr_eeprom.h>

#define VYZ_VERSION "1.3"

#define THERMOSTAT_SET_POINT_DEFAULT 10.0f
#define THERMOSTAT_SET_POINT_MIN     2.0f
#define THERMOSTAT_SET_POINT_MAX     25.0f

#define EEPROM_ADDR_SETPOINT 0x0000
#define EEPROM_MAGIC         0xC0FFEE01UL

typedef struct __attribute__((packed)) {
    uint32_t magic;
    float    set_point;
} setpoint_blob_t;

static float thermostat_set_point = THERMOSTAT_SET_POINT_DEFAULT;
static bool heating = false;

static void thermostat_set_point_load(void)
{
    setpoint_blob_t b;
    if (twr_eeprom_read(EEPROM_ADDR_SETPOINT, &b, sizeof(b))
        && b.magic == EEPROM_MAGIC
        && b.set_point >= THERMOSTAT_SET_POINT_MIN
        && b.set_point <= THERMOSTAT_SET_POINT_MAX)
    {
        thermostat_set_point = b.set_point;
    }
}

static void thermostat_set_point_save(float v)
{
    if (v < THERMOSTAT_SET_POINT_MIN) v = THERMOSTAT_SET_POINT_MIN;
    if (v > THERMOSTAT_SET_POINT_MAX) v = THERMOSTAT_SET_POINT_MAX;

    if (fabsf(v - thermostat_set_point) < 0.01f) return;  // šetří wear

    setpoint_blob_t b = { .magic = EEPROM_MAGIC, .set_point = v };
    twr_eeprom_write(EEPROM_ADDR_SETPOINT, &b, sizeof(b));
    thermostat_set_point = v;
}

/*
 ============================================================================
  4-RELÉ DESKA  (5V, active-LOW, open-drain výstupy jako ve vyz-bazen)
 ----------------------------------------------------------------------------
  - R1 = P12 ventilátor garáže, R2..R4 = P13..P15 rezerva (ovládání přes MQTT)
  - Open-drain: pin jen stahuje IN k zemi, log. 1 drží pull-up desky na 5 V.
  - P11 (TXD2) záměrně nepoužit – drží ho UART logu.
 ============================================================================
*/

#define RELAY_BOARD_COUNT     4
#define RELAY_BOARD_ON_LEVEL  0

static const twr_gpio_channel_t relay_board_gpio[RELAY_BOARD_COUNT] = {
    TWR_GPIO_P12, TWR_GPIO_P13, TWR_GPIO_P14, TWR_GPIO_P15
};
static bool relay_board_on[RELAY_BOARD_COUNT];
static twr_scheduler_task_id_t relay_board_pulse_task_id[RELAY_BOARD_COUNT];

static void relay_board_write(int i, bool on)
{
    twr_gpio_set_output(relay_board_gpio[i], on ? RELAY_BOARD_ON_LEVEL : !RELAY_BOARD_ON_LEVEL);
    relay_board_on[i] = on;
}

// Topic relay/<1..4>/state – číslování podle svorek na desce.
static void relay_board_publish(int i)
{
    char topic[24];
    snprintf(topic, sizeof(topic), "relay/%d/state", i + 1);
    twr_radio_pub_bool(topic, &relay_board_on[i]);
}

static void relay_board_pulse_end_task(void *param)
{
    int i = (int) (intptr_t) param;
    relay_board_write(i, false);
    relay_board_publish(i);
}

static void relay_board_init(void)
{
    for (int i = 0; i < RELAY_BOARD_COUNT; i++)
    {
        // Nejdřív úroveň OFF, pak teprve výstup – jinak relé při bootu cvakne.
        twr_gpio_init(relay_board_gpio[i]);
        relay_board_write(i, false);
        twr_gpio_set_mode(relay_board_gpio[i], TWR_GPIO_MODE_OUTPUT_OD);
        relay_board_pulse_task_id[i] = twr_scheduler_register(relay_board_pulse_end_task, (void *) (intptr_t) i, TWR_TICK_INFINITY);
    }
}

/*
 ============================================================================
  VENTILÁTOR GARÁŽE  (chlazení, nezávislé na termostatu topení baterie)
 ----------------------------------------------------------------------------
  - Akční člen: relé R1 na 4-relé desce (P12)
  - Teplota:    EXTERNÍ DS18B20 v garáži (ne onboard Climate Module!)
                device 0x3200000ceb33a428, topic ext-thermometer/.../temperature
  - Logika:     CHLAZENÍ s hysterezí. ON při t >= práh, OFF při t <= práh - hyst.
  - Dálkově:    jen nastavení prahu (vent/-/set-point/set), stav se publikuje.
 ============================================================================
*/

// Cílové externí čidlo (garáž). Pojistka, kdyby přibyl další DS18B20.
#define VENT_SENSOR_ADDRESS  0x3200000ceb33a428ULL

#define VENT_RELAY_INDEX     0   // R1

#define VENT_SET_POINT_DEFAULT   30.0f   // °C – ON nad tuto teplotu
#define VENT_SET_POINT_MIN       20.0f
#define VENT_SET_POINT_MAX       45.0f
#define VENT_HYSTERESIS          3.0f    // OFF při (práh - 3 °C), tj. 27 °C default

#define EEPROM_ADDR_VENT_SETPOINT 0x0010 // mimo blok topení (0x0000)

static float vent_set_point = VENT_SET_POINT_DEFAULT;
static bool  vent_on = false;

static void vent_set_point_load(void)
{
    setpoint_blob_t b;
    if (twr_eeprom_read(EEPROM_ADDR_VENT_SETPOINT, &b, sizeof(b))
        && b.magic == EEPROM_MAGIC
        && b.set_point >= VENT_SET_POINT_MIN
        && b.set_point <= VENT_SET_POINT_MAX)
    {
        vent_set_point = b.set_point;
    }
}

static void vent_set_point_save(float v)
{
    if (v < VENT_SET_POINT_MIN) v = VENT_SET_POINT_MIN;
    if (v > VENT_SET_POINT_MAX) v = VENT_SET_POINT_MAX;

    if (fabsf(v - vent_set_point) < 0.01f) return;  // šetří EEPROM wear

    setpoint_blob_t b = { .magic = EEPROM_MAGIC, .set_point = v };
    twr_eeprom_write(EEPROM_ADDR_VENT_SETPOINT, &b, sizeof(b));
    vent_set_point = v;
}

// Fyzicky nastaví relé + publikuje stav jen při změně (pro graf spínání).
static void vent_relay_set(bool on)
{
    if (on == vent_on) return;

    relay_board_write(VENT_RELAY_INDEX, on);
    vent_on = on;
    twr_radio_pub_bool("vent/-/state", &vent_on);
    twr_log_debug("Ventilator garaz: %s (prah %.1f C)", on ? "ON" : "OFF", (double) vent_set_point);
}

// Rozhodovací logika chlazení s hysterezí. Volá se z DS18B20 handleru.
static void vent_control(float t)
{
    if (isnan(t)) return;

    if (!vent_on && t >= vent_set_point)
    {
        vent_relay_set(true);
    }
    else if (vent_on && t <= (vent_set_point - VENT_HYSTERESIS))
    {
        vent_relay_set(false);
    }
}

/*
 SENSOR MODULE CONNECTION
==========================

Sensor Module R1.1 - 5 pin connector
- , GND , VCC , - , DATA


 DS18B20 sensor pinout
=======================
VCC - red
GND - black
DATA- yellow (white)
*/

#define TEMPERATURE_PUB_NO_CHANGE_INTEVAL (1 * MINUT)
#define TEMPERATURE_PUB_VALUE_CHANGE 0.2f
#define TEMPERATURE_UPDATE_INTERVAL (10 * SEKUND)

#define HYGROMETER_UPDATE_INTERVAL (1 * MINUT)
#define LUX_UPDATE_INTERVAL        (1 * SEKUND)
#define BAROMETER_UPDATE_INTERVAL  (1 * MINUT)

#define TEMPERATURE_TAG_PUB_NO_CHANGE_INTEVAL (1 * MINUT)
#define TEMPERATURE_TAG_PUB_VALUE_CHANGE 0.2f

#define HUMIDITY_TAG_PUB_NO_CHANGE_INTEVAL (1 * MINUT)
#define HUMIDITY_TAG_PUB_VALUE_CHANGE 5.0f

#define LUX_METER_TAG_PUB_NO_CHANGE_INTEVAL (15 * MINUT)
#define LUX_METER_TAG_PUB_VALUE_CHANGE 25.0f

#define BAROMETER_TAG_PUB_NO_CHANGE_INTEVAL (15 * MINUT)
#define BAROMETER_TAG_PUB_VALUE_CHANGE 20.0f

#define TEMPERATURE_DS18B20_PUB_NO_CHANGE_INTEVAL (1 * MINUT)
#define TEMPERATURE_DS18B20_PUB_VALUE_CHANGE 0.2f

struct {
    event_param_t temperature;
    event_param_t humidity;
    event_param_t illuminance;
    event_param_t pressure;
    event_param_t temperature_ds18b20;
} params;

// LED instance
twr_led_t led;
bool led_state = false;

// Button instance
twr_button_t button;

static twr_ds18b20_t ds18b20;

// Temperature instance
twr_tag_temperature_t temperature;
event_param_t temperature_event_param = { .next_pub = 0 };

// Led strip
static uint32_t _twr_module_power_led_strip_dma_buffer[LED_STRIP_COUNT * LED_STRIP_TYPE * 2];
const twr_led_strip_buffer_t led_strip_buffer =
{
    .type = LED_STRIP_TYPE,
    .count = LED_STRIP_COUNT,
    .buffer = _twr_module_power_led_strip_dma_buffer
};

static struct
{
    enum
    {
        LED_STRIP_SHOW_COLOR = 0,
        LED_STRIP_SHOW_COMPOUND = 1,
        LED_STRIP_SHOW_EFFECT = 2,
        LED_STRIP_SHOW_THERMOMETER = 3

    } show;
    twr_led_strip_t self;
    uint32_t color;
    struct
    {
        uint8_t data[TWR_RADIO_NODE_MAX_COMPOUND_BUFFER_SIZE];
        int length;
    } compound;
    struct
    {
        float temperature;
        int8_t min;
        int8_t max;
        uint8_t white_dots;
        float set_point;
        uint32_t color;

    } thermometer;

    twr_scheduler_task_id_t update_task_id;

} led_strip = { .show = LED_STRIP_SHOW_COLOR, .color = 0 };

// Barva parkovací signalizace (0 = neaktivní). Má přednost před MQTT barvou pásku.
static uint32_t parking_color = 0;

void button_event_handler(twr_button_t *self, twr_button_event_t event, void *event_param)
{
    (void) self;
    (void) event_param;

    if (event == TWR_BUTTON_EVENT_PRESS)
    {
        twr_led_pulse(&led, 100);
    }
}

void temperature_tag_event_handler(twr_tag_temperature_t *self, twr_tag_temperature_event_t event, void *event_param)
{
    float value;
    event_param_t *param = (event_param_t *)event_param;

    if (event == TWR_TAG_TEMPERATURE_EVENT_UPDATE)
    {
        if (twr_tag_temperature_get_temperature_celsius(self, &value))
        {
            if ((fabs(value - param->value) >= TEMPERATURE_PUB_VALUE_CHANGE) || (param->next_pub < twr_scheduler_get_spin_tick()))
            {
                twr_radio_pub_temperature(param->channel, &value);

                param->value = value;
                param->next_pub = twr_scheduler_get_spin_tick() + TEMPERATURE_PUB_NO_CHANGE_INTEVAL;
            }
        }
    }
}

void twr_radio_node_on_state_get(uint64_t *id, uint8_t state_id)
{
    (void) id;

    if (state_id == TWR_RADIO_NODE_STATE_POWER_MODULE_RELAY)
    {
        bool state = twr_module_power_relay_get_state();

        twr_radio_pub_state(TWR_RADIO_PUB_STATE_POWER_MODULE_RELAY, &state);
    }
    else if (state_id == TWR_RADIO_NODE_STATE_LED)
    {
        twr_radio_pub_state(TWR_RADIO_PUB_STATE_LED, &led_state);
    }
}

void twr_radio_node_on_state_set(uint64_t *id, uint8_t state_id, bool *state)
{
    (void) id;

    if (state_id == TWR_RADIO_NODE_STATE_POWER_MODULE_RELAY)
    {
        twr_module_power_relay_set_state(*state);

        twr_radio_pub_state(TWR_RADIO_PUB_STATE_POWER_MODULE_RELAY, state);
    }
    else if (state_id == TWR_RADIO_NODE_STATE_LED)
    {
        led_state = *state;

        twr_led_set_mode(&led, led_state ? TWR_LED_MODE_ON : TWR_LED_MODE_OFF);

        twr_radio_pub_state(TWR_RADIO_PUB_STATE_LED, &led_state);
    }
}

void led_strip_update_task(void *param)
{
    (void) param;

    if (!twr_led_strip_is_ready(&led_strip.self))
    {
        twr_scheduler_plan_current_now();

        return;
    }

    twr_led_strip_write(&led_strip.self);

    twr_scheduler_plan_current_relative(250);
}

void led_strip_fill(void)
{
    if (parking_color != 0)
    {
        twr_led_strip_fill(&led_strip.self, parking_color);
    }
    else if (led_strip.show == LED_STRIP_SHOW_COLOR)
    {
        twr_led_strip_fill(&led_strip.self, led_strip.color);
    }
    else if (led_strip.show == LED_STRIP_SHOW_COMPOUND)
    {
        int from = 0;
        int to;
        uint8_t *color;

        for (int i = 0; i < led_strip.compound.length; i += 5)
        {
            color = led_strip.compound.data + i + 1;
            to = from + led_strip.compound.data[i];

            for (;(from < to) && (from < LED_STRIP_COUNT); from++)
            {
                twr_led_strip_set_pixel_rgbw(&led_strip.self, from, color[3], color[2], color[1], color[0]);
            }

            from = to;
        }
    }
    else if (led_strip.show == LED_STRIP_SHOW_THERMOMETER)
    {
        twr_led_strip_thermometer(&led_strip.self, led_strip.thermometer.temperature, led_strip.thermometer.min, led_strip.thermometer.max, led_strip.thermometer.white_dots, led_strip.thermometer.set_point, led_strip.thermometer.color);
    }
}

void twr_radio_node_on_led_strip_color_set(uint64_t *id, uint32_t *color)
{
    (void) id;

    twr_led_strip_effect_stop(&led_strip.self);

    led_strip.color = *color;

    led_strip.show = LED_STRIP_SHOW_COLOR;

    led_strip_fill();

    twr_scheduler_plan_now(led_strip.update_task_id);
}

void twr_radio_node_on_led_strip_brightness_set(uint64_t *id, uint8_t *brightness)
{
    (void) id;

    twr_led_strip_set_brightness(&led_strip.self, *brightness);

    led_strip_fill();

    twr_scheduler_plan_now(led_strip.update_task_id);
}

void twr_radio_node_on_led_strip_compound_set(uint64_t *id, uint8_t *compound, size_t length)
{
    (void) id;

    twr_led_strip_effect_stop(&led_strip.self);

    memcpy(led_strip.compound.data, compound, length);

    led_strip.compound.length = length;

    led_strip.show = LED_STRIP_SHOW_COMPOUND;

    led_strip_fill();

    twr_scheduler_plan_now(led_strip.update_task_id);
}

void twr_radio_node_on_led_strip_effect_set(uint64_t *id, twr_radio_node_led_strip_effect_t type, uint16_t wait, uint32_t *color)
{
    (void) id;

    switch (type) {
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_TEST:
        {
            twr_led_strip_effect_test(&led_strip.self);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_RAINBOW:
        {
            twr_led_strip_effect_rainbow(&led_strip.self, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_RAINBOW_CYCLE:
        {
            twr_led_strip_effect_rainbow_cycle(&led_strip.self, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_THEATER_CHASE_RAINBOW:
        {
            twr_led_strip_effect_theater_chase_rainbow(&led_strip.self, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_COLOR_WIPE:
        {
            twr_led_strip_effect_color_wipe(&led_strip.self, *color, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_THEATER_CHASE:
        {
            twr_led_strip_effect_theater_chase(&led_strip.self, *color, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_STROBOSCOPE:
        {
            twr_led_strip_effect_stroboscope(&led_strip.self, *color, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_ICICLE:
        {
            twr_led_strip_effect_icicle(&led_strip.self, *color, wait);
            break;
        }
        case TWR_RADIO_NODE_LED_STRIP_EFFECT_PULSE_COLOR:
        {
            twr_led_strip_effect_pulse_color(&led_strip.self, *color, wait);
            break;
        }
        default:
            return;
    }

    led_strip.show = LED_STRIP_SHOW_EFFECT;
}

void twr_radio_node_on_led_strip_thermometer_set(uint64_t *id, float *temperature, int8_t *min, int8_t *max, uint8_t *white_dots, float *set_point, uint32_t *set_point_color)
{
    (void) id;

    twr_led_strip_effect_stop(&led_strip.self);

    led_strip.thermometer.temperature = *temperature;
    led_strip.thermometer.min = *min;
    led_strip.thermometer.max = *max;
    led_strip.thermometer.white_dots = *white_dots;

    if (set_point != NULL)
    {
        led_strip.thermometer.set_point = *set_point;
        led_strip.thermometer.color = *set_point_color;
    }
    else
    {
        led_strip.thermometer.set_point = *min - 1;
    }

    led_strip.show = LED_STRIP_SHOW_THERMOMETER;

    led_strip_fill();

    twr_scheduler_plan_now(led_strip.update_task_id);
}

/*
 ============================================================================
  GARÁŽOVÁ VRATA – vstupy a parkovací signalizace na LED pásku
 ----------------------------------------------------------------------------
  - P4 (Sensor A): žárovka pohonu 32,5 V přes optočlen PC814 (OC k GND).
                   Žárovka svítí = pin v 0. Funguje pro AC (pulzy 100 Hz) i DC.
  - P7 (Sensor C): jazýčkový kontakt vrat k GND, sepnutý = vrata zavřená.
                   Nezapojený kontakt (pull-up) = "otevřeno".
  - P6:            fotobuňka v rovině vrat, kontakt NC/COM k GND,
                   sepnutý = paprsek přerušený (nebo fotobuňka bez napájení).
  - Pásek:  přerušení paprsku delší než PARKING_CAR_MIN_TIME = auto ve vratech
            → ČERVENÁ; paprsek se pak uvolní → ZELENÁ (lze zavřít) na
            PARKING_CLEAR_TIME; zavření vrat → zhasnout.
 ============================================================================
*/

#define LAMP_GPIO               TWR_GPIO_P4
#define LAMP_SAMPLE_INTERVAL    10                // ms – zachytí i AC pulzy
#define LAMP_OFF_DELAY          (1500)            // ms bez signálu = zhasnuto (přemostí i blikání)

#define DOOR_GPIO               TWR_GPIO_P7
#define DOOR_DEBOUNCE_TIME      200

#define BEAM_GPIO               TWR_GPIO_P6
#define BEAM_DEBOUNCE_TIME      50

#define PARKING_CAR_MIN_TIME    (1 * SEKUND)      // kratší přerušení (člověk, pes) se ignoruje
#define PARKING_CLEAR_TIME      (30 * SEKUND)

// Barvy 0xRRGGBBWW, ~25 % jasu kvůli odběru celého pásku
#define PARKING_COLOR_CAR       0x40000000
#define PARKING_COLOR_CLEAR     0x00400000

static twr_button_t door_input;
static twr_button_t beam_input;
static bool door_open = false;  // = výchozí stav twr_button; otevřená vrata hlásí PRESS hned po startu
static bool lamp_on = false;
static twr_tick_t lamp_last_active = 0;

static enum
{
    PARKING_IDLE = 0,
    PARKING_CAR = 1,
    PARKING_CLEAR = 2

} parking_state = PARKING_IDLE;

static twr_scheduler_task_id_t parking_timeout_task_id;

static const char *parking_state_name(void)
{
    switch (parking_state)
    {
        case PARKING_CAR:   return "car";
        case PARKING_CLEAR: return "clear";
        case PARKING_IDLE:
        default:            return "idle";
    }
}

static void parking_set_state(int state)
{
    if ((int) parking_state == state) return;

    parking_state = state;

    if (state == PARKING_CAR)
    {
        parking_color = PARKING_COLOR_CAR;
    }
    else if (state == PARKING_CLEAR)
    {
        parking_color = PARKING_COLOR_CLEAR;
    }
    else
    {
        parking_color = 0;
    }

    if (parking_color != 0)
    {
        // Efekt by parkovací barvu přepisoval vlastním taskem
        twr_led_strip_effect_stop(&led_strip.self);

        if (led_strip.show == LED_STRIP_SHOW_EFFECT)
        {
            led_strip.show = LED_STRIP_SHOW_COLOR;
            led_strip.color = 0;
        }
    }

    twr_scheduler_plan_absolute(parking_timeout_task_id, state == PARKING_CLEAR ? twr_tick_get() + PARKING_CLEAR_TIME : TWR_TICK_INFINITY);

    led_strip_fill();
    twr_scheduler_plan_now(led_strip.update_task_id);

    twr_radio_pub_string("parking/-/state", parking_state_name());
    twr_log_debug("Parkovani: %s", parking_state_name());
}

static void parking_timeout_task(void *param)
{
    (void) param;
    parking_set_state(PARKING_IDLE);
}

static void door_input_event_handler(twr_button_t *self, twr_button_event_t event, void *event_param)
{
    (void) self; (void) event_param;

    if (event == TWR_BUTTON_EVENT_PRESS || event == TWR_BUTTON_EVENT_RELEASE)
    {
        door_open = event == TWR_BUTTON_EVENT_PRESS;
        twr_radio_pub_bool("door/-/open", &door_open);

        if (!door_open)
        {
            parking_set_state(PARKING_IDLE);
        }
    }
}

static void beam_input_event_handler(twr_button_t *self, twr_button_event_t event, void *event_param)
{
    (void) self; (void) event_param;

    if (event == TWR_BUTTON_EVENT_PRESS || event == TWR_BUTTON_EVENT_RELEASE)
    {
        bool blocked = event == TWR_BUTTON_EVENT_PRESS;
        twr_radio_pub_bool("door-beam/-/blocked", &blocked);

        if (!blocked && parking_state == PARKING_CAR)
        {
            parking_set_state(PARKING_CLEAR);
        }
    }
    else if (event == TWR_BUTTON_EVENT_HOLD)
    {
        // Paprsek přerušený déle než PARKING_CAR_MIN_TIME
        if (door_open)
        {
            parking_set_state(PARKING_CAR);
        }
    }
}

// Žárovka: při AC je výstup optočlenu pulzní, proto "svítí" = byl aktivní
// v posledních LAMP_OFF_DELAY ms, ne okamžitá úroveň pinu.
static void lamp_task(void *param)
{
    (void) param;

    twr_tick_t now = twr_tick_get();

    if (twr_gpio_get_input(LAMP_GPIO) == 0)
    {
        lamp_last_active = now;
    }

    bool on = lamp_last_active != 0 && (now - lamp_last_active) < LAMP_OFF_DELAY;

    if (on != lamp_on)
    {
        lamp_on = on;
        twr_radio_pub_bool("door-light/-/state", &lamp_on);
    }

    twr_scheduler_plan_current_relative(LAMP_SAMPLE_INTERVAL);
}

// Volat AŽ po twr_ds18b20_init_* – twr_module_sensor_init reinicializuje P4/P5/P7.
static void garage_inputs_init(void)
{
    twr_gpio_init(LAMP_GPIO);
    twr_gpio_set_mode(LAMP_GPIO, TWR_GPIO_MODE_INPUT);
    twr_gpio_set_pull(LAMP_GPIO, TWR_GPIO_PULL_UP);
    twr_scheduler_register(lamp_task, NULL, 0);

    // "stisk" = vrata otevřená (pin 1 přes pull-up, kontakt rozepnutý)
    twr_button_init(&door_input, DOOR_GPIO, TWR_GPIO_PULL_UP, 0);
    twr_button_set_debounce_time(&door_input, DOOR_DEBOUNCE_TIME);
    twr_button_set_event_handler(&door_input, door_input_event_handler, NULL);

    // "stisk" = paprsek přerušený (kontakt NC sepnutý k GND)
    twr_button_init(&beam_input, BEAM_GPIO, TWR_GPIO_PULL_UP, 1);
    twr_button_set_debounce_time(&beam_input, BEAM_DEBOUNCE_TIME);
    twr_button_set_hold_time(&beam_input, PARKING_CAR_MIN_TIME);
    twr_button_set_event_handler(&beam_input, beam_input_event_handler, NULL);

    parking_timeout_task_id = twr_scheduler_register(parking_timeout_task, NULL, TWR_TICK_INFINITY);
}

static void thermostat_publish_set_point(void)
{
    twr_radio_pub_float("thermostat/-/set-point", &thermostat_set_point);
}

static void on_set_point_set(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) param;
    float v = *(float *) value;
    thermostat_set_point_save(v);
    thermostat_publish_set_point();
}

static void on_set_point_get(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) value; (void) param;
    thermostat_publish_set_point();
}

// --- Ventilátor garáže: dálkové nastavení prahu + dotaz na stav ---
static void vent_publish_set_point(void)
{
    twr_radio_pub_float("vent/-/set-point", &vent_set_point);
}

static void on_vent_set_point_set(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) param;
    float v = *(float *) value;
    vent_set_point_save(v);
    vent_publish_set_point();
}

static void on_vent_set_point_get(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) value; (void) param;
    vent_publish_set_point();
}

static void on_vent_state_get(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) value; (void) param;
    twr_radio_pub_bool("vent/-/state", &vent_on);
}

// --- Rezervní relé R2..R4: param = index relé (0-based) ---
static void on_relay_set(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic;
    int i = (int) (intptr_t) param;
    twr_scheduler_plan_absolute(relay_board_pulse_task_id[i], TWR_TICK_INFINITY);  // zruší běžící pulz
    relay_board_write(i, *(bool *) value);
    relay_board_publish(i);
}

static void on_relay_pulse(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic;
    int i = (int) (intptr_t) param;
    int duration = *(int *) value;
    if (duration <= 0) return;
    relay_board_write(i, true);
    relay_board_publish(i);
    twr_scheduler_plan_from_now(relay_board_pulse_task_id[i], duration);
}

static void on_relay_get(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) value;
    relay_board_publish((int) (intptr_t) param);
}

static void on_parking_get(uint64_t *id, const char *topic, void *value, void *param)
{
    (void) id; (void) topic; (void) value; (void) param;
    twr_radio_pub_string("parking/-/state", parking_state_name());
    twr_radio_pub_bool("door/-/open", &door_open);
    twr_radio_pub_bool("door-light/-/state", &lamp_on);
}

static twr_radio_sub_t subs[] = {
    { "relay/2/state/set", TWR_RADIO_SUB_PT_BOOL, on_relay_set,   (void *) 1 },
    { "relay/3/state/set", TWR_RADIO_SUB_PT_BOOL, on_relay_set,   (void *) 2 },
    { "relay/4/state/set", TWR_RADIO_SUB_PT_BOOL, on_relay_set,   (void *) 3 },
    { "relay/2/pulse/set", TWR_RADIO_SUB_PT_INT,  on_relay_pulse, (void *) 1 },
    { "relay/3/pulse/set", TWR_RADIO_SUB_PT_INT,  on_relay_pulse, (void *) 2 },
    { "relay/4/pulse/set", TWR_RADIO_SUB_PT_INT,  on_relay_pulse, (void *) 3 },
    { "relay/2/state/get", TWR_RADIO_SUB_PT_NULL, on_relay_get,   (void *) 1 },
    { "relay/3/state/get", TWR_RADIO_SUB_PT_NULL, on_relay_get,   (void *) 2 },
    { "relay/4/state/get", TWR_RADIO_SUB_PT_NULL, on_relay_get,   (void *) 3 },
    { "parking/-/state/get", TWR_RADIO_SUB_PT_NULL, on_parking_get, NULL },
    { "thermostat/-/set-point/set", TWR_RADIO_SUB_PT_FLOAT, on_set_point_set, NULL },
    { "thermostat/-/set-point/get", TWR_RADIO_SUB_PT_NULL,  on_set_point_get, NULL },
    { "vent/-/set-point/set",       TWR_RADIO_SUB_PT_FLOAT, on_vent_set_point_set, NULL },
    { "vent/-/set-point/get",       TWR_RADIO_SUB_PT_NULL,  on_vent_set_point_get, NULL },
    { "vent/-/state/get",           TWR_RADIO_SUB_PT_NULL,  on_vent_state_get, NULL },
};

void climate_module_event_handler(twr_module_climate_event_t event, void *event_param)
{
    (void) event_param;

    float value;

    if (event == TWR_MODULE_CLIMATE_EVENT_UPDATE_THERMOMETER)
    {
        if (twr_module_climate_get_temperature_celsius(&value))
        {
            if ((fabs(value - params.temperature.value) >= TEMPERATURE_TAG_PUB_VALUE_CHANGE) || (params.temperature.next_pub < twr_scheduler_get_spin_tick()))
            {
                twr_radio_pub_temperature(TWR_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_DEFAULT, &value);
                params.temperature.value = value;
                params.temperature.next_pub = twr_scheduler_get_spin_tick() + TEMPERATURE_TAG_PUB_NO_CHANGE_INTEVAL;
            }
            bool new_heating = value < thermostat_set_point;
            if (new_heating != heating) 
            {
                twr_module_power_relay_set_state(new_heating);
                twr_radio_pub_state(TWR_RADIO_PUB_STATE_POWER_MODULE_RELAY, &new_heating);
                heating = new_heating;
            }
        }
    }
    else if (event == TWR_MODULE_CLIMATE_EVENT_UPDATE_HYGROMETER)
    {
        if (twr_module_climate_get_humidity_percentage(&value))
        {
            if ((fabs(value - params.humidity.value) >= HUMIDITY_TAG_PUB_VALUE_CHANGE) || (params.humidity.next_pub < twr_scheduler_get_spin_tick()))
            {
                twr_radio_pub_humidity(TWR_RADIO_PUB_CHANNEL_R3_I2C0_ADDRESS_DEFAULT, &value);
                params.humidity.value = value;
                params.humidity.next_pub = twr_scheduler_get_spin_tick() + HUMIDITY_TAG_PUB_NO_CHANGE_INTEVAL;
            }
        }
    }
    else if (event == TWR_MODULE_CLIMATE_EVENT_UPDATE_LUX_METER)
    {
        if (twr_module_climate_get_illuminance_lux(&value))
        {
            if (value < 1)
            {
                value = 0;
            }
            if ((fabs(value - params.illuminance.value) >= LUX_METER_TAG_PUB_VALUE_CHANGE) || (params.illuminance.next_pub < twr_scheduler_get_spin_tick()) ||
                    ((value == 0) && (params.illuminance.value != 0)) || ((value > 1) && (params.illuminance.value == 0)))
            {
                twr_radio_pub_luminosity(TWR_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_DEFAULT, &value);
                params.illuminance.value = value;
                params.illuminance.next_pub = twr_scheduler_get_spin_tick() + LUX_METER_TAG_PUB_NO_CHANGE_INTEVAL;
            }
        }
    }
    else if (event == TWR_MODULE_CLIMATE_EVENT_UPDATE_BAROMETER)
    {
        if (twr_module_climate_get_pressure_pascal(&value))
        {
            if ((fabs(value - params.pressure.value) >= BAROMETER_TAG_PUB_VALUE_CHANGE) || (params.pressure.next_pub < twr_scheduler_get_spin_tick()))
            {
                float meter;

                if (!twr_module_climate_get_altitude_meter(&meter))
                {
                    return;
                }

                twr_radio_pub_barometer(TWR_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_DEFAULT, &value, &meter);
                params.pressure.value = value;
                params.pressure.next_pub = twr_scheduler_get_spin_tick() + BAROMETER_TAG_PUB_NO_CHANGE_INTEVAL;
            }
        }
    }
}

void ds18b20_event_handler(twr_ds18b20_t *self, uint64_t device_address, twr_ds18b20_event_t e, void *p)
{
    (void) p;

    if (e == TWR_DS18B20_EVENT_UPDATE)
    {
        float value = NAN;

        twr_ds18b20_get_temperature_celsius(self, device_address, &value);

        //twr_log_debug("UPDATE %" PRIx64 "(%d) = %f", device_address, device_index, value);

        if ((fabs(value - params.temperature_ds18b20.value) >= TEMPERATURE_DS18B20_PUB_VALUE_CHANGE) || (params.temperature_ds18b20.next_pub < twr_scheduler_get_spin_tick()))
        {
            static char topic[64];
            snprintf(topic, sizeof(topic), "ext-thermometer/%" PRIx64 "/temperature", device_address);
            twr_radio_pub_float(topic, &value);
            params.temperature_ds18b20.value = value;
            params.temperature_ds18b20.next_pub = twr_scheduler_get_spin_tick() + TEMPERATURE_DS18B20_PUB_NO_CHANGE_INTEVAL;
        }

        // Lokální řízení ventilátoru garáže – jen z externího čidla v garáži.
        // Vyhodnocuje se z každého měření (interval ~10 s), ne jen při publishi.
        if (device_address == VENT_SENSOR_ADDRESS)
        {
            vent_control(value);
        }
    }

    twr_scheduler_plan_now(0);
}

void application_init(void)
{
    // Relé deska co nejdřív – do té doby drží OFF jen pull-up desky.
    relay_board_init();

    twr_log_init(TWR_LOG_LEVEL_DUMP, TWR_LOG_TIMESTAMP_ABS);

    // Initialize LED
    twr_led_init(&led, TWR_GPIO_LED, false, false);
    twr_led_set_mode(&led, TWR_LED_MODE_OFF);

    twr_radio_init(TWR_RADIO_MODE_NODE_LISTENING);
    twr_radio_set_subs(subs, sizeof(subs) / sizeof(subs[0]));

    thermostat_set_point_load();
    vent_set_point_load();

    // Initialize button
    twr_button_init(&button, TWR_GPIO_BUTTON, TWR_GPIO_PULL_DOWN, false);
    twr_button_set_scan_interval(&button, 20);
    twr_button_set_event_handler(&button, button_event_handler, NULL);

    // Initialize temperature
    temperature_event_param.channel = TWR_RADIO_PUB_CHANNEL_R1_I2C0_ADDRESS_ALTERNATE;
    twr_tag_temperature_init(&temperature, TWR_I2C_I2C0, TWR_TAG_TEMPERATURE_I2C_ADDRESS_ALTERNATE);
    twr_tag_temperature_set_update_interval(&temperature, TEMPERATURE_UPDATE_INTERVAL);
    twr_tag_temperature_set_event_handler(&temperature, temperature_tag_event_handler, &temperature_event_param);

    // Initialize climate module
    twr_module_climate_init();
    twr_module_climate_set_event_handler(climate_module_event_handler, NULL);
    twr_module_climate_set_update_interval_thermometer(TEMPERATURE_UPDATE_INTERVAL);
    twr_module_climate_set_update_interval_hygrometer(HYGROMETER_UPDATE_INTERVAL);
    twr_module_climate_set_update_interval_lux_meter(LUX_UPDATE_INTERVAL);
    twr_module_climate_set_update_interval_barometer(BAROMETER_UPDATE_INTERVAL);
    twr_module_climate_measure_all_sensors();

    // For multiple sensor you can call twr_ds18b20_init() more in sdk/_excamples/ds18b20_multiple
    twr_ds18b20_init_single(&ds18b20, TWR_DS18B20_RESOLUTION_BITS_12);
    twr_ds18b20_set_event_handler(&ds18b20, ds18b20_event_handler, NULL);
    twr_ds18b20_set_update_interval(&ds18b20, TEMPERATURE_UPDATE_INTERVAL);

    // Vstupy garáže (žárovka P4, vrata P7, fotobuňka P6).
    // POZOR: musí být AŽ po twr_ds18b20_init_single (uvnitř twr_module_sensor_init,
    // který reinicializuje kanály A/B/C). Jinak by se P4/P7 přenastavily.
    garage_inputs_init();

    // Initialize power module
    twr_module_power_init();

    // Initialize led-strip on power module
    twr_led_strip_init(&led_strip.self, twr_module_power_get_led_strip_driver(), &led_strip_buffer);

    led_strip.update_task_id = twr_scheduler_register(led_strip_update_task, NULL, 0);

    twr_radio_pairing_request("vyz-fve-battery-monitor", VYZ_VERSION);

    thermostat_publish_set_point();
    vent_publish_set_point();

    twr_led_pulse(&led, 2000);
    twr_log_debug("Konec init: topeni set-point = %.1f C, vent set-point = %.1f C",
                  (double) thermostat_set_point, (double) vent_set_point);
}
