# AzerothCore Catalogue submission

GitHub repository description

> Catch up on quest chains from a party member and continue questing together on AzerothCore WotLK.

Publish the repository changes before adding the `azerothcore-module` topic in GitHub's About
settings. The live catalogue indexes public repositories by topic on a schedule.
See the [catalogue data workflow](https://github.com/azerothcore/azerothcore.github.io/blob/master/.github/workflows/update-catalogue-data.yml).

For the maintainer-reviewed submission described on the [submit page](https://www.azerothcore.org/module-submit),
use the text below in `_submissions/module/mod-quest-catch-up.md` in a fork of the
[active AzerothCore website repository](https://github.com/azerothcore/azerothcore.github.io/tree/master/_submissions/module).

## Module information

- **Name** — Quest Catch-Up (`mod-quest-catch-up`)
- **Author** — [eveletspb](https://github.com/eveletspb)
- **Repository** — [eveletspb/mod-quest-catch-up](https://github.com/eveletspb/mod-quest-catch-up)
- **Download** — [ZIP from main](https://github.com/eveletspb/mod-quest-catch-up/archive/refs/heads/main.zip)
- **License** — GNU Affero General Public License v3.0 (`AGPL-3.0-only`).

## Module integration

- **Compilation evidence** — `QuestCatchUp.cpp` compiled against `mod-playerbots/azerothcore-wotlk`
  at `7edad06c290c6c5e2598a636c2dd181cc5023fc4` in a module-only build. This identifies the
  tested Core revision and build scope.
- **Configuration** — Core CMake installs `conf/mod_quest_catch_up.conf.dist`. The administrator
  copies it to `mod_quest_catch_up.conf` to override defaults. Transfers are experimental and
  disabled by default. Preview is available with the default configuration.
- **SQL patches** — None. The module uses existing character database tables through AzerothCore APIs.
- **Custom CMake hooks** — None. Core discovers the module through its standard module loader.
- **Core hooks**
  - `CommandScript` — `GetCommands`
  - `WorldScript` — `OnAfterConfigLoad`, `OnUpdate`
  - `ServerScript` — `CanPacketReceive`
  - `PlayerScript` — `OnPlayerLogin`, `OnPlayerLogout`, `OnPlayerBeforeQuestComplete`,
    `OnPlayerCalculateTalentsPoints`

## Description

Quest Catch-Up helps players at different points in quest chains reach a common step.
The player who is behind targets a same-faction group member on the same map and runs
`.questcatchup preview` to see how many completed chain quests are candidates for transfer.
When experimental transfers are enabled, `.questcatchup request` records eligible missed
completions in the requester's history. Both players can then continue with the next quest
they qualify for through normal quest sharing or a quest giver.

The module uses existing AzerothCore quest and character data. It restores eligible quest
rewards and updates quest-driven phases. Item rewards go to bags with in-game mail for overflow.
Daily, weekly, monthly, Dungeon Finder, and unfinished quests are excluded. Seasonal quest
completions are evaluated for the current event.
