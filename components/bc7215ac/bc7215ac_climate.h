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
//
// Command frame (MCU -> BC7215A):
//   Byte 0 : 0xA5  (start marker)
//   Byte 1 : command  (BC_CMD_*)
//   Byte 2 : temperature (16-30)
//   Byte 3 : mode  (0=Auto 1=Cool 2=Heat 3=Dry 4=Fan)
//   Byte 4 : fan   (0=Auto 1=Low  2=Med  3=High)
//   Byte 5 : XOR checksum of bytes 0-4
//
// The chip sends back:
//   0x06 = ACK (command accepted / pairing complete)
//   0x15 = NAK (command rejected)
//   0xB5 ... = async IR report frame (some firmware variants)
//
// Pairing: MOD pin held HIGH for the entire session.
// ---------------------------------------------------------------------------

static const uint8_t BC_START         = 0xA5;
static const uint8_t BC_CMD_SET       = 0x01;
static const uint8_t BC_CMD_POWER_ON  = 0x02;
static const uint8_t BC_CMD_POWER_OFF = 0x03;
static const uint8_t BC_CMD_PAIR      = 0x04;
static const uint8_t BC_REP_START     = 0xB5;
static const uint8_t BC_ACK_OK        = 0x06;
static const uint8_t BC_ACK_FAIL      = 0x15;

static const uint8_t  BC_MIN_TEMP           = 16;
static const uint8_t  BC_MAX_TEMP           = 30;
static const uint32_t BC_BUSY_TIMEOUT_MS    = 2000;
// After sending BC_CMD_PAIR, wait this long before opening the IR window.
// Increased to 2 s to handle slower chip variants.
static const uint32_t BC_PAIR_CMD_SETTLE_MS = 2000;
// How long the user has to press the remote button.
static const uint32_t BC_PAIR_IR_TIMEOUT_MS = 30000;
// How long the raw UART sniff diagnostic runs.
static const uint32_t BC_SNIFF_DURATION_MS  = 10000;

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
// Pairing state machine
// ---------------------------------------------------------------------------
enum class PairState {
  IDLE,
  WAIT_CMD_SETTLE,   // pair cmd sent, waiting settle period before IR window
  WAIT_IR_CAPTURE,   // chip should be in learning mode; waiting for remote
};

// ---------------------------------------------------------------------------
// BC7215ACClimate component
// ---------------------------------------------------------------------------

class BC7215ACClimate : public climate::Climate, public Component {
 public:
  void set_uart(uart::UARTComponent *uart)           { uart_     = uart; }
  void set_mod_pin(switch_::Switch *pin)             { mod_pin_  = pin; }
  void set_busy_pin(binary_sensor::BinarySensor *b)  { busy_pin_ = b; }

  // -------------------------------------------------------------------------
  // ESPHome lifecycle
  // -------------------------------------------------------------------------

  void setup() override {
    ESP_LOGI(TAG, "BC7215A climate initialising");
    if (mod_pin_)
      mod_pin_->turn_off();

    this->mode               = climate::CLIMATE_MODE_COOL;
    this->target_temperature = 24.0f;
    this->fan_mode           = climate::CLIMATE_FAN_AUTO;
  }

