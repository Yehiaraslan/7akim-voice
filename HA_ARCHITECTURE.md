# Home Assistant Architecture (from Zaki)

## Connection
- URL: http://192.168.68.79:8123
- Token: stored in ~/.config/home-assistant/config.json on Zaki's machine
- Version: HA 2026.3.0
- Timezone: Asia/Dubai

## Entity Inventory (126 total)

### Lights (15)
| Entity ID | Name | State |
|-----------|------|-------|
| light.led_bulb_living_room_2 | LED Bulb Living Room | on |
| light.office_smart_led_bulb | Office Smart LED Bulb | on |
| light.salma_floor_lamp_basic_2 | Salma Floor Lamp | off (+ 7 segments) |
| light.net_lights | Net Lights | off |
| light.curtain_lights | Curtain Lights | off |
| light.guarage_light | Garage Light (Ring cam) | off |
| light.front_light | Front Light (Ring cam) | off |
| light.backyard_light | Backyard Light (Ring cam) | off |

### Climate / AC (4)
| Entity ID | Current > Target | Mode |
|-----------|-----------------|------|
| climate.office | 25.8C > 30C | cool |
| climate.living_room | 25.3C > 30C | cool |
| climate.talya_salma_ali | 23.8C > 22.3C | cool |
| climate.bedroom_thermostat | 24.1C > 23.9C | cool |
All support: cool / off modes, fan on / off.

### Media Players (2)
| Entity ID | State |
|-----------|-------|
| media_player.yehia_55_odyssey_ark_g9_ls55bg970nmxue | on (Samsung Odyssey Ark) |
| media_player.lg_webos_tv_oled65c9pva | unavailable (LG OLED C9) |

### Cameras (3)
Ring cameras: camera.guarage_live_view, camera.front_live_view, camera.backyard_live_view — all idle.

### Switches (12)
- Govee light power: office bulb, living room bulb, salma lamp, net lights, curtain lights
- Ring camera motion detection: garage (off), front (on), backyard (on)
- Govee effects: gradient toggle, dream view toggle for salma lamp & net lights

### Sensors (51)
- Temperature/Humidity: office, living room, talya/salma/ali room, bedroom (Nest thermostats)
- Network: X60 router speeds (72 Mbps down / 20 Mbps up)
- Ring cameras: battery levels (front 96%, backyard 98%, garage 0%)
- Tesla Wall Connector: all currently unavailable
- Govee device status: all Available

### Other
- 1 automation: automation.reload_nest_integration_hourly
- 0 scenes, 0 scripts
- Weather: weather.forecast_home — partly cloudy
- TTS: tts.google_translate_en_com available
- Shopping list: todo.shopping_list

## Rooms
Office, Living Room, Bedroom, Kids Room (Talya/Salma/Ali), Garage, Front, Backyard

## Key Service Domains
light, climate, media_player, switch, camera, cover, fan, scene, script, automation, tts, notify, remote, siren, webostv
