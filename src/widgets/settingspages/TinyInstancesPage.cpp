#include "widgets/settingspages/TinyInstancesPage.hpp"

#include "Application.hpp"
#include "controllers/tinyemotesinstances/TinyemotesInstanceModel.hpp"
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

TinyInstancesPage::TinyInstancesPage()
{
    auto *s = getSettings();

    LayoutCreator<TinyInstancesPage> layoutCreator(this);

    auto layout = layoutCreator.setLayoutType<QVBoxLayout>();
    auto tabs = layout.emplace<QTabWidget>();
    this->tabWidget_ = tabs.getElement();

    auto emoteInstancesTab = tabs.appendTab(new QVBoxLayout, "TinyEmotes");
    {
        emoteInstancesTab.emplace<QLabel>(
            ""
            "Tinyrino allows you to use emotes "
            "from different TinyEmotes instances.\nYou can add an existing "
            "instance here or self-host your own!");
        auto linkLabel = emoteInstancesTab.emplace<QLabel>(
            "<a href='https://github.com/shungitescasa/tinyemotes' "
            "style='color:#99f'>More info...</a>");
        linkLabel->setOpenExternalLinks(true);

        EditableModelView *view =
            emoteInstancesTab
                .emplace<EditableModelView>(
                    (new TinyemotesInstanceModel(nullptr))
                        ->initialized(&s->tinyemotesInstances))
                .getElement();
        this->view_ = view;

        view->setTitles({"Base URL", "Global emotes", "Channel emotes",
                         "Avatars", "Badges"});
        view->getTableView()->horizontalHeader()->setSectionResizeMode(
            QHeaderView::Interactive);
        view->getTableView()->horizontalHeader()->setSectionResizeMode(
            0, QHeaderView::Stretch);

        QTimer::singleShot(1, [view] {
            view->getTableView()->resizeColumnsToContents();
            view->getTableView()->setColumnWidth(1, 125);
            view->getTableView()->setColumnWidth(2, 125);
            view->getTableView()->setColumnWidth(3, 110);
            view->getTableView()->setColumnWidth(4, 110);
        });

        // We can safely ignore this signal connection since we own the view
        std::ignore = view->addButtonPressed.connect([s] {
            s->tinyemotesInstances.append(
                TinyemotesInstance("alright.party", true, true, true, true));
        });
    }
}

void TinyInstancesPage::onShow()
{
}

}  // namespace chatterino
