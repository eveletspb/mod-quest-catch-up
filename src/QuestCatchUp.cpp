#include "Chat.h"
#include "ChatCommand.h"
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

    struct PreviewRequest
    {
        ObjectGuid Recipient;
        Clock::time_point Expires;
    };

    // Player chat commands run on the world thread.
    std::unordered_map<ObjectGuid, PreviewRequest> pendingPreviews;

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

    bool RequestPreview(ChatHandler* handler)
    {
        Player* recipient = handler->GetPlayer();
        Player* source = recipient->GetSelectedPlayer();
        if (!source || !CanPreview(*recipient, *source))
        {
            handler->SendSysMessage("Select a character from your faction in your group.");
            return true;
        }

        Clock::time_point const now = Clock::now();
        std::erase_if(pendingPreviews, [now](auto const& entry) { return entry.second.Expires <= now; });
        if (!pendingPreviews.try_emplace(source->GetGUID(), PreviewRequest{
                recipient->GetGUID(), now + std::chrono::minutes(1)}).second)
        {
            handler->SendSysMessage("That character already has a pending quest catch-up preview request.");
            return true;
        }

        ChatHandler(source->GetSession()).PSendSysMessage(
            "{} wants to compare your completed quest chains. Use .questcatchup accept within one minute.",
            recipient->GetName());
        handler->PSendSysMessage("Quest catch-up preview requested from {}.", source->GetName());
        return true;
    }

    bool AcceptPreview(ChatHandler* handler)
    {
        Player* source = handler->GetPlayer();
        auto const request = pendingPreviews.extract(source->GetGUID());
        if (request.empty() || request.mapped().Expires <= Clock::now())
        {
            handler->SendSysMessage("You have no pending quest catch-up preview request.");
            return true;
        }

        Player* recipient = ObjectAccessor::FindConnectedPlayer(request.mapped().Recipient);
        if (!recipient || !CanPreview(*recipient, *source))
        {
            handler->SendSysMessage("The player who requested the preview is no longer available in your group.");
            return true;
        }

        auto const chainQuests = GetChainQuests();
        uint32 missing = 0;
        for (uint32 id : source->getRewardedQuests())
        {
            Quest const* quest = sObjectMgr->GetQuestTemplate(id);
            if (quest && chainQuests.contains(id) && !quest->IsDaily() && !recipient->IsQuestRewarded(id))
                ++missing;
        }

        ChatHandler(recipient->GetSession()).PSendSysMessage(
            "Quest catch-up preview with {}: {} completed chain quest(s) missing from your history.",
            source->GetName(), missing);
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
                { "preview", RequestPreview, SEC_PLAYER, Console::No },
                { "accept", AcceptPreview, SEC_PLAYER, Console::No }
            };
            return { { "questcatchup", subcommands } };
        }
    };
}

void Addmod_quest_catch_upScripts()
{
    new QuestCatchUpCommands();
}
