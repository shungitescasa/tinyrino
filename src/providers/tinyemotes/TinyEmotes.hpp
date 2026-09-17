#pragma once

#include "common/Aliases.hpp"
#include "common/Atomic.hpp"

#include <pajlada/signals/scoped-connection.hpp>
#include <QJsonObject>
#include <qobject.h>
#include <QString>

#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;
class EmoteMap;
class Channel;
struct TinyELiveUpdateEmoteUpdateAddMessage;
struct TinyELiveUpdateEmoteRemoveMessage;

namespace tinyemotes::detail {

    EmoteMap parseChannelEmotes(const QString &instanceUrl,
                                const QJsonObject &jsonRoot,
                                const QString &channelDisplayName);

}  // namespace tinyemotes::detail

class TinyEmotes final
{
    static constexpr const char *tinyemotesGlobalEmotesApiUrl =
        "%1%2/emotesets.php?id=global";
    static constexpr const char *tinyemotesUserApiUrl =
        "%1%2/users.php?alias_id=%3";

public:
    TinyEmotes();

    std::shared_ptr<const EmoteMap> emotes(const QString &instanceUrl) const;
    std::optional<EmotePtr> emote(const QString &instanceUrl,
                                EmoteNameView name) const;
    void loadEmotes();
    void setEmotes(const QString &instanceUrl,
                   std::shared_ptr<const EmoteMap> emotes);
    static void loadChannel(const QString &instanceUrl,
                            std::weak_ptr<Channel> channel,
                            const QString &channelId,
                            const QString &channelDisplayName,
                            std::function<void(EmoteMap &&)> callback,
                            bool manualRefresh, bool cacheHit);

    /**
     * Adds an emote to the `channelEmoteMap`.
     * This will _copy_ the emote map and
     * update the `Atomic`.
     *
     * @return The added emote.
     */
    static EmotePtr addEmote(
        const QString &instanceUrl, const QString &channelDisplayName,
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const TinyELiveUpdateEmoteUpdateAddMessage &message);

    /**
     * Updates an emote in this `channelEmoteMap`.
     * This will _copy_ the emote map and
     * update the `Atomic`.
     *
     * @return pair<old emote, new emote> if any emote was updated.
     */
    static std::optional<std::pair<EmotePtr, EmotePtr>> updateEmote(
        const QString &instanceUrl, const QString &channelDisplayName,
        Atomic<std::shared_ptr<const EmoteMap>> &channelEmoteMap,
        const TinyELiveUpdateEmoteUpdateAddMessage &message);

    /**
     * Removes an emote from this `channelEmoteMap`.
     * This will _copy_ the emote map and
     * update the `Atomic`.
     *
     * @return The removed emote if any emote was removed.
     */
    static std::optional<EmotePtr> removeEmote(
        const QString &instanceUrl,
        std::map<QString, Atomic<std::shared_ptr<const EmoteMap>>>
            &channelTinyEmotesMap,
        const TinyELiveUpdateEmoteRemoveMessage &message);

private:
    std::map<QString, Atomic<std::shared_ptr<const EmoteMap>>> global_;

    std::vector<std::unique_ptr<pajlada::Signals::ScopedConnection>>
        managedConnections;
};

}  // namespace chatterino
