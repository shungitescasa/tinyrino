#include "widgets/settingspages/TinyrinoPage.hpp"

#include "Application.hpp"
#include "controllers/tinyemotesinstances/TinyemotesInstanceModel.hpp"
#include "providers/mediapreview/MediaPreview.hpp"
#include "singletons/Settings.hpp"
#include "singletons/WindowManager.hpp"
#include "util/LayoutCreator.hpp"
#include "widgets/helper/EditableModelView.hpp"
#include "widgets/settingspages/GeneralPageView.hpp"
#include "widgets/settingspages/SettingWidget.hpp"

#include <QHeaderView>
#include <QTableView>

#include <utility>

namespace chatterino {

TinyrinoPage::TinyrinoPage()
    : view(GeneralPageView::withoutNavigation(this))
{
    auto *y = new QVBoxLayout;
    auto *x = new QHBoxLayout;
    x->addWidget(this->view);
    auto *z = new QFrame;
    z->setLayout(x);
    y->addWidget(z);
    this->setLayout(y);

    this->initLayout(*this->view);
}

void TinyrinoPage::initLayout(GeneralPageView &layout)
{
    auto *s = getSettings();

    {
        layout.addTitle("TinyEmotes");

        SettingWidget::checkbox("Prefer role badges over custom badges",
                                s->preferTinyRoleBadgesOverCustom)
            ->addTo(layout);
    }

    {
        layout.addTitle("Embedded media");
        layout.addDescription(
            "Show images (including animated GIF and WEBP), videos and audio "
            "from links right in chat, up to 256x256. Hover over embedded "
            "media to see its title and link.");
        layout.addDescription(
            "Images: click to open the link. Animated images play on their "
            "own or while hovered.\n"
            "Videos & audio: left click to play or pause (only one plays at "
            "a time), click or drag the bar at the bottom to seek, right "
            "click for the link menu.");
        layout.addDescription(
            "Media is only loaded for messages you can see and is cached on "
            "disk. Playback stops when the media is scrolled out of view or "
            "the app loses focus. Formats the system can't play (e.g. WebM) "
            "are converted if ffmpeg is installed.");

        SettingWidget::checkbox("Enable embedded media", s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::checkbox("Use OpenGraph thumbnails of web pages",
                                s->mediaPreviewOpenGraph)
            ->setTooltip("Loads the web page and embeds its og:image.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::checkbox("Embed direct media links",
                                s->mediaPreviewDirectMedia)
            ->setTooltip("Checks the Content-Type of the link with a HEAD "
                         "request and loads images, videos and audio files "
                         "directly.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        auto *video = SettingWidget::checkbox("Show video players",
                                              s->mediaPreviewVideo)
                          ->setTooltip("Replace links to video/* files with a "
                                       "video player.")
                          ->conditionallyEnabledBy(s->mediaPreviewEnabled);
        auto *audio = SettingWidget::checkbox("Show audio players",
                                              s->mediaPreviewAudio)
                          ->setTooltip("Replace links to audio/* files with an "
                                       "audio player.")
                          ->conditionallyEnabledBy(s->mediaPreviewEnabled);
        if (!MediaPreviewManager::canPlayMedia())
        {
            video = video->setDescription(
                "This build has no Qt Multimedia - videos and audio can't be "
                "played.");
        }
        video->addTo(layout);
        audio->addTo(layout);

        SettingWidget::checkbox("Enable sound for videos",
                                s->mediaPreviewVideoSound)
            ->setTooltip("Play the sound of videos you start. Auto-played "
                         "videos are muted until you click them.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::checkbox("Auto-play videos", s->mediaPreviewAutoplayVideo)
            ->setTooltip("Play a visible video automatically (muted). Only "
                         "one video plays at a time.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::checkbox("Auto-play animated images (GIF, WEBP)",
                                s->mediaPreviewAutoplayAnimated)
            ->setTooltip("Play visible animated images automatically. "
                         "Otherwise, they play while they're hovered.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::intInput("Maximum media size (MB)",
                                s->mediaPreviewMaxSizeMb,
                                {.min = 1, .max = 1024, .singleStep = 1})
            ->setTooltip("Images, videos and audio files larger than this "
                         "are shown as links.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::checkbox("Show media at the end of messages",
                                s->mediaPreviewAtEnd)
            ->setTooltip("Show all embedded media of a message in a row at "
                         "its end and remove their links from the text. "
                         "Links without media stay in the text.")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::lineEdit("Whitelist", s->mediaPreviewWhitelist,
                                "e.g. imgur.com, cdn.discordapp.com")
            ->setTooltip(
                "Only embed links matching one of these entries. Leave "
                "empty to allow all links. If set, the blacklist is "
                "ignored.\n"
                "Entries are separated by commas or spaces and can be a "
                "domain (includes subdomains), a domain with a path prefix "
                "(example.com/media/) or a wildcard pattern "
                "(*.example.com/*.png).")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);

        SettingWidget::lineEdit("Blacklist", s->mediaPreviewBlacklist,
                                "e.g. example.com, *.gif")
            ->setTooltip(
                "Never embed links matching one of these entries. Only "
                "used when the whitelist is empty.\n"
                "Entries are separated by commas or spaces and can be a "
                "domain (includes subdomains), a domain with a path prefix "
                "(example.com/media/) or a wildcard pattern "
                "(*.example.com/*.png).")
            ->conditionallyEnabledBy(s->mediaPreviewEnabled)
            ->addTo(layout);
    }

    {
        layout.addTitle("Encryption");
        layout.addDescription("Encrypt your messages");

        SettingWidget::checkbox("Enable message encryption",
                                s->enableMessageEncryption)
            ->addTo(layout);

        SettingWidget::checkbox("Add random spaces after encryption",
                                s->randomSpaces)
            ->addTo(layout);

        SettingWidget::dropdown("Encryption encoding",
                                s->messageEncryptionEncoding)
            ->addTo(layout);

        SettingWidget::lineEdit("Password", s->messagePassword)->addTo(layout);
    }

    layout.addStretch();
}

void TinyrinoPage::onShow()
{
}

}  // namespace chatterino
