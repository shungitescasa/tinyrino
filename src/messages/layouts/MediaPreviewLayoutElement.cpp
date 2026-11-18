#include "messages/layouts/MediaPreviewLayoutElement.hpp"

#include "Application.hpp"
#include "messages/layouts/MessageLayoutContainer.hpp"
#include "messages/MessageElement.hpp"
#include "providers/mediapreview/MediaPreview.hpp"
#include "singletons/Fonts.hpp"
#include "singletons/Theme.hpp"

#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace {

using namespace chatterino;
using Kind = MediaPreview::Kind;

constexpr qreal AUDIO_HEIGHT = 52;
constexpr qreal AUDIO_CONTROLS_HEIGHT = 42;
constexpr qreal CORNER_RADIUS = 4;

QString formatTime(qint64 ms)
{
    auto total = std::max<qint64>(ms, 0) / 1000;
    auto hours = total / 3600;
    auto minutes = (total % 3600) / 60;
    auto seconds = total % 60;
    if (hours > 0)
    {
        return QStringLiteral("%1:%2:%3")
            .arg(hours)
            .arg(minutes, 2, 10, QChar(u'0'))
            .arg(seconds, 2, 10, QChar(u'0'));
    }
    return QStringLiteral("%1:%2").arg(minutes).arg(seconds, 2, 10,
                                                     QChar(u'0'));
}

void drawPlayIcon(QPainter &painter, QPointF center, qreal size)
{
    QPainterPath path;
    path.moveTo(center.x() - size * 0.35, center.y() - size * 0.5);
    path.lineTo(center.x() + size * 0.55, center.y());
    path.lineTo(center.x() - size * 0.35, center.y() + size * 0.5);
    path.closeSubpath();
    painter.fillPath(path, Qt::white);
}

void drawPauseIcon(QPainter &painter, QPointF center, qreal size)
{
    const qreal barWidth = size * 0.3;
    const qreal gap = size * 0.2;
    painter.fillRect(QRectF(center.x() - gap / 2 - barWidth,
                            center.y() - size / 2, barWidth, size),
                     Qt::white);
    painter.fillRect(
        QRectF(center.x() + gap / 2, center.y() - size / 2, barWidth, size),
        Qt::white);
}

void drawPlayOverlay(QPainter &painter, const QRectF &rect, qreal scale)
{
    const qreal radius =
        std::min({18 * scale, rect.width() / 4, rect.height() / 4});
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 150));
    painter.drawEllipse(rect.center(), radius, radius);
    drawPlayIcon(painter, rect.center(), radius);
}

QRectF seekBarFor(const QRectF &rect, qreal scale)
{
    const qreal inset = 8 * scale;
    const qreal height = 4 * scale;
    const qreal centerY = rect.bottom() - 10 * scale;
    return {rect.left() + inset, centerY - height / 2,
            std::max(rect.width() - 2 * inset, 1.0), height};
}

void drawFrame(QPainter &painter, const QRectF &rect, const QPixmap &frame)
{
    if (frame.isNull())
    {
        return;
    }
    auto target = QSizeF(frame.size()).scaled(rect.size(), Qt::KeepAspectRatio);
    QRectF targetRect(QPointF(), target);
    targetRect.moveCenter(rect.center());
    painter.drawPixmap(targetRect, frame, QRectF(frame.rect()));
}

void drawTitle(QPainter &painter, const QRectF &rect, const QString &title,
               qreal scale)
{
    if (title.isEmpty())
    {
        return;
    }

    auto font = getApp()->getFonts()->getFont(FontStyle::ChatSmall, scale);
    QFontMetricsF metrics(font);
    const qreal pad = 6 * scale;
    const QRectF band(rect.left(), rect.top(), rect.width(),
                      metrics.height() + pad * 2);

    QLinearGradient gradient(band.topLeft(), band.bottomLeft());
    gradient.setColorAt(0, QColor(0, 0, 0, 170));
    gradient.setColorAt(1, QColor(0, 0, 0, 0));
    painter.fillRect(band, gradient);

    const qreal textWidth = band.width() - pad * 2;
    if (textWidth <= 0)
    {
        return;
    }
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(QRectF(band.left() + pad, band.top() + pad / 2,
                            textWidth, metrics.height()),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     metrics.elidedText(title, Qt::ElideRight, textWidth));
}

QSizeF fitPreviewSize(QSizeF size, qreal maxSide, qreal availableWidth)
{
    const QSizeF box(std::min(maxSide, std::max(availableWidth, 1.0)),
                     maxSide);
    if (size.width() > box.width() || size.height() > box.height())
    {
        size.scale(box, Qt::KeepAspectRatio);
    }
    return size;
}

}  // namespace

