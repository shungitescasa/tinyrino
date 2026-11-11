#include "controllers/tinyemotesinstances/TinyemotesInstanceModel.hpp"

#include "controllers/tinyemotesinstances/TinyemotesInstance.hpp"
#include "util/StandardItemHelper.hpp"

#include <QIcon>
#include <QPixmap>

namespace chatterino {

// commandmodel
TinyemotesInstanceModel ::TinyemotesInstanceModel(QObject *parent)
    : SignalVectorModel<TinyemotesInstance>(5, parent)
{
}

// turn a vector item into a model row
TinyemotesInstance TinyemotesInstanceModel::getItemFromRow(
    std::vector<QStandardItem *> &row, const TinyemotesInstance &original,
    std::optional<QModelIndex> index, std::optional<int> role)
{
    return {row[Column::Url]->data(Qt::DisplayRole).toString(),
            row[Column::GlobalEmotes]->data(Qt::CheckStateRole).toBool(),
            row[Column::ChannelEmotes]->data(Qt::CheckStateRole).toBool(),
            row[Column::Avatars]->data(Qt::CheckStateRole).toBool(),
            row[Column::Badges]->data(Qt::CheckStateRole).toBool()};
}

// turns a row in the model into a vector item
void TinyemotesInstanceModel::getRowFromItem(const TinyemotesInstance &item,
                                             std::vector<QStandardItem *> &row)
{
    setStringItem(row[Column::Url], item.getUrl());
    setBoolItem(row[Column::GlobalEmotes], item.isGlobalEmotesEnabled());
    setBoolItem(row[Column::ChannelEmotes], item.isChannelEmotesEnabled());
    setBoolItem(row[Column::Avatars], item.isAvatarEnabled());
    setBoolItem(row[Column::Badges], item.isBadgeEnabled());
}

}  // namespace chatterino
