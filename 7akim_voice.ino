/*
 * 7akim Voice Assistant v2.0 — ESP32-S3 Knob Display
 * Waveshare ESP32-S3-Knob-Touch-LCD-1.8
 * fqbn: esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240
 *
 * Wake word: "Jarvis" (on-device WakeNet9, ~300ms latency)
 * Fallback: Touch screen button (hold to record, release to send)
 * Knob: Volume control (rotary encoder)
 * UI: Animated Jarvis-style interface with pulsing rings
 * Backend: Voice server with Home Assistant integration
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

// ─── ESP_SR (WakeNet) ────────────────────────────────
#include <ESP_I2S.h>
#include <ESP_SR.h>

static const sr_cmd_t sr_commands[] = {
  {0, "Turn on the light"},
  {1, "Turn off the light"},
};

I2SClass i2s_mic;

// ─── State ───────────────────────────────────────────
typedef enum {
  STATE_BOOT,         // Boot animation
  STATE_LISTENING,    // WakeNet listening for "Jarvis"
  STATE_RECORDING,    // Recording user's voice command
  STATE_PROCESSING,   // Sending to server, waiting for response
  STATE_SPEAKING,     // Playing back TTS audio
  STATE_SHOWING,      // Showing HA action result on screen
  STATE_ERROR         // Error state (temporary)
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
static volatile int volume = 80;  // 0-100
static volatile int encoder_pos = 80;
static int last_encoder_a = HIGH;

// ─── UI handles ───────────────────────────────────────
static lv_obj_t *ui_screen       = NULL;
static lv_obj_t *ui_ring_outer   = NULL;
static lv_obj_t *ui_ring_middle  = NULL;
static lv_obj_t *ui_ring_inner   = NULL;
static lv_obj_t *ui_core         = NULL;
static lv_obj_t *ui_name_label   = NULL;
static lv_obj_t *ui_status_label = NULL;
static lv_obj_t *ui_info_label   = NULL;
static lv_obj_t *ui_vol_bar      = NULL;
static lv_obj_t *ui_vol_label    = NULL;
static lv_obj_t *ui_btn          = NULL;
static lv_obj_t *ui_btn_label    = NULL;
static lv_obj_t *ui_response_label = NULL;

// ─── Animation state ─────────────────────────────────
static lv_timer_t *anim_timer    = NULL;
static uint8_t     anim_phase    = 0;
static bool        pulse_growing = true;

// ─── Colors ──────────────────────────────────────────
#define COL_BG          0x0a0a0f
#define COL_CORE_IDLE   0x0d1b2a
#define COL_RING_IDLE   0x1b2838
#define COL_ACCENT_CYAN 0x00d4ff
#define COL_ACCENT_GREEN 0x00ff88
#define COL_LISTENING   0x00d4ff
#define COL_RECORDING   0xff3333
#define COL_PROCESSING  0xff9500
#define COL_SPEAKING    0x00ff88
#define COL_ERROR       0xff4444
#define COL_TEXT_DIM    0x556677
#define COL_TEXT_BRIGHT 0xeeffff

// ─── Thread-safe UI helpers ──────────────────────────
void ui_set_status(const char *status_txt, uint32_t ring_color) {
  lvgl_acquire();
  if (ui_status_label) lv_label_set_text(ui_status_label, status_txt);
  if (ui_ring_inner) {
    lv_obj_set_style_border_color(ui_ring_inner, lv_color_hex(ring_color), 0);
    lv_obj_set_style_shadow_color(ui_ring_inner, lv_color_hex(ring_color), 0);
  }
  if (ui_ring_middle) {
    lv_obj_set_style_border_color(ui_ring_middle, lv_color_hex(ring_color), 0);
  }
  lvgl_release();
}

void ui_set_info(const char *info_txt) {
  lvgl_acquire();
  if (ui_info_label) lv_label_set_text(ui_info_label, info_txt);
  lvgl_release();
}

void ui_set_response(const char *resp_txt) {
  lvgl_acquire();
  if (ui_response_label) {
    lv_label_set_text(ui_response_label, resp_txt);
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
  if (device_state != STATE_LISTENING && device_state != STATE_RECORDING) return;
  
  anim_phase = (anim_phase + 1) % 60;
  
  lvgl_acquire();
  
  if (device_state == STATE_LISTENING) {
    // Gentle breathing pulse on outer ring
    uint8_t alpha = 80 + (uint8_t)(40.0f * sin(anim_phase * 3.14159f / 30.0f));
    if (ui_ring_outer) {
      lv_obj_set_style_border_opa(ui_ring_outer, alpha, 0);
    }
    // Subtle shadow pulse on inner ring
    uint8_t shadow = 15 + (uint8_t)(10.0f * sin(anim_phase * 3.14159f / 30.0f));
    if (ui_ring_inner) {
      lv_obj_set_style_shadow_width(ui_ring_inner, shadow, 0);
    }
  } else if (device_state == STATE_RECORDING) {
    // Fast pulse — red glow
    uint8_t alpha = 120 + (uint8_t)(80.0f * sin(anim_phase * 3.14159f / 15.0f));
    if (ui_ring_outer) {
      lv_obj_set_style_border_opa(ui_ring_outer, alpha, 0);
      lv_obj_set_style_border_color(ui_ring_outer, lv_color_hex(COL_RECORDING), 0);
    }
    if (ui_ring_inner) {
      lv_obj_set_style_shadow_width(ui_ring_inner, 20 + (anim_phase % 10), 0);
    }
  }
  
  lvgl_release();
}

// ─── Build the Jarvis-style UI ───────────────────────
void build_ui() {
  lvgl_acquire();

  ui_screen = lv_scr_act();
  lv_obj_set_style_bg_color(ui_screen, lv_color_hex(COL_BG), 0);
  lv_obj_set_style_bg_opa(ui_screen, LV_OPA_COVER, 0);

  // ── Outer ring (subtle pulse ring) ──
  ui_ring_outer = lv_obj_create(ui_screen);
  lv_obj_set_size(ui_ring_outer, 280, 280);
  lv_obj_align(ui_ring_outer, LV_ALIGN_CENTER, 0, -25);
  lv_obj_set_style_radius(ui_ring_outer, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ui_ring_outer, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui_ring_outer, 1, 0);
  lv_obj_set_style_border_color(ui_ring_outer, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_border_opa(ui_ring_outer, 80, 0);
  lv_obj_clear_flag(ui_ring_outer, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // ── Middle ring ──
  ui_ring_middle = lv_obj_create(ui_screen);
  lv_obj_set_size(ui_ring_middle, 240, 240);
  lv_obj_align(ui_ring_middle, LV_ALIGN_CENTER, 0, -25);
  lv_obj_set_style_radius(ui_ring_middle, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_opa(ui_ring_middle, LV_OPA_TRANSP, 0);
  lv_obj_set_style_border_width(ui_ring_middle, 2, 0);
  lv_obj_set_style_border_color(ui_ring_middle, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_border_opa(ui_ring_middle, 120, 0);
  lv_obj_clear_flag(ui_ring_middle, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

  // ── Inner ring (main glow ring) ──
  ui_ring_inner = lv_obj_create(ui_screen);
  lv_obj_set_size(ui_ring_inner, 200, 200);
  lv_obj_align(ui_ring_inner, LV_ALIGN_CENTER, 0, -25);
  lv_obj_set_style_radius(ui_ring_inner, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_ring_inner, lv_color_hex(COL_CORE_IDLE), 0);
  lv_obj_set_style_bg_opa(ui_ring_inner, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui_ring_inner, 3, 0);
  lv_obj_set_style_border_color(ui_ring_inner, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_shadow_width(ui_ring_inner, 20, 0);
  lv_obj_set_style_shadow_color(ui_ring_inner, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_shadow_opa(ui_ring_inner, 150, 0);
  lv_obj_clear_flag(ui_ring_inner, LV_OBJ_FLAG_SCROLLABLE);

  // ── Core circle (dark center) ──
  ui_core = lv_obj_create(ui_ring_inner);
  lv_obj_set_size(ui_core, 160, 160);
  lv_obj_center(ui_core);
  lv_obj_set_style_radius(ui_core, LV_RADIUS_CIRCLE, 0);
  lv_obj_set_style_bg_color(ui_core, lv_color_hex(0x060810), 0);
  lv_obj_set_style_bg_opa(ui_core, LV_OPA_COVER, 0);
  lv_obj_set_style_border_width(ui_core, 0, 0);
  lv_obj_clear_flag(ui_core, LV_OBJ_FLAG_SCROLLABLE);

  // ── Name label ──
  ui_name_label = lv_label_create(ui_core);
  lv_label_set_text(ui_name_label, "7AKIM");
  lv_obj_set_style_text_font(ui_name_label, &lv_font_montserrat_28, 0);
  lv_obj_set_style_text_color(ui_name_label, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_text_letter_space(ui_name_label, 4, 0);
  lv_obj_align(ui_name_label, LV_ALIGN_CENTER, 0, -20);

  // ── Status label ──
  ui_status_label = lv_label_create(ui_core);
  lv_label_set_text(ui_status_label, "BOOTING");
  lv_obj_set_style_text_font(ui_status_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_status_label, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_set_style_text_letter_space(ui_status_label, 2, 0);
  lv_obj_align(ui_status_label, LV_ALIGN_CENTER, 0, 10);

  // ── Info label (small, below status) ──
  ui_info_label = lv_label_create(ui_core);
  lv_label_set_text(ui_info_label, "");
  lv_obj_set_style_text_font(ui_info_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(ui_info_label, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_align(ui_info_label, LV_ALIGN_CENTER, 0, 30);

  // ── Volume bar (bottom left arc area) ──
  ui_vol_bar = lv_bar_create(ui_screen);
  lv_obj_set_size(ui_vol_bar, 100, 6);
  lv_obj_align(ui_vol_bar, LV_ALIGN_BOTTOM_LEFT, 20, -55);
  lv_bar_set_range(ui_vol_bar, 0, 100);
  lv_bar_set_value(ui_vol_bar, volume, LV_ANIM_OFF);
  lv_obj_set_style_bg_color(ui_vol_bar, lv_color_hex(0x1a1a2e), 0);
  lv_obj_set_style_bg_color(ui_vol_bar, lv_color_hex(COL_ACCENT_CYAN), LV_PART_INDICATOR);
  lv_obj_set_style_radius(ui_vol_bar, 3, 0);
  lv_obj_set_style_radius(ui_vol_bar, 3, LV_PART_INDICATOR);

  ui_vol_label = lv_label_create(ui_screen);
  char vol_buf[16];
  snprintf(vol_buf, sizeof(vol_buf), "VOL %d%%", volume);
  lv_label_set_text(ui_vol_label, vol_buf);
  lv_obj_set_style_text_font(ui_vol_label, &lv_font_montserrat_10, 0);
  lv_obj_set_style_text_color(ui_vol_label, lv_color_hex(COL_TEXT_DIM), 0);
  lv_obj_align(ui_vol_label, LV_ALIGN_BOTTOM_LEFT, 20, -63);

  // ── Touch button (bottom) ──
  ui_btn = lv_btn_create(ui_screen);
  lv_obj_set_size(ui_btn, 180, 40);
  lv_obj_align(ui_btn, LV_ALIGN_BOTTOM_MID, 0, -8);
  lv_obj_set_style_radius(ui_btn, 20, 0);
  lv_obj_set_style_bg_color(ui_btn, lv_color_hex(0x0d1b2a), LV_STATE_DEFAULT);
  lv_obj_set_style_bg_color(ui_btn, lv_color_hex(COL_RECORDING), LV_STATE_PRESSED);
  lv_obj_set_style_border_width(ui_btn, 1, 0);
  lv_obj_set_style_border_color(ui_btn, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_shadow_width(ui_btn, 8, 0);
  lv_obj_set_style_shadow_color(ui_btn, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_set_style_shadow_opa(ui_btn, 80, 0);

  ui_btn_label = lv_label_create(ui_btn);
  lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Say 'Jarvis'");
  lv_obj_set_style_text_font(ui_btn_label, &lv_font_montserrat_14, 0);
  lv_obj_set_style_text_color(ui_btn_label, lv_color_hex(COL_ACCENT_CYAN), 0);
  lv_obj_center(ui_btn_label);

  // ── Response overlay label (hidden by default) ──
  ui_response_label = lv_label_create(ui_screen);
  lv_label_set_text(ui_response_label, "");
  lv_label_set_long_mode(ui_response_label, LV_LABEL_LONG_WRAP);
  lv_obj_set_width(ui_response_label, 300);
  lv_obj_set_style_text_font(ui_response_label, &lv_font_montserrat_12, 0);
  lv_obj_set_style_text_color(ui_response_label, lv_color_hex(COL_ACCENT_GREEN), 0);
  lv_obj_set_style_text_align(ui_response_label, LV_TEXT_ALIGN_CENTER, 0);
  lv_obj_align(ui_response_label, LV_ALIGN_TOP_MID, 0, 5);
  lv_obj_add_flag(ui_response_label, LV_OBJ_FLAG_HIDDEN);

  // ── Start animation timer ──
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
    case SR_EVENT_COMMAND:
      Serial.printf("[7akim] Command detected: id=%d\n", command_id);
      break;
    case SR_EVENT_TIMEOUT:
      Serial.println("[7akim] SR timeout");
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
  uint32_t total=pcm_bytes+36, br=SAMPLE_RATE*2;
  uint16_t pt=1, ch=1, ba=2, bd=16; uint32_t sr=SAMPLE_RATE, fs=16;
  memcpy(h,"RIFF",4); memcpy(h+4,&total,4); memcpy(h+8,"WAVE",4);
  memcpy(h+12,"fmt ",4); memcpy(h+16,&fs,4); memcpy(h+20,&pt,2);
  memcpy(h+22,&ch,2); memcpy(h+24,&sr,4); memcpy(h+28,&br,4);
  memcpy(h+32,&ba,2); memcpy(h+34,&bd,2);
  memcpy(h+36,"data",4); memcpy(h+40,&pcm_bytes,4);
}

// ─── Volume-adjusted playback ────────────────────────
void play_audio(uint8_t *data, size_t len) {
  // Apply volume scaling to PCM data
  int16_t *samples = (int16_t*)data;
  size_t num_samples = len / 2;
  float vol_scale = volume / 100.0f;
  
  for (size_t i = 0; i < num_samples; i++) {
    int32_t s = (int32_t)(samples[i] * vol_scale);
    if (s > 32767) s = 32767;
    if (s < -32768) s = -32768;
    samples[i] = (int16_t)s;
  }
  
  size_t written=0, offset=0;
  while (offset < len) {
    size_t chunk = min((size_t)2048, len-offset);
    i2s_channel_write(tx_chan, data+offset, chunk, &written, 1000);
    offset += written;
  }
}

// ─── Send audio to server and play response ───────────
bool send_and_play() {
  if (rec_bytes < 2000) return false;

  uint32_t wav_size = rec_bytes + 44;
  uint8_t *wav = (uint8_t*)heap_caps_malloc(wav_size, MALLOC_CAP_SPIRAM);
  if (!wav) return false;
  build_wav_header(wav, rec_bytes);
  memcpy(wav+44, rec_buf, rec_bytes);

  HTTPClient http;
  http.begin(String("http://") + SERVER_HOST + ":" + SERVER_PORT + "/voice");
  http.addHeader("Content-Type", "audio/wav");
  http.setTimeout(30000);
  int code = http.POST(wav, wav_size);
  heap_caps_free(wav);

  if (code != 200) {
    Serial.printf("[7akim] Server returned %d\n", code);
    http.end();
    return false;
  }

  // Check for X-7akim-Text header (response text for display)
  String response_text = http.header("X-7akim-Text");
  String ha_action = http.header("X-7akim-HA-Action");
  
  if (response_text.length() > 0) {
    Serial.printf("[7akim] Response: %s\n", response_text.c_str());
    // Truncate for display
    if (response_text.length() > 80) {
      response_text = response_text.substring(0, 77) + "...";
    }
    ui_set_response(response_text.c_str());
  }
  
  if (ha_action.length() > 0) {
    Serial.printf("[7akim] HA Action: %s\n", ha_action.c_str());
    ui_set_info(ha_action.c_str());
  }

  int plen = http.getSize();
  if (plen > 0 && plen < (int)AUDIO_BUF_SIZE) {
    uint8_t *resp = (uint8_t*)heap_caps_malloc(plen, MALLOC_CAP_SPIRAM);
    if (resp) {
      WiFiClient *stream = http.getStreamPtr();
      size_t got = 0;
      unsigned long t = millis();
      while (got < (size_t)plen && millis()-t < 20000) {
        int av = stream->available();
        if (av>0) got += stream->readBytes(resp+got, av);
        else delay(1);
      }
      device_state = STATE_SPEAKING;
      ui_set_status("SPEAKING", COL_SPEAKING);
      lvgl_acquire();
      if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_PLAY "  Speaking...");
      lvgl_release();
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
    size_t got = i2s_mic.readBytes((char*)rec_buf + rec_bytes, 1024);
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

          ui_set_status("LISTENING", COL_RECORDING);
          lvgl_acquire();
          if (from_wakeword) {
            if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_STOP "  Speak now...");
          } else {
            if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_STOP "  Release to send");
          }
          // Reset outer ring color for recording animation
          if (ui_ring_outer) {
            lv_obj_set_style_border_color(ui_ring_outer, lv_color_hex(COL_RECORDING), 0);
          }
          lvgl_release();
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
          ui_set_status("THINKING", COL_PROCESSING);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_REFRESH "  Processing...");
          // Reset ring colors
          if (ui_ring_outer) {
            lv_obj_set_style_border_color(ui_ring_outer, lv_color_hex(COL_ACCENT_CYAN), 0);
          }
          lvgl_release();
          Serial.printf("[7akim] Recorded %d bytes, sending to server\n", rec_bytes);
        }
        break;
      }

      case STATE_PROCESSING: {
        if (send_and_play()) {
          // Show response for a few seconds then return to listening
          device_state = STATE_SHOWING;
          ui_set_status("READY", COL_ACCENT_CYAN);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Say 'Jarvis'");
          lvgl_release();
          vTaskDelay(pdMS_TO_TICKS(4000));  // Show response text
          ui_hide_response();
          ui_set_info("");
          device_state = STATE_LISTENING;
          ui_set_status("AWAITING", COL_ACCENT_CYAN);
        } else {
          device_state = STATE_ERROR;
          ui_set_status("ERROR", COL_ERROR);
          ui_set_info("Server unreachable");
          vTaskDelay(pdMS_TO_TICKS(2000));
          ui_set_info("");
          device_state = STATE_LISTENING;
          ui_set_status("AWAITING", COL_ACCENT_CYAN);
          lvgl_acquire();
          if (ui_btn_label) lv_label_set_text(ui_btn_label, LV_SYMBOL_AUDIO "  Say 'Jarvis'");
          lvgl_release();
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
  Serial.println("[7akim]   7akim Voice Assistant v2.0");
  Serial.println("[7akim]   Wake word: 'Jarvis' (WakeNet9)");
  Serial.println("[7akim]   Home Assistant Integration");
  Serial.println("[7akim] ═══════════════════════════════════════");

  // ── Display + Touch ──
  Touch_Init();
  lcd_lvgl_Init();
  lcd_bl_pwm_bsp_init(LCD_PWM_MODE_255);
  vTaskDelay(pdMS_TO_TICKS(300));

  build_ui();
  ui_set_status("BOOTING", COL_PROCESSING);

  // ── Encoder pins ──
  pinMode(ENCODER_A_PIN, INPUT_PULLUP);
  pinMode(ENCODER_B_PIN, INPUT_PULLUP);

  // ── WiFi ──
  ui_set_status("WIFI", COL_PROCESSING);
  ui_set_info("Connecting...");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  int tries = 0;
  while (WiFi.status() != WL_CONNECTED && tries++ < 40) {
    vTaskDelay(pdMS_TO_TICKS(500));
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("[7akim] WiFi connected: %s\n", WiFi.localIP().toString().c_str());
    ui_set_status("ONLINE", COL_ACCENT_GREEN);
    ui_set_info(WiFi.localIP().toString().c_str());
    vTaskDelay(pdMS_TO_TICKS(1500));
  } else {
    Serial.println("[7akim] WiFi connection failed!");
    ui_set_status("NO WIFI", COL_ERROR);
    ui_set_info("Check config");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // ── Audio buffer (PSRAM) ──
  rec_buf = (int16_t*)heap_caps_malloc(AUDIO_BUF_SIZE, MALLOC_CAP_SPIRAM);
  if (!rec_buf) {
    Serial.println("[7akim] PSRAM allocation failed!");
    ui_set_status("PSRAM FAIL", COL_ERROR);
    while(1) vTaskDelay(pdMS_TO_TICKS(1000));
  }
  Serial.printf("[7akim] Audio buffer: %d bytes in PSRAM\n", AUDIO_BUF_SIZE);

  // ── Speaker ──
  init_speaker();
  Serial.println("[7akim] Speaker initialized (I2S_NUM_1)");

  // ── Microphone ──
  init_microphone();

  // ── ESP_SR Wake Word Engine ──
  ui_set_status("WAKENET", COL_PROCESSING);
  ui_set_info("Loading model...");
  Serial.println("[7akim] Initializing ESP_SR with 'Jarvis' wake word...");

  ESP_SR.onEvent(onSrEvent);
  bool sr_ok = ESP_SR.begin(
    i2s_mic,
    sr_commands,
    sizeof(sr_commands) / sizeof(sr_cmd_t),
    SR_CHANNELS_MONO,
    SR_MODE_WAKEWORD,
    "MN"
  );

  if (sr_ok) {
    Serial.println("[7akim] ESP_SR started successfully!");
  } else {
    Serial.println("[7akim] ESP_SR failed! Touch-only mode.");
    ui_set_status("NO WAKENET", COL_ERROR);
    ui_set_info("Touch to talk");
    vTaskDelay(pdMS_TO_TICKS(2000));
  }

  // ── Event group ──
  app_events = xEventGroupCreate();

  // ── Ready ──
  device_state = STATE_LISTENING;
  ui_set_status("AWAITING", COL_ACCENT_CYAN);
  ui_set_info("Say 'Jarvis'");
  Serial.println("[7akim] Ready! Say 'Jarvis' or touch the screen.");

  // ── Tasks ──
  xTaskCreatePinnedToCore(voice_task, "voice", 8192, NULL, 5, NULL, 1);
  xTaskCreate(touch_poll_task, "touch_poll", 2048, NULL, 4, NULL);
  xTaskCreate(encoder_poll_task, "encoder", 2048, NULL, 3, NULL);
}

void loop() {
  vTaskDelay(pdMS_TO_TICKS(1000));
}
