# 4G Connectivity via A7682E Modem

## Status: RESEARCH COMPLETE — Ready to implement

## Hardware Pinout (confirmed from LilyGO T-Deck-Pro repo)

| Pin | GPIO | Function |
|-----|------|----------|
| BOARD_A7682E_POWER_EN | 41 | Power rail enable (was BOARD_6609_EN) |
| BOARD_A7682E_PWRKEY | 40 | Power on/off toggle pulse |
| BOARD_A7682E_RST | 9 | Hardware reset |
| BOARD_A7682E_RXD | 10 | Modem UART RX (ESP32 TX → Modem RX) |
| BOARD_A7682E_TXD | 11 | Modem UART TX (Modem TX → ESP32 RX) |
| BOARD_A7682E_DTR | 8 | Data Terminal Ready (LOW to wake) |
| BOARD_A7682E_RI | 7 | Ring Indicator |

UART config: `Serial1.begin(115200, SERIAL_8N1, GPIO_11, GPIO_10)`

## Modem Specs
- SIMCom A7682E: LTE Cat 1 (10Mbps down / 5Mbps up)
- Bands: LTE-FDD B1/B3/B5/B7/B8/B20 + GSM 900/1800
- Backward-compatible with SIM800C/SIM868 AT commands
- Supports TCP/IP/PPP/RNDIS protocols
- Peak current: 2A (battery considerations)
- Nano-SIM, 1.8V/3V

## Power On/Off Sequence

```cpp
// Power ON: enable rail + short PWRKEY pulse
digitalWrite(BOARD_A7682E_POWER_EN, HIGH);  // Power rail ON
delay(100);
digitalWrite(BOARD_A7682E_PWRKEY, LOW);
delay(10);
digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
delay(50);   // 50ms HIGH pulse = power ON
digitalWrite(BOARD_A7682E_PWRKEY, LOW);
// Wait ~5s for modem to boot, then retry AT up to 10 times

// Power OFF: long PWRKEY pulse (3s)
digitalWrite(BOARD_A7682E_PWRKEY, HIGH);
delay(3000);
digitalWrite(BOARD_A7682E_PWRKEY, LOW);
digitalWrite(BOARD_A7682E_POWER_EN, LOW);  // Cut power rail
```

**Note from LilyGO**: After first power-on, RST pin may lose shutdown ability.
Always use PWRKEY for on/off. RST only for hard reset.

## Connectivity Approach: PPP over Serial (PPPoS)

**PPPoS is the right path for SSH.** LibSSH-ESP32 uses POSIX sockets internally,
so we need a real lwIP network interface. PPPoS provides exactly this — once
established, SSH works identically to WiFi.

AT command sequence:
```
AT+CGDCONT=1,"IP","your_apn"    // Set APN
AT+CGATT=1                       // Attach to GPRS
ATD*99#                          // Enter PPP data mode
// → hand UART to ESP32 lwIP PPPoS stack
```

Two libraries for PPPoS:
1. **esp_modem** (Espressif official) — modern, well-supported
2. **ESP32-PPPOS-EXAMPLE** (loboris) — older reference implementation

### TinyGSM Alternative
- A7682E NOT officially supported, but `#define TINY_GSM_MODEM_SIM7600` works for A76XX family
- LilyGO fork: https://github.com/lewisxhe/TinyGSM
- Problem: TinyGSM gives Arduino `Client` API, not POSIX sockets. LibSSH can't use it directly.

## Gotchas
1. **GPIO 10 conflict**: May share with BOARD_POWERON on some board revisions. Check carefully.
2. **A76XX CMUX bug**: 2-byte CMUX headers can overflow buffers. Disable `ESP_MODEM_CMUX_DEFRAGMENT_PAYLOAD`.
3. **Power draw**: 2A peak on 1400mAh battery. Power modem off when not in use.
4. **Boot time**: ~4-5 seconds after PWRKEY pulse before UART responds to AT.
5. **Band support**: B1/B3/B5/B7/B8/B20 are primarily European. US LTE coverage depends on carrier.
6. **Serial1 is free**: Current firmware doesn't use Serial1 or GPIOs 7-11.

## Implementation Plan

### Phase 1: Basic AT communication
1. Add modem pin definitions (GPIO 7-11, 40-41)
2. Add modemPowerOn() / modemPowerOff() / modemReset() functions
3. Add sendAT() helper for AT command exchange
4. Test with basic AT, AT+CPIN?, AT+CSQ, AT+CEREG? over Serial monitor
5. Add Alt+M keybind to toggle modem on/off

### Phase 2: PPP connectivity
1. Add PPPoS setup (AT+CGDCONT, ATD*99#, lwIP PPPoS)
2. Use `esp_modem` component or manual PPPoS from esp-idf
3. Add APN configuration (hardcoded initially)
4. Verify network interface gets IP address

### Phase 3: Integration with SSH
1. SSH connect should work automatically once PPP interface is up
2. Add modem status to terminal status bar (signal strength, carrier)
3. Add connection priority: WiFi first, fall back to 4G
4. Power management: turn modem off when disconnecting

## Sources
- [LilyGO T-Deck-Pro repo](https://github.com/Xinyuan-LilyGO/T-Deck-Pro) — pin configs, test_AT example
- [TinyGSM A7682E issue #588](https://github.com/vshymanskyy/TinyGSM/issues/588) — no official support
- [LilyGO Modem Series](https://github.com/Xinyuan-LilyGO/LilyGo-Modem-Series) — A7670/A7608 examples
- [A76XX AT Command Manual (PDF)](http://df.mchobby.be/HAT-GSM-4G-A7682E/A76XX_Series_AT_Command_Manual_V1_04.pdf)
- [SIMCom A7682E product page](https://www.simcom.com/product/A7682E.html)
- [Espressif esp_modem docs](https://docs.espressif.com/projects/esp-protocols/esp_modem/docs/latest/README.html)
- [ESP32-PPPOS-EXAMPLE](https://github.com/loboris/ESP32-PPPOS-EXAMPLE)
