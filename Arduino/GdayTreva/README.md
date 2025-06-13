# GdayTreva
 Smart Switch

** Version **
 - 0.0.1: Building out web server.
 - 0.0.2: Serve HTML from littleFS. Upload data using plugin on arduino IDE >2.2. 10 Feb 25
 - 0.0.3: Very basic implementation of reading and storing json settings. 21 Feb 25
 - 0.0.4: Read WiFI config from json file on flash. 22 Feb 25
 - 0.0.5: Code cleaned up by Grok. 22 Feb 25
 - 0.0.6: single event handler for Eth. 22 Feb 25
 - 0.0.7: mDNS added. 22 Feb 25
 - 0.0.8: NTP added. 22 Feb 25
 - 0.0.9: NTP daily. 22 Feb 25
 - 0.0.10: NTP and internet checks on exponential delay. Websocket added. 23 Feb 25
 - 0.0.11: only trigger NTP and internet checks from timer. 23 Feb 25
 - 0.0.12: Building top div on webpage. Remove blocking code. Optimise and debug NTP.
 - 0.0.13: Send time and TZ back to server over websocket.
 - 0.0.14: Show TZ in webpage
 - 0.0.15: Send memory in websocket, send data as json. Remove TZ from config, only use browser offset in client.
 - 0.0.16: HeartBeat timeout.
 - 0.0.17: Print mem to serial.
 - 0.0.18: Formatting.
 - 0.0.19: Show Eth info on settings page.
 - 0.0.20: NTP Check.
 - 0.0.21: React Routing
 - 0.0.22: DNS issues. Kinda verbose wordarounds.
 - 0.1: Select and run program.
 - 0.1.1: OTA.
 - 0.1.2: Push program state to react.
 - 0.1.2.1: TriggerInfo refined.
 - 0.1.2.2: TriggerInfo refined.
 - 0.1.3: improved output control and WS message efficiency.
 - 0.1.4: pass millis().
 - 0.2: read sensors.
 - 0.2.1: Disable WiFi when ETH is connected (DNS resetting to 0.0.0.0).
 - 0.2.2: Pass sensor capabilities, advanced sensor scanning etc.
 - 0.2.3: Differentiate ACS71020 from MCP9600.