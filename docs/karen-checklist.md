# Karen Verification Checklist

## Pre-Commit Verification

Before any commit that touches UI code, verify:

### 1. Build Status
```bash
cd /home/yetisoldier/.openclaw/workspace/t5-ereader-firmware
pio run
```
- [ ] Build passes
- [ ] RAM ≤ 60%
- [ ] Flash ≤ 45%

### 2. Layout Overflow Check

**Screen dimensions:**
- Total: 960px (PORTRAIT_H)
- Header: 66px
- Footer: 50px
- Margins: 32px (16px × 2)
- **Available for content: 812px**

**For each screen with rows/items:**
```python
# Calculate if content fits
available = 960 - 66 - 50 - 32  # 812px
rows_needed = row_count * row_height
if rows_needed > available:
    FAIL: overflow by {rows_needed - available}px
```

**Checklist:**
- [ ] Settings: Each page fits (currently 8 rows Reading, 7 rows Device)
- [ ] Reader Menu: 6 items max (Back, Go to, TOC, Bookmarks, Settings, Library)
- [ ] TOC/Bookmarks: Scrolling implemented for >10 items
- [ ] Library Poster: 2 rows max, pagination implemented
- [ ] Library List: Scrolling implemented for >7 items

### 3. Touch Zone Sizes

**Minimum recommended:** 44px for comfortable touch
- [ ] All menu items ≥ 44px height
- [ ] All touch zones don't overlap
- [ ] Footer buttons are properly sized

### 4. Text Truncation

- [ ] Long titles are truncated with "..."
- [ ] Author names are truncated
- [ ] Filenames are truncated
- [ ] No text extends past screen edges

### 5. State Machine

- [ ] All AppState values handled in touch handlers
- [ ] Navigation between screens works
- [ ] Back button returns to correct previous state

## Memory Checklist

### 6. Glyph Cache
- [ ] Glyph cache initialized on startup
- [ ] Font change invalidates cache
- [ ] No memory leaks in glyph cache

### 7. Preferences
- [ ] Settings saved on exit/timeout
- [ ] Settings loaded on startup
- [ ] Defaults applied when missing

## Functional Checklist

### 8. Light Sleep
- [ ] GPIO wakeup configured correctly
- [ ] WiFi/OTA/OPDS excluded from light sleep
- [ ] Touch hold handled (no accidental sleep)
- [ ] Boot settling period respected

### 9. Reading Statistics
- [ ] Page time recorded on turn
- [ ] Rolling average bounded (10 samples)
- [ ] Time estimates displayed correctly

### 10. Navigation History
- [ ] History pushed on TOC jump
- [ ] History pushed on bookmark jump
- [ ] History pushed on Go To jump
- [ ] History bounded at 10 entries
- [ ] History cleared on book close
- [ ] "Back" only shown when history exists

## Code Quality

### 11. Includes
- [ ] No unused includes
- [ ] Forward declarations where needed

### 12. Error Handling
- [ ] SD card errors handled gracefully
- [ ] File not found handled
- [ ] Memory allocation failures handled

---

## Quick Reference: Screen Fit Calculations

```python
# Settings Page 1 (Reading)
rows_p1 = 8
SETTINGS_ROW_H = 58
needed_p1 = 464  # fits in 812px ✅

# Settings Page 2 (Device)
rows_p2 = 7
needed_p2 = 406  # fits in 812px ✅

# Reader Menu
MENU_ITEM_H = 74
max_items = 6
menu_needed = 444  # fits in 709px (menu bottom) ✅

# TOC/Bookmarks
max_visible = (960 - 82 - 50 - 16) // 74  # = 10 items

# Library Poster
poster_h = 310
gap = 14
rows_visible = (960 - 132 - 50 - 16) // (310 + 14)  # = 2 rows
```