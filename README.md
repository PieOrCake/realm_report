# Realm Report

A Guild Wars 2 addon for [Raidcore Nexus](https://raidcore.gg/Nexus) that provides a live WvW (World vs World) objective tracker and battlefield map overlay. See the state of every camp, tower, keep, and castle across all four WvW maps at a glance — right inside the game.

![Realm Report](screenshots/main.png)

## ⚠️ Upcoming Change Notice

The **next** release of Realm Report will read Guild Wars 2's memory to show live WvW objective ownership and upgrade tiers, and to detect which guild has claimed each objective along with its slotted tactics and improvements — faster and more reliably than the public API allows.

If you are not comfortable with an addon reading game memory, please **uninstall Realm Report or disable auto-updates** before the next release.

## AI Notice

This addon has been created largely using Claude. I understand that some folks have a moral, financial or political objection to creating software using an LLM. I just wanted to make a useful tool for the GW2 community, and this was the only way I could do it.

If an LLM creating software upsets you, then perhaps this repo isn't for you. Move on, and enjoy your day.

## Features

### Objective Tracker
- **Live WvW objective tracking** across all four maps
- **World selection** — searchable list of all GW2 worlds
- **Color-coded team ownership** — Red, Blue, Green at a glance
- **Sortable, filterable table** — by map, name, type, owner, flip time, upgrade tier, claimed guild, or PPT
- **Scoreboard** — current match scores for all three teams
- **Upgrade tier display** — T0–T3 with exact yak tooltip
- **Guild claim display** — shows which guild has claimed each objective
- **Pinned objectives** — pin the ones you care about to the top
- **Nearest waypoint** — right-click an objective to copy its nearest waypoint chat link
- **Stale data indicators** — maps flag when data goes quiet

### WvW Battlefield Map
- **Live map overlay** — a floating window toggled with **Alt+M** or the toolbar button
- **Real GW2 map tiles** — cached to disk across sessions
- **Four map tabs** — Eternal Battlegrounds and the three Borderlands
- **Objective icons** — team-coloured shapes with community short names (SMC, Garri, etc.)
- **Flip ring** — gold pulsing ring on recently flipped objectives
- **Player position** — your location on the correct map tab
- **Pan and zoom**, with per-objective tooltips

### Flip Notifications
- **Toast notifications** when any objective changes hands
- **Configurable sound** — play a WAV or MP3 on each flip
- **Configurable position and duration**
- **Auto-paused** when you leave WvW

### General
- **GW2 theme** — dark interface with gold accents
- **Persistent settings** saved between sessions
- **Quick access icon** in the Nexus toolbar

## Installation

1. Install [Raidcore Nexus](https://raidcore.gg/Nexus) if you haven't already.
2. Download **`RealmReport.dll`** from the [latest release](https://github.com/PieOrCake/realm_report/releases/latest).
3. Copy it into `Guild Wars 2/addons/`.
4. Launch the game — the addon appears in the Nexus library.

## Usage

- Press **ALT+SHIFT+W** to toggle the objective tracker (or click the Realm Report icon in the Nexus Quick Access bar).
- Press **Alt+M** to toggle the battlefield map.
- Select your world from the dropdown.
- Right-click any objective to pin it or copy its nearest waypoint.

## License

Realm Report is closed source. See [LICENSE](LICENSE). You may download and use the addon free of charge, including for streaming and content creation. The source code is not public — see the license for why, and for the access ArenaNet developers may request.

## Third-Party Notices

The compiled addon uses the following open-source libraries:

- [Dear ImGui](https://github.com/ocornut/imgui) — MIT License, Copyright (c) 2014-2021 Omar Cornut
- [nlohmann/json](https://github.com/nlohmann/json) — MIT License, Copyright (c) 2013-2025 Niels Lohmann
- [Nexus API](https://raidcore.gg/Nexus) — MIT License, Copyright (c) Raidcore.GG

---

Realm Report is an unofficial, fan-made addon. Guild Wars 2 and ArenaNet are trademarks of ArenaNet, LLC and NCSOFT Corporation. Realm Report is not affiliated with, endorsed by, or sponsored by ArenaNet or NCSOFT.