namespace chatterino {

bool canShowMediaPreview(const MediaPreviewPtr &preview)
{
    if (!preview || !preview->isResolved() ||
        !MediaPreviewManager::isKindEnabled(preview->kind()))
    {
        return false;
    }

    switch (preview->kind())
    {
        case Kind::Image:
            return !preview->frame().isNull() &&
                   !preview->mediaSize().isEmpty();
        case Kind::Video:
        case Kind::Audio:
            return true;
        default:
            return false;
    }
}

bool addMediaPreviewToContainer(MessageElement &creator,
                                const MediaPreviewPtr &preview,
                                const QString &copyText,
                                MessageLayoutContainer &container)
{
    if (!canShowMediaPreview(preview))
    {
        return false;
    }

    const float scale = container.getScale();
    const qreal maxSide = MediaPreviewManager::MAX_PREVIEW_SIZE * scale;

    auto computeSize = [&](qreal available) -> QSizeF {
        switch (preview->kind())
        {
            case Kind::Image:
                return fitPreviewSize(QSizeF(preview->mediaSize()) * scale,
                                      maxSide, available);

            case Kind::Video: {
                QSizeF natural = preview->mediaSize().isEmpty()
                                     ? QSizeF(maxSide, maxSide * 9 / 16)
                                     : QSizeF(preview->mediaSize()) * scale;
                auto size = fitPreviewSize(natural, maxSide, available);
                const qreal minSide = 64 * scale;
                if (size.width() < minSide || size.height() < minSide)
                {
                    size.scale(QSizeF(minSide, minSide),
                               Qt::KeepAspectRatioByExpanding);
                }
                return size;
            }

            default:
                return {std::min(maxSide, std::max(available, 1.0)),
                        AUDIO_HEIGHT * scale};
        }
    };

    const bool continuesRow =
        !container.atStartOfLine() &&
        dynamic_cast<const MediaPreviewLayoutElement *>(
            container.lastElement()) != nullptr;

    QSizeF size;
    if (continuesRow)
    {
        size = computeSize(maxSide);
        if (!container.fitsInLine(size.width()))
        {
            container.breakLine();
            size = computeSize(container.remainingWidth());
        }
    }
    else
    {
        if (!container.atStartOfLine())
        {
            container.breakLine();
        }
        size = computeSize(container.remainingWidth());
    }

    container.addElement(new MediaPreviewLayoutElement(creator, preview, size,
                                                       copyText, scale));
    container.addSpacing(MediaPreviewManager::PREVIEW_GAP * scale);
    return true;
}

void updateMediaPreviewHover(const MessageLayoutElement *hovered)
{
    const auto *element =
        dynamic_cast<const MediaPreviewLayoutElement *>(hovered);
    if (element)
    {
        MediaPreviewManager::instance().setHovered(element->preview());
    }
    else if (auto *manager = MediaPreviewManager::existingInstance())
    {
        manager->setHovered(nullptr);
    }
}

const MediaPreviewLayoutElement *asMediaPlayer(
    const MessageLayoutElement *element)
{
    const auto *preview =
        dynamic_cast<const MediaPreviewLayoutElement *>(element);
    if (preview && preview->isPlayer())
    {
        return preview;
    }
    return nullptr;
}

MediaPreviewLayoutElement::MediaPreviewLayoutElement(MessageElement &creator,
                                                     MediaPreviewPtr preview,
                                                     QSizeF size,
                                                     QString copyText,
                                                     float scale)
    : MessageLayoutElement(creator, size)
    , preview_(std::move(preview))
    , copyText_(std::move(copyText))
    , scale_(scale)
{
    this->trailingSpace = false;
}

const MediaPreviewPtr &MediaPreviewLayoutElement::preview() const
{
    return this->preview_;
}

bool MediaPreviewLayoutElement::isPlayer() const
{
    return this->preview_->isPlayerKind();
}

QRectF MediaPreviewLayoutElement::seekBarRect() const
{
    auto bar = seekBarFor(this->getRect(), this->scale_);
    return bar.adjusted(0, -8 * this->scale_, 0, 6 * this->scale_);
}

bool MediaPreviewLayoutElement::isOnSeekBar(QPointF pos) const
{
    return this->isPlayer() && this->preview_->duration() > 0 &&
           this->seekBarRect().contains(pos);
}

void MediaPreviewLayoutElement::addCopyTextToString(QString &str,
                                                    uint32_t /*from*/,
                                                    uint32_t to) const
{
    str += this->copyText_;
    if (to >= 2)
    {
        str += ' ';
    }
}

size_t MediaPreviewLayoutElement::getSelectionIndexCount() const
{
    return 2;
}

bool MediaPreviewLayoutElement::isStatic() const
{
    return this->preview_->kind() == Kind::Image &&
           !this->preview_->isAnimated();
}

void MediaPreviewLayoutElement::paint(QPainter &painter,
                                      const MessageColors & /*messageColors*/)
{
    if (!this->isStatic())
    {
        return;
    }

    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    drawFrame(painter, this->getRect(), this->preview_->frame());
}

bool MediaPreviewLayoutElement::paintAnimated(QPainter &painter, qreal yOffset)
{
    if (this->isStatic())
    {
        return false;
    }

    auto &manager = MediaPreviewManager::instance();
    manager.notifyPainted(this->preview_);
    if (this->preview_->kind() == Kind::Video)
    {
        manager.requestPoster(this->preview_);
    }

    auto rect = this->getRect();
    rect.moveTop(rect.y() + yOffset);

    painter.save();
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);

