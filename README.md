# SteamHapticsPlayer-PipeWire

Use the 2026 Steam Controller's haptic motors as a PipeWire audio output. Fork of [Pixel1011/SteamHapticsPlayer](https://github.com/Pixel1011/SteamHapticsPlayer).

## Run

Connect the controller by USB or its wireless puck. USB sounds cleaner. You'll need Linux with PipeWire, HIDAPI and access to the controller's hidraw device.

Download the [Linux x86-64 build](https://github.com/Elandig/SteamHapticsPlayer-PipeWire/releases), then run:

```sh
tar -xzf steam-haptics-linux-x86_64.tar.gz
cd steam-haptics
./steam-haptics-sink
```

Select **Steam Controller Haptics** in your sound settings. Press Ctrl+C to stop.

## Build

Install the build dependencies for your distribution:

```sh
# Ubuntu 24.04
sudo apt install git build-essential pkg-config libpipewire-0.3-dev libhidapi-dev

# Arch Linux
sudo pacman -S --needed git base-devel libpipewire hidapi
```

```sh
git clone --recurse-submodules https://github.com/Elandig/SteamHapticsPlayer-PipeWire.git
cd SteamHapticsPlayer-PipeWire
make -j steam-haptics-sink
./steam-haptics-sink
```

For an existing clone, run `git submodule update --init` before building.
