# cpp-process

A container template for making a C++ program into a Vivarium process using a newline-delimited JSON-over-TCP protocol.

---

## Build

### Docker

    docker build -t cpp-process .

### Local (CMake)

    cmake -S . -B build
    cmake --build build --config Release
    ./build/vivarium_cpp_process

**Requirements (local build):**
- C++17 compiler
- nlohmann-json3-dev (header-only JSON library)

---

## Run

### Docker (with config mount)

    docker run -p 11111:11111 -v ./test:/config cpp-process

- The server listens on port 11111 by default (HOST=0.0.0.0).
- It looks for /config/config.json. If missing, it falls back to config/default_config.json in the image.

### Environment Overrides

    docker run \
      -e PORT=22222 \
      -e HOST=0.0.0.0 \
      -e CONFIG_PATH=/config/myconfig.json \
      -p 22222:22222 \
      -v "$(pwd)/myconfig:/config" \
      cpp-process

---

## Configuration

- **Primary path:** /config/config.json (mount a host folder to /config)
- **Fallback:** config/default_config.json (baked into the image)

**Example configuration:**
    {
      "process": "counter",
      "rate": 2.0
    }
This config selects the sample CounterProcess and sets its increment rate (units per second).

---

## Commands

Connect to the TCP socket and send one JSON object per line:

- {"command":"inputs"} → returns the input schema expected by the process
- {"command":"outputs"} → returns the output schema produced by the process
- {"command":"update","arguments":{"state":{...},"interval":<seconds>}} → runs one update step

**Example (with netcat):**
    # Connect to the server
    nc -v localhost 11111

Then send commands (one per line):
    {"command":"inputs"}
    {"command":"outputs"}
    {"command":"update","arguments":{"state":{"counter":10.0},"interval":0.5}}

Expected response (with default rate=2.0):
    {"counter":11.0}

On macOS, use `nc -N localhost 11111` so the socket closes cleanly.

---

## Process Design

The core interface is a simple C++ class with three methods:
- json inputs() const
- json outputs() const
- json update(const json& state, double interval)

An example CounterProcess is included. It reads counter from the state and returns a new value incremented by rate * interval.
To add your own process, subclass Process and register it in build_process_from_config().