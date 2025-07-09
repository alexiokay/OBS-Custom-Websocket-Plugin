# Banner/Ad Window Closing Issue Fix - Debug Checklist

## Problem Fixed
The banner/ad window wasn't closing properly after the duration timer expired. Banners were being auto-restored after 5 seconds even when they should have stayed hidden.

## Root Cause
When a banner's duration timer expired, it called `hide_banner()` which has auto-restore logic for free users:
1. Duration timer expires → calls `hide_banner()` 
2. `hide_banner()` for free users → hides banner but sets 5-second auto-restore timer
3. After 5 seconds → checks if enough time passed since last ad
4. If enough time passed → **automatically restores the banner!** 🤦‍♂️

## Changes Made
1. **Fixed Duration Timer Logic** - Duration timer now bypasses `hide_banner()` auto-restore
2. **Timing Fix** - Record ad end time BEFORE hiding to prevent auto-restore
3. **Direct Banner Removal** - Call `remove_banner_from_scenes()` directly instead of `hide_banner()`
4. **Explicit State Setting** - Set `m_banner_visible = false` to prevent confusion

## Test Procedure

### 1. Banner Duration Test (Main Issue)
- [ ] Start OBS Studio and load the VortiDeck plugin
- [ ] Create a banner with a specific duration (e.g., 30 seconds)
- [ ] Display the banner
- [ ] Wait for the duration to expire
- [ ] **VERIFY**: Banner disappears and STAYS hidden (no auto-restore after 5 seconds)

### 2. Manual Hide Test (Should Still Work)
- [ ] Show a banner manually
- [ ] Hide it manually using the hide function
- [ ] **VERIFY**: For free users, banner should auto-restore after 5 seconds (this is expected behavior)
- [ ] **VERIFY**: For premium users, banner should stay hidden until manually shown

### 3. Ad Rotation Test
- [ ] Set up multiple banners in a rotation queue
- [ ] Let them rotate through several cycles
- [ ] **VERIFY**: Each banner shows for its specified duration then disappears
- [ ] **VERIFY**: Next banner appears after the rotation interval

### 4. Frequency Control Test
- [ ] Set a banner to show for 30 seconds
- [ ] Wait for it to expire and disappear
- [ ] Try to show a new banner immediately
- [ ] **VERIFY**: Should respect the 30-second frequency limit for free users

## Expected Behavior
✅ Banner appears for specified duration
✅ Banner disappears when duration expires
✅ Banner STAYS hidden (no auto-restore)
✅ Next banner can appear after frequency limit
✅ Manual hide still works as expected

## If Issue Persists
Check these areas:
1. Duration timer implementation in `start_duration_timer()`
2. Auto-restore logic in `hide_banner()` for free users
3. `m_last_ad_end_time` timing and frequency control
4. `remove_banner_from_scenes()` function effectiveness

## Technical Details
- **Root Cause**: Duration timer called `hide_banner()` which had 5-second auto-restore logic
- **Fix**: Duration timer now directly calls `remove_banner_from_scenes()` and sets proper state
- **Key Change**: Record `m_last_ad_end_time` BEFORE hiding to prevent auto-restore timing issues
- **Prevention**: Bypass auto-restore logic for duration-based hiding

## Log Messages to Watch For
Look for these messages in OBS logs:
- `TIMER: Banner duration expired (X)s - auto-hiding`
- `TIMER: Performing permanent banner hide (no auto-restore)`
- `TIMER: Banner permanently hidden, window closed, ad end time recorded`
- `Banner removed from X scenes` (should see this when banner disappears) 