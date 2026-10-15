# <img src="packaging/kraken-explorer.png" width="48" height="48"> Kraken Explorer: Day of the N2K Tentacle

_"Deeper than a Peak. Wireshark is stuck in shallow waters."_

Open-source CAN / CAN FD / LIN / NMEA 2000 bus analyzer for Linux and Windows. Dear ImGui GUI in a
deep-sea dark theme, GPL-2.0, fork of [CANgaroo](https://github.com/Schildkroet/CANgaroo).

[![Kraken Explorer demo](docs/demo.gif)](docs/demo.mp4)

▶ [Demo video (1:36)](docs/demo.mp4): an 8 GB log (183 million frames) opens in 4 s, then the file
in the trace, the Ctrl+P signal finder, Value Search, replay with breakpoints and Step, and live
DBC-decoded traffic on `vcan0`.

## Download

Get the packages from the [latest release](https://github.com/stoffej/kraken-explorer/releases/latest)
([all releases](https://github.com/stoffej/kraken-explorer/releases)):

| Platform | Package |
|---|---|
| Ubuntu / Debian | `kraken-explorer_<version>_amd64.deb` |
| Linux, no install | `Kraken_Explorer-<version>-x86_64.AppImage` |
| Windows installer | `kraken-explorer-<version>-win64-setup.exe` |
| Windows portable | `kraken-explorer-<version>-win64.zip` |

### Linux installation

Choose **one** of these alternatives:

**Method A — Ubuntu / Debian installer**

Downloads and installs the latest `.deb`, including the icon, menu entry and SocketCAN polkit
rule, and adds you to `netdev`. Log out and back in if prompted.

```bash
curl -fsSL https://raw.githubusercontent.com/stoffej/kraken-explorer/main/scripts/get.sh | sh
```

**Method B — Manual `.deb` installation (Ubuntu / Debian)**

Download the `kraken-explorer_*_amd64.deb` asset from the
[latest release](https://github.com/stoffej/kraken-explorer/releases/latest).
In the folder containing the download, with only one matching `.deb`, run:

```bash
sudo apt install ./kraken-explorer_*_amd64.deb
```

Both `.deb` methods install `kraken-explorer`, which you can launch from the application menu
or your terminal.

**Method C — AppImage (Linux, no installation)**

Download the `Kraken_Explorer-*-x86_64.AppImage` asset from the
[latest release](https://github.com/stoffej/kraken-explorer/releases/latest).
In the folder containing the download, with only one matching AppImage, run:

```bash
chmod +x ./Kraken_Explorer-*-x86_64.AppImage
./Kraken_Explorer-*-x86_64.AppImage
```

These commands have no inline comments. If you paste other commands containing `#` comments
into zsh, enable them with `setopt INTERACTIVE_COMMENTS` or use commands without comments.

### Windows installation

Run the `...-win64-setup.exe` installer, or unzip the portable `...-win64.zip` anywhere and start
`kraken-explorer.exe`.

The Windows binaries are currently **not code-signed**, so SmartScreen shows
"Windows protected your PC" on first run: click **More info → Run anyway**. The packages are
built unmodified from this repository by [GitHub Actions](.github/workflows/cmake.yml); if in
doubt, build from source instead (see `docs/windows-port.md`).

* Privacy policy: this program will not transfer any information to other networked systems
  unless specifically requested by the user or the person installing or operating it. The REST
  API (`--api PORT`) listens on 127.0.0.1 only and only when started with that flag.

## Build from source

One-liner (installs build dependencies, builds a `.deb` and installs it):

```bash
git clone https://github.com/stoffej/kraken-explorer && cd kraken-explorer && scripts/install.sh
```

By hand, build and run in place:

```bash
cmake -S . -B build -G Ninja && cmake --build build --target kraken-explorer
build/src/kraken-explorer
```

Or, after building, create and install a `.deb`:

```bash
scripts/build_deb.sh && sudo dpkg -i build/deb/kraken-explorer_*.deb
```

Running the binary without the `.deb`? SocketCAN link control needs the polkit rule, one line to install (log out and back in once for the group; re-run the `cp` after pulling, the rule only matches the `ip link` command lines of the matching build):

```bash
sudo cp packaging/10-kraken-explorer-socketcan.rules /usr/share/polkit-1/rules.d/ && sudo usermod -aG netdev $USER
```

Interfaces, features, dependencies and permissions: [docs/manual.md](docs/manual.md).
