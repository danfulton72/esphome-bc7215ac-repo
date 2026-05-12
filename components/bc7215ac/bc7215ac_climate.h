#pragma once

#include "esphome/core/component.h"
#include "esphome/core/log.h"
#include "esphome/components/uart/uart.h"
#include "esphome/components/climate/climate.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/binary_sensor/binary_sensor.h"

namespace esphome {
namespace bc7215ac {

static const char *const TAG = "bc7215ac";

// ---------------------------------------------------------------------------
// Protocol constants
// Frame sent from MCU to BC7215A:
//   Byte 0 : 0xA5  (start)
//   Byte 1 : command  (see BC_CMD_* below)
//   Byte 2 : temperature (16-30)
//   Byte 3 : mode  (0=Auto 1=Cool 2=Heat 3=Dry 4=Fan)
//   Byte 4 : fan   (0=Auto 1=Low  2=Med  3=High)
//   Byte 5 : XOR checksum of bytes 0-4
//
// Async report frame from BC7215A when it decodes an incoming IR signal:
//   Byte 0 : 0xB5  (start)
//   Byte 1 : payload length N
//   Byte 2..N+1 : payload  (power, mode, temp, fan at offsets 0-3)
//   Byte N+2 : XOR checksum of bytes 0..N+1
// ---------------------------------------------------------------------------

static const uint8_t BC_START         = 0xA5;
static const uint8_t BC_CMD_SET       = 0x01;
static const uint8_t BC_CMD_POWER_ON  = 0x02;
static const uint8_t BC_CMD_POWER_OFF = 0x03;
static const uint8_t BC_CMD_PAIR      = 0x04;
static const uint8_t BC_REP_START     = 0xB5;

static const uint8_t  BC_MIN_TEMP        = 16;
static const uint8_t  BC_MAX_TEMP        = 30;
static const uint32_t BC_BUSY_TIMEOUT_MS = 2000;

// ---------------------------------------------------------------------------
// Mode conversion helpers
// ---------------------------------------------------------------------------

static uint8_t climate_mode_to_bc(climate::ClimateMode m) {
  switch (m) {
    case climate::CLIMATE_MODE_AUTO:     return 0;
    case climate::CLIMATE_MODE_COOL:     return 1;
    case climate::CLIMATE_MODE_HEAT:     return 2;
    case climate::CLIMATE_MODE_DRY:      return 3;
    case climate::CLIMATE_MODE_FAN_ONLY: return 4;
    default:                             return 1;
  }
}

static climate::ClimateMode bc_to_climate_mode(uint8_t bc) {
  switch (bc) {
    case 0:  return climate::CLIMATE_MODE_AUTO;
    case 1:  return climate::CLIMATE_MODE_COOL;
    case 2:  return climate::CLIMATE_MODE_HEAT;
    case 3:  return climate::CLIMATE_MODE_DRY;
    case 4:  return climate::CLIMATE_MODE_FAN_ONLY;
    default: return climate::CLIMATE_MODE_COOL;
  }
}

static uint8_t fan_mode_to_bc(optional<climate::ClimateFanMode> f) {
  if (!f.has_value()) return 0;
  switch (f.value()) {
    case climate::CLIMATE_FAN_LOW:    return 1;
    case climate::CLIMATE_FAN_MEDIUM: return 2;
    case climate::CLIMATE_FAN_HIGH:   return 3;
    default:                          return 0;
  }
}

static optional<climate::ClimateFanMode> bc_to_fan_mode(uint8_t bc) {
  switch (bc) {
    case 1:  return climate::CLIMATE_FAN_LOW;
    case 2:  return climate::CLIMATE_FAN_MEDIUM;
    case 3:  return climate::CLIMATE_FAN_HIGH;
    default: return climate::CLIMATE_FAN_AUTO;
  }
}

// ---------------------------------------------------------------------------
// BC7215ACClimate component
// ---------------------------------------------------------------------------

class BC7215ACClimate : public climate::Climate, public Component {
 public:
  // Setters wired up by generated code from climate.py
  void set_uart(uart::UARTComponent *uart)           { uart_     = uart; }
  void set_mod_pin(switch_::Switch *pin)             { mod_pin_  = pin; }
  void set_busy_pin(binary_sensor::BinarySensor *b)  { busy_pin_ = b; }

