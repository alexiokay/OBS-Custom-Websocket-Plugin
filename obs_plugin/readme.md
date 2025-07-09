add cuyrrent content to : # VortiDeck OBS Plugin

This is a plugin for [OBS Studio](https://obsproject.com/) exposing actions to [VortiDeck](https://www.vortideck.com), allowing users to take control of their OBS setup through VortiDeck peripherals.

This is customized obs plugin for Logitech GHub, it includes constant reconnecting logic and Will include mdns searching in future

## File structure

* `obs_plugin` directory contains the source code for the plugin.
* `obs_plugin/libs` directory contains precompiled libraries of [OBS Studio](https://github.com/obsproject/obs-studio).
* `other` directory contains libraries and other dependencies for the plugin.
* `other/obs-studio` directory contains source code for an API-only version of [OBS Studio](https://github.com/obsproject/obs-studio).

## Libraries and Other Dependencies
* [Asio C++ Library](https://github.com/chriskohlhoff/asio)
* [JSON for Modern C++](https://github.com/nlohmann/json)
* [OBS Studio](https://github.com/obsproject/obs-studio)
* [WebSocket++](https://github.com/zaphoyd/websocketpp)



## Commands 

The following commands are used to build, install, and interact with the VortiDeck OBS Plugin:

1. **Build the Plugin**: 
   - Run the following commands in your terminal:
     ```
     mkdir build && cd build
     cmake .. -DCMAKE_INSTALL_PREFIX=../install
     cmake --build . --config Release
     cmake --install .
     ```

2. **Connecting to the WebSocket**: 
   - Use the `wscat` tool to connect to the WebSocket server:
     ```
     wscat -c ws://127.0.0.1:9001 -s "json"
     ```

3. **Activating an Instance**: 
   - Before activating an integration instance, ensure that the plugin client is recognized by the WebSocket server. Send an activation message with the generated `integrationGuid` and `instanceGuid`, which will be stored on the server. If this step is not completed, the plugin will not function correctly.
   - Send a JSON payload to activate an integration instance:
     ```json
     {"path": "/api/v1/integration/activate", "verb": "SET", "payload": {"integrationGuid": "1abcdx", "instanceGuid": "1abcdx"}}
     ```

4. **Invoking Commands**: 
   - To invoke specific actions, send a JSON payload like the following:
     ```json
     {"path": "/api/v1/integration/sdk/action/invoke", "verb": "SET", "payload": {"actionId": "obs_desktop_mute_toggle", "integrationGuid": "ae67192054b1d99f", "parameters": {}}}
     ```

These commands facilitate the setup and operation of the VortiDeck OBS Plugin, enabling seamless integration with OBS Studio.
