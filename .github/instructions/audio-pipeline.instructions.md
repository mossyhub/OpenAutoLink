---
description: "Use when working on audio playback, AudioTrack configuration, microphone capture, audio focus, audio routing by purpose (media/nav/call/siri/alert), or ring buffer implementation."
---
# Audio Pipeline Knowledge

## 5-Purpose Routing (Proven Pattern)
Audio from aasdk JNI carries a `purpose` field. Each purpose gets its own pre-allocated AudioTrack:

| Purpose | AudioAttributes Usage | Sample Rate | Channels | When Active |
|---------|----------------------|-------------|----------|-------------|
| Media | USAGE_MEDIA | 48000 Hz | Stereo | Music, podcasts (default) |
| Navigation | USAGE_ASSISTANCE_NAVIGATION_GUIDANCE | 16000 Hz | Mono | Turn-by-turn prompts |
| Siri/Assistant | USAGE_ASSISTANT | 16000 Hz | Mono | Voice assistant active |
| Phone Call | USAGE_VOICE_COMMUNICATION | 8000 Hz | Mono | Active call |
| Alert | USAGE_NOTIFICATION_RINGTONE | 24000 Hz | Mono | Incoming call ring, system alerts |

## AudioTrack Pre-allocation
- Create all 5 AudioTracks at session start — DO NOT create/destroy per audio event
- Use `AudioTrack.Builder` with `AudioAttributes` and `AudioFormat`
- Set `THREAD_PRIORITY_URGENT_AUDIO` on playback threads
- Buffer size: `AudioTrack.getMinBufferSize() * 2` minimum

## Ring Buffer (Jitter Absorption)
- 500ms ring buffer between TCP read thread and AudioTrack write thread
- Lock-free SPSC (single-producer single-consumer) preferred
- On underrun: write silence, don't block. Log underrun count for diagnostics
- On overflow: drop oldest samples (not newest)

## Audio Focus
- Request focus BEFORE writing to AudioTrack
- `AUDIOFOCUS_GAIN` for media, `AUDIOFOCUS_GAIN_TRANSIENT_MAY_DUCK` for nav/alerts
- On focus loss: pause AudioTrack, don't release. Resume on focus regain
- Navigation audio ducks media (half volume) — AAOS handles this via AudioAttributes

## Microphone Capture
- Timer-based sampling at 40ms intervals (~25 Hz)
- Sample rate: 8000 Hz for Siri, 16000 Hz for calls (aasdk tells app via control message)
- 512-sample circular buffer
- Send on audio TCP channel (direction=1 in header)
- Start on aasdk `mic_start` control message, stop on `mic_stop`
- RECORD_AUDIO permission required — handle denial gracefully

## Audio Rules
- Audio is a CONTINUOUS TIME SIGNAL that must never stall
- Buffer aggressively — opposite of video
- Never block the video pipeline waiting on audio
- AudioTrack.write() can block — always on dedicated thread, never on TCP read thread
