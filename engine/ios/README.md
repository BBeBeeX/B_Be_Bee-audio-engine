# BBeBee Audio Engine — iOS & Apple Platform Bridge

## Overview

The `engine/ios/` module provides Objective-C++ bridging (`BBeBeeAudioBridge`) for iOS and Apple platforms. It wraps the C++ audio engine, `libmpv` CoreAudio/AudioUnit output, and the lock-free PCM tap ring buffer with realtime FFT spectrum calculations.

## Architecture & Integration

- **Language**: Objective-C++ (`.mm`) consuming pure C++17 headers (`pcm_ring_buffer.hpp`, `fft.hpp`, `mpv_client.h`).
- **Target Distribution**: Packaged as an `xcframework` or compiled directly into React Native iOS Native Modules / Swift apps.
- **Audio Output**: Uses iOS `audiounit` / `coreaudio` backends within `libmpv`.
- **Concurrency**:
  - The real-time CoreAudio callback thread feeds PCM audio samples into `PcmRingBuffer` with zero locks and zero allocations.
  - The UI/CADisplayLink thread invokes `-getFrequencyData:timeDomainData:count:` to obtain smoothed FFT magnitudes for visualizer rendering.

## Interface Summary

```objc
#import "BBeBeeAudioBridge.h"

NSError *error = nil;
BBeBeeAudioBridge *bridge = [[BBeBeeAudioBridge alloc] initWithConfigDirectory:nil error:&error];

// Load & Play
[bridge loadURI:@"https://example.com/audio.flac" error:&error];
[bridge play];

// Visualizer Polling (CADisplayLink / 60 FPS)
uint8_t freqData[64];
uint8_t timeData[64];
if ([bridge getFrequencyData:freqData timeDomainData:timeData count:64]) {
    // Render visualizer bars...
}

// Cleanup
[bridge destroy];
```
