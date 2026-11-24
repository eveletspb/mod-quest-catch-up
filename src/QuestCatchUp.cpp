#include "AsyncCallbackProcessor.h"
#include "Bag.h"
#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "DatabaseEnv.h"
#include "GameTime.h"
#include "Item.h"
#include "Log.h"
#include "Mail.h"
#include "MailMgr.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Player.h"
#include "QuestDef.h"
#include "ScriptMgr.h"
#include "WorldPacket.h"
#include "WorldSession.h"
#include <cstdlib>
#include <memory>
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
        struct RewardMail
        {
            std::unique_ptr<Mail> Letter;
            std::vector<std::unique_ptr<Item>> Attachments;
        };
        struct InventoryReward
        {
            ObjectGuid Guid;
            uint16 Position;
            uint32 Added;
            uint32 OriginalCount;
            ItemUpdateState OriginalState;
        };
        std::vector<RewardMail> RewardMails;
        std::vector<InventoryReward> InventoryRewards;
        uint32 Committed = 0;
    };

    std::unordered_map<ObjectGuid, Transfer> transfers;
    AsyncCallbackProcessor<TransactionCallback> transactionCallbacks;
    AsyncCallbackProcessor<SQLQueryHolderCallback> recoveryCallbacks;
    std::unordered_set<ObjectGuid> needsRelog;
    bool progressOnlyEnabled = false;

    struct PendingBatch
    {
        std::vector<uint32> Quests;
        bool TurnInAttempted = false;
        bool InventoryReserved = false;
    };

    // Quest packets and objective updates may run on the map update threads.
    std::mutex pendingBatchesMutex;
    std::unordered_map<ObjectGuid, PendingBatch> pendingBatches;

    bool IsPermanentQuest(Quest const& quest)
    {
        return !quest.IsRepeatable() && !quest.IsDailyOrWeekly() && !quest.IsMonthly() && !quest.IsSeasonal() &&
            !quest.IsDFQuest();
    }

    void FinishInventoryReward(Player& recipient, Transfer::InventoryReward const& reward, bool saved)
    {
        Item* item = recipient.GetItemByPos(reward.Position);
        if (!item || item->GetGUID() != reward.Guid)
        {
            LOG_ERROR("module.questcatchup", "Reserved item {} moved while saving rewards for {}",
                reward.Guid.ToString(), recipient.GetGUID().ToString());
            return;
        }

        item->SetInTrade(false);
        item->m_lootGenerated = false;
        if (!saved)
        {
            if (reward.OriginalCount)
            {
                if (reward.OriginalState != ITEM_UNCHANGED)
                {
                    item->FSetState(reward.OriginalState);
                    item->AddToUpdateQueueOf(&recipient);
                }
                return;
            }

            item->RemoveFromUpdateQueueOf(&recipient);
            recipient.RemoveItem(uint8(reward.Position >> 8), uint8(reward.Position & 255), false);
            if (item->IsInWorld())
            {
                item->RemoveFromWorld();
                item->DestroyForPlayer(&recipient);
            }
            delete item;
            return;
        }

        if (reward.OriginalCount)
            item->SetCount(reward.OriginalCount + reward.Added);
        else if (recipient.IsInWorld() && !item->IsInWorld())
            item->AddToWorld();

        if (recipient.IsInWorld())
        {
            item->SendUpdateToPlayer(&recipient);
            recipient.SendNewItem(item, reward.Added, true, false);
        }
    }

    bool AppendItemRewards(CharacterDatabaseTransaction const& transaction, Player& recipient,
        Quest const& quest, Transfer& transfer)
    {
        std::vector<std::pair<uint32, uint32>> rewards;
        for (uint32 i = 0; i < QUEST_REWARDS_COUNT; ++i)
            if (quest.RewardItemId[i] && quest.RewardItemIdCount[i])
                rewards.emplace_back(quest.RewardItemId[i], quest.RewardItemIdCount[i]);

        uint64 bestPrice = 0;
        uint32 bestChoice = QUEST_REWARD_CHOICES_COUNT;
        for (uint32 i = 0; i < QUEST_REWARD_CHOICES_COUNT; ++i)
        {
            ItemTemplate const* item = sObjectMgr->GetItemTemplate(quest.RewardChoiceItemId[i]);
            uint32 const count = quest.RewardChoiceItemCount[i];
            if (!item || !count)
                continue;

            uint64 const price = uint64(item->SellPrice) * count;
            if (bestChoice == QUEST_REWARD_CHOICES_COUNT || price > bestPrice)
            {
                bestChoice = i;
                bestPrice = price;
            }
        }

        if (bestChoice != QUEST_REWARD_CHOICES_COUNT)
            rewards.emplace_back(quest.RewardChoiceItemId[bestChoice], quest.RewardChoiceItemCount[bestChoice]);
        else if (quest.GetRewChoiceItemsCount())
            return false;

        // Validate every entry before reserving inventory, so an invalid template cannot leave a partial reward.
        for (auto const& reward : rewards)
            if (!sObjectMgr->GetItemTemplate(reward.first))
                return false;

        std::vector<Transfer::InventoryReward> inventory;
        std::vector<Transfer::RewardMail> staged;
        for (auto [entry, count] : rewards)
        {
            ItemTemplate const* itemTemplate = sObjectMgr->GetItemTemplate(entry);
            ItemPosCountVec destinations;

            if (!itemTemplate->Duration)
                recipient.CanStoreNewItem(NULL_BAG, NULL_SLOT, destinations, entry, count);

            for (ItemPosCount const& destination : destinations)
            {
                Item* item = recipient.GetItemByPos(destination.pos);
                uint32 const originalCount = item ? item->GetCount() : 0;
                if (item && (item->IsInTrade() || item->m_lootGenerated))
                    continue;

                if (!item)
                {
                    item = Item::CreateItem(entry, destination.count, &recipient);
                    if (!item)
                        continue;

                    item = recipient.StoreItem({destination}, item, false);
                }

                inventory.push_back({item->GetGUID(), destination.pos, destination.count,
                    originalCount, item->GetState()});
                // Stock storage skips merging into looted stacks and does not consume items in trade.
                item->m_lootGenerated = true;
                item->SetInTrade();
                count -= destination.count;
            }

            while (count)
            {
                if (staged.empty() || staged.back().Attachments.size() == MAX_MAIL_ITEMS)
                    staged.push_back({std::make_unique<Mail>(), {}});

                uint32 const stack = std::min(count, itemTemplate->GetMaxStackSize());
                std::unique_ptr<Item> item(Item::CreateItem(entry, stack));
                if (!item)
                {
                    for (auto const& reward : inventory)
                        FinishInventoryReward(recipient, reward, false);
                    return false;
                }
                staged.back().Attachments.push_back(std::move(item));
                count -= stack;
            }
        }

        for (auto const& reward : inventory)
        {
            Item* item = recipient.GetItemByPos(reward.Position);
            item->RemoveFromUpdateQueueOf(&recipient);
            if (reward.OriginalCount)
            {
                item->SetCount(reward.OriginalCount + reward.Added);
                item->SetState(ITEM_CHANGED);
            }

            item->SaveToDB(transaction);
            if (reward.OriginalCount)
                item->SetCount(reward.OriginalCount);

            Bag const* bag = item->GetContainer();
            auto* insert = CharacterDatabase.GetPreparedStatement(CHAR_REP_INVENTORY_ITEM);
            insert->SetData(0, recipient.GetGUID().GetCounter());
            insert->SetData(1, bag ? bag->GetGUID().GetCounter() : 0);
            insert->SetData(2, item->GetSlot());
            insert->SetData(3, item->GetGUID().GetCounter());
            transaction->Append(insert);
            transfer.InventoryRewards.push_back(reward);
        }

        for (auto& reward : staged)
        {
            Mail& mail = *reward.Letter;
            mail.messageID = sObjectMgr->GenerateMailID();
            mail.messageType = MAIL_CREATURE;
            mail.stationery = MAIL_STATIONERY_DEFAULT;
            mail.sender = 34337; // The Postmaster.
            mail.receiver = recipient.GetGUID().GetCounter();
            mail.subject = "Quest catch-up rewards";
            mail.body = quest.GetTitle();
            mail.deliver_time = GameTime::GetGameTime().count();
            mail.expire_time = mail.deliver_time + 30 * DAY;
            mail.checked = MAIL_CHECK_MASK_COPIED;
            mail.state = MAIL_STATE_UNCHANGED;

            auto* insert = CharacterDatabase.GetPreparedStatement(CHAR_INS_MAIL);
            insert->SetData(0, mail.messageID);
            insert->SetData(1, mail.messageType);
            insert->SetData(2, mail.stationery);
            insert->SetData(3, mail.mailTemplateId);
            insert->SetData(4, mail.sender);
            insert->SetData(5, mail.receiver);
            insert->SetData(6, mail.subject);
            insert->SetData(7, mail.body);
            insert->SetData(8, true);
            insert->SetData(9, uint32(mail.expire_time));
            insert->SetData(10, uint32(mail.deliver_time));
            insert->SetData(11, mail.money);
            insert->SetData(12, mail.COD);
            insert->SetData(13, uint8(mail.checked));
            transaction->Append(insert);

            for (auto const& item : reward.Attachments)
            {
                item->SaveToDB(transaction);
                auto const itemGuid = item->GetGUID().GetCounter();
                mail.AddItem(itemGuid, item->GetEntry());
                auto* attachment = CharacterDatabase.GetPreparedStatement(CHAR_INS_MAIL_ITEM);
                attachment->SetData(0, mail.messageID);
                attachment->SetData(1, itemGuid);
                attachment->SetData(2, mail.receiver);
                transaction->Append(attachment);
            }

            transfer.RewardMails.push_back(std::move(reward));
        }

        return true;
    }

    void PublishRewardMails(Transfer& transfer, Player* recipient)
    {
        for (auto& reward : transfer.RewardMails)
        {
            sMailMgr->OnMailSent(reward.Letter->receiver);
            if (!recipient)
                continue;

            time_t const deliverTime = reward.Letter->deliver_time;
            recipient->AddMail(reward.Letter.release());
            for (auto& item : reward.Attachments)
                recipient->AddMItem(item.release());
            recipient->AddNewMailDeliverTime(deliverTime);
        }
        transfer.RewardMails.clear();
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

        if (recipient->GetTradeData() || recipient->IsNonMeleeSpellCast(false))
        {
            FinishTransfer(guid, "Finish trading or casting before requesting catch-up again.");
            return;
        }

        CharacterDatabaseTransaction transaction = CharacterDatabase.BeginTransaction();
        std::unordered_set<int32> exclusiveGroups;
        for (uint32 id : transfer.Remaining)
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (!quest || !IsPermanentQuest(*quest) || quest->HasSpecialFlag(QUEST_SPECIAL_FLAGS_TIMED) ||
                quest->GetRewMailTemplateId() ||
                quest->GetRewSpell() || quest->GetRewSpellCast() || quest->GetCharTitleId() || quest->GetBonusTalents())
                continue;

            if (recipient->GetQuestStatus(id) == QUEST_STATUS_NONE && !recipient->CanTakeQuest(quest, false))
                continue;

            int32 const group = quest->GetExclusiveGroup();
            if (group > 0 && exclusiveGroups.contains(group))
                continue;

            if (!AppendItemRewards(transaction, *recipient, *quest, transfer))
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
            pendingBatches.emplace(guid, PendingBatch{transfer.InFlight, false, !transfer.InventoryRewards.empty()});
        }

        auto complete = [guid](bool success, bool indeterminate)
        {
            auto& completed = transfers.at(guid);
            Player* player = ObjectAccessor::FindConnectedPlayer(guid);
            bool turnInAttempted;
            {
                std::lock_guard lock(pendingBatchesMutex);
                turnInAttempted = pendingBatches.extract(guid).mapped().TurnInAttempted;
                if (indeterminate && player)
                    needsRelog.insert(guid);
            }
            bool const progressChanged = player && !completed.RecipientHistory.Matches(*player);

            if (player)
                for (auto const& reward : completed.InventoryRewards)
                    FinishInventoryReward(*player, reward, success);

            if (!success)
            {
                FinishTransfer(guid, indeterminate ? "The database could not confirm the last batch. Log in again before retrying." :
                    "The database did not save the last batch. Request catch-up again.");
                return;
            }

            if (player)
            {
                ApplyCommittedBatch(*player, completed.InFlight);
                for (auto const& reward : completed.InventoryRewards)
                    if (Item* item = player->GetItemByPos(reward.Position); item && item->GetGUID() == reward.Guid)
                        player->ItemAddedQuestCheck(item->GetEntry(), reward.Added);

                if (!progressChanged)
                    completed.RecipientHistory = QuestHistory(*player);
            }
            completed.InventoryRewards.clear();
            PublishRewardMails(completed, player);

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
        };

        transactionCallbacks.AddCallback(CharacterDatabase.AsyncCommitTransaction(transaction)).AfterComplete([guid, complete](bool success)
        {
            if (success)
            {
                complete(true, false);
                return;
            }

            // A disconnected COMMIT can return an error after the server has already saved the transaction.
            auto holder = std::make_shared<SQLQueryHolder<CharacterDatabaseConnection>>();
            holder->SetSize(2);
            auto* character = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARACTER);
            character->SetData(0, guid.GetRawValue());
            holder->SetPreparedQuery(0, character);
            auto* rewarded = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHARACTER_QUESTSTATUSREW);
            rewarded->SetData(0, guid.GetRawValue());
            holder->SetPreparedQuery(1, rewarded);

            recoveryCallbacks.AddCallback(CharacterDatabase.DelayQueryHolder(holder)).AfterComplete(
                [guid, complete](SQLQueryHolderBase const& result)
            {
                auto rows = result.GetPreparedResult(1);
                if (!result.GetPreparedResult(0) || !rows)
                {
                    complete(false, true);
                    return;
                }

                auto const& inFlight = transfers.at(guid).InFlight;
                std::size_t saved = 0;
                do
                {
                    uint32 const id = rows->Fetch()[0].Get<uint32>();
                    saved += std::ranges::find(inFlight, id) != inFlight.end();
                }
                while (rows->NextRow());

                complete(saved == inFlight.size(), saved != 0 && saved != inFlight.size());
            });
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

        if (apply && needsRelog.contains(recipient->GetGUID()))
        {
            handler->SendSysMessage("Log in again to refresh your quest and inventory state before retrying catch-up.");
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

        void OnUpdate(uint32) override
        {
            transactionCallbacks.ProcessReadyCallbacks();
            recoveryCallbacks.ProcessReadyCallbacks();
        }
    };

    bool BlockPendingQuest(ObjectGuid guid, uint32 questId, bool turnIn = false)
    {
        std::lock_guard lock(pendingBatchesMutex);
        if (needsRelog.contains(guid))
            return true;

        auto const batch = pendingBatches.find(guid);
        if (batch == pendingBatches.end())
            return false;

        if (!batch->second.InventoryReserved &&
            std::ranges::find(batch->second.Quests, questId) == batch->second.Quests.end())
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

            // While a bag slot is reserved, the player can keep the connection alive while the world keeps ticking.
            if (opcode != MSG_MOVE_HEARTBEAT && opcode != MSG_MOVE_WORLDPORT_ACK && opcode != MSG_MOVE_TELEPORT_ACK &&
                opcode != CMSG_TIME_SYNC_RESP && opcode != CMSG_KEEP_ALIVE && opcode != CMSG_LOGOUT_REQUEST &&
                opcode != CMSG_LOGOUT_CANCEL && BlockPendingQuest(player->GetGUID(), 0))
                return false;

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
            {
                std::lock_guard lock(pendingBatchesMutex);
                needsRelog.erase(player->GetGUID());
            }
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
