# Quest Catch-Up for AzerothCore

![Quest Catch-Up — two adventurers meet at a trail sign](banner.png)

`mod-quest-catch-up` helps players with different levels of quest progress continue quest chains together.
A character who is behind can catch up on completed steps from a member of their group.
Progress transfers in one direction, from the targeted character to the character who runs the command.

## How to catch up

Suppose **Vellorin** has already completed the first few quests in a chain and **Mirevash** wants to catch up.

1. Both characters log in, join the same group, and meet on the same map.
   They must belong to the same faction.
2. **Mirevash** targets **Vellorin** and runs these commands in the in-game chat.

   ```text
   .questcatchup preview
   .questcatchup request
   ```

   `preview` shows how many quests are candidates for transfer. `request` starts the
   transfer and reports the result in the requesting character's system chat.
3. If **Vellorin** already has the next quest and it can be shared, he can share it
   with the group. **Mirevash** can also pick up an available quest from its quest giver.

## What the transfer does

- The module adds completed steps in quest chains to the recipient's quest history.
  If a transferred quest is still in the recipient's quest log, the module removes
  the entry and records the quest as completed.
- The module puts item rewards in the recipient's bags and sends any overflow by
  in-game mail. When a quest offers a choice of rewards, the module selects the
  option with the highest total vendor sell value.
- The module grants reputation awarded directly by quests, restores spells,
  titles, and bonus talent points granted by quest completion, and updates
  quest-driven phases.
- The transfer skips the targeted character's current objectives, progress
  counters, and unfinished quests. It also excludes daily, weekly, and monthly
  quests, as well as Dungeon Finder quests. Seasonal quest completions count
  within the current event.
- The number of saved quests can be lower than the count shown by `preview`.
  The module checks quest chain prerequisites, character restrictions, and
  whether it can grant the rewards.

## Installation and configuration

1. From your AzerothCore WotLK 3.3.5a checkout, clone the module.

   ```bash
   git clone https://github.com/eveletspb/mod-quest-catch-up.git modules/mod-quest-catch-up
   ```

2. Reconfigure, rebuild, and install `worldserver` using your usual AzerothCore build workflow.
   The module requires no SQL migrations or client addon.
3. Copy the installed [mod_quest_catch_up.conf.dist](conf/mod_quest_catch_up.conf.dist)
   to `mod_quest_catch_up.conf` in the same module configuration directory.

The default value of `QuestCatchUp.ProgressOnly` is `0`. Players can run `preview` with
this setting. To try experimental transfers on a test realm, set the option to `1`
and load the updated configuration.

```ini
QuestCatchUp.ProgressOnly = 1
```

## Test results on develop

The character names in this example are fictional. The numbers come from the actual test.

```text
Quest catch-up preview with Vellorin: 512 completed chain quest(s) missing from your history.
Quest catch-up with Vellorin: 427 quest(s) saved, 85 skipped.
```

After both characters logged out, the database confirmed that **Mirevash** had
**1,138 completed quests**, up from **711**. **Vellorin**'s quest history and quest
log matched the original snapshot. Before the transfer, **Mirevash** could not
accept *The Spirits of Southwind* (1125) or *The Stones That Bind Us* (2681).
After the transfer, the server confirmed that he could accept both quests.
Both were active in **Vellorin**'s quest log and could be shared with the group.
The test used server sessions, and both quests were left available for the players.

## Author and license

Author — [eveletspb](https://github.com/eveletspb).

Quest Catch-Up is distributed under the [GNU Affero General Public License v3.0](LICENSE)
(`AGPL-3.0-only`).
