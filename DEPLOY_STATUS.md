# Deployment Status — 7akim-voice v2.0

## Zaki's Report (2026-03-19)

### 1. Cloudflare Tunnel: ha.alibondabo.com → HA — ALREADY DONE
- Tunnel openclaw-mcp (ID dbe8dc0c) already has ingress rule
- ha.alibondabo.com routes to http://192.168.68.79:8123
- Tunnel active with 4 connections (Dubai, Frankfurt, Hong Kong)

### 2. Backend Server: Running on Ubuntu — DEPLOYED
- PID 173028, running at /home/yehiaopenclaw/sketches/7akim_voice/
- Health: OK, version 2.0
- HA: connected (direct LAN http://192.168.68.79:8123)
- HA Token: Set
- OpenAI Key: Set
- LLM Model: gpt-4o-mini
- TTS: ElevenLabs
- Port: 5050 on 0.0.0.0
- MISSING: ELEVENLABS_KEY not in process env

### 3. ESP32 Flashing: Needs physical setup
- Server is now directly on LAN at 192.168.68.76
- No more Windows port forwarding needed
- user_config.h needs SERVER_HOST=192.168.68.76

### Key Discovery
- Ubuntu machine is at 192.168.68.76 (same subnet as HA)
- Direct LAN access to HA works — no tunnel needed for server→HA
- Cloudflare tunnel still useful for external access
