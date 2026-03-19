# 7akim Voice Assistant v2.0

A Jarvis-like smart home voice assistant running on the **Waveshare ESP32-S3-Knob-Touch-LCD-1.8** with full **Home Assistant** integration.

## What It Does

Say **"Jarvis"** and the device wakes up, listens to your command (in English or Arabic), sends it to the backend server, which uses OpenAI Whisper for speech-to-text, GPT-4o-mini with function calling for smart home control, and ElevenLabs for natural text-to-speech response.

The LLM has direct access to your Home Assistant instance and can control lights, AC, TVs, cameras, and run scenes — all through natural voice commands.

## Hardware

| Component | Details |
|-----------|---------|
| Board | Waveshare ESP32-S3-Knob-Touch-LCD-1.8 |
| Display | 1.8" round AMOLED, 360x360, SH8601 driver |
| Touch | CST816 capacitive touch (I2C) |
| Audio Out | PCM5100A DAC, 3.5mm headphone jack |
| Microphone | Built-in PDM digital mic |
| Knob | Rotary encoder (volume control) |
| Wake Word | WakeNet9 on-device ("Jarvis") |

## Architecture

```
ESP32 (WiFi) ──HTTP POST /voice──> Backend Server (Flask)
                                     ├── Whisper STT
                                     ├── GPT-4o-mini + HA Function Calling
                                     ├── Home Assistant REST API
                                     └── ElevenLabs TTS
                                   <──── PCM Audio Response
```

## Quick Start

### 1. Backend Server

```bash
cd 7akim-voice
pip install -r requirements.txt

# Set environment variables
export OPENAI_API_KEY="sk-..."
export ELEVENLABS_KEY="..."
export HA_URL="http://192.168.68.79:8123"
export HA_TOKEN="eyJ..."
export LLM_MODEL="gpt-4o-mini"

# Start server
python3 7akim_voice_server.py
```

### 2. ESP32 Firmware

1. Copy `user_config.template.h` to `user_config.h`
2. Fill in your WiFi SSID, password, and server IP
3. Flash using Arduino IDE or arduino-cli:

```bash
arduino-cli compile --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240" .
arduino-cli upload -p /dev/ttyACM0 --fqbn "esp32:esp32:esp32s3:PSRAM=opi,FlashMode=dio,FlashSize=16M,PartitionScheme=esp_sr_16,USBMode=default,CPUFreq=240" .
```

## Voice Commands (Examples)

| Command | What Happens |
|---------|-------------|
| "Turn on the office light" | Turns on office Govee LED bulb |
| "Set bedroom AC to 22" | Sets Nest thermostat to 22°C |
| "What's the temperature in the living room?" | Reads current temperature |
| "Goodnight" | All lights off, bedroom AC to 23°C |
| "Turn on the Samsung TV" | Powers on Odyssey Ark |
| "نور المكتب" (Arabic) | Turns on office light |
| "كم الحرارة؟" (Arabic) | Reports temperatures |

## API Endpoints

| Endpoint | Method | Description |
|----------|--------|-------------|
| `/voice` | POST | Main voice pipeline (WAV in, PCM out) |
| `/health` | GET | Server health + HA connection status |
| `/ha/states` | GET | List all HA entities |
| `/ha/test` | POST | Test HA service call |
| `/conversation/reset` | POST | Reset conversation history |

## Files

| File | Description |
|------|-------------|
| `7akim_voice.ino` | ESP32 firmware (wake word + UI + audio) |
| `7akim_voice_server.py` | Backend server (STT + LLM + HA + TTS) |
| `user_config.template.h` | WiFi and pin configuration template |
| `lcd_bsp.c/h` | Display + LVGL init |
| `cst816.cpp/h` | Touch driver |
| `.env.example` | Environment variable template |
| `requirements.txt` | Python dependencies |

## Network Setup

See [SETUP.md](SETUP.md) for detailed network configuration including Windows port forwarding and USB passthrough instructions.

## Home Assistant Integration

See [HA_ARCHITECTURE.md](HA_ARCHITECTURE.md) for the full entity inventory and room mappings.
