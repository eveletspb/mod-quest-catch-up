#include "AsyncCallbackProcessor.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldSession.h"
#include <chrono>
#include <cstdlib>
#include <unordered_map>
#include <unordered_set>

using namespace Acore::ChatCommands;

namespace
{
    using Clock = std::chrono::steady_clock;

    struct Request
    {
        ObjectGuid Recipient;
        Clock::time_point Expires;
        bool Apply;
    };

    // Player chat commands run on the world thread.
    std::unordered_map<ObjectGuid, Request> pendingRequests;

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

    bool CanPreview(Player const& recipient, Player const& source)
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

        if (!recipient || !source || !CanPreview(*recipient, *source) ||
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
            if (!quest || quest->IsRepeatable() || quest->IsWeekly() || quest->IsMonthly() || quest->IsSeasonal() ||
                quest->IsDFQuest() || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) ||
                quest->GetRewItemsCount() || quest->GetRewChoiceItemsCount() || quest->GetRewMailTemplateId() ||
                quest->GetRewSpell() || quest->GetRewSpellCast() || quest->GetCharTitleId() || quest->GetBonusTalents() ||
                !recipient->CanTakeQuest(quest, false))
                continue;

            int32 const group = quest->GetExclusiveGroup();
            if (group > 0 && exclusiveGroups.contains(group))
                continue;

            if (!recipient->ReserveQuestHistoryTransfer(id))
                continue;

            if (group > 0)
                exclusiveGroups.insert(group);
            transfer.InFlight.push_back(id);

            auto* insert = CharacterDatabase.GetPreparedStatement(CHAR_INS_CHAR_QUESTSTATUS_REWARDED);
            insert->SetData(0, guid.GetRawValue());
            insert->SetData(1, id);
            transaction->Append(insert);
            auto* activate = CharacterDatabase.GetPreparedStatement(CHAR_UPD_CHAR_QUESTSTATUS_REWARDED_ACTIVE_BY_QUEST);
            activate->SetData(0, guid.GetRawValue());
            activate->SetData(1, id);
            transaction->Append(activate);
            auto* removeActive = CharacterDatabase.GetPreparedStatement(CHAR_DEL_CHAR_QUESTSTATUS_BY_QUEST);
            removeActive->SetData(0, guid.GetRawValue());
            removeActive->SetData(1, id);
            transaction->Append(removeActive);

            if (transfer.InFlight.size() == 24)
                break;
        }

        if (transfer.InFlight.empty())
        {
            FinishTransfer(guid, transfer.Remaining.empty() ? "No eligible quests remain." :
                "Remaining quests require rewards, additional prerequisites or other quest state.");
            return;
        }

        transactionCallbacks.AddCallback(CharacterDatabase.AsyncCommitTransaction(transaction)).AfterComplete([guid](bool success)
        {
            auto& completed = transfers.at(guid);
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            bool const progressChanged = player && !completed.RecipientHistory.Matches(*player);
            if (player)
                player->ReleaseQuestHistoryTransfers(completed.InFlight, success);

            if (!success)
            {
                FinishTransfer(guid, "The database could not save the last batch. Request catch-up again.");
                return;
            }

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

            if (progressChanged)
            {
                FinishTransfer(guid, "Quest progress changed while saving. Request catch-up again.");
                return;
            }
            ApplyNextBatch(guid);
        });
    }

    bool RequestCatchUp(ChatHandler* handler, bool apply)
    {
        Player* recipient = handler->GetPlayer();
        if (apply && !progressOnlyEnabled)
        {
            handler->SendSysMessage("Quest catch-up transfer is disabled. Use .questcatchup preview.");
            return true;
        }

        Player* source = recipient->GetSelectedPlayer();
        if (!source || !CanPreview(*recipient, *source))
        {
            handler->SendSysMessage("Select a character from your faction in your group.");
            return true;
        }

        Clock::time_point const now = Clock::now();
        std::erase_if(pendingRequests, [now](auto const& entry) { return entry.second.Expires <= now; });
        if (!pendingRequests.try_emplace(source->GetGUID(), Request{
                recipient->GetGUID(), now + std::chrono::minutes(1), apply}).second)
        {
            handler->SendSysMessage("That character already has a pending quest catch-up request.");
            return true;
        }

        ChatHandler(source->GetSession()).PSendSysMessage(
            "{} requests quest catch-up {}. Use .questcatchup accept within one minute.",
            recipient->GetName(), apply ? "transfer" : "preview");
        handler->PSendSysMessage("Quest catch-up requested from {}.", source->GetName());
        return true;
    }

    bool AcceptRequest(ChatHandler* handler)
    {
        Player* source = handler->GetPlayer();
        auto const request = pendingRequests.extract(source->GetGUID());
        if (request.empty() || request.mapped().Expires <= Clock::now())
        {
            handler->SendSysMessage("You have no pending quest catch-up request.");
            return true;
        }

        Player* recipient = ObjectAccessor::FindConnectedPlayer(request.mapped().Recipient);
        if (!recipient || !CanPreview(*recipient, *source))
        {
            handler->SendSysMessage("The player who requested quest catch-up is no longer available in your group.");
            return true;
        }

        auto missing = FindMissingQuests(*source, *recipient);
        if (request.mapped().Apply)
        {
            if (!progressOnlyEnabled || !transfers.try_emplace(recipient->GetGUID(), *source, *recipient,
                std::move(missing)).second)
            {
                handler->SendSysMessage("Quest catch-up transfer is unavailable or already running.");
                return true;
            }

            handler->PSendSysMessage("Quest catch-up approved for {}.", recipient->GetName());
            ApplyNextBatch(recipient->GetGUID());
            return true;
        }

        ChatHandler(recipient->GetSession()).PSendSysMessage(
            "Quest catch-up preview with {}: {} completed chain quest(s) missing from your history.",
            source->GetName(), missing.size());
        handler->PSendSysMessage("Quest catch-up preview shared with {}.", recipient->GetName());
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
                { "request", Apply, SEC_PLAYER, Console::No },
                { "accept", AcceptRequest, SEC_PLAYER, Console::No }
            };
            return { { "questcatchup", subcommands } };
        }

    private:
        static bool Preview(ChatHandler* handler) { return RequestCatchUp(handler, false); }
        static bool Apply(ChatHandler* handler) { return RequestCatchUp(handler, true); }
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

    class QuestCatchUpPlayer : public PlayerScript
    {
    public:
        QuestCatchUpPlayer() : PlayerScript("QuestCatchUpPlayer", { PLAYERHOOK_ON_LOGIN }) { }

        void OnPlayerLogin(Player* player) override
        {
            auto const transfer = transfers.find(player->GetGUID());
            if (transfer != transfers.end())
                for (uint32 id : transfer->second.InFlight)
                    player->ReserveQuestHistoryTransfer(id);
        }
    };
}

void Addmod_quest_catch_upScripts()
{
    new QuestCatchUpCommands();
    new QuestCatchUpWorld();
    new QuestCatchUpPlayer();
}
