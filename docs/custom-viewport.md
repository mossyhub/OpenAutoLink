# Custom Viewport — Display Mode Design

> **Status: Implemented.** ViewportEditorScreen with draggable edge bars (top, right), aspect ratio lock (AspectRatio/CropFree icons), preset dropdown, manual pixel input, checkerboard preview. Persisted via DataStore. ProjectionScreen sizes SurfaceView to custom rect (bottom-left anchored). Touch coordinates map naturally from the sized SurfaceView.

## Problem

AAOS head unit displays (e.g. GM Blazer EV 2914×1134) have non-standard aspect ratios. Android Auto's UI is designed for common ratios (16:9, ~21:9). When the projection fills the full display, the AA UI either stretches awkwardly or gets cropped in the wrong places.

Current approach in carlink_native: global "fullscreen" or "show system bars" toggle. No fine-grained control.

## Solution

A **Custom Viewport** display mode where the user visually defines the exact rectangle on screen that the projection occupies. Think PowerToys FancyZones, but for a single zone on the car's display.

```
┌───────────────────── 2914×1134 physical ─────────────────────┐
│ ████████████████████████████████████████████████████████████ │
│ ████████████████████████████████████████████████████████████ │
│ ████████████████████████████████████████──── drag top ██████ │
│ ┌──────────────────────────────────────┐████████████████████ │
│ │                                      │████████████████████ │
│ │     AA Projection Surface            │ drag               │
│ │     (anchored bottom-left)           │ right               │
│ │                                      │████████████████████ │
│ └──────────────────────────────────────┘████████████████████ │
└─────────────────────────────────────────────────────────────┘
  ████ = solid black fill    ⌞ = anchor point (bottom-left)
```

## UI: Viewport Editor

Activated from Settings > Display > Custom Viewport > "Edit Viewport".

### Editor Layout

```
┌──────────────────────────────────────────────────────┐
│  ┌─[Aspect Ratio: 21:9 ▼]──[Lock 🔒]──[Done ✓]─┐  │
│  │                                                │  │
│  │              ══════════════════════             │  │
│  │         ╔══════════════════════╗                │  │
│  │         ║                      ║                │  │
│  │         ║   Live Preview or    ║ ║              │  │
│  │         ║   Checkerboard       ║ ║ drag right   │  │
│  │         ║                      ║                │  │
│  │         ╚══════════════════════╝                │  │
│  │    ⌞ anchored bottom-left                      │  │
│  │                                                │  │
│  │  ══ = drag top handle    ║ = drag right handle │  │
│  └────────────────────────────────────────────────┘  │
│              W: 2100 × H: 900  (21:9)                │
└──────────────────────────────────────────────────────┘
```

### Interaction

1. **Anchor point**: bottom-left corner is fixed to the bottom-left of the AAOS-reported usable area. The viewport grows/shrinks from that corner
2. **Two draggable edges**: top handle (adjusts height) and right handle (adjusts width). Each is an invisible touch strip (~32dp wide) along the edge. Visual indicator: thin bright line or subtle glow on the active edge
3. **Drag behavior**:
   - With ratio lock ON (default): dragging the top edge adjusts width proportionally via the right edge, and vice versa. Both edges move together to maintain the locked aspect ratio
   - With ratio lock OFF: each edge moves independently. Free-form rectangle
4. **Corner drag** (optional enhancement): dragging near the top-right corner adjusts both axes simultaneously, always maintaining aspect ratio when locked
5. **Minimum size**: 800×480 (smallest useful AA resolution)
6. **Maximum size**: full AAOS usable area (what the system reports as available to the app)

### Aspect Ratio Snapping

Toggle: "Lock Aspect Ratio" (ON by default)

When ON, the viewport snaps to the nearest standard ratio:

| Ratio | Dimensions (example at 1134 height) | Use Case |
|-------|--------------------------------------|----------|
| 16:9 | 2016×1134 | Standard widescreen |
| 21:9 | 2382×1134 (or 1890×810) | Ultra-wide, good for AA |
| 32:9 | 2268×709 (or full width) | Super ultra-wide |
| 4:3 | 1512×1134 | Legacy, not ideal |
| 16:10 | 1814×1134 | Slight variation on 16:9 |
| 2:1 | 2268×1134 | Close to 18:9 phone ratio |

**Snap behavior:**
- User drags edge → compute resulting aspect ratio → snap to nearest standard ratio in the table
- Show the computed ratio and pixel dimensions in a footer label: `2100 × 900 (21:9)`
- Snapping should feel immediate (no animation delay) but the handle should "resist" slightly as it passes through a snap point (haptic feedback if available)

When OFF, free-form dimensions. Label shows `2100 × 873 (custom)`.

### Manual Dimension Input

Tapping the dimension label (`2100 × 900 (21:9)`) in the footer opens an inline edit mode:

