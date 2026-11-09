# Bob

Bob — eigene Firmware für das ehemalige Taby-Gerät (Waveshare ESP32-S3-Touch-AMOLED-1.64, **Board-Revision V1 → LCD_CS = GPIO9**).
Ein lebendiges Gesicht, das auf Claude Code reagiert.

| Stimmung | Auslöser | Aussehen |
|---|---|---|
| idle | – | weiße Augen, blinzeln, schauen umher |
| working | Prompt abgeschickt / Tool gelaufen | orange, fokussiert, „liest“ |
| attention | Claude braucht eine Freigabe | große gelbe Augen, wackeln |
| done | Claude ist fertig | weiß, fröhliche ^^-Augen, hüpft ab und zu, bis du tippst |
| sleep | 3 min nichts los | Augen zu, gedimmt, Zzz, nach 20 min Display aus |

Tippen aufs Display: bestätigt „fertig“ oder weckt Bob auf.
Lange drücken (~1 s): startet den **Fokus-Timer** (25 min, dann 5 min Pause) bzw. stoppt ihn.
Oben läuft dann ein dünner Balken; kurz tippen zeigt die Restzeit.

**Mehrere Claude-Sessions:** jede Session meldet sich mit ihrer ID. Bob zeigt die wichtigste Stimmung
(braucht dich > arbeitet > fertig). Ab zwei Sessions zeigen Punkte unter dem Mund den Stand jeder Session
(orange = arbeitet, gelb = braucht dich, weiß = fertig).

## Ordner
- `firmware/` – PlatformIO-Projekt (Arduino, GFX Library for Arduino)
- `host/bob_notify.py` – Hook-Skript, schreibt den Status per USB-Seriell
- `backup/original_firmware_16MB.bin` – komplette Originalfirmware (**nur lokal**, nicht im Repo: proprietär + gerätespezifische Daten)

## Befehle
```sh
.venv/bin/pio run -d firmware -t upload          # bauen + flashen
.venv/bin/pio device monitor -b 115200            # seriell: ping, demo, stats, dbg, sessions, focus [min|stop], hello, working, tool, done, attention, end, idle, sleep, bright 0-255
```

## Original wiederherstellen
```sh
.venv/bin/esptool --port /dev/cu.usbmodem101 write-flash 0 backup/original_firmware_16MB.bin
```

## Hooks
In `~/.claude/settings.json` (Backup: `~/.claude/settings.json.bak-taby`): UserPromptSubmit → working, PostToolUse → tool,
Notification(permission_prompt) → attention, Stop → done, SessionEnd → end (jeweils mit `@<session-id>`).

## Schrift
`firmware/src/font_bob.h` wird aus Nunito (SIL OFL, `tools/fonts/`) erzeugt:
`.venv/bin/python tools/gen_font.py tools/fonts/Nunito.ttf firmware/src/font_bob.h`

## Flashen ohne Hook-Störung
`touch host/.flashing` vor dem Flashen, danach `rm host/.flashing`.

## Musik (Spotify)
`host/bob_music.py` fragt alle 2 s Spotify ab (AppleScript, startet Spotify nicht) und schickt `music play …` / `music stop`.
Läuft Musik, steht unten klein „Künstler – Titel“ (lange Titel laufen durch). Bob schläft dann nicht ein.

Autostart installieren:
```sh
cp host/com.bob.music.plist ~/Library/LaunchAgents/ && launchctl bootstrap gui/$(id -u) ~/Library/LaunchAgents/com.bob.music.plist
```
Entfernen:
```sh
launchctl bootout gui/$(id -u)/com.bob.music && rm ~/Library/LaunchAgents/com.bob.music.plist
```
