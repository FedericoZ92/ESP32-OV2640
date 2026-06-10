# Stream Latency and Stability Improvements

This document describes the changes introduced to reduce camera stream delay, eliminate burst-then-freeze behavior, and improve frame freshness in the browser view.

## Problem Summary

Observed behavior:
- Browser stream appeared several seconds behind LED state changes.
- Refresh cadence was irregular (brief fast updates followed by freezes).
- Aggressive client polling (100 ms) increased stalls instead of improving smoothness.

## Root Causes Identified

1. Producer and consumer contention on the same frame lock.
- Capture and HTTP send path competed for shared access to one frame buffer.
- HTTP send can hold the path long enough to delay producer updates.

2. Request pacing was too aggressive for variable processing/network load.
- Fixed 100 ms browser polling can create request pressure and response jitter.

3. Frame publication happened after expensive work.
- Inference and preprocessing time pushed visible frame freshness farther behind.

4. High log volume increased runtime jitter.
- Verbose and frequent per-frame logs on an embedded target can cause scheduling jitter.

5. Capture loop scheduling issue.
- Missing end-of-loop yield created periods of task starvation and unstable cadence.

## What Was Changed

### 1) Browser polling changed to adaptive pacing
File: main/http-server/http-server.cpp

- Added adaptive pacing logic based on measured request round-trip time.
- Added minimum delay floor and target period to avoid over-polling.
- Added cache-busting timestamp query and explicit no-store fetch mode.

Result:
- Reduces request pileups under load.
- Keeps frame requests near the device processing capacity.

### 2) HTTP callback changed from blocking/waiting behavior
File: main/main.cpp

- Removed long blocking waits on frame sharing path.
- Reworked callback to avoid lock-wait-induced stalls in the serving path.

Result:
- Reduces freeze windows caused by lock contention during response send.

### 3) Frame sharing redesigned to double buffering
File: main/main.cpp

- Replaced single shared frame plus mutex contention with a two-buffer publish model.
- Producer writes into inactive buffer, then atomically swaps active index.
- HTTP serves from stable active snapshot without blocking producer writes.

Result:
- Greatly lowers producer/consumer interference.
- Improves smoothness and reduces burst-then-freeze behavior.

### 4) Frame publication moved earlier
File: main/main.cpp

- Latest grayscale frame is published before inference path.

Result:
- Browser freshness improves even when inference timing varies.

### 5) Capture loop and logging tuned
File: main/main.cpp

- Added small loop yield to avoid CPU hog behavior.
- Reduced overly chatty per-frame logging and global verbosity level.

Result:
- Better task fairness for Wi-Fi/HTTP stack.
- Lower jitter in refresh cadence.

## Expected User-Visible Impact

- Lower apparent delay between scene changes and browser frame updates.
- Smoother stream cadence with fewer multi-second freezes.
- More stable behavior at moderate polling rates.

## Recommended Runtime Settings

- Browser target period: about 200 to 300 ms.
- Keep adaptive pacing enabled rather than forcing very low fixed intervals.
- If needed, further decouple inference by running it every N frames.

## Quick Validation Checklist

1. Build and flash firmware.
2. Open the stream page and observe for at least 2 to 3 minutes.
3. Verify no long freeze bursts under normal activity.
4. Compare perceived latency versus LED state changes.
5. If occasional stutter remains, reduce inference frequency (every 2nd or 3rd frame).