    switch (this->preview_->kind())
    {
        case Kind::Image:
            this->paintImage(painter, rect);
            break;
        case Kind::Video: {
            QPainterPath clip;
            clip.addRoundedRect(rect, CORNER_RADIUS * this->scale_,
                                CORNER_RADIUS * this->scale_);
            painter.setClipPath(clip);
            this->paintVideo(painter, rect);
        }
        break;
        default: {
            QPainterPath clip;
            clip.addRoundedRect(rect, CORNER_RADIUS * this->scale_,
                                CORNER_RADIUS * this->scale_);
            painter.setClipPath(clip);
            this->paintAudio(painter, rect);
        }
        break;
    }

    painter.restore();
    return true;
}

void MediaPreviewLayoutElement::paintLabel(QPainter &painter,
                                           const QRectF &rect,
                                           const QString &label) const
{
    const qreal s = this->scale_;
    auto font = getApp()->getFonts()->getFont(FontStyle::ChatSmall, s);
    QFontMetricsF metrics(font);
    const qreal pad = 3 * s;
    QRectF labelRect(0, 0, metrics.horizontalAdvance(label) + pad * 2,
                     metrics.height() + pad);
    const qreal bottomOffset = this->isPlayer() ? 18 * s : 6 * s;
    labelRect.moveBottomLeft(
        QPointF(rect.left() + 6 * s, rect.bottom() - bottomOffset));
    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(0, 0, 0, 170));
    painter.drawRoundedRect(labelRect, 3 * s, 3 * s);
    painter.setFont(font);
    painter.setPen(Qt::white);
    painter.drawText(labelRect, Qt::AlignCenter, label);
}

void MediaPreviewLayoutElement::paintImage(QPainter &painter,
                                           const QRectF &rect) const
{
    const auto &preview = *this->preview_;
    drawFrame(painter, rect, preview.frame());

    if (!preview.isPlaying())
    {
        drawPlayOverlay(painter, rect, this->scale_);
        paintLabel(painter, rect,
                   preview.formatLabel().isEmpty() ? QStringLiteral("GIF")
                                                   : preview.formatLabel());
    }
}

void MediaPreviewLayoutElement::paintSeekBar(QPainter &painter,
                                             const QRectF &rect) const
{
    const auto &preview = *this->preview_;
    if (preview.duration() <= 0)
    {
        return;
    }

    const qreal s = this->scale_;
    auto bar = seekBarFor(rect, s);
    const qreal radius = bar.height() / 2;

    painter.setPen(Qt::NoPen);
    painter.setBrush(QColor(255, 255, 255, 70));
    painter.drawRoundedRect(bar, radius, radius);

    auto progress = std::clamp(static_cast<qreal>(preview.position()) /
                                   static_cast<qreal>(preview.duration()),
                               0.0, 1.0);
    QRectF filled = bar;
    filled.setWidth(bar.width() * progress);
    painter.setBrush(getTheme()->accent);
    painter.drawRoundedRect(filled, radius, radius);

    const qreal knobRadius = 5 * s;
    painter.setBrush(Qt::white);
    painter.drawEllipse(QPointF(filled.right(), bar.center().y()), knobRadius,
                        knobRadius);
}