  // -------------------------------------------------------------------------
  // ESPHome lifecycle
  // -------------------------------------------------------------------------

  void setup() override {
    ESP_LOGI(TAG, "BC7215A climate initialising");
    if (mod_pin_)
      mod_pin_->turn_off();   // LOW = normal IR-transmit mode

    this->mode               = climate::CLIMATE_MODE_COOL;
    this->target_temperature = 24.0f;
    this->fan_mode           = climate::CLIMATE_FAN_AUTO;
  }

  void loop() override {
    while (uart_ && uart_->available()) {
      uint8_t b;
      uart_->read_byte(&b);
      rx_buf_.push_back(b);
      if (rx_buf_.size() >= 7)
        try_parse_report_();
    }
  }

  float get_setup_priority() const override { return setup_priority::DATA; }

  // -------------------------------------------------------------------------
  // Climate traits
  // -------------------------------------------------------------------------

  climate::ClimateTraits traits() override {
    auto t = climate::ClimateTraits();
    t.set_supported_modes({
      climate::CLIMATE_MODE_OFF,
      climate::CLIMATE_MODE_COOL,
      climate::CLIMATE_MODE_HEAT,
      climate::CLIMATE_MODE_DRY,
      climate::CLIMATE_MODE_FAN_ONLY,
      climate::CLIMATE_MODE_AUTO,
    });
    t.set_supported_fan_modes({
      climate::CLIMATE_FAN_AUTO,
      climate::CLIMATE_FAN_LOW,
      climate::CLIMATE_FAN_MEDIUM,
      climate::CLIMATE_FAN_HIGH,
    });
    t.set_visual_min_temperature(BC_MIN_TEMP);
    t.set_visual_max_temperature(BC_MAX_TEMP);
    t.set_visual_temperature_step(1.0f);
    return t;
  }

  // -------------------------------------------------------------------------
  // Control entry-point (called by Home Assistant or button lambdas)
  // -------------------------------------------------------------------------

  void control(const climate::ClimateCall &call) override {
    if (call.get_mode().has_value())
      this->mode = *call.get_mode();
    if (call.get_target_temperature().has_value())
      this->target_temperature = *call.get_target_temperature();
    if (call.get_fan_mode().has_value())
      this->fan_mode = *call.get_fan_mode();

    if (this->mode == climate::CLIMATE_MODE_OFF)
      send_power_(false);
    else
      send_ac_state_();

    this->publish_state();
  }

  // -------------------------------------------------------------------------
  // Public helpers (callable via HA service lambda)
  // -------------------------------------------------------------------------

  // Drive the chip into pairing/learning mode.
  // Point the original AC remote at the BC7215A sensor and press any button.
  void start_pairing() {
    ESP_LOGI(TAG, "Entering BC7215A pairing mode - point AC remote at sensor");
    if (mod_pin_)
      mod_pin_->turn_on();
    uint8_t frame[6];
    build_frame_(frame, BC_CMD_PAIR, 25, 1, 0);
    send_frame_(frame);
    pairing_ = true;
  }

  void end_pairing() {
    if (mod_pin_)
      mod_pin_->turn_off();
    pairing_ = false;
    ESP_LOGI(TAG, "Pairing ended - normal operation resumed");
  }

 protected:
  uart::UARTComponent         *uart_     {nullptr};
  switch_::Switch             *mod_pin_  {nullptr};
  binary_sensor::BinarySensor *busy_pin_ {nullptr};

  std::vector<uint8_t> rx_buf_;
  bool pairing_{false};

  // -------------------------------------------------------------------------
  // Private helpers
  // -------------------------------------------------------------------------

