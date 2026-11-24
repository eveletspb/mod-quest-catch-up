#include "AsyncCallbackProcessor.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <cstdlib>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    struct QuestHistory
    {
        explicit QuestHistory(Player& player) : Rewarded(player.getRewardedQuests()), Active(player.getQuestStatusMap()) { }

        bool Matches(Player& player) const
        {
            if (Rewarded != player.getRewardedQuests() || Active.size() != player.getQuestStatusMap().size())
                return false;

            for (auto const& [id, expected] : Active)
            {
                auto found = player.getQuestStatusMap().find(id);
                if (found == player.getQuestStatusMap().end())
                    return false;

                auto const& current = found->second;
                if (current.Status != expected.Status || current.Explored != expected.Explored ||
                    current.PlayerCount != expected.PlayerCount ||
                    !std::ranges::equal(current.ItemCount, expected.ItemCount) ||
                    !std::ranges::equal(current.CreatureOrGOCount, expected.CreatureOrGOCount))
                    return false;
            }
            return true;
        }

        RewardedQuestSet Rewarded;
        QuestStatusMap Active;
    };

    struct Transfer
    {
        Transfer(Player& source, Player& recipient, std::vector<uint32> missing)
            : Source(source.GetGUID()), SourceName(source.GetName()), SourceHistory(source), RecipientHistory(recipient),
              Remaining(std::move(missing)) { }

        ObjectGuid Source;
        std::string SourceName;
        QuestHistory SourceHistory;
        QuestHistory RecipientHistory;
        std::vector<uint32> Remaining;
        std::vector<uint32> InFlight;
        uint32 Committed = 0;
    };

    std::unordered_map<ObjectGuid, Transfer> transfers;
    AsyncCallbackProcessor<TransactionCallback> transactionCallbacks;
    bool progressOnlyEnabled = false;

    struct PendingBatch
    {
        std::vector<uint32> Quests;
        bool TurnInAttempted = false;
    };

    // Quest packets and objective updates may run on the map update threads.
    std::mutex pendingBatchesMutex;
    std::unordered_map<ObjectGuid, PendingBatch> pendingBatches;

    bool IsPermanentQuest(Quest const& quest)
    {
        return !quest.IsRepeatable() && !quest.IsDailyOrWeekly() && !quest.IsMonthly() && !quest.IsSeasonal() &&
            !quest.IsDFQuest();
    }

    void RemoveJournalEntry(Player& player, uint32 id)
    {
        uint16 const slot = player.FindQuestSlot(id);
        if (slot < MAX_QUEST_LOG_SIZE)
        {
            player.SetQuestSlot(slot, 0);
            player.RemoveTimedAchievement(ACHIEVEMENT_TIMED_TYPE_QUEST, id);
        }
        player.RemoveActiveQuest(id, false);
    }

    void ApplyCommittedBatch(Player& player, std::vector<uint32> const& quests)
    {
        for (uint32 id : quests)
            player.SetRewardedQuest(id);

        bool updatePvP = false;
        for (uint32 id : quests)
        {
            if (player.getQuestStatusMap().contains(id))
            {
                if (Quest const* quest = sObjectMgr->GetQuestTemplate(id))
                    updatePvP |= quest->HasFlag(QUEST_FLAGS_FLAGS_PVP);
                RemoveJournalEntry(player, id);
            }
        }

        if (updatePvP)
        {
            player.pvpInfo.IsHostile = player.pvpInfo.IsInHostileArea || player.HasPvPForcingQuest();
            player.UpdatePvPState();
        }

        for (uint32 id : quests)
            player.SendQuestUpdate(id);

        player.UpdateZoneDependentAuras(player.GetZoneId());
        player.UpdateAreaDependentAuras(player.GetAreaId());
        WorldPacket query(CMSG_QUESTGIVER_STATUS_MULTIPLE_QUERY);
        player.GetSession()->HandleQuestgiverStatusMultipleQuery(query);
    }

    bool CanUseSource(Player const& recipient, Player const& source)
    {
        return &recipient != &source && recipient.IsInWorld() && source.IsInWorld() &&
            recipient.GetTeamId(true) == source.GetTeamId(true) &&
            recipient.GetGroup() && recipient.GetGroup() == source.GetGroup();
    }

    std::unordered_set<uint32> GetChainQuests()
    {
        std::unordered_set<uint32> chainQuests;
        for (auto const& [id, quest] : sObjectMgr->GetQuestTemplates())
        {
            for (int32 previous : quest->prevQuests)
            {
                chainQuests.insert(id);
                chainQuests.insert(static_cast<uint32>(std::abs(previous)));
            }

            if (uint32 next = quest->GetNextQuestInChain())
            {
                chainQuests.insert(id);
                chainQuests.insert(next);
            }

            if (uint32 next = quest->GetBreadcrumbForQuestId())
            {
                chainQuests.insert(id);
                chainQuests.insert(next);
            }
        }
        return chainQuests;
    }

    std::vector<uint32> FindMissingQuests(Player const& source, Player const& recipient)
    {
        auto const chainQuests = GetChainQuests();
        std::vector<uint32> missing;
        for (uint32 id : source.getRewardedQuests())
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (quest && chainQuests.contains(id) && !quest->IsDaily() && !recipient.IsQuestRewarded(id))
                missing.push_back(id);
        }
        std::ranges::sort(missing);
        return missing;
    }

    std::string_view GetAccessRestriction(Player const& recipient, Quest const& quest)
    {
        if (!recipient.SatisfyQuestLevel(&quest, false))
            return "level";
        if (!recipient.SatisfyQuestClass(&quest, false))
            return "class";
        if (!recipient.SatisfyQuestRace(&quest, false))
            return "race or faction";
        if (!recipient.SatisfyQuestReputation(&quest, false))
            return "reputation";
        return {};
    }

    void ReportAccessRestrictions(Player& recipient, Transfer const& transfer)
    {
        std::unordered_set<std::string_view> reported;
        ChatHandler handler(recipient.GetSession());
        auto report = [&](uint32 id)
        {
            if (Quest const* quest = sObjectMgr->GetQuestTemplate(id); quest && !quest->IsDaily())
            {
                auto const restriction = GetAccessRestriction(recipient, *quest);
                if (!restriction.empty() && reported.insert(restriction).second)
                    handler.PSendSysMessage("Quest {} ({}) is blocked by a {} requirement.",
                        id, quest->GetTitle(), restriction);
            }
        };

        for (uint32 id : transfer.Remaining)
            report(id);

        auto const chainQuests = GetChainQuests();
        for (auto const& [id, status] : transfer.SourceHistory.Active)
            if ((status.Status == QUEST_STATUS_INCOMPLETE || status.Status == QUEST_STATUS_COMPLETE) &&
                chainQuests.contains(id) && recipient.GetQuestStatus(id) == QUEST_STATUS_NONE)
                report(id);
    }

    void FinishTransfer(ObjectGuid guid, char const* reason)
    {
        auto transfer = transfers.extract(guid);
        if (Player* recipient = ObjectAccessor::FindConnectedPlayer(guid))
        {
            auto const& result = transfer.mapped();
            ChatHandler(recipient->GetSession()).PSendSysMessage(
                "Quest catch-up with {}: {} quest(s) saved, {} skipped. {}",
                result.SourceName, result.Committed, result.Remaining.size(), reason);
        }
    }

    void ApplyNextBatch(ObjectGuid guid)
    {
        auto& transfer = transfers.at(guid);
        Player* recipient = ObjectAccessor::FindConnectedPlayer(guid);
        Player* source = ObjectAccessor::FindConnectedPlayer(transfer.Source);
        if (!progressOnlyEnabled)
        {
            FinishTransfer(guid, "Quest catch-up transfer is disabled.");
            return;
        }

        if (!recipient || !source || !CanUseSource(*recipient, *source) ||
            !transfer.SourceHistory.Matches(*source) || !transfer.RecipientHistory.Matches(*recipient))
        {
            FinishTransfer(guid, "A participant left or quest progress changed. Request catch-up again.");
            return;
        }

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        std::unordered_set<int32> exclusiveGroups;
        for (uint32 id : transfer.Remaining)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (!quest || !IsPermanentQuest(*quest) || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) ||
                quest->GetRewItemsCount() || quest->GetRewChoiceItemsCount() || quest->GetRewMailTemplateId() ||
                quest->GetRewSpell() || quest->GetRewSpellCast() || quest->GetCharTitleId() || quest->GetBonusTalents())
                continue;

            if (recipient->GetQuestStatus(id) == QUEST_STATUS_NONE && !recipient->CanTakeQuest(quest, false))
                continue;

            int32 const group = quest->GetExclusiveGroup();
            if (group > 0 && exclusiveGroups.contains(group))
                continue;

            if (group > 0)
                exclusiveGroups.insert(group);
            transfer.InFlight.push_back(id);

            auto* insert = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_QUESTSTATUS_REWARDED);
            insert->SetData(0, guid.GetRawValue());
            insert->SetData(1, id);
            transaction->Append(insert);
            auto* removeActive = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_QUESTSTATUS_BY_QUEST);
            removeActive->SetData(0, guid.GetRawValue());
            removeActive->SetData(1, id);
            transaction->Append(removeActive);

            if (transfer.InFlight.size() == 24)
                break;
        }

        if (transfer.InFlight.empty())
        {
            ReportAccessRestrictions(*recipient, transfer);
            char const* result = "Quest history synchronized.";
            if (!transfer.Remaining.empty())
                result = "Some quests require rewards, additional prerequisites or other quest state.";
            else if (!transfer.Committed)
                result = "No changes were needed.";
            FinishTransfer(guid, result);
            return;
        }

        {
            std::lock_guard lock(pendingBatchesMutex);
            pendingBatches.emplace(guid, PendingBatch{transfer.InFlight});
        }

        transactionCallbacks.AddCallback(CharacterDatabase.AsyncCommitTransaction(transaction)).AfterComplete([guid](bool success)
        {
            auto& completed = transfers.at(guid);
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            bool turnInAttempted;
            {
                std::lock_guard lock(pendingBatchesMutex);
                turnInAttempted = pendingBatches.extract(guid).mapped().TurnInAttempted;
            }
            bool const progressChanged = player && !completed.RecipientHistory.Matches(*player);

            if (!success)
            {
                FinishTransfer(guid, "The database could not save the last batch. Request catch-up again.");
                return;
            }

            if (player)
                ApplyCommittedBatch(*player, completed.InFlight);

            for (uint32 id : completed.InFlight)
            {
                completed.RecipientHistory.Rewarded.insert(id);
                completed.RecipientHistory.Active.erase(id);
            }
            completed.Committed += static_cast<uint32>(completed.InFlight.size());
            std::erase_if(completed.Remaining, [&completed](uint32 id)
            {
                return completed.RecipientHistory.Rewarded.contains(id);
            });
            completed.InFlight.clear();

            if (progressChanged || turnInAttempted)
            {
                FinishTransfer(guid, "Quest progress changed while saving. Request catch-up again.");
                return;
            }
            ApplyNextBatch(guid);
        });
    }

    bool RunCatchUp(ChatHandler* handler, bool apply)
    {
        Player* recipient = handler->GetPlayer();
        if (apply && !progressOnlyEnabled)
        {
            handler->SendSysMessage("Quest catch-up transfer is disabled. Use .questcatchup preview.");
            return true;
        }

        Player* source = recipient->GetSelectedPlayer();
        if (source && recipient->GetGroup() && recipient->GetGroup() == source->GetGroup() &&
            recipient->GetTeamId(true) != source->GetTeamId(true))
        {
            handler->SendSysMessage("Quest catch-up is unavailable across factions.");
            return true;
        }

        if (!source || !CanUseSource(*recipient, *source))
        {
            handler->SendSysMessage("Select a character from your faction in your group.");
            return true;
        }

        auto missing = FindMissingQuests(*source, *recipient);
        if (apply)
        {
            ObjectGuid const recipientGuid = recipient->GetGUID();
            if (!transfers.try_emplace(recipientGuid, *source, *recipient, std::move(missing)).second)
            {
                handler->SendSysMessage("Quest catch-up transfer is already running.");
                return true;
            }

            handler->PSendSysMessage("Quest catch-up with {} started.", source->GetName());
            ApplyNextBatch(recipientGuid);
            return true;
        }

        handler->PSendSysMessage(
            "Quest catch-up preview with {}: {} completed chain quest(s) missing from your history.",
            source->GetName(), missing.size());
        return true;
    }

    class QuestCatchUpCommands : public CommandScript
    {
    public:
        QuestCatchUpCommands() : CommandScript("QuestCatchUpCommands") { }

        ChatCommandTable GetCommands() const override
        {
            static ChatCommandTable subcommands =
            {
                { "preview", Preview, SEC_PLAYER, Console::No },
                { "request", Apply, SEC_PLAYER, Console::No }
            };
            return { { "questcatchup", subcommands } };
        }

    private:
        static bool Preview(ChatHandler* handler) { return RunCatchUp(handler, false); }
        static bool Apply(ChatHandler* handler) { return RunCatchUp(handler, true); }
    };

    class QuestCatchUpWorld : public WorldScript
    {
    public:
        QuestCatchUpWorld() : WorldScript("QuestCatchUpWorld") { }

        void OnAfterConfigLoad(bool) override
        {
            progressOnlyEnabled = sConfigMgr->GetOption<bool>("QuestCatchUp.ProgressOnly", false);
        }

        void OnUpdate(uint32) override { transactionCallbacks.ProcessReadyCallbacks(); }
    };

    bool BlockPendingQuest(ObjectGuid guid, uint32 questId, bool turnIn = false)
    {
        std::lock_guard lock(pendingBatchesMutex);
        auto const batch = pendingBatches.find(guid);
        if (batch == pendingBatches.end() || std::ranges::find(batch->second.Quests, questId) == batch->second.Quests.end())
            return false;

        batch->second.TurnInAttempted |= turnIn;
        return true;
    }

    class QuestCatchUpPackets : public ServerScript
    {
    public:
        QuestCatchUpPackets() : ServerScript("QuestCatchUpPackets", { SERVERHOOK_CAN_PACKET_RECEIVE }) { }

        bool CanPacketReceive(WorldSession* session, WorldPacket const& packet) override
        {
            if (!session)
                return true;

            auto const opcode = packet.GetOpcode();
            if (opcode == CMSG_PLAYER_LOGIN && packet.size() >= sizeof(uint64))
            {
                ObjectGuid const guid(packet.read<uint64>(packet.rpos()));
                {
                    std::lock_guard lock(pendingBatchesMutex);
                    if (!pendingBatches.contains(guid))
                        return true;
                }
                session->SendCharLoginFailed(LoginFailureReason::LockedForTransfer);
                return false;
            }

            Player* player = session->GetPlayer();
            if (!player)
                return true;

            if (opcode == CMSG_QUESTLOG_REMOVE_QUEST && packet.size() >= sizeof(uint8))
            {
                uint8 const slot = packet.read<uint8>(packet.rpos());
                return slot >= MAX_QUEST_LOG_SIZE ||
                    !BlockPendingQuest(player->GetGUID(), player->GetQuestSlotQuestId(slot));
            }

            if (opcode == CMSG_QUEST_CONFIRM_ACCEPT && packet.size() >= sizeof(uint32))
                return !BlockPendingQuest(player->GetGUID(), packet.read<uint32>(packet.rpos()));

            bool const turnIn = opcode == CMSG_QUESTGIVER_CHOOSE_REWARD || opcode == CMSG_QUESTGIVER_COMPLETE_QUEST ||
                opcode == CMSG_QUESTGIVER_REQUEST_REWARD;
            if ((turnIn || opcode == CMSG_QUESTGIVER_ACCEPT_QUEST || opcode == CMSG_QUESTGIVER_QUERY_QUEST) &&
                packet.size() >= sizeof(uint64) + sizeof(uint32))
            {
                uint32 const id = packet.read<uint32>(packet.rpos() + sizeof(uint64));
                return !BlockPendingQuest(player->GetGUID(), id, turnIn);
            }

            return true;
        }
    };

    class QuestCatchUpPlayer : public PlayerScript
    {
    public:
        QuestCatchUpPlayer() : PlayerScript("QuestCatchUpPlayer",
            { PLAYERHOOK_ON_LOGIN, PLAYERHOOK_ON_BEFORE_QUEST_COMPLETE }) { }

        bool OnPlayerBeforeQuestComplete(Player* player, uint32 id) override
        {
            return !BlockPendingQuest(player->GetGUID(), id);
        }

        void OnPlayerLogin(Player* player) override
        {
            if (!progressOnlyEnabled)
                return;

            // An older autosave may finish after the transfer's journal delete.
            std::vector<uint32> obsolete;
            for (auto const& [id, status] : player->getQuestStatusMap())
                if (player->IsQuestRewarded(id))
                    if (Quest const* quest = sObjectMgr->GetQuestTemplate(id); quest && IsPermanentQuest(*quest))
                        obsolete.push_back(id);

            for (uint32 id : obsolete)
            {
                RemoveJournalEntry(*player, id);
                player->SendQuestUpdate(id);
            }
        }
    };
}

void Addmod_quest_catch_upScripts()
{
    new QuestCatchUpCommands();
    new QuestCatchUpWorld();
    new QuestCatchUpPackets();
    new QuestCatchUpPlayer();
}
