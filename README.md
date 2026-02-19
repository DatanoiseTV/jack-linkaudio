# jack-linkaudio

A bridge for routing audio between the JACK Audio Connection Kit and Ableton Link's audio streaming feature.

This application acts as a standalone client that creates a bridge between the JACK audio server and the Ableton Link network. It allows you to send audio from JACK applications into Link Audio streams, and receive audio from Link Audio streams into JACK applications.

## Features

- **Dynamic Port Creation**: Automatically creates JACK output ports for discovered Link Audio streams on the network.
- **Multi-channel Support**: Can be configured to have multiple JACK input ports, each creating a separate Link Audio stream.
- **Latency Compensation**: Reads latency information from JACK to ensure proper audio alignment with the Link timeline.

## Requirements

- A C++17 compatible compiler
- CMake (version 3.10 or later)
- JACK Audio Connection Kit (including development headers)
- `pkg-config`

## Building

First, clone the repository and its submodules:

```bash
git clone --recursive https://github.com/DatanoiseTV/jack-linkaudio.git
cd jack-linkaudio
```

Then, build the project using CMake:

```bash
mkdir build
cd build
cmake ..
make
```

The executable will be located at `build/jack-linkaudio`.

## Usage

Run the bridge from your terminal:

```bash
./build/jack-linkaudio [client-name] [num-inputs]
```

### Arguments

- `client-name` (optional): The name that the client will use to register with JACK. Defaults to `jack-linkaudio`.
- `num-inputs` (optional): The number of JACK input ports to create. Each input port will create a corresponding Link Audio stream. Defaults to 2.

Once running, `jack-linkaudio` will create the specified number of input ports (e.g., `jack-linkaudio:in_1`). Any audio sent to these ports will be broadcast as a Link Audio stream.

When other devices on the network create Link Audio streams, `jack-linkaudio` will automatically create corresponding JACK output ports (e.g., `OtherDevice_TrackName:out_1`) that you can connect to other JACK applications.
