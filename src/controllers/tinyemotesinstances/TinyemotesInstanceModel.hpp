#pragma once

#include "common/SignalVectorModel.hpp"

#include <QObject>

namespace chatterino {

class TinyemotesInstance;

class TinyemotesInstanceModel : public SignalVectorModel<TinyemotesInstance>
{
public:
    explicit TinyemotesInstanceModel(QObject *parent);

    enum Column {
        Url = 0,
        GlobalEmotes = 1,
        ChannelEmotes = 2,
        Avatars = 3,
        Badges = 4
    };

protected:
    // turn a vector item into a model row
    TinyemotesInstance getItemFromRow(std::vector<QStandardItem *> &row,
                                      const TinyemotesInstance &original,
                                      std::optional<QModelIndex> index,
                                      std::optional<int> role) override;

    // turns a row in the model into a vector item
    void getRowFromItem(const TinyemotesInstance &item,
                        std::vector<QStandardItem *> &row) override;
};

}  // namespace chatterino
