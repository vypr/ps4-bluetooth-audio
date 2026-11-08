# PS4 Bluetooth Audio — V14 HDMI-Unlock

**Build:** V14 HDMI-Unlock · **Tested firmware:** PS4 14.00 (GoldHEN)

### What's included
- Exact working V14 Bluetooth-audio payload (`BTA-Audio-FW14-V14-HDMI-UNLOCK.bin`).
- Complete V14 *payload* source and reproducible build instructions.
- Bluetooth control webpage on port **8097**, volume boost, latency controls and test tone.
- V14 HDMI mute keepalive moved outside the audio-state mutex, with timing diagnostics.

### Tested configuration
- Controller via **USB communication** for dramatically cleaner playback.
- High-compatibility stereo **On**.
- Runtime wake interval **750 µs**, buffer **16 ms**, margin **0 ms**.
- Wireless DualShock 4 Bluetooth communication still causes heavy crackling on the tested PS4.

### Verification
- V14 host tests: **9,295 passed, 0 failures**.
- Rebuilt `build/BTA-Audio.bin` is **byte-identical** to the included payload.
- Other PS4 firmware and headphone combinations are not verified.

### Loading
Enable GoldHEN Payload Server (9090) and FTP (2121), then:

```bash
python3 tools/deploy_variant.py BTA-Audio-FW14-V14-HDMI-UNLOCK.bin --host YOUR_PS4_IP
```

For LAN controls, visit `http://YOUR_PS4_IP:8097/`. The binary runs in RAM and does not install a PS4 home-screen application.

**Important:** This is the *Bluetooth Audio* project, not the Web Audio network-streaming project. No `.pkg` is included in this payload-only release.