void MediaPreviewLayoutElement::paintVideo(QPainter &painter,
                                           const QRectF &rect) const
{
    const auto &preview = *this->preview_;
    const qreal s = this->scale_;

    painter.fillRect(rect, Qt::black);
    drawFrame(painter, rect, preview.frame());

    if (!preview.isPlaying())
    {
        drawPlayOverlay(painter, rect, s);
        drawTitle(painter, rect, preview.title(), s);
    }

    QString label;
    if (preview.hasPlaybackError())
    {
        label = preview.errorText().isEmpty() ? QStringLiteral("Can't play video")
                                              : preview.errorText();
    }
    else if (preview.isConverting())
    {
        label = QStringLiteral("Converting...");
    }
    else if (!MediaPreviewManager::canPlayMedia())
    {
        label = QStringLiteral("Video");
    }
    else if (preview.isLoading() && preview.isPlaying())
    {
        label = QStringLiteral("Loading...");
    }
    else if (preview.duration() > 0)
    {
        label = formatTime(preview.position()) + " / " +
                formatTime(preview.duration());
    }
    if (!label.isEmpty())
    {
        this->paintLabel(painter, rect, label);
    }

    if (preview.duration() > 0)
    {
        QRectF shade(rect.left(), rect.bottom() - 20 * s, rect.width(),
                     20 * s);
        QLinearGradient gradient(shade.topLeft(), shade.bottomLeft());
        gradient.setColorAt(0, QColor(0, 0, 0, 0));
        gradient.setColorAt(1, QColor(0, 0, 0, 140));
        painter.fillRect(shade, gradient);
    }
    this->paintSeekBar(painter, rect);
}

void MediaPreviewLayoutElement::paintAudio(QPainter &painter,
                                           const QRectF &rect) const
{
    const auto &preview = *this->preview_;
    const qreal s = this->scale_;

    painter.fillRect(rect, QColor(32, 32, 36, 235));

    const qreal controlsCenterY = rect.top() + AUDIO_CONTROLS_HEIGHT * s / 2;

    const qreal radius = 13 * s;
    const QPointF buttonCenter(rect.left() + 8 * s + radius, controlsCenterY);
    painter.setPen(Qt::NoPen);
    painter.setBrush(getTheme()->accent);
    painter.drawEllipse(buttonCenter, radius, radius);
    if (preview.isPlaying())
    {
        drawPauseIcon(painter, buttonCenter, radius * 0.8);
    }
    else
    {
        drawPlayIcon(painter, buttonCenter, radius * 0.8);
    }

    const qreal textLeft = buttonCenter.x() + radius + 8 * s;
    const qreal textWidth = rect.right() - textLeft - 8 * s;
    if (textWidth > 0)
    {
        auto titleFont =
            getApp()->getFonts()->getFont(FontStyle::ChatMediumSmall, s);
        auto infoFont = getApp()->getFonts()->getFont(FontStyle::ChatSmall, s);
        QFontMetricsF titleMetrics(titleFont);
        QFontMetricsF infoMetrics(infoFont);

        const qreal totalHeight = titleMetrics.height() + infoMetrics.height();
        const qreal top = controlsCenterY - totalHeight / 2;

        painter.setFont(titleFont);
        painter.setPen(Qt::white);
        auto title = preview.title().isEmpty() ? QStringLiteral("Audio")
                                               : preview.title();
        painter.drawText(
            QRectF(textLeft, top, textWidth, titleMetrics.height()),
            Qt::AlignLeft | Qt::AlignVCenter,
            titleMetrics.elidedText(title, Qt::ElideMiddle, textWidth));

        QString info;
        if (preview.hasPlaybackError())
        {
            info = preview.errorText().isEmpty()
                       ? QStringLiteral("Can't play audio")
                       : preview.errorText();
        }
        else if (preview.isConverting())
        {
            info = QStringLiteral("Converting...");
        }
        else if (!MediaPreviewManager::canPlayMedia())
        {
            info = QStringLiteral("Audio");
        }
        else if (preview.isLoading())
        {
            info = QStringLiteral("Loading...");
        }
        else if (preview.duration() > 0)
        {
            info = formatTime(preview.position()) + " / " +
                   formatTime(preview.duration());
        }
        else
        {
            info = QStringLiteral("Click to play");
        }

        painter.setFont(infoFont);
        painter.setPen(QColor(255, 255, 255, 170));
        painter.drawText(QRectF(textLeft, top + titleMetrics.height(),
                                textWidth, infoMetrics.height()),
                         Qt::AlignLeft | Qt::AlignVCenter, info);
    }

    this->paintSeekBar(painter, rect);
}

int MediaPreviewLayoutElement::getMouseOverIndex(QPointF abs) const
{
    if (abs.x() >= this->getRect().center().x())
    {
        return static_cast<int>(this->getSelectionIndexCount());
    }
    return 0;
}

qreal MediaPreviewLayoutElement::getXFromIndex(size_t index)
{
    if (index == 0)
    {
        return this->getRect().left();
    }
    return this->getRect().right();
}

}  // namespace chatterino
