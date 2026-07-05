# VRC Downbeat

Desktop app that lets bands jam together over the internet with
sample-accurate sync - and stream the mix straight into VRChat.

**Download the ready-to-run Windows build from the official site:**
<https://vrcdownbeat.vercel.app>

## How it works

Real-time online jamming normally fails because of network latency. VRC
Downbeat sidesteps the problem instead of fighting it:

- Every musician plays along to a **local backing track** (per-stem mix, e.g.
  mute your own instrument's stem).
- Their recording is streamed to the host **tagged from sample 0** of the
  backing track.
- The host starts playback only once a few seconds are buffered from
  everyone, then plays the backing track and all musician streams in perfect,
  sample-accurate lockstep.

Network latency becomes irrelevant; only the musician's device latency is
compensated (DAW principle - reported by ASIO, plus a manual offset).

One app, two modes:

- **Host** - song library (songs with stems), jam control, live mix of all
  musicians, multi-stem recording with MP3 export.
- **Musician** - connect with a room code (or IP), download songs, mix stems
  locally, play along; VST3 instruments (e.g. drum samplers) with MIDI input
  are hosted in-app.

## Features

- Room codes via a relay server: no port forwarding, no IP sharing on either
  side (direct IP+port connections remain available as a fallback)
- **End-to-end encryption** (X25519 + XChaCha20-Poly1305 via
  [Monocypher](https://monocypher.org)) - the relay only ever pipes ciphertext
- Stream the mix into VRChat through a virtual microphone (VB-CABLE, with
  in-app installer) with a per-source routing board ("You" / "VRChat Mic"
  per app, backing track, instrument and talk mic)
- Band text chat + voice chat with a dedicated talk mic
- Multi-stem jam recordings: remix stems afterwards and export to MP3; hosts
  can send recordings to musicians in-app
- ASIO, WASAPI and DirectSound; VST3 plugin hosting with MIDI

## Building from source (Windows)

Requirements: Visual Studio (MSVC, C++ workload), CMake 3.24+, Ninja.

```
tools\configure.cmd    (once; fetches JUCE 8 via FetchContent)
tools\build.cmd        Debug build   -> build\BandJam_artefacts\Debug
tools\release.cmd      Release build -> build-release\BandJam_artefacts\Release
```

The Release exe is fully standalone (static MSVC runtime) - copy it anywhere.

## Relay server

`relay/RelayServer.cpp` is the standalone room relay (Linux, no dependencies):

```
g++ -O2 -std=c++17 -pthread -o bandjam-relay RelayServer.cpp
./bandjam-relay [port] [-v]        # default port 47900
```

`relay/bandjam-relay.service` is a ready-made systemd unit. The relay pairs
host and musicians by room code and pipes bytes; it never sees plaintext
traffic and its logs contain display names only - no IP addresses.

To run your own relay, change `relay::kDefaultAddress` in
`Source/common/Protocol.h` (or the `relayAddress` entry in `settings.json`).

## License

GPLv3 - see [LICENSE](LICENSE). VRC Downbeat uses [JUCE 8](https://juce.com)
under its GPLv3 option (the ASIO SDK sources ship with JUCE under the same
terms) and [Monocypher](https://monocypher.org) (public domain / BSD).