- Two text fields: **Width** and **Height** (in pixels)
- Numeric keyboard only (`inputType = TYPE_CLASS_NUMBER`)
- When aspect ratio lock is ON: editing one field auto-calculates the other
- **Apply** button commits the values and repositions the viewport (centered on screen)
- Validation: clamp to min 800×480, max display size. Show error for out-of-range values
- Dismisses on Apply or tap-outside

This is essential because:
- Far edges of ultrawide displays (2914px wide) may be physically unreachable by finger
- Users may want exact pixel values (e.g. matching a known AA resolution like 1920×1080)
- Easier than drag-guessing for precise sizing

### Dropdown Presets

Quick-select dropdown in the editor toolbar:

- **Fill Display** — matches physical display, same as Fullscreen mode
- **16:9** — centered, max height
- **21:9** — centered, max height
- **32:9** — centered, max height
- **Custom** — current user-defined rect (editable via drag or manual input)

Selecting a preset immediately resizes the viewport and updates the handles.

## Data Model

```kotlin
data class ViewportConfig(
    val mode: DisplayMode,                  // FULLSCREEN, SYSTEM_BARS, CUSTOM
    val customWidth: Int = 0,               // pixels, 0 = use full usable width
    val customHeight: Int = 0,              // pixels, 0 = use full usable height
    val aspectRatioLocked: Boolean = true,  // default ON
    val lockedRatio: AspectRatio? = null    // null = auto-detect nearest
)
// Rect is derived: bottom-left anchored, so:
//   left = 0
//   bottom = usableHeight
//   right = customWidth
//   top = usableHeight - customHeight

enum class AspectRatio(val w: Int, val h: Int) {
    RATIO_16_9(16, 9),
    RATIO_21_9(21, 9),
    RATIO_32_9(32, 9),
    RATIO_4_3(4, 3),
    RATIO_16_10(16, 10),
    RATIO_2_1(2, 1)
}
```

Persisted via DataStore. The `customRect` is in physical pixel coordinates relative to the display.

## Integration Points

### SurfaceView Sizing
```kotlin
// In ProjectionScreen composable
when (viewportConfig.mode) {
    FULLSCREEN -> fillMaxSize()
    SYSTEM_BARS -> fillMaxSize() with WindowInsets padding
    CUSTOM -> {
        // Anchored bottom-left: align to bottom-start within the usable area
        Modifier
            .align(Alignment.BottomStart)
            .size(viewportConfig.customWidth.toDp(), viewportConfig.customHeight.toDp())
    }
}
```

### Touch Coordinate Scaling
When viewport is custom-sized, touch coordinates must be mapped from the **viewport rect** (not the full display) to the **video resolution** the bridge is sending.

Since the viewport is bottom-left anchored:
```
viewportTop = usableHeight - customHeight
viewportRight = customWidth

videoX = (screenX / customWidth) * videoWidth
videoY = ((screenY - viewportTop) / customHeight) * videoHeight
```

Touch events above `viewportTop` or right of `viewportRight` are ignored (outside the projection area).

### Black Fill
The area outside the custom viewport must be solid black. Two approaches:
1. **Activity background = black** + SurfaceView only covers the viewport rect (simpler, preferred)
2. Compose `Canvas` drawing black rects around the viewport (more flexible but more code)

Approach 1 should work — `Window.setBackgroundDrawable(ColorDrawable(Color.BLACK))` in Activity.

### MediaCodec Configuration
The MediaCodec decoder should be configured with the viewport dimensions (or the AA stream dimensions, whichever the bridge sends). The SurfaceView handles scaling from codec output to physical pixels via `SCALE_TO_FIT` or `SCALE_TO_FIT_WITH_CROPPING`.

If the viewport aspect ratio matches the stream aspect ratio (which is the whole point of ratio locking), `SCALE_TO_FIT` will fill the viewport perfectly with no letterboxing.

## Implementation Notes

### What Compose provides
- `Modifier.pointerInput` for drag gestures — use `detectDragGestures` for edge handles
- `Modifier.offset` + `Modifier.size` for positioning the SurfaceView within the custom rect
- `AndroidView` wrapping `SurfaceView` already handles pixel-level sizing

### Performance
- Viewport editor is only active during configuration (not during streaming)
- During streaming, the custom rect is just a static `Modifier.offset + size` — zero overhead
- No continuous recomposition needed for the viewport bounds

### Edge Cases
- **Display rotation**: AAOS head units don't rotate, but if they did, custom rect should be invalidated
- **Resolution change**: if the user changes AA resolution tier in settings, the viewport should remain valid (it's in physical pixels, not video pixels)
- **First-time setup**: default to FULLSCREEN mode. Custom viewport is opt-in from settings

## Milestone
This is an **M8 (Polish)** feature. It requires:
- M1 (transport) — for connection
- M2 (video) — for the SurfaceView and codec pipeline
- M4 (touch) — for coordinate scaling

It does NOT require audio, mic, settings sync, or vehicle integration.
