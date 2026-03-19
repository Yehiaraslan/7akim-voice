/*
 * 7akim Voice Assistant v2.1 — ESP32-S3 Knob Display
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 * fqbn: esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240
 *
 * Wake word: "Jarvis" (on-device WakeNet9 only — no MultiNet)
 * Fallback: Touch screen button (hold to record, release to send)
 * Knob: Volume control (rotary encoder)
 * UI: Clean minimal design — single cyan ring, centered text, pill button
 * Backend: Voice server with Home Assistant integration
 *
 * v2.1 fixes:
 *   - Removed MultiNet command recognition (was crashing on invalid commands)
 *   - WakeNet-only mode: just wake word detection, no on-device commands
 *   - Redesigned UI to match reference: single ring, clean layout, pill button
 */

#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "lcd_bsp.h"
#include "lcd_bl_pwm_bsp.h"
#include "cst816.h"
#include "lcd_config.h"
#include "user_config.h"

// ─── ESP_SR (WakeNet only — no MultiNet) ─────────────
#include <ESP_I2S.h>
#include <ESP_SR.h>

// Empty command list — WakeNet only, no MultiNet commands
static const sr_cmd_t sr_commands[] = {};

I2SClass i2s_mic;

// ─── State ───────────────────────────────────────────
typedef enum {
  STATE_BOOT,
  STATE_LISTENING,
  STATE_RECORDING,
  STATE_PROCESSING,
  STATE_SPEAKING,
  STATE_SHOWING,
  STATE_ERROR
} device_state_t;
volatile device_state_t device_state = STATE_BOOT;

// ─── Event flags ─────────────────────────────────────
static EventGroupHandle_t app_events = NULL;
#define EVT_WAKEWORD_BIT    BIT0
#define EVT_TOUCH_START_BIT BIT1
#define EVT_TOUCH_STOP_BIT  BIT2
#define EVT_REC_TIMEOUT_BIT BIT3

// ─── Audio ───────────────────────────────────────────
static int16_t          *rec_buf    = NULL;
static size_t            rec_bytes  = 0;
static unsigned long     rec_start_time = 0;
static i2s_chan_handle_t  tx_chan    = NULL;

// ─── Volume (knob controlled) ────────────────────────
static volatile int volume = 80;
static volatile int encoder_pos = 80;

// ─── UI handles ───────────────────────────────────────
static lv_obj_t *ui_screen       = NULL;
static lv_obj_t *ui_ring         = NULL;   // Single main ring
static lv_obj_t *ui_center       = NULL;   // Dark center area
static lv_obj_t *ui_name_label   = NULL;   // "7akim"
static lv_obj_t *ui_status_label = NULL;   // "Tap to talk" / status
static lv_obj_t *ui_btn          = NULL;   // Bottom pill button
static lv_obj_t *ui_btn_label    = NULL;   // Button text
static lv_obj_t *ui_response_label = NULL; // Response text overlay
static lv_obj_t *ui_vol_bar      = NULL;
static lv_obj_t *ui_vol_label    = NULL;

// ─── Animation state ─────────────────────────────────
static lv_timer_t *anim_timer    = NULL;
static uint8_t     anim_phase    = 0;

// ─── Colors ──────────────────────────────────────────
#define COL_BG          0x0a0a0f
#define COL_RING_CYAN   0x00d4ff
#define COL_RING_DIM    0x0a3040
#define COL_CENTER_BG   0x0c1520
#define COL_RECORDING   0xff3333
#define COL_PROCESSING  0xff9500
#define COL_SPEAKING    0x00ff88
#define COL_ERROR       0xff4444
#define COL_TEXT_WHITE  0xffffff
#define COL_TEXT_DIM    0x88aacc
#define COL_BTN_BG     0x00d4ff
#define COL_BTN_TEXT   0x0a0a0f

