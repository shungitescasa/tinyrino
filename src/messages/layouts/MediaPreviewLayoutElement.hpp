#pragma once

#include "messages/layouts/MessageLayoutElement.hpp"

#include <QString>

#include <memory>

namespace chatterino {

class MediaPreview;
using MediaPreviewPtr = std::shared_ptr<MediaPreview>;
struct MessageLayoutContainer;

bool canShowMediaPreview(const MediaPreviewPtr &preview);

bool addMediaPreviewToContainer(MessageElement &creator,
                                const MediaPreviewPtr &preview,
                                const QString &copyText,
                                MessageLayoutContainer &container);

void updateMediaPreviewHover(const MessageLayoutElement *hovered);

class MediaPreviewLayoutElement;

const MediaPreviewLayoutElement *asMediaPlayer(
    const MessageLayoutElement *element);

class MediaPreviewLayoutElement : public MessageLayoutElement
{
public:
    MediaPreviewLayoutElement(MessageElement &creator, MediaPreviewPtr preview,
                              QSizeF size, QString copyText, float scale);

    const MediaPreviewPtr &preview() const;

    bool isPlayer() const;

    QRectF seekBarRect() const;

    bool isOnSeekBar(QPointF pos) const;

protected:
    void addCopyTextToString(QString &str, uint32_t from = 0,
                             uint32_t to = UINT32_MAX) const override;
    size_t getSelectionIndexCount() const override;
    void paint(QPainter &painter, const MessageColors &messageColors) override;
    bool paintAnimated(QPainter &painter, qreal yOffset) override;
    int getMouseOverIndex(QPointF abs) const override;
    qreal getXFromIndex(size_t index) override;

private:
    bool isStatic() const;

    void paintImage(QPainter &painter, const QRectF &rect) const;
    void paintVideo(QPainter &painter, const QRectF &rect) const;
    void paintAudio(QPainter &painter, const QRectF &rect) const;
    void paintLabel(QPainter &painter, const QRectF &rect,
                    const QString &label) const;
    void paintSeekBar(QPainter &painter, const QRectF &rect) const;

    MediaPreviewPtr preview_;
    QString copyText_;
    float scale_;
};

}  // namespace chatterino
