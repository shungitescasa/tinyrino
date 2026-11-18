#pragma once

#include <pajlada/signals/signalholder.hpp>
#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <unordered_map>
#include <vector>

class QNetworkAccessManager;
class QNetworkRequest;
class QTimer;
class QImage;
#ifdef TINYRINO_WITH_MULTIMEDIA
class QMediaPlayer;
class QVideoSink;
class QVideoFrame;
#endif

namespace chatterino {

class MediaPlaybackSession;

class MediaPreview
{
public:
    enum class Kind : std::uint8_t {
        Unknown,

        None,

        Image,
        Video,
        Audio,
    };

    using Clock = std::chrono::steady_clock;

    explicit MediaPreview(QString url, std::uint32_t generation);

    MediaPreview(const MediaPreview &) = delete;
    MediaPreview(MediaPreview &&) = delete;
    MediaPreview &operator=(const MediaPreview &) = delete;
    MediaPreview &operator=(MediaPreview &&) = delete;
    ~MediaPreview() = default;

    const QString &url() const;
    Kind kind() const;
    bool isResolved() const;
    bool isPlayerKind() const;
    std::uint32_t generation() const;
    const QString &title() const;
    const QPixmap &frame() const;
    QSize mediaSize() const;
    bool isAnimated() const;
    const QString &formatLabel() const;
    qint64 duration() const;
    qint64 position() const;
    bool isPlaying() const;
    bool isLoading() const;
    bool hasPlaybackError() const;
    bool isConverting() const;
    const QString &errorText() const;
    bool shouldRetry() const;

private:
    friend class MediaPreviewManager;
    friend class MediaPlaybackSession;
    friend class AnimatedImageSession;
    friend class MultimediaSession;

    const QString url_;
    const std::uint32_t generation_;

    Kind kind_ = Kind::Unknown;
    QString title_;

    bool resolvedFromCache_ = false;
    bool resolveStarted_ = false;

    QString imageUrl_;

    QByteArray imageData_;
    bool imageLoading_ = false;
    bool animated_ = false;
    QString formatLabel_;

    QString mediaFile_;
    bool mediaLoading_ = false;
    bool converting_ = false;
    QString errorText_;

    QPixmap frame_;
    QSize mediaSize_;
    qint64 duration_ = 0;
    qint64 position_ = 0;
    bool playing_ = false;
    bool playbackError_ = false;
    bool posterRequested_ = false;

    Clock::time_point lastPainted_{};

    bool retry_ = false;
    Clock::time_point retryAfter_{};
};

using MediaPreviewPtr = std::shared_ptr<MediaPreview>;

class MediaPreviewManager : public QObject
{
    Q_OBJECT

public:
    static constexpr int MAX_PREVIEW_SIZE = 256;
    static constexpr int PREVIEW_GAP = 8;

    static MediaPreviewManager &instance();
    static MediaPreviewManager *existingInstance();
    ~MediaPreviewManager() override;

    static bool shouldPreview(const QString &url);
    static bool isKindEnabled(MediaPreview::Kind kind);
    static bool canPlayMedia();
    static bool previewsAtEnd();
    static qint64 maxMediaBytes();
    MediaPreviewPtr get(const QString &url);
    void ensureResolving(const MediaPreviewPtr &preview);
    std::uint32_t generation() const;
    void requestPoster(const MediaPreviewPtr &preview);
    void notifyPainted(const MediaPreviewPtr &preview);
    void setHovered(const MediaPreviewPtr &preview);
    void togglePlayback(const MediaPreviewPtr &preview);
    void seek(const MediaPreviewPtr &preview, double fraction);
    void requestRepaint();
    void requestLayout();
    void loadImage(const MediaPreviewPtr &preview);
    void loadMediaFile(const MediaPreviewPtr &preview);
    void onMediaFileReady(const MediaPreviewPtr &preview, const QString &path);

    void failMediaFile(const MediaPreviewPtr &preview,
                       const QString &errorText);
#ifdef TINYRINO_WITH_MULTIMEDIA
    void convertMedia(const MediaPreviewPtr &preview, const QString &path);
#endif
    void onSessionError(MediaPreview *preview);

private:
    MediaPreviewManager();

    using FetchCallback =
        std::function<void(bool ok, QString path, QByteArray data)>;

    void fetchData(const QString &url, bool wantData, FetchCallback callback);
    void download(const QString &url);
    void finishFetch(const QString &url, bool ok, const QString &path,
                     const QByteArray &data);

    void resolve(const MediaPreviewPtr &preview);
    void resolveFromNetwork(const MediaPreviewPtr &preview);
    void fetchOpenGraph(const std::weak_ptr<MediaPreview> &weak);

    void finishWithMedia(const MediaPreviewPtr &preview,
                         MediaPreview::Kind kind, const QString &title = {});

    void finishWithImage(const MediaPreviewPtr &preview,
                         const QString &imageUrl, const QString &title = {});

    void finishWithNothing(MediaPreview &preview, bool definitive,
                           bool temporary = false);

    void scheduleRetry(MediaPreview &preview);
    void onImageDecoded(const std::weak_ptr<MediaPreview> &weak, QImage image,
                        QSize naturalSize, bool animated, QString format,
                        QByteArray data);
    void onImageFailed(MediaPreview &preview);

    void dropPreview(MediaPreview &preview);
    QNetworkRequest makeRequest(const QUrl &url) const;

    bool matchesFilters(const QUrl &url);
    void rebuildFilters();

    static bool isPlayable(const MediaPreview &preview);

    static bool wantsAutoplay(const MediaPreview &preview);

    static bool isAppActive();

    void stopAll();

    static bool userMuted(const MediaPreview &preview);
    MediaPlaybackSession *findSession(const MediaPreview *preview) const;
    MediaPlaybackSession *createSession(const MediaPreviewPtr &preview);

    MediaPlaybackSession *userSession(const MediaPreviewPtr &preview);
    bool hasPlayerSession() const;
    void stopPlayerSessions(const MediaPreview *except);
    std::size_t countAutoplaying(MediaPreview::Kind kind) const;
    void processAutoplayQueue();

    void collectSessions();

#ifdef TINYRINO_WITH_MULTIMEDIA
    void processNextPoster();
    void finishPoster();
    void onPosterFrame(const QVideoFrame &frame);
    void ensurePosterPlayer();
#endif

    QNetworkAccessManager *network_;

    QHash<QString, std::weak_ptr<MediaPreview>> cache_;
    std::uint32_t generation_ = 1;

    QHash<QString, std::vector<std::pair<bool, FetchCallback>>> fetches_;

    QStringList whitelist_;
    QStringList blacklist_;
    bool filtersDirty_ = true;

    bool layoutQueued_ = false;
    bool repaintQueued_ = false;

    std::unordered_map<const MediaPreview *,
                       std::unique_ptr<MediaPlaybackSession>>
        sessions_;
    MediaPreviewPtr hovered_;
    std::vector<std::weak_ptr<MediaPreview>> autoplayQueue_;
    bool autoplayQueued_ = false;
    QTimer *collectTimer_ = nullptr;
    int collectTicks_ = 0;

#ifdef TINYRINO_WITH_MULTIMEDIA

    QMediaPlayer *posterPlayer_ = nullptr;
    QVideoSink *posterSink_ = nullptr;
    QTimer *posterTimeout_ = nullptr;
    std::deque<std::weak_ptr<MediaPreview>> posterQueue_;
    MediaPreviewPtr posterCurrent_;
    bool posterBusy_ = false;
#endif

    pajlada::Signals::SignalHolder signalHolder_;
};

}  // namespace chatterino