// ─── Thread-safe UI helpers ──────────────────────────
void ui_set_status(const char *text, uint32_t ring_color) {
  lvgl_acquire();
  if (ui_status_label) lv_label_set_text(ui_status_label, text);
  if (ui_ring) {
    lv_obj_set_style_border_color(ui_ring, lv_color_hex(ring_color), 0);
    lv_obj_set_style_shadow_color(ui_ring, lv_color_hex(ring_color), 0);
  }
  lvgl_release();
}

void ui_set_name(const char *text) {
  lvgl_acquire();
  if (ui_name_label) lv_label_set_text(ui_name_label, text);
  lvgl_release();
}

void ui_set_btn_text(const char *text) {
  lvgl_acquire();
  if (ui_btn_label) lv_label_set_text(ui_btn_label, text);
  lvgl_release();
}

void ui_set_btn_color(uint32_t bg_color, uint32_t text_color) {
  lvgl_acquire();
  if (ui_btn) lv_obj_set_style_bg_color(ui_btn, lv_color_hex(bg_color), 0);
  if (ui_btn_label) lv_obj_set_style_text_color(ui_btn_label, lv_color_hex(text_color), 0);
  lvgl_release();
}

void ui_set_response(const char *text) {
  lvgl_acquire();
  if (ui_response_label) {
    lv_label_set_text(ui_response_label, text);
    lv_obj_clear_flag(ui_response_label, LV_OBJ_FLAG_HIDDEN);
  }
  lvgl_release();
}

void ui_hide_response() {
  lvgl_acquire();
  if (ui_response_label) {
    lv_obj_add_flag(ui_response_label, LV_OBJ_FLAG_HIDDEN);
  }
  lvgl_release();
}

void ui_update_volume() {
  lvgl_acquire();
  if (ui_vol_bar) lv_bar_set_value(ui_vol_bar, volume, LV_ANIM_ON);
  if (ui_vol_label) {
    char buf[16];
    snprintf(buf, sizeof(buf), "VOL %d%%", volume);
    lv_label_set_text(ui_vol_label, buf);
  }
  lvgl_release();
}

// ─── Pulse animation callback ────────────────────────
static void anim_timer_cb(lv_timer_t *timer) {
  anim_phase = (anim_phase + 1) % 60;
  float sine_val = sin(anim_phase * 3.14159f / 30.0f);

  lvgl_acquire();

  if (device_state == STATE_LISTENING) {
    // Gentle breathing glow on the ring
    uint8_t shadow_w = 15 + (uint8_t)(10.0f * sine_val);
    uint8_t border_opa = 180 + (uint8_t)(50.0f * sine_val);
    if (ui_ring) {
      lv_obj_set_style_shadow_width(ui_ring, shadow_w, 0);
      lv_obj_set_style_border_opa(ui_ring, border_opa, 0);
    }
  } else if (device_state == STATE_RECORDING) {
    // Fast red pulse
    uint8_t shadow_w = 20 + (uint8_t)(15.0f * sin(anim_phase * 3.14159f / 10.0f));
    if (ui_ring) {
      lv_obj_set_style_shadow_width(ui_ring, shadow_w, 0);
    }
  } else if (device_state == STATE_PROCESSING) {
    // Orange spinning feel — pulsing shadow
    uint8_t shadow_w = 10 + (uint8_t)(20.0f * sine_val);
    if (ui_ring) {
      lv_obj_set_style_shadow_width(ui_ring, shadow_w, 0);
    }
  }

  lvgl_release();
}

