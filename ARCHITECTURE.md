# 7akim Voice Assistant — System Architecture v2.0

## Overview
Jarvis-like smart home voice assistant using ESP32-S3-Knob-Touch-LCD-1.8 with HA integration.

## Data Flow
```
User says "Jarvis" → WakeNet9 detects (on-device, ~300ms)
  → ESP32 records voice command (PDM mic, 16kHz 16-bit mono)
  → HTTP POST /voice → Backend Server
    → Whisper STT (speech to text, auto-detect Arabic/English)
    → LLM (GPT-4o-mini) with HA context + function calling
      → If HA command detected: call HA REST API
      → Generate natural language response
    → ElevenLabs TTS (text to speech)
    → Convert MP3 → PCM 16kHz
  ← Return PCM audio + JSON metadata
  → ESP32 plays audio through DAC/speaker
  → LCD updates with response text + HA state
```

## Components

### 1. ESP32 Firmware (7akim_voice.ino)
- Wake word: "Jarvis" via WakeNet9
- Fallback: Touch screen hold-to-record
- Knob: Volume control (rotary encoder)
- LCD UI: Creative animated Jarvis-style interface
- Audio: PDM mic input, PCM5100A DAC output
- Network: WiFi HTTP to backend server

### 2. Backend Server (7akim_voice_server.py)
- Flask/FastAPI on Ubuntu machine
- Pipeline: Whisper → LLM (with HA tools) → ElevenLabs TTS
- HA Integration: REST API calls via long-lived token
- Function calling: LLM decides when to control HA devices
- Conversation memory: Last 20 messages
- Environment-based config (HA_URL, HA_TOKEN, API keys)

### 3. Home Assistant Integration
- URL: Configurable (local IP or Cloudflare tunnel)
- Auth: Long-lived access token
- Capabilities:
  - Lights: on/off/brightness/color (Govee via MQTT)
  - Climate: set temp, mode (Nest thermostats)
  - Media: Samsung TV, LG TV control
  - Cameras: Ring camera snapshots
  - Sensors: Temperature, humidity, weather
  - Switches: Various Govee/Ring toggles

### 4. Network
```
ESP32 (192.168.68.x) → Windows PC (192.168.68.73:5050)
  → portproxy → Ubuntu (172.31.102.164:5050) Flask server
  → HA via Cloudflare tunnel (ha.alibondabo.com) or direct LAN
```

## HA Function Calling Schema
The LLM gets these tools:
- control_light(entity_id, action, brightness, color)
- control_climate(entity_id, temperature, mode)
- control_media(entity_id, action, volume)
- get_sensor(entity_id) → returns current value
- get_weather() → returns forecast
- control_switch(entity_id, action)
- run_scene(scene_name)

## Entity Room Mapping
- "office" → climate.office, light.office_smart_led_bulb
- "living room" → climate.living_room, light.led_bulb_living_room_2
- "bedroom" → climate.bedroom_thermostat
- "kids room" → climate.talya_salma_ali, light.salma_floor_lamp_basic_2
- "garage" → light.guarage_light, camera.guarage_live_view
- "front" → light.front_light, camera.front_live_view
- "backyard" → light.backyard_light, camera.backyard_live_view
