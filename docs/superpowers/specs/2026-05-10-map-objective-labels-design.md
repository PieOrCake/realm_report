# Design: Map Objective Labels

**Date:** 2026-05-10

## Goal

Draw a short text label below every objective icon on the WvW battlefield map, visible at all zoom levels, so players can identify objectives without hovering.

## Abbreviation Strategy

A hardcoded lookup table (`std::unordered_map<std::string, std::string>`) keyed by objective ID provides community-standard labels for well-known objectives. Everything else falls back to the first word of `obj.name`.

### Hardcoded table

| Objective ID | Full name | Label |
|---|---|---|
| 38-9 | Stonemist Castle | SMC |
| 38-18 | Anzalias Pass | ANZ |
| 96-37 | Garrison (Blue BL) | Garri |
| 95-37 | Garrison (Green BL) | Garri |
| 96-32 | Askalion Hills | Hills |
| 95-32 | Shadaran Hills | Hills |
| 96-33 | Ascension Bay | Bay |
| 95-33 | Dreadfall Bay | Bay |
| 1099-106 | Blistering Undercroft | Fire Keep |

### First-word fallback

Split `obj.name` at the first space character and take the leading token. Examples: "Overlook", "Valley", "Lowlands", "Klovan", "Osprey's", "Stoic", "Pangloss".

## Rendering

All label drawing happens inside `MapWindow::RenderObjectives` in `src/MapWindow.cpp`, immediately after the existing icon shape and flip-ring code for each objective.

Steps per objective:
1. Resolve label: check hardcoded table by `obj.id`; if not found, extract first word of `obj.name`.
2. Compute screen position: `label_pos.x = sp.x - textSize.x * 0.5f`, `label_pos.y = sp.y + r + 3.f`.
3. Draw background rect: `dl->AddRectFilled` with `IM_COL32(0, 0, 0, 160)` padded 2px around the text, with no rounding.
4. Draw text: `dl->AddText(label_pos, IM_COL32(255, 255, 255, 230), label.c_str())`.

No zoom gating — labels render at every zoom level. No new user-facing setting.

## Scope

- Modify: `src/MapWindow.cpp` only (add table, add label draw call).
- No changes to header, API layer, tile cache, or settings.