// ─── Build the clean minimal UI ──────────────────────
void build_ui() {
  lvgl_acquire();

  ui_screen = lv_scr_act();
  lv_obj_set_style_bg_color(ui_screen, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(ui_screen, LV_OPA_COVER, 0);

  // ── Single main ring (cyan glowing circle) ──
  // Positioned slightly above center to leave room for button
  ui_ring = lv_obj_create(ui_screen);
  lv_obj_set_size(ui_ring, 210, 210);
  lv_obj_align(ui_ring, LV_ALIGN_CENTER, 0, -30);
  lv_obj_set_style_radius(ui_ring, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_ring, lv_color_hex(COL_CENTER_BG), 0);
  lv_obj_set_style_bg_opa(ui_ring, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui_ring, 3, 0);
  lv_obj_set_style_border_color(ui_ring, lv_color_hex(COL_RING_CYAN), 0);
  lv_obj_set_style_border_opa(ui_ring, 200, 0);
  lv_obj_set_style_shadow_width(ui_ring, 20, 0);
  lv_obj_set_style_shadow_color(ui_ring, lv_color_hex(COL_RING_CYAN), 0);
  lv_obj_set_style_shadow_opa(ui_ring, 120, 0);
  lv_obj_set_style_shadow_spread(ui_ring, 3, 0);
  lv_obj_clear_flag(ui_ring, LV_OBJ_FLAG_SCROLLABLE);

  // ── Name label: "7akim" ──
  ui_name_label = lv_label_create(ui_ring);
  lv_label_set_text(ui_name_label, "7akim");
  lv_obj_set_style_text_font(ui_name_label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(ui_name_label, lv_color_hex(COL_TEXT_WHITE), 0);
  lv_obj_set_style_text_letter_space(ui_name_label, 2, 0);
  lv_obj_align(ui_name_label, LV_ALIGN_CENTER, 0, -12);

  // ── Status label: "Tap to talk" ──
  ui_status_label = lv_label_create(ui_ring);
  lv_label_set_text(ui_status_label, "Booting...");
  lv_obj_set_style_text_font(ui_status_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_align(ui_status_label, LV_ALIGN_CENTER, 0, 18);

  // ── Bottom pill button: "Hold to Talk" ──
  ui_btn = lv_btn_create(ui_screen);
  lv_obj_set_size(ui_btn, 200, 42);
  lv_obj_align(ui_btn, LV_ALIGN_BOTTOM_MID, 0, -15);
  lv_obj_set_style_radius(ui_btn, 21, 0);  // Fully rounded pill
  lv_obj_set_style_bg_color(ui_btn, lv_color_hex(COL_BTN_BG), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_opa(ui_btn, 220, LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(ui_btn, lv_color_hex(COL_RECORDING), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(ui_btn, 0, 0);
  lv_obj_set_style_shadow_width(ui_btn, 15, 0);
  lv_obj_set_style_shadow_color(ui_btn, lv_color_hex(COL_BTN_BG), 0);
  lv_obj_set_style_shadow_opa(ui_btn, 100, 0);

  ui_btn_label = lv_label_create(ui_btn);
  lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Hold to Talk");
  lv_obj_set_style_text_font(ui_btn_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_btn_label, lv_color_hex(COL_BTN_TEXT), 0);
  lv_obj_center(ui_btn_label);

  // ── Volume bar (small, bottom-left) ──
  ui_vol_bar = lv_bar_create(ui_screen);
  lv_obj_set_size(ui_vol_bar, 80, 4);
  lv_obj_align(ui_vol_bar, LV_ALIGN_BOTTOM_LEFT, 15, -5);
  lv_bar_set_range(ui_vol_bar, 0, 100);
  lv_bar_set_value(ui_vol_bar, volume, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(ui_vol_bar, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_style_bg_color(ui_vol_bar, lv_color_hex(COL_RING_CYAN), LV_PART_INDICATOR);
  lv_obj_set_style_radius(ui_vol_bar, 2, 0);
  lv_obj_set_style_radius(ui_vol_bar, 2, LV_PART_INDICATOR);

  ui_vol_label = lv_label_create(ui_screen);
  char vol_buf[16];
  snprintf(vol_buf, sizeof(vol_buf), "VOL %d%%", volume);
  lv_label_set_text(ui_vol_label, vol_buf);
  lv_obj_set_style_text_font(ui_vol_label, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(ui_vol_label, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_align(ui_vol_label, LV_ALIGN_BOTTOM_RIGHT, -15, -3);

  // ── Response overlay label (hidden by default) ──
  ui_response_label = lv_label_create(ui_screen);
  lv_label_set_text(ui_response_label, "");
  lv_label_set_long_mode(ui_response_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ui_response_label, 300);
  lv_obj_set_style_text_font(ui_response_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(ui_response_label, lv_color_hex(COL_SPEAKING), 0);
  lv_obj_set_style_text_align(ui_response_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui_response_label, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_add_flag(ui_response_label, LV_OBJ_FLAG_HIDDEN);

  // ── Start animation timer (50ms = 20fps) ──
  anim_timer = lv_timer_create(anim_timer_cb, 50, NULL);

  lvgl_release();
}

// ─── ESP_SR wake word callback ────────────────────────
void onSrEvent(sr_event_t event, int command_id, int phrase_id) {
  switch (event) {
    case SR_EVENT_WAKEWORD:
      Serial.println("[7akim] Wake word 'Jarvis' detected!");
      if (device_state == STATE_LISTENING) {
        xEventGroupSetBits(app_events, EVT_WAKEWORD_BIT);
      }
      break;
    case SR_EVENT_WAKEWORD_CHANNEL:
      Serial.printf("[7akim] Wake word channel verified (phrase_id=%d)\n", phrase_id);
      break;
    default:
      break;
  }
}

// ─── Touch polling task ───────────────────────────────
static void touch_poll_task(void *arg) {
  uint16_t tx, ty;
  bool was_touched = false;
  for (;;) {
    bool touched = (getTouch(&tx, &ty) != 0);
    if (touched && !was_touched) {
      if (device_state == STATE_LISTENING) {
        xEventGroupSetBits(app_events, EVT_TOUCH_START_BIT);
      }
    } else if (!touched && was_touched) {
      if (device_state == STATE_RECORDING) {
        xEventGroupSetBits(app_events, EVT_TOUCH_STOP_BIT);
      }
    }
    was_touched = touched;
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

// ─── Encoder (knob) polling task ──────────────────────
static void encoder_poll_task(void *arg) {
  int last_a = digitalRead(ENCODER_A_PIN);
  for (;;) {
    int a = digitalRead(ENCODER_A_PIN);
    if (a != last_a && a == LOW) {
      int b = digitalRead(ENCODER_B_PIN);
      if (b != a) {
        encoder_pos = min(100, encoder_pos + 3);
      } else {
        encoder_pos = max(0, encoder_pos - 3);
      }
      volume = encoder_pos;
      ui_update_volume();
      Serial.printf("[7akim] Volume: %d%%\n", volume);
    }
    last_a = a;
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

// ─── Speaker hardware ─────────────────────────────────
void init_speaker() {
  gpio_config_t gc = {};
  gc.pin_bit_mask = (1ULL << PCM_ENABLE_PIN);
  gc.mode = GPIO_MODE_OUTPUT;
  gc.pull_up_en = GPIO_PULLUP_ENABLE;
  gc.intr_type = GPIO_INTR_DISABLE;
  gpio_config(&gc);
  gpio_set_level((gpio_num_t)PCM_ENABLE_PIN, 1);

  i2s_chan_config_t tx_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_1, I2S_ROLE_MASTER);
  i2s_new_channel(&tx_cfg, &tx_chan, NULL);
  i2s_std_config_t tx_std = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(SAMPLE_RATE),
    .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED, .bclk = (gpio_num_t)I2S_BCLK_PIN,
      .ws   = (gpio_num_t)I2S_WS_PIN, .dout = (gpio_num_t)I2S_DOUT_PIN,
      .din  = I2S_GPIO_UNUSED, .invert_flags = {false, false, false},
    },
  };
  i2s_channel_init_std_mode(tx_chan, &tx_std);
  i2s_channel_enable(tx_chan);
}

// ─── PDM Microphone ──────────────────────────────────
void init_microphone() {
  i2s_mic.setTimeout(1000);
  i2s_mic.setPinsPdmRx(PDM_CLK_PIN, PDM_DATA_PIN);
  i2s_mic.begin(I2S_MODE_PDM_RX, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO);
  Serial.println("[7akim] PDM microphone initialized");
}

// ─── WAV builder ─────────────────────────────────────
void build_wav_header(uint8_t *h, uint32_t pcm_bytes) {
  uint32_t total = pcm_bytes + 36, br = SAMPLE_RATE * 2;
  uint16_t pt = 1, ch = 1, ba = 2, bd = 16;
  uint32_t sr = SAMPLE_RATE, fs = 16;
  memcpy(h, "RIFF", 4); memcpy(h + 4, &total, 4); memcpy(h + 8, "WAVE", 4);
  memcpy(h + 12, "fmt ", 4); memcpy(h + 16, &fs, 4); memcpy(h + 20, &pt, 2);
  memcpy(h + 22, &ch, 2); memcpy(h + 24, &sr, 4); memcpy(h + 28, &br, 4);
  memcpy(h + 32, &ba, 2); memcpy(h + 34, &bd, 2);
  memcpy(h + 36, "data", 4); memcpy(h + 40, &pcm_bytes, 4);
}

// ─── Volume-adjusted playback ────────────────────────
void play_audio(uint8_t *data, size_t len) {
  int16_t *samples = (int16_t *)data;
  size_t num_samples = len / 2;
  float vol_scale = volume / 100.0f;

  for (size_t i = 0; i < num_samples; i++) {
    int32_t s = (int32_t)(samples[i] * vol_scale);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    samples[i] = (int16_t)s;
  }

  size_t written = 0, offset = 0;
  while (offset < len) {
    size_t chunk = min((size_t)2048, len - offset);
    i2s_channel_write(tx_chan, data + offset, chunk, &written, 1000);
    offset += written;
  }
}

// ─── Send audio to server and play response ───────────
bool send_and_play() {
  if (rec_bytes < 2000) return false;

  uint32_t wav_size = rec_bytes + 44;
  uint8_t *wav = (uint8_t *)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
  if (!wav) return false;
  build_wav_header(wav, rec_bytes);
  memcpy(wav + 44, rec_buf, rec_bytes);

  HTTPClient http;
  http.begin(String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/voice");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(30000);
  // Collect response headers
  const char *headerKeys[] = {"X-7akim-Text", "X-7akim-HA-Action"};
  http.collectHeaders(headerKeys, 2);
  int code = http.POST(wav, wav_size);
  heap_caps_free(wav);

  if (code != 200) {
    Serial.printf("[7akim] Server returned %d\n", code);
    http.end();
    return false;
  }

  // Display response text and HA action
  String response_text = http.header("X-7akim-Text");
  String ha_action = http.header("X-7akim-HA-Action");

  if (response_text.length() > 0) {
    Serial.printf("[7akim] Response: %s\n", response_text.c_str());
    if (response_text.length() > 80) {
      response_text = response_text.substring(0, 77) + "...";
    }
    ui_set_response(response_text.c_str());
  }

  if (ha_action.length() > 0) {
    Serial.printf("[7akim] HA Action: %s\n", ha_action.c_str());
  }

  // Stream and play audio response
  int plen = http.getSize();
  if (plen > 0 && plen < (int)AUDIO_BUF_SIZE) {
    uint8_t *resp = (uint8_t *)heap_caps_malloc(plen, MALLOC_CAP_SPIRAM);
    if (resp) {
      WiFiClient *stream = http.getStreamPtr();
      size_t got = 0;
      unsigned long t = millis();
      while (got < (size_t)plen && millis() - t < 20000) {
        int av = stream->available();
        if (av > 0) got += stream->readBytes(resp + got, av);
        else delay(1);
      }

      device_state = STATE_SPEAKING;
      ui_set_status("Speaking...", COL_SPEAKING);
      ui_set_btn_text(LV_SYMBOL_PLAY "  Speaking...");
      ui_set_btn_color(COL_SPEAKING, COL_BTN_TEXT);
      play_audio(resp, got);
      heap_caps_free(resp);
    }
  }
  http.end();
  return true;
}

// ─── Record audio chunk ──────────────────────────────
void record_audio_chunk() {
  if (rec_bytes + 1024 <= AUDIO_BUF_SIZE) {
    size_t got = i2s_mic.readBytes((char *)rec_buf + rec_bytes, 1024);
    rec_bytes += got;
  }
}

// ─── Voice task (main state machine) ──────────────────
void voice_task(void *arg) {
  for (;;) {
    switch (device_state) {

      case STATE_LISTENING: {
        EventBits_t bits = xEventGroupWaitBits(
          app_events,
          EVT_WAKEWORD_BIT | EVT_TOUCH_START_BIT,
          pdTRUE, pdFALSE, portMAX_DELAY
        );

        bool from_wakeword = (bits & EVT_WAKEWORD_BIT);
        bool from_touch    = (bits & EVT_TOUCH_START_BIT);

        if (from_wakeword || from_touch) {
          ESP_SR.pause();
          Serial.println("[7akim] ESP_SR paused, starting recording");

          rec_bytes = 0;
          rec_start_time = millis();
          device_state = STATE_RECORDING;
          ui_hide_response();

          // Update UI for recording
          ui_set_status("Listening...", COL_RECORDING);
          ui_set_btn_color(COL_RECORDING, COL_TEXT_WHITE);
          if (from_wakeword) {
            ui_set_btn_text(LV_SYMBOL_STOP "  Speak now...");
          } else {
            ui_set_btn_text(LV_SYMBOL_STOP "  Release to send");
          }
        }
        break;
      }

      case STATE_RECORDING: {
        record_audio_chunk();

        bool stop = false;

        if (xEventGroupGetBits(app_events) & EVT_TOUCH_STOP_BIT) {
          xEventGroupClearBits(app_events, EVT_TOUCH_STOP_BIT);
          stop = true;
          Serial.println("[7akim] Touch released, stopping recording");
        }

        if (millis() - rec_start_time >= (unsigned long)(MAX_REC_SECONDS * 1000)) {
          stop = true;
          Serial.println("[7akim] Max recording time reached");
        }

        if (rec_bytes >= AUDIO_BUF_SIZE) {
          stop = true;
          Serial.println("[7akim] Recording buffer full");
        }

        if (millis() - rec_start_time >= WAKEWORD_REC_TIMEOUT_MS) {
          stop = true;
          Serial.println("[7akim] Wake word recording timeout");
        }

        if (stop) {
          device_state = STATE_PROCESSING;
          ui_set_status("Thinking...", COL_PROCESSING);
          ui_set_btn_text(LV_SYMBOL_REFRESH "  Processing...");
          ui_set_btn_color(COL_PROCESSING, COL_BTN_TEXT);
          Serial.printf("[7akim] Recorded %d bytes, sending to server\n", rec_bytes);
        }
        break;
      }

      case STATE_PROCESSING: {
        if (send_and_play()) {
          // Show response for a few seconds then return to listening
          device_state = STATE_SHOWING;
          ui_set_status("Tap to talk", COL_RING_CYAN);
          ui_set_btn_text(LV_SYMBOL_AUDIO "  Hold to Talk");
          ui_set_btn_color(COL_BTN_BG, COL_BTN_TEXT);
          vTaskDelay(pdMS_TO_TICKS(4000));
          ui_hide_response();
          device_state = STATE_LISTENING;
          ui_set_status("Tap to talk", COL_RING_CYAN);
        } else {
          device_state = STATE_ERROR;
          ui_set_status("Error", COL_ERROR);
          ui_set_btn_text("Server error");
          ui_set_btn_color(COL_ERROR, COL_TEXT_WHITE);
          vTaskDelay(pdMS_TO_TICKS(2000));
          device_state = STATE_LISTENING;
          ui_set_status("Tap to talk", COL_RING_CYAN);
          ui_set_btn_text(LV_SYMBOL_AUDIO "  Hold to Talk");
          ui_set_btn_color(COL_BTN_BG, COL_BTN_TEXT);
        }

        ESP_SR.resume();
        Serial.println("[7akim] ESP_SR resumed, listening for wake word");
        break;
      }

      default:
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    vTaskDelay(pdMS_TO_TICKS(10));
  }
}

// ─── setup ────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  Serial.println("\n[7akim] ═══════════════════════════════════════");
  Serial.println("[7akim]   7akim Voice Assistant v2.1");
  Serial.println("[7akim]   Wake word: 'Jarvis' (WakeNet9 only)");
  Serial.println("[7akim]   Home Assistant Integration");
  Serial.println("[7akim] ═══════════════════════════════════════");

  // ── Display + Touch ──
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  vTaskDelay(pdMS_TO_TICKS(300));

  build_ui();
  ui_set_status("Booting...", COL_PROCESSING);

  // ── Encoder pins ──
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);

  // ── WiFi ──
  ui_set_status("Connecting WiFi...", COL_PROCESSING);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[7akim] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    ui_set_status("Online", COL_SPEAKING);
    vTaskDelay(pdMS_TO_TICKS(1000));
  } else {
    Serial.println("[7akim] WiFi connection failed!");
    ui_set_status("No WiFi", COL_ERROR);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // ── Audio buffer (PSRAM) ──
  rec_buf = (int16_t *)heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!rec_buf) {
    Serial.println("[7akim] PSRAM allocation failed!");
    ui_set_status("Memory Error", COL_ERROR);
    while (1) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  Serial.printf("[7akim] Audio buffer: %d bytes in PSRAM\n", AUDIO_BUF_SIZE);

  // ── Speaker ──
  init_speaker();
  Serial.println("[7akim] Speaker initialized (I2S_NUM_1)");

  // ── Microphone ──
  init_microphone();

  // ── ESP_SR Wake Word Engine (WakeNet only, NO MultiNet) ──
  ui_set_status("Loading model...", COL_PROCESSING);
  Serial.println("[7akim] Initializing ESP_SR (WakeNet only, no MultiNet)...");

  ESP_SR.onEvent(onSrEvent);
  // Pass 0 commands and SR_MODE_WAKEWORD to skip MultiNet entirely
  bool sr_ok = ESP_SR.begin(
    i2s_mic,
    sr_commands,
    0,                    // Zero commands = skip MultiNet
    SR_CHANNELS_MONO,
    SR_MODE_WAKEWORD      // WakeNet only mode
  );

  if (sr_ok) {
    Serial.println("[7akim] WakeNet started successfully! Listening for 'Jarvis'...");
  } else {
    Serial.println("[7akim] WakeNet failed! Touch-only mode.");
    ui_set_status("No wake word", COL_ERROR);
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // ── Event group ──
  app_events = xEventGroupCreate();

  // ── Ready ──
  device_state = STATE_LISTENING;
  ui_set_status("Tap to talk", COL_RING_CYAN);
  ui_set_btn_text(LV_SYMBOL_AUDIO "  Hold to Talk");
  Serial.println("[7akim] Ready! Say 'Jarvis' or touch the screen.");

  // ── Tasks ──
  xTaskCreatePinnedToCore(voice_task, "voice", 8192, NULL, 5, NULL, 1);
  xTaskCreate(touch_poll_task, "touch_poll", 2048, NULL, 4, NULL);
  xTaskCreate(encoder_poll_task, "encoder", 2048, NULL, 3, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
