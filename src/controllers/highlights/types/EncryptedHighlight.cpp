// SPDX-FileCopyrightText: 2026 Contributors to Chatterino <https://chatterino.com>
//
// SPDX-License-Identifier: MIT

#include "controllers/highlights/types/EncryptedHighlight.hpp"

#include "controllers/highlights/HighlightCheck.hpp"
#include "controllers/highlights/HighlightResult.hpp"
#include "messages/MessageFlag.hpp"

namespace chatterino::highlights {

HighlightCheck EncryptedHighlight::buildCheck() const
{
    using Params = HighlightCheckParams;

    return {
        [highlight = *this](const Params &p) -> std::optional<HighlightResult> {
            if (!p.messageFlags.has(MessageFlag::Encrypted))
            {
                return std::nullopt;
            }

            // Color-only highlight: ignore any mention/alert/sound config
            return HighlightResult{
                .alert = false,
                .sound = {},
                .color = highlight.outcome.getBackgroundColor(),
                .showInMentions = false,
            };
        },
    };
}

QDebug operator<<(QDebug dbg, const EncryptedHighlight &v)
{
    dbg.nospace() << "EncryptedHighlight("        //
                  << "name:" << v.name               //
                  << ',' << "enabled:" << v.enabled  //
                  << ',' << "outcome:" << v.outcome  //
                  << ')';

    return dbg;
}

}  // namespace chatterino::highlights
