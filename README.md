# Realm Report

A Guild Wars 2 addon for [Raidcore Nexus](https://raidcore.gg/Nexus) that provides a live WvW (World vs World) objective tracker and battlefield map overlay. Squad commanders can see the state of every camp, tower, keep, and castle across all four WvW maps at a glance — right inside the game.

## AI Notice

This addon has been created largely using Claude. I understand that some folks have a moral, financial or political objection to creating software using an LLM. I just wanted to make a useful tool for the GW2 community, and this was the only way I could do it.

If an LLM creating software upsets you, then perhaps this repo isn't for you. Move on, and enjoy your day.

## Screenshot

![Realm Report](screenshots/main.png)

## Features

### Objective Tracker
- **Live WvW objective tracking** — polls the official GW2 API every 30 seconds (configurable)
- **World selection dropdown** — searchable/filterable list of all GW2 worlds
- **Color-coded team ownership** — Red, Blue, Green at a glance
- **Sortable table** — sort by map, name, type, owner, flip time, upgrade tier, claimed guild, or PPT
- **Map & type filters** — toggle EBG/borderlands and camps/towers/keeps/castle visibility
- **Scoreboard** — current match scores for all three teams
- **Upgrade tier display** — derived from yaks delivered (T0–T3) with exact yak tooltip
- **Guild claim display** — shows which guild has claimed each objective
- **Pinned objectives** — right-click any objective to pin it to a gold section at the top of the list
- **Nearest waypoint** — right-click any objective to copy its nearest waypoint chat link to clipboard
- **Stale data indicators** — map sections turn red if no flips have occurred in 5 minutes
- **No API key required** — WvW data is public on the GW2 API

### WvW Battlefield Map
- **Live map overlay** — separate floating window toggled with **Alt+M** or the Map button in the toolbar
- **Real GW2 map tiles** — loaded from the ArenaNet tile CDN and cached to disk across sessions
- **Four map tabs** — Eternal Battlegrounds, Blue/Green/Red Borderlands
- **Objective icons** — team-coloured shapes (circle=camp, diamond=tower, hexagon=keep/castle) with community short names (SMC, ANZ, Garri, etc.)
- **Flip ring** — gold pulsing ring on objectives flipped in the last 5 minutes
- **Player position** — yellow dot shows your location on the correct map tab
- **Pan and zoom** — drag to pan, scroll to zoom
- **Objective tooltips** — name, owner, tier, guild, and time since last flip

### Flip Notifications
- **Toast notifications** — floating alerts when any objective changes hands
- **Configurable sound** — play a WAV or MP3 on each flip
- **Configurable position and duration** — drag toasts to any screen corner
- **Auto-paused off-map** — notifications and polling pause when you leave WvW

### General
- **GW2 theme** — dark interface with gold accents to match the game aesthetic
- **Persistent settings** — world, poll interval, filters, pinned objectives, and layout saved between sessions
- **Quick access icon** — Realm Report icon in the Nexus Quick Access bar

## GW2 API Endpoints Used

- `/v2/worlds?ids=all` — world list for the dropdown
- `/v2/wvw/matches?world=<id>` — match data including all objectives and scores
- `/v2/wvw/objectives?ids=all` — objective name, type, and coordinate resolution (cached)
- `/v2/guild/<id>` — guild name/tag resolution for claims (cached to disk)
- `/v2/continents/2/floors/3/regions/7/maps/{id}` — map bounds for coordinate conversion (cached to disk)

## Building

### Prerequisites

- CMake 3.20+
- MinGW cross-compiler (`x86_64-w64-mingw32-gcc`, `x86_64-w64-mingw32-g++`)

### Setup

Download dependencies (ImGui and nlohmann/json):

```
chmod +x scripts/setup.sh
./scripts/setup.sh
```

### Build

```
mkdir build && cd build
cmake ..
make
```

This produces `RealmReport.dll`.

## Installation

1. Install [Raidcore Nexus](https://raidcore.gg/Nexus) if you haven't already
2. Copy `RealmReport.dll` into `<GW2>/addons/`
3. Launch the game — the addon loads automatically

## Usage

- Press **ALT+SHIFT+W** to toggle the objective tracker (or click the Realm Report icon in Nexus Quick Access)
- Press **Alt+M** to toggle the battlefield map
- Select your world from the dropdown
- Right-click any objective row to pin it or copy its nearest waypoint

## License

This project is licensed under the MIT License.

## Third-Party Notices

This project uses the following open-source libraries:

- [Dear ImGui](https://github.com/ocornut/imgui) — MIT License, Copyright (c) 2014-2021 Omar Cornut
- [nlohmann/json](https://github.com/nlohmann/json) — MIT License, Copyright (c) 2013-2025 Niels Lohmann
- [Nexus API](https://raidcore.gg/Nexus) — MIT License, Copyright (c) Raidcore.GG
