# VortiDeck Banner Manager - CEF/obs-browser.dll Crash Root Cause Analysis

## Executive Summary

The banner_manager implementation triggers CEF (Chromium Embedded Framework) crashes and obs-browser.dll hangs during OBS shutdown due to multiple critical issues with how it manages shared browser sources across scenes.

---

## Root Causes Identified

### 1. CRITICAL: Rapid Toggle Pattern with obs_source_set_enabled

**Location**: banner_manager.cpp lines 1127-1128, 1457-1458

**The Code**:
```cpp
obs_source_update(m_banner_source, settings);
obs_source_set_enabled(m_banner_source, false);
obs_source_set_enabled(m_banner_source, true);
```

**Why It's Catastrophic**:

- `obs_source_set_enabled(false)` triggers CEF subprocess shutdown
- `obs_source_set_enabled(true)` IMMEDIATELY triggers CEF restart (no wait/cleanup)
- This rapid cycle causes CEF's internal state machine to collide with cleanup operations
- Results in corrupted CEF state, use-after-free, or double-free conditions
- CEF is NOT designed for rapid enable/disable cycles on the same instance

**Impact**: Every content update triggers this toggle, corrupting CEF with each banner change.

---

### 2. CRITICAL: Shared Source in Multiple Scenes Without Proper Lifecycle Management

**Location**: banner_manager.cpp lines 1593, 1669, 2561, 2586

**The Pattern**:
```cpp
// SAME m_banner_source added to MULTIPLE scenes
obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
```

**Why It's Problematic**:

- Single `m_banner_source` object (one pointer) added to multiple scenes
- Each `obs_scene_add()` creates a scene item referencing the SAME source
- Browser source spawns ONE CEF subprocess per obs_source_t
- Multiple scene items = multiple references to one CEF process
- When one scene item is updated, ALL references are affected
- CEF subprocess becomes confused about rendering state across multiple targets

**The Deadlock**:
- During shutdown, scenes being destroyed hold references to m_banner_source
- obs_source_release() can't cleanup CEF until all references are gone
- But scenes can't be destroyed until source cleanup completes
- Circular dependency = DEADLOCK

**Impact**: Free users get banners in ALL scenes, maximizing reference count. Shutdown hangs waiting for CEF cleanup.

---

### 3. CRITICAL: Free User Forced Persistence Model

**Location**: Multiple functions enforce forced banner across ALL scenes for free users

**Why It Amplifies Problems**:

- Free users get banners in EVERY scene
- If OBS has 10 scenes, same m_banner_source referenced 10 times
- Multiplies the reference counting problem by scene count
- Multiplies the toggle corruption by number of content updates
- Constant enforcement through signal handlers triggers restoration cycles

**Impact**: Maximum damage scenario - most problematic reference count combined with most toggle cycles.

---

### 4. CRITICAL: Inadequate Shutdown Sequence

**Location**: obs_plugin.cpp lines 206-213 and banner_manager.cpp lines 170-213

**The Issues**:

1. **Incomplete Item Cleanup**
```cpp
void banner_manager::remove_banner_from_scenes() {
    // Only sets visible=false, does NOT remove items
    obs_sceneitem_set_visible(item, false);
}
```
- Scene items continue to exist and hold references to m_banner_source
- When OBS destroys scenes during shutdown, items still hold references
- CEF subprocess doesn't receive proper cleanup signals

2. **Race Condition During Shutdown**
- disconnect_scene_signals() disconnects future signals
- But in-flight signal handlers continue executing in other threads
- Cleanup races with scene destruction
- No synchronization wait for in-flight handlers to complete

3. **CEF Subprocess State**
- During shutdown, CEF is in corrupted state (from toggle cycles)
- OBS calls obs_source_release() expecting clean shutdown
- CEF subprocess can't cleanup because state is corrupted
- Unload hangs waiting for subprocess to exit

---

### 5. ARCHITECTURAL ISSUE: Browser Source Anti-Pattern

**Why Browser Sources Shouldn't Be Shared**:

Browser sources are STATEFUL:
- Own a CEF subprocess with threads, GPU contexts, rendering pipeline
- CEF process lifecycle is SEPARATE from OBS scene hierarchy
- Design expects 1:1 mapping: one source per rendering context

Banner Manager uses them incorrectly:
- Single source shared across multiple scenes
- Multiple rendering targets per CEF process (unsupported)
- Results in rendering state confusion and resource leaks

---

## Why Normal Browser Sources Don't Crash

**Normal Usage**:
1. Create source
2. Add to ONE scene (or limited reuse)
3. Modify through OBS UI (no toggle cycles)
4. User closes scene
5. OBS destroys scene → removes item → releases source
6. CEF subprocess exits cleanly

**Banner Manager Usage**:
1. Create source
2. Add to MULTIPLE scenes (2, 3, 10+ references)
3. Toggle enable/disable on every content update (CEF corruption)
4. Forced persistence prevents removal (constant references)
5. During shutdown: multiple items hold references
6. CEF subprocess can't cleanup from corrupted state
7. DEADLOCK

---

## Technical Evidence From Code

### Evidence 1: Rapid Toggle Pattern
```cpp
// banner_manager.cpp:1127-1128
obs_source_update(m_banner_source, settings);
obs_source_set_enabled(m_banner_source, false);
obs_source_set_enabled(m_banner_source, true);
```
No sleep, no wait, no cleanup between disable/enable. CEF state machine collision is guaranteed.

### Evidence 2: Multiple Scene Adds
```cpp
// banner_manager.cpp:1669
for (size_t i = 0; i < scenes.sources.num; i++) {
    obs_sceneitem_t* scene_item = obs_scene_add(scene, m_banner_source);
}
```
Same m_banner_source added to every scene in loop.

### Evidence 3: Incomplete Cleanup
```cpp
// banner_manager.cpp:1704
obs_sceneitem_set_visible(item, false);  // Only hides
// Missing: obs_sceneitem_remove(item);
```
Items continue holding references to source.

### Evidence 4: Double Cleanup Attempt
- `obs_plugin.cpp:212`: remove_banner_from_scenes()
- `banner_manager.cpp:183` (in shutdown()): remove_banner_from_scenes()

Called twice but only hides items, doesn't actually remove them.

---

## Shutdown Failure Timeline

```
TIME 1: OBS EXIT event fires
        disconnect_scene_signals() + remove_banner_from_scenes()

TIME 2: m_banner_manager.shutdown() called
        Tries obs_source_release()

TIME 3: CEF subprocess cleanup starts
        But CEF is corrupted from toggle cycles
        Cleanup stalls, subprocess hangs

TIME 4: OBS destroys scenes
        Each scene tries to destroy items
        Items still hold references to source

TIME 5: DEADLOCK
        OBS waiting for scenes to destroy
        Scenes waiting for source cleanup
        Source waiting for items to release
        All waiting on corrupted CEF subprocess
```

---

## Summary

### CRITICAL Issues (Must Fix)
1. **Rapid toggle pattern** (obs_source_set_enabled false→true)
2. **Shared source across scenes** (same m_banner_source → multiple obs_scene_add)
3. **Incomplete item cleanup** (hide but don't remove scene items)

### HIGH Issues (Must Fix)
4. **Inadequate shutdown synchronization** (race with in-flight handlers)
5. **Free user forced persistence** (amplifies problems across all scenes)

### Root Problem
The banner_manager uses browser_source in ways it's not designed for:
- Rapid state cycles that corrupt CEF internals
- Multi-scene sharing that confuses CEF subprocess
- Incomplete cleanup that leaves hanging references

**All three issues must be fixed to prevent crashes.**

