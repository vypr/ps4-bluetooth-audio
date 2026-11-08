# PS4 Bluetooth Audio — V14 (HDMI-Unlock)

Stream PS4 game audio **directly to Bluetooth headphones paired with the PS4**, without a PC or network-audio relay. This repository contains the **V14 payload-only build**, its source code, tests and deployment tools.

> **Compatibility:** Tested with a PS4 on firmware **14.00** and GoldHEN. Other firmware versions may work, but have not been verified for this V14 build. This is an experimental RAM payload, not a standalone PS4 PKG.

## Features

- Bluetooth Classic / A2DP headphone audio using the PS4's internal Bluetooth hardware.
- Headphone connection and audio controls through a local web interface on **port 8097**.
- Adjustable audio buffering, sender polling and volume (up to 200% software gain).
- Two-second test tone, diagnostics and HDMI/TV sound controls.
- V14 improvement: performs periodic HDMI mute keepalive operations **outside the sender's shared audio mutex**, reducing a potential cause of audio interruptions.
- Runs in RAM; this repository does **not** install or replace a home-screen application.

## Important limitation: wireless controller interference

On the tested setup, audio works with V14, but a **DualShock 4 connected via Bluetooth causes heavy crackling**. Switching the controller's communication method to **USB** makes headphone audio much cleaner. V14 does not eliminate that shared-Bluetooth coexistence problem.

The tuning values that worked best during testing were:

| Setting | Recommended starting value |
| --- | --- |
| Sender wake interval | **750 µs** |
| Audio buffer / cushion | **16 ms** |
| Capture margin | **0 ms** |
| High-compatibility stereo | **On** |
| Volume | **100%** initially; increase if needed |

**Note:** The original V14 binary's startup wake-interval default is **500 µs**. Select **750 µs** in the runtime controls for the recommended setting. Disabling high-compatibility stereo produced silence in the tested setup.

## Requirements

- Jailbroken PS4 running GoldHEN (V14 tested on FW 14.00).
- GoldHEN **Payload Server / BinLoader on port 9090**.
- GoldHEN **FTP server on port 2121** for the deployment helper and diagnostic logs.
- A PC running Python 3 on the same local network.
- Bluetooth headphones paired or discoverable by the PS4.

## Download and load the payload

Download **`BTA-Audio-FW14-V14-HDMI-UNLOCK.bin`** from the repository or release assets, then from your PC run:

```bash
python3 tools/deploy_variant.py BTA-Audio-FW14-V14-HDMI-UNLOCK.bin --host YOUR_PS4_IP
```

For example, replace `YOUR_PS4_IP` with your PS4's local IP address. The helper requests the existing Bluetooth Audio worker to stop, then sends V14 to GoldHEN's payload server. It does **not** overwrite your installed boot payload.

Open the Bluetooth Audio control interface at:

```text
http://YOUR_PS4_IP:8097/
```

LAN control may require enabling phone control and pairing through the console. The service's own PS4 loopback address is `http://127.0.0.1:8097/`.

To verify the running build from the console's FTP logs:

```bash
curl -fsS ftp://YOUR_PS4_IP:2121/data/bta_audio/service.log | grep 'FW14 V14 build=' | tail -1
```

To collect runtime status:

```bash
curl -fsS ftp://YOUR_PS4_IP:2121/data/bta_audio/status.json
```

**Safety:** Test in RAM before configuring automatic loading. If the console becomes unstable, restart it and avoid automatically loading the problematic payload.

## Compile from source (Linux / WSL)

The **payload** build requires Clang, LLD, Python 3 and a C compiler for tests. It does not require the OpenOrbis PKG toolchain.

```bash
sudo apt update
sudo apt install -y clang lld build-essential python3

# Run the V14 host test suite
make test

# Rebuild the exact release binary
make payload V7_SPACEBUDS_RECOVER=1 V8_FREEPODS_GAIN_PCT=130 V8_PACE_CATCHUP=0

# Confirm that the locally built binary matches the included V14 release payload
make verify V7_SPACEBUDS_RECOVER=1 V8_FREEPODS_GAIN_PCT=130 V8_PACE_CATCHUP=0
```

The generated payload is `build/BTA-Audio.bin`. On the source snapshot used for this release, the tests pass **9,295 checks** and the rebuilt binary is **byte-identical** to the bundled V14 payload.

**PKG note:** The original upstream build included a separate OpenOrbis home-screen application. Its `native/` directory is not part of this V14 source snapshot, so this repository deliberately provides a **payload-only build**, not `make pkg`.

## Repository files

| Path | Contents |
| --- | --- |
| `app/` | Bluetooth audio payload, HDMI-unlock logic and port 8097 control page |
| `app/v14_hdmi.h` | V14 HDMI mute diagnostics and helpers |
| `tests/` | Host-side module and behavior tests |
| `tools/test-v14-core.sh` | Supported V14 host-test runner |
| `tools/deploy_variant.py` | RAM-only deployment helper |
| `tools/install_bta.py` | Optional GoldHEN AutoRun installer; use only after testing |
| `BTA-Audio-FW14-V14-HDMI-UNLOCK.bin` | Exact verified V14 release payload |
| `SHA256SUMS` | Binary integrity checksum |
| `Makefile` | Payload compilation, tests and reproducibility check |
| `LICENSE` | MIT copyright and license notice |

## Donations

If you find PS4 Bluetooth Audio useful and want to support continued development, donations are optional and appreciated.

| Cryptocurrency | Network | Receiving address |
| --- | --- | --- |
| **Bitcoin (BTC)** | Bitcoin (BTC) | `16hghGSssvfd18ggW4ZvpKuzxgpaQY2w3u` |
| **USDT** | TRON (TRC20) | `TLq7Vfuos49WEUSBiKeYEKwhBBdmrJRpqZ` |
| **Ethereum (ETH)** | Ethereum mainnet | `0x6fdd52a2b6105af212e77e5ed5b5dd8733e02a0b` |
| **Litecoin (LTC)** | Litecoin | `LYpn7yZyhBLTfpS8YAje5dwCun2xpU63nE` |

**Check the network and address carefully before sending.** In particular, send USDT **only over TRON (TRC20)** to the listed USDT address; transferring on an incompatible network can cause permanent loss. Some exchanges may impose minimum deposit amounts.

## License and credits

MIT licensed. Portions of the code originate from [PS4 Bluetooth Audio](https://github.com/XB-01/ps4-bt-audio); the original copyright notice and terms are retained in [`LICENSE`](LICENSE).

## Notes

- This project is **Bluetooth headphone audio**, **not** the separate Web Audio / LAN browser-streaming application.
- Microphone and headset voice-chat input are not provided.
- Firmware compatibility and device behavior depend on the console's Bluetooth and kernel interfaces; use at your own risk.