  void build_frame_(uint8_t *f, uint8_t cmd,
                    uint8_t temp, uint8_t mode, uint8_t fan) {
    f[0] = BC_START;
    f[1] = cmd;
    f[2] = std::max((uint8_t) BC_MIN_TEMP, std::min(temp, (uint8_t) BC_MAX_TEMP));
    f[3] = mode;
    f[4] = fan;
    f[5] = f[0] ^ f[1] ^ f[2] ^ f[3] ^ f[4];  // XOR checksum
  }

  void send_frame_(const uint8_t *f) {
    if (!uart_) return;
    // Wait for chip to finish any in-progress operation
    uint32_t t0 = millis();
    while (busy_pin_ && busy_pin_->state) {
      if (millis() - t0 > BC_BUSY_TIMEOUT_MS) {
        ESP_LOGW(TAG, "BUSY timeout - sending anyway");
        break;
      }
      delay(5);
    }
    ESP_LOGD(TAG, "TX -> %02X %02X %02X %02X %02X %02X",
             f[0], f[1], f[2], f[3], f[4], f[5]);
    uart_->write_array(f, 6);
  }

  void send_ac_state_() {
    uint8_t temp = (uint8_t) roundf(this->target_temperature);
    uint8_t mode = climate_mode_to_bc(this->mode);
    uint8_t fan  = fan_mode_to_bc(this->fan_mode);
    uint8_t f[6];
    build_frame_(f, BC_CMD_SET, temp, mode, fan);
    send_frame_(f);
    ESP_LOGI(TAG, "Sent: %dC  mode=%d  fan=%d", temp, mode, fan);
  }

  void send_power_(bool on) {
    uint8_t f[6];
    build_frame_(f,
                 on ? BC_CMD_POWER_ON : BC_CMD_POWER_OFF,
                 (uint8_t) roundf(this->target_temperature),
                 climate_mode_to_bc(this->mode),
                 fan_mode_to_bc(this->fan_mode));
    send_frame_(f);
    ESP_LOGI(TAG, "Power %s", on ? "ON" : "OFF");
  }

  // Parse an async report frame emitted by the BC7215A when it decodes
  // an IR signal from the original remote (IR pass-through / parsing mode).
  void try_parse_report_() {
    // Discard bytes before the report start marker
    auto it = std::find(rx_buf_.begin(), rx_buf_.end(), BC_REP_START);
    if (it == rx_buf_.end()) { rx_buf_.clear(); return; }
    rx_buf_.erase(rx_buf_.begin(), it);

    if (rx_buf_.size() < 3) return;
    uint8_t len   = rx_buf_[1];
    size_t  total = (size_t) len + 3u;  // start + len + payload + checksum
    if (rx_buf_.size() < total) return;

    // Validate checksum
    uint8_t cs = 0;
    for (size_t i = 0; i < total - 1; i++) cs ^= rx_buf_[i];
    if (cs != rx_buf_[total - 1]) {
      ESP_LOGW(TAG, "Bad checksum in report frame");
      rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + 1);
      return;
    }

    // Payload layout: [power, mode, temp, fan] at offsets 2..5
    if (len >= 4) {
      uint8_t power = rx_buf_[2];
      uint8_t mode  = rx_buf_[3];
      uint8_t temp  = rx_buf_[4];
      uint8_t fan   = rx_buf_[5];

      if (power == 0) {
        this->mode = climate::CLIMATE_MODE_OFF;
      } else {
        this->mode = bc_to_climate_mode(mode);
        this->target_temperature = std::max((float) BC_MIN_TEMP,
                                   std::min((float) temp, (float) BC_MAX_TEMP));
        this->fan_mode = bc_to_fan_mode(fan);
      }
      ESP_LOGI(TAG, "Decoded IR: power=%d mode=%d temp=%d fan=%d",
               power, mode, temp, fan);
      this->publish_state();
    }

    rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + total);
    if (pairing_) end_pairing();
  }
};

}  // namespace bc7215ac
}  // namespace esphome
