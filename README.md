An Arduino sketch to run on a nodeMCU and mononitor the environment around the laser cutter.

- log temperature, locally on the nodeMCU's spiff
- upload to io.adafruit.com, currently at https://io.adafruit.com/morrowwm/dashboards/maker-space
- send alarm to Telegram if temperature is too low (< 10C) or too high (> 25C)

TODO:
- indicate if the cutter is ready to cut:
    - vent is open
    - vent blower is running
    - water pump is running
    - cutting head air is flowing

Based on:
- Adafruit MQTT Library ESP8266 Adafruit IO SSL/TLS example
- Pieter P's excellent introduction to the ESP8266: https://tttapa.github.io/ESP8266/Chap16%20-%20Data%20Logging.html
