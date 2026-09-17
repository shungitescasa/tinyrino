#pragma once

#include "common/Aliases.hpp"
#include "util/ThreadGuard.hpp"

#include <QColor>

#include <memory>
#include <optional>
#include <shared_mutex>
#include <unordered_map>
#include <vector>

namespace chatterino {

struct Emote;
using EmotePtr = std::shared_ptr<const Emote>;

const QString TINYEMOTES_BADGES_API = "%1%2/badges";
const QString TINYEMOTES_BADGE_IMG_API =
    "%1%2/static/userdata/emotes/%3/%4.webp";

class TinyBadges
{
public:
    TinyBadges() = default;

    struct Badge {
        EmotePtr badge;
        QString id;
        bool isRoleBadge;
    };

    std::optional<TinyBadges::Badge> getUserBadge(const QString &instanceUrl,
                                                  const UserId &id,
                                                  bool findRoleBadge) const;

    void load(const QString &instanceUrl);

private:
    std::shared_mutex mutex_;

    std::unordered_map<QString, std::unordered_map<QString, std::vector<Badge>>>
        userBadges;

    ThreadGuard tgBadges;
};

}  // namespace chatterino
