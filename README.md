An Arduino sketch to run on a nodeMCU and mononitor the environment around the laser cutter.

- log temperature, locally on the nodeMCU's spiff
- upload to io.adafruit.com

TODO:
- alarm if temperature gets low enough to risk freezing the laser's water jacket
- indicate if the cutter is ready to cut:
    - vent is open
    - water pump is running
    - cutting head air is flowing
