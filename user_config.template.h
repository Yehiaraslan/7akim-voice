#ifndef USER_CONFIG_H
#define USER_CONFIG_H

// ─── WiFi ────────────────────────────────────────────
#define WIFI_SSID       "YOUR_WIFI_SSID"
#define WIFI_PASSWORD   "YOUR_WIFI_PASSWORD"

// ─── 7akim voice backend server ──────────────────────
#define SERVER_HOST     "YOUR_SERVER_IP"
#define SERVER_PORT     5050

// ─── I2S TX (PCM5100A DAC → headphone jack) ──────────
#define I2S_BCLK_PIN    39
#define I2S_WS_PIN      40
#define I2S_DOUT_PIN    41

// ─── I2S RX (Digital PDM Mic) ────────────────────────
#define PDM_DATA_PIN    46
#define PDM_CLK_PIN     45

// ─── Encoder (knob) ──────────────────────────────────
#define ENCODER_A_PIN   8
#define ENCODER_B_PIN   7

// ─── PCM5100A power enable ────────────────────────────
#define PCM_ENABLE_PIN  0

// ─── Audio settings ───────────────────────────────────
#define SAMPLE_RATE     16000
#define BIT_DEPTH       16
#define MAX_REC_SECONDS 10
#define AUDIO_BUF_SIZE  (SAMPLE_RATE * (BIT_DEPTH/8) * MAX_REC_SECONDS)  // 320KB

// ─── Wake word settings ──────────────────────────────
// After "Jarvis" is detected, auto-stop recording after this many ms
// Increase if you need longer commands, decrease for snappier response
#define WAKEWORD_REC_TIMEOUT_MS  5000

#endif