  void loop() override {
    handle_pair_timers_();
    handle_sniff_timer_();

    while (uart_ && uart_->available()) {
      uint8_t b;
      uart_->read_byte(&b);

      // Sniff mode: log every byte raw regardless of other state
      if (sniffing_) {
        ESP_LOGI(TAG, "SNIFF RX: 0x%02X (%d)", b, b);
        continue;
      }

      // Pairing: log every byte and route to pairing handler
      if (pair_state_ != PairState::IDLE) {
        ESP_LOGI(TAG, "Pair RX: 0x%02X  state=%d", b, (int) pair_state_);
        handle_pair_rx_(b);
        continue;
      }

      // Normal operation: accumulate for report frame parser
      rx_buf_.push_back(b);
      if (rx_buf_.size() > 64)
        rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + 32);
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
  // Control entry-point
  // -------------------------------------------------------------------------

  void control(const climate::ClimateCall &call) override {
    if (pair_state_ != PairState::IDLE || sniffing_) {
      ESP_LOGW(TAG, "Ignoring control call - diagnostic mode active");
      return;
    }
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
  // Public API
  // -------------------------------------------------------------------------

  void start_pairing() {
    if (pair_state_ != PairState::IDLE) {
      ESP_LOGW(TAG, "Pairing already in progress");
      return;
    }
    if (sniffing_) {
      ESP_LOGW(TAG, "Sniff mode active - call sniff first, then pair");
      return;
    }

    ESP_LOGI(TAG, "--- BC7215A pairing start ---");
    ESP_LOGI(TAG, "Asserting MOD pin HIGH");
    if (mod_pin_)
      mod_pin_->turn_on();

    // Hold MOD high before sending the command
    delay(200);

    rx_buf_.clear();
    pair_state_    = PairState::WAIT_CMD_SETTLE;
    pair_timer_ms_ = millis();

    uint8_t frame[6];
    build_frame_(frame, BC_CMD_PAIR, 25, 1, 0);
    ESP_LOGI(TAG, "Sending pair command");
    send_frame_(frame);
    ESP_LOGI(TAG, "Pair command sent - watching for chip response (settle: 2 s)...");
  }

  // Diagnostic: log every raw byte from the BC7215A for 10 seconds.
  // Call this service, then press any remote button.
  // If bytes appear: chip alive, RX wiring good.
  // If nothing appears: check RX wiring (GPIO33) and chip power.
  void start_sniff() {
    if (pair_state_ != PairState::IDLE) {
      ESP_LOGW(TAG, "Cannot sniff during pairing");
      return;
    }
    ESP_LOGI(TAG, "--- UART sniff started (10 s) ---");
    ESP_LOGI(TAG, "Press any button on the AC remote now.");
    ESP_LOGI(TAG, "Bytes received = chip alive. Silence = wiring problem.");
    rx_buf_.clear();
    sniffing_       = true;
    sniff_start_ms_ = millis();
  }

 protected:
  uart::UARTComponent         *uart_     {nullptr};
  switch_::Switch             *mod_pin_  {nullptr};
  binary_sensor::BinarySensor *busy_pin_ {nullptr};

  std::vector<uint8_t> rx_buf_;

  PairState pair_state_    {PairState::IDLE};
  uint32_t  pair_timer_ms_ {0};

  bool     sniffing_       {false};
  uint32_t sniff_start_ms_ {0};

  // -------------------------------------------------------------------------
  // Timer handlers (called every loop)
  // -------------------------------------------------------------------------

  void handle_pair_timers_() {
    if (pair_state_ == PairState::WAIT_CMD_SETTLE) {
      if (millis() - pair_timer_ms_ > BC_PAIR_CMD_SETTLE_MS) {
        ESP_LOGI(TAG, "Settle period elapsed - assuming chip is in learning mode");
        ESP_LOGI(TAG, ">>> Point remote at sensor and press any button (30 s) <<<");
        pair_state_    = PairState::WAIT_IR_CAPTURE;
        pair_timer_ms_ = millis();
      }
    } else if (pair_state_ == PairState::WAIT_IR_CAPTURE) {
      if (millis() - pair_timer_ms_ > BC_PAIR_IR_TIMEOUT_MS) {
        ESP_LOGW(TAG, "Pairing timed out - no IR signal received within 30 s");
        abort_pairing_();
      }
    }
  }

  void handle_sniff_timer_() {
    if (sniffing_ && millis() - sniff_start_ms_ > BC_SNIFF_DURATION_MS) {
      ESP_LOGI(TAG, "--- UART sniff ended ---");
      sniffing_ = false;
      rx_buf_.clear();
    }
  }

  // -------------------------------------------------------------------------
  // Pairing RX handler
  // -------------------------------------------------------------------------

  void handle_pair_rx_(uint8_t b) {
    if (pair_state_ == PairState::WAIT_CMD_SETTLE) {
      // Log everything during settle; timer drives the state transition.
      if (b == BC_ACK_OK)
        ESP_LOGI(TAG, "  -> chip ACK (0x06) during settle");
      else if (b == BC_ACK_FAIL)
        ESP_LOGE(TAG, "  -> chip NAK (0x15) - check MOD pin and power");
      else
        ESP_LOGI(TAG, "  -> chip byte during settle: 0x%02X", b);

    } else if (pair_state_ == PairState::WAIT_IR_CAPTURE) {
      if (b == BC_ACK_FAIL) {
        // 0x15 is the only unambiguous failure code
        ESP_LOGE(TAG, "IR capture failed - 0x15 NAK (remote not recognised)");
        abort_pairing_();
      } else {
        // ANY other byte during the IR capture window means the chip decoded
        // something. Different firmware versions send:
        //   0x06        - bare ACK (standard)
        //   0xB5 ...    - full report frame
        //   0xF8 ...    - proprietary status frame (observed in the field)
        // Accept all of them as success and drain the remainder of the burst.
        ESP_LOGI(TAG, "IR data received (0x%02X) - pairing successful", b);
        // Drain any remaining bytes from this burst (200 ms window)
        uint32_t t = millis();
        while (uart_ && millis() - t < 200) {
          if (uart_->available()) {
            uint8_t tail;
            uart_->read_byte(&tail);
            ESP_LOGI(TAG, "  pair frame tail: 0x%02X", tail);
          }
          delay(1);
        }
        finish_pairing_();
      }
    }
  }

  void finish_pairing_() {
    if (mod_pin_)
      mod_pin_->turn_off();
    pair_state_ = PairState::IDLE;
    rx_buf_.clear();
    ESP_LOGI(TAG, "MOD pin released - normal operation resumed");
    ESP_LOGI(TAG, "--- BC7215A pairing complete ---");
  }

  void abort_pairing_() {
    if (mod_pin_)
      mod_pin_->turn_off();
    pair_state_ = PairState::IDLE;
    rx_buf_.clear();
    ESP_LOGW(TAG, "MOD pin released - pairing aborted");
  }

  // -------------------------------------------------------------------------
  // Frame helpers
  // -------------------------------------------------------------------------

  void build_frame_(uint8_t *f, uint8_t cmd,
                    uint8_t temp, uint8_t mode, uint8_t fan) {
    f[0] = BC_START;
    f[1] = cmd;
    f[2] = std::max((uint8_t) BC_MIN_TEMP, std::min(temp, (uint8_t) BC_MAX_TEMP));
    f[3] = mode;
    f[4] = fan;
    f[5] = f[0] ^ f[1] ^ f[2] ^ f[3] ^ f[4];
  }

  void send_frame_(const uint8_t *f) {
    if (!uart_) return;
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

  // -------------------------------------------------------------------------
  // Async IR report frame parser (normal operation only)
  // -------------------------------------------------------------------------

  void try_parse_report_() {
    auto it = std::find(rx_buf_.begin(), rx_buf_.end(), BC_REP_START);
    if (it == rx_buf_.end()) { rx_buf_.clear(); return; }
    rx_buf_.erase(rx_buf_.begin(), it);

    if (rx_buf_.size() < 3) return;
    uint8_t len   = rx_buf_[1];
    size_t  total = (size_t) len + 3u;
    if (rx_buf_.size() < total) return;

    uint8_t cs = 0;
    for (size_t i = 0; i < total - 1; i++) cs ^= rx_buf_[i];
    if (cs != rx_buf_[total - 1]) {
      ESP_LOGW(TAG, "Bad checksum in report frame");
      rx_buf_.erase(rx_buf_.begin(), rx_buf_.begin() + 1);
      return;
    }

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
  }
};

}  // namespace bc7215ac
}  // namespace esphome