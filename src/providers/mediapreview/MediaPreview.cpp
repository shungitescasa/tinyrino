#include "providers/mediapreview/MediaPreview.hpp"

#include "Application.hpp"
#include "common/QLogging.hpp"
#include "common/Version.hpp"
#include "singletons/Paths.hpp"
#include "singletons/Settings.hpp"
#include "singletons/StreamerMode.hpp"
#include "singletons/WindowManager.hpp"

#include <QBuffer>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QMovie>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThreadPool>
#include <QTimer>

#ifdef TINYRINO_WITH_MULTIMEDIA
#    include <QAudioOutput>
#    include <QMediaFormat>
#    include <QMediaPlayer>
#    include <QVideoFrame>
#    include <QVideoSink>
#endif

#include <algorithm>
#include <climits>
#include <optional>

namespace {

using namespace chatterino;
using namespace std::chrono_literals;
using Kind = MediaPreview::Kind;
using Clock = MediaPreview::Clock;

constexpr qsizetype MAX_HTML_SIZE = 2 * 1024 * 1024;
constexpr int REQUEST_TIMEOUT_MS = 15000;
constexpr int POSTER_TIMEOUT_MS = 20000;

constexpr int MAX_PIXEL_SIDE = MediaPreviewManager::MAX_PREVIEW_SIZE * 2;

constexpr std::size_t MAX_AUTOPLAY_IMAGES = 24;

constexpr qint64 MAX_CACHE_BYTES = 512LL * 1024 * 1024;

constexpr qint64 META_TTL_SECONDS = 3LL * 24 * 60 * 60;

constexpr auto RETRY_DELAY = 30s;

constexpr auto VISIBILITY_TIMEOUT = 1200ms;

constexpr auto PAUSED_SESSION_TIMEOUT = 10s;

constexpr auto USER_SESSION_TIMEOUT = 60s;

constexpr auto IMAGE_DATA_TIMEOUT = 60s;

QSize storedPixelSize(QSize size)
{
    if (size.width() > MAX_PIXEL_SIDE || size.height() > MAX_PIXEL_SIDE)
    {
        size.scale(MAX_PIXEL_SIDE, MAX_PIXEL_SIDE, Qt::KeepAspectRatio);
    }
    return size.expandedTo({1, 1});
}

QPixmap toPreviewPixmap(QImage image)
{
    if (image.isNull())
    {
        return {};
    }
    auto target = storedPixelSize(image.size());
    if (target != image.size())
    {
        image =
            image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    return QPixmap::fromImage(std::move(image));
}

struct DecodedImage {
    QImage image;
    QSize naturalSize;
    bool animated = false;
    QString format;
};

DecodedImage decodeImage(const QByteArray &data)
{
    DecodedImage out;

    QBuffer buffer;
    buffer.setData(data);
    buffer.open(QIODevice::ReadOnly);

    QImageReader reader(&buffer);
    reader.setAutoTransform(true);

    out.format = QString::fromLatin1(reader.format()).toUpper();
    out.animated = reader.supportsAnimation() && reader.imageCount() > 1;

    auto natural = reader.size();
    if (natural.isValid())
    {
        auto target = storedPixelSize(natural);
        if (target != natural)
        {
            reader.setScaledSize(target);
        }
    }

    auto image = reader.read();
    if (image.isNull())
    {
        return out;
    }

    if (!natural.isValid())
    {
        natural = image.size();
    }

    if ((image.width() > image.height()) !=
        (natural.width() > natural.height()))
    {
        natural.transpose();
    }

    auto target = storedPixelSize(image.size());
    if (target != image.size())
    {
        image =
            image.scaled(target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }

    out.image = std::move(image);
    out.naturalSize = natural;
    return out;
}

template <typename Result>
void runAsync(QObject *context, std::function<Result()> work,
              std::function<void(Result)> then)
{
    QPointer<QObject> guard(context);
    QThreadPool::globalInstance()->start([guard, work, then] {
        Result result = work();
        auto *app = QCoreApplication::instance();
        if (!app)
        {
            return;
        }
        QMetaObject::invokeMethod(
            app,
            [guard, then, result] {
                if (guard)
                {
                    then(result);
                }
            },
            Qt::QueuedConnection);
    });
}

QString &cacheDirectory()
{
    static QString dir;
    return dir;
}

void initCacheDirectory()
{
    if (!cacheDirectory().isEmpty())
    {
        return;
    }

    QString base;
    if (auto *app = tryGetApp())
    {
        base = app->getPaths().cacheDirectory();
    }
    if (base.isEmpty())
    {
        base = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
    }

    QDir dir(base);
    dir.mkpath(QStringLiteral("media"));
    cacheDirectory() = dir.filePath(QStringLiteral("media"));
}

QString cacheKey(const QString &url)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Sha1)
            .toHex());
}

QString findCachedDataFile(const QString &url)
{
    static const QStringList legacySuffixes{
        QStringLiteral(".bin"),
        QStringLiteral(".mpga"),
    };

    QDir dir(cacheDirectory());
    const auto names = dir.entryList({cacheKey(url) + u".*"}, QDir::Files);

    for (const auto &name : names)
    {
        if (name.contains(u".converted.") && !name.endsWith(u".part"))
        {
            return dir.filePath(name);
        }
    }

    for (const auto &name : names)
    {
        if (name.endsWith(u".json") || name.endsWith(u".part"))
        {
            continue;
        }
        if (std::ranges::any_of(legacySuffixes, [&](const auto &suffix) {
                return name.endsWith(suffix);
            }))
        {
            QFile::remove(dir.filePath(name));
            continue;
        }
        return dir.filePath(name);
    }
    return {};
}

QString dataFileSuffix(const QString &contentType, const QUrl &url)
{
    static const QHash<QString, QString> knownSuffixes{

        {"audio/mpeg", "mp3"},       {"audio/mp3", "mp3"},
        {"audio/mpeg3", "mp3"},      {"audio/x-mpeg-3", "mp3"},
        {"audio/mp4", "m4a"},        {"audio/x-m4a", "m4a"},
        {"audio/m4a", "m4a"},        {"audio/aac", "aac"},
        {"audio/x-aac", "aac"},      {"audio/wav", "wav"},
        {"audio/wave", "wav"},       {"audio/x-wav", "wav"},
        {"audio/vnd.wave", "wav"},   {"audio/flac", "flac"},
        {"audio/x-flac", "flac"},    {"audio/aiff", "aiff"},
        {"audio/x-aiff", "aiff"},    {"audio/ogg", "ogg"},
        {"application/ogg", "ogg"},  {"audio/opus", "opus"},
        {"audio/webm", "webm"},

        {"video/mp4", "mp4"},        {"video/x-m4v", "m4v"},
        {"video/quicktime", "mov"},  {"video/webm", "webm"},
        {"video/x-matroska", "mkv"}, {"video/ogg", "ogv"},
        {"video/mpeg", "mpg"},       {"video/3gpp", "3gp"},

        {"image/gif", "gif"},        {"image/webp", "webp"},
        {"image/png", "png"},        {"image/apng", "png"},
        {"image/jpeg", "jpg"},       {"image/jpg", "jpg"},
        {"image/avif", "avif"},      {"image/bmp", "bmp"},
        {"image/svg+xml", "svg"},
    };

    if (auto it = knownSuffixes.find(contentType); it != knownSuffixes.end())
    {
        return *it;
    }

    static const QRegularExpression validSuffix(R"(^[a-z0-9]{1,5}$)");
    auto suffix = QFileInfo(url.path()).suffix().toLower();
    if (suffix != u"bin" && suffix != u"mpga" &&
        validSuffix.match(suffix).hasMatch())
    {
        return suffix;
    }

    static const QMimeDatabase db;
    if (!contentType.isEmpty() && contentType != u"application/octet-stream" &&
        contentType != u"binary/octet-stream")
    {
        auto mime = db.mimeTypeForName(contentType);
        if (mime.isValid() && !mime.preferredSuffix().isEmpty())
        {
            return mime.preferredSuffix();
        }
    }
    return QStringLiteral("media");
}

QString cacheDataPath(const QString &url, const QString &suffix)
{
    return cacheDirectory() + u'/' + cacheKey(url) + u'.' + suffix;
}

QString cacheMetaPath(const QString &url)
{
    return cacheDirectory() + u'/' + cacheKey(url) + u".json";
}

QString kindToString(Kind kind)
{
    switch (kind)
    {
        case Kind::None:
            return QStringLiteral("none");
        case Kind::Image:
            return QStringLiteral("image");
        case Kind::Video:
            return QStringLiteral("video");
        case Kind::Audio:
            return QStringLiteral("audio");
        default:
            return {};
    }
}

Kind kindFromString(const QString &kind)
{
    if (kind == u"none")
    {
        return Kind::None;
    }
    if (kind == u"image")
    {
        return Kind::Image;
    }
    if (kind == u"video")
    {
        return Kind::Video;
    }
    if (kind == u"audio")
    {
        return Kind::Audio;
    }
    return Kind::Unknown;
}

struct CachedMeta {
    Kind kind = Kind::Unknown;
    QString imageUrl;
    QString title;
};

CachedMeta readCachedMeta(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        return {};
    }

    auto root = QJsonDocument::fromJson(file.readAll()).object();
    auto time = root.value(u"time").toInteger();

    if (!root.contains(u"title") ||
        QDateTime::currentSecsSinceEpoch() - time > META_TTL_SECONDS)
    {
        return {};
    }

    return {
        .kind = kindFromString(root.value(u"kind").toString()),
        .imageUrl = root.value(u"imageUrl").toString(),
        .title = root.value(u"title").toString(),
    };
}

void writeCachedMeta(const QString &path, Kind kind, const QString &imageUrl,
                     const QString &title)
{
    QJsonObject root{
        {"kind", kindToString(kind)},
        {"imageUrl", imageUrl},
        {"title", title},
        {"time", QDateTime::currentSecsSinceEpoch()},
    };

    QSaveFile file(path);
    if (file.open(QIODevice::WriteOnly))
    {
        file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
        file.commit();
    }
}

void trimCache(const QString &directory)
{
    QDir dir(directory);
    auto files = dir.entryInfoList(QDir::Files, QDir::Time | QDir::Reversed);

    qint64 total = 0;
    for (const auto &file : files)
    {
        total += file.size();
    }

    for (const auto &file : files)
    {
        if (total <= MAX_CACHE_BYTES)
        {
            break;
        }
        total -= file.size();
        QFile::remove(file.absoluteFilePath());
    }
}

bool isMediaKind(Kind kind)
{
    return kind == Kind::Image || kind == Kind::Video || kind == Kind::Audio;
}

Kind kindFromMimeType(const QString &mimeType)
{
    if (mimeType.startsWith(u"image/"))
    {
        return Kind::Image;
    }
    if (mimeType.startsWith(u"video/"))
    {
        return Kind::Video;
    }
    if (mimeType.startsWith(u"audio/") || mimeType == u"application/ogg")
    {
        return Kind::Audio;
    }
    return Kind::None;
}

QString contentTypeOf(const QNetworkReply *reply)
{
    return reply->header(QNetworkRequest::ContentTypeHeader)
        .toString()
        .section(u';', 0, 0)
        .trimmed()
        .toLower();
}

Kind kindOfReply(const QNetworkReply *reply)
{
    auto contentType = contentTypeOf(reply);
    auto kind = kindFromMimeType(contentType);
    if (kind != Kind::None)
    {
        return kind;
    }

    if (contentType.isEmpty() || contentType == u"application/octet-stream" ||
        contentType == u"binary/octet-stream")
    {
        static QMimeDatabase db;
        const auto mime = db.mimeTypeForFile(reply->url().path(),
                                             QMimeDatabase::MatchExtension);
        return kindFromMimeType(mime.name());
    }

    return Kind::None;
}

QString decodeHtmlEntities(const QString &input)
{
    if (!input.contains(u'&'))
    {
        return input;
    }

    static const QRegularExpression entity(
        R"(&(#[xX][0-9a-fA-F]{1,6}|#[0-9]{1,7}|amp|quot|apos|lt|gt);)");

    QString out;
    out.reserve(input.size());
    qsizetype last = 0;
    auto it = entity.globalMatch(input);
    while (it.hasNext())
    {
        auto match = it.next();
        out += QStringView(input).mid(last, match.capturedStart() - last);
        auto name = match.capturedView(1);
        if (name == u"amp")
        {
            out += u'&';
        }
        else if (name == u"quot")
        {
            out += u'"';
        }
        else if (name == u"apos")
        {
            out += u'\'';
        }
        else if (name == u"lt")
        {
            out += u'<';
        }
        else if (name == u"gt")
        {
            out += u'>';
        }
        else
        {
            bool ok = false;
            const bool hex =
                name.size() > 1 && (name[1] == u'x' || name[1] == u'X');
            auto code =
                hex ? name.mid(2).toUInt(&ok, 16) : name.mid(1).toUInt(&ok, 10);
            if (ok && code > 0 && code <= 0x10FFFF)
            {
                out += QString::fromUcs4(
                    reinterpret_cast<const char32_t *>(&code), 1);
            }
        }
        last = match.capturedEnd();
    }
    out += QStringView(input).mid(last);
    return out;
}

class OpenGraphScanner
{
public:
    explicit OpenGraphScanner(QUrl base)
        : base_(std::move(base))
    {
    }

    bool feed(const QByteArray &chunk)
    {
        this->buffer_.append(chunk);
        this->lower_.append(chunk.toLower());

        this->scanTitleTag();

        while (true)
        {
            auto start = this->lower_.indexOf("<meta", this->scanned_);
            auto headEnd = this->lower_.indexOf("</head", this->scanned_);
            if (headEnd >= 0 && (start < 0 || headEnd < start))
            {
                return true;
            }
            if (start < 0)
            {
                this->scanned_ =
                    std::max(this->scanned_, this->lower_.size() - 6);
                break;
            }
            auto end = this->lower_.indexOf('>', start);
            if (end < 0)
            {
                this->scanned_ = start;
                break;
            }

            this->handleTag(
                QString::fromUtf8(this->buffer_.mid(start, end - start + 1)));
            this->scanned_ = end + 1;

            if (this->bestPriority_ == 0 && !this->ogTitle_.isEmpty())
            {
                return true;
            }
        }

        return this->buffer_.size() >= MAX_HTML_SIZE;
    }

    QString result() const
    {
        return this->best_;
    }

    QString title() const
    {
        for (const auto *title :
             {&this->ogTitle_, &this->twitterTitle_, &this->htmlTitle_})
        {
            if (!title->isEmpty())
            {
                return *title;
            }
        }
        return {};
    }

private:
    static QString cleanTitle(const QString &title)
    {
        return decodeHtmlEntities(title).simplified();
    }

    void scanTitleTag()
    {
        if (this->titleTagDone_)
        {
            return;
        }

        auto start = this->lower_.indexOf("<title", this->titleScanned_);
        if (start < 0)
        {
            this->titleScanned_ = std::max<qsizetype>(this->titleScanned_,
                                                      this->lower_.size() - 7);
            return;
        }

        this->titleScanned_ = start;

        auto open = this->lower_.indexOf('>', start);
        if (open < 0)
        {
            return;
        }
        auto close = this->lower_.indexOf("</title", open);
        if (close < 0)
        {
            return;
        }

        this->htmlTitle_ = cleanTitle(
            QString::fromUtf8(this->buffer_.mid(open + 1, close - open - 1)));
        this->titleTagDone_ = true;
    }

    void handleTag(const QString &tag)
    {
        static const QRegularExpression attribute(
            R"re(([a-zA-Z_:][-a-zA-Z0-9_:.]*)\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s"'=<>`]+)))re");

        QString key;
        QString content;
        auto it = attribute.globalMatch(tag);
        while (it.hasNext())
        {
            auto match = it.next();
            auto name = match.captured(1).toLower();
            QString value;
            for (int i = 2; i <= 4; i++)
            {
                if (match.capturedStart(i) >= 0)
                {
                    value = match.captured(i);
                    break;
                }
            }

            if (name == u"property" || (name == u"name" && key.isEmpty()))
            {
                key = value.trimmed().toLower();
            }
            else if (name == u"content")
            {
                content = value;
            }
        }

        if (content.isEmpty())
        {
            return;
        }

        if (key == u"og:title")
        {
            if (this->ogTitle_.isEmpty())
            {
                this->ogTitle_ = cleanTitle(content);
            }
            return;
        }
        if (key == u"twitter:title")
        {
            if (this->twitterTitle_.isEmpty())
            {
                this->twitterTitle_ = cleanTitle(content);
            }
            return;
        }

        int priority = INT_MAX;
        if (key == u"og:image")
        {
            priority = 0;
        }
        else if (key == u"og:image:url")
        {
            priority = 1;
        }
        else if (key == u"og:image:secure_url")
        {
            priority = 2;
        }
        else if (key == u"twitter:image" || key == u"twitter:image:src")
        {
            priority = 3;
        }

        if (priority >= this->bestPriority_)
        {
            return;
        }

        auto url =
            this->base_.resolved(QUrl(decodeHtmlEntities(content).trimmed()));
        if (!url.isValid() ||
            (url.scheme() != u"http" && url.scheme() != u"https"))
        {
            return;
        }

        this->best_ = url.toString(QUrl::FullyEncoded);
        this->bestPriority_ = priority;
    }

    QUrl base_;
    QByteArray buffer_;
    QByteArray lower_;
    qsizetype scanned_ = 0;
    QString best_;
    int bestPriority_ = INT_MAX;

    QString ogTitle_;
    QString twitterTitle_;
    QString htmlTitle_;
    qsizetype titleScanned_ = 0;
    bool titleTagDone_ = false;
};

QString fileNameFromDisposition(const QNetworkReply *reply)
{
    const auto header =
        QString::fromUtf8(reply->rawHeader("Content-Disposition"));
    if (header.isEmpty())
    {
        return {};
    }

    static const QRegularExpression extended(
        R"re(filename\*\s*=\s*[^']*'[^']*'([^;]+))re",
        QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression plain(
        R"re(filename\s*=\s*(?:"([^"]*)"|([^;]+)))re",
        QRegularExpression::CaseInsensitiveOption);

    QString name;
    if (auto match = extended.match(header); match.hasMatch())
    {
        name = QUrl::fromPercentEncoding(match.captured(1).trimmed().toUtf8());
    }
    else if (auto match = plain.match(header); match.hasMatch())
    {
        name = match.capturedStart(1) >= 0 ? match.captured(1)
                                           : match.captured(2).trimmed();
    }

    return QFileInfo(name.replace(u'\\', u'/')).fileName().simplified();
}

QString titleForUrl(const QString &url)
{
    QUrl parsed(url);
    auto name = parsed.fileName(QUrl::FullyDecoded);
    if (name.isEmpty())
    {
        name = parsed.host();
    }
    return name;
}

QStringList parseFilterList(const QString &value)
{
    static const QRegularExpression separators(R"([\s,;]+)");
    QStringList out;
    for (const auto &part : value.split(separators, Qt::SkipEmptyParts))
    {
        auto entry = part.trimmed().toLower();

        if (auto idx = entry.indexOf(u"://"); idx >= 0)
        {
            entry = entry.mid(idx + 3);
        }
        if (!entry.isEmpty())
        {
            out.append(entry);
        }
    }
    return out;
}

bool matchesAny(const QStringList &entries, const QUrl &url)
{
    const auto host = url.host().toLower();
    const auto hostAndPath = host + url.path().toLower();

    for (const auto &entry : entries)
    {
        if (entry.contains(u'*') || entry.contains(u'?'))
        {
            auto re = QRegularExpression(
                QRegularExpression::wildcardToRegularExpression(
                    entry, QRegularExpression::NonPathWildcardConversion),
                QRegularExpression::CaseInsensitiveOption);
            if (re.match(host).hasMatch() || re.match(hostAndPath).hasMatch())
            {
                return true;
            }
        }
        else if (entry.contains(u'/'))
        {
            if (hostAndPath.startsWith(entry))
            {
                return true;
            }
        }
        else if (host == entry || host.endsWith(u'.' + entry))
        {
            return true;
        }
    }

    return false;
}

#ifdef TINYRINO_WITH_MULTIMEDIA

bool isDecodable(const QString &path)
{
    static const auto supported =
        QMediaFormat().supportedFileFormats(QMediaFormat::Decode);
    if (supported.isEmpty())
    {
        return true;
    }

    static const QHash<QString, QMediaFormat::FileFormat> formats{
        {"webm", QMediaFormat::WebM},      {"weba", QMediaFormat::WebM},
        {"mkv", QMediaFormat::Matroska},   {"ogg", QMediaFormat::Ogg},
        {"ogv", QMediaFormat::Ogg},        {"oga", QMediaFormat::Ogg},
        {"opus", QMediaFormat::Ogg},       {"mp4", QMediaFormat::MPEG4},
        {"m4v", QMediaFormat::MPEG4},      {"mov", QMediaFormat::QuickTime},
        {"avi", QMediaFormat::AVI},        {"wmv", QMediaFormat::WMV},
        {"wma", QMediaFormat::WMA},        {"mp3", QMediaFormat::MP3},
        {"m4a", QMediaFormat::Mpeg4Audio}, {"aac", QMediaFormat::AAC},
        {"wav", QMediaFormat::Wave},       {"flac", QMediaFormat::FLAC},
    };

    auto it = formats.find(QFileInfo(path).suffix().toLower());
    if (it == formats.end())
    {
        return true;
    }
    return supported.contains(*it);
}

QString findFfmpeg()
{
    static const QString ffmpeg = [] {
        auto found = QStandardPaths::findExecutable(QStringLiteral("ffmpeg"));
        if (found.isEmpty())
        {
            found = QStandardPaths::findExecutable(
                QStringLiteral("ffmpeg"),
                {QStringLiteral("/opt/homebrew/bin"),
                 QStringLiteral("/usr/local/bin"), QStringLiteral("/usr/bin")});
        }
        return found;
    }();
    return ffmpeg;
}

#endif

QPointer<MediaPreviewManager> &managerInstance()
{
    static QPointer<MediaPreviewManager> instance;
    return instance;
}

}  // namespace

namespace chatterino {

MediaPreview::MediaPreview(QString url, std::uint32_t generation)
    : url_(std::move(url))
    , generation_(generation)
{
}

const QString &MediaPreview::url() const
{
    return this->url_;
}

MediaPreview::Kind MediaPreview::kind() const
{
    return this->kind_;
}

bool MediaPreview::isResolved() const
{
    return this->kind_ != Kind::Unknown;
}

bool MediaPreview::isPlayerKind() const
{
    return this->kind_ == Kind::Video || this->kind_ == Kind::Audio;
}

std::uint32_t MediaPreview::generation() const
{
    return this->generation_;
}

const QString &MediaPreview::title() const
{
    return this->title_;
}

const QPixmap &MediaPreview::frame() const
{
    return this->frame_;
}

QSize MediaPreview::mediaSize() const
{
    return this->mediaSize_;
}

bool MediaPreview::isAnimated() const
{
    return this->animated_;
}

const QString &MediaPreview::formatLabel() const
{
    return this->formatLabel_;
}

qint64 MediaPreview::duration() const
{
    return this->duration_;
}

qint64 MediaPreview::position() const
{
    return this->position_;
}

bool MediaPreview::isPlaying() const
{
    return this->playing_;
}

bool MediaPreview::isLoading() const
{
    return this->mediaLoading_;
}

bool MediaPreview::hasPlaybackError() const
{
    return this->playbackError_;
}

bool MediaPreview::isConverting() const
{
    return this->converting_;
}

const QString &MediaPreview::errorText() const
{
    return this->errorText_;
}

bool MediaPreview::shouldRetry() const
{
    return this->retry_ && Clock::now() >= this->retryAfter_;
}

class MediaPlaybackSession : public QObject
{
public:
    MediaPlaybackSession(MediaPreviewPtr preview, MediaPreviewManager *manager)
        : preview_(std::move(preview))
        , manager_(manager)
    {
    }

    ~MediaPlaybackSession() override
    {
        this->preview_->playing_ = false;
        this->manager_->requestRepaint();
    }

    MediaPlaybackSession(const MediaPlaybackSession &) = delete;
    MediaPlaybackSession(MediaPlaybackSession &&) = delete;
    MediaPlaybackSession &operator=(const MediaPlaybackSession &) = delete;
    MediaPlaybackSession &operator=(MediaPlaybackSession &&) = delete;

    virtual void play(bool muted) = 0;
    virtual void pause() = 0;

    virtual void setMuted(bool /*muted*/)
    {
    }

    virtual void seek(qint64 /*position*/)
    {
    }

    virtual void onDataReady()
    {
    }

    const MediaPreviewPtr &preview() const
    {
        return this->preview_;
    }

    bool hovered = false;

    bool autoplay = false;
    Clock::time_point pausedSince{};

protected:
    void setPlaying(bool playing)
    {
        this->preview_->playing_ = playing;
        if (!playing)
        {
            this->pausedSince = Clock::now();
        }
        this->manager_->requestRepaint();
    }

    MediaPreviewPtr preview_;
    MediaPreviewManager *manager_;
};

class AnimatedImageSession final : public MediaPlaybackSession
{
public:
    AnimatedImageSession(MediaPreviewPtr preview, MediaPreviewManager *manager)
        : MediaPlaybackSession(std::move(preview), manager)
        , buffer_(new QBuffer(this))
        , movie_(new QMovie(this))
    {
        this->movie_->setCacheMode(QMovie::CacheNone);

        QObject::connect(this->movie_, &QMovie::frameChanged, this, [this] {
            auto pixmap = toPreviewPixmap(this->movie_->currentImage());
            if (pixmap.isNull())
            {
                return;
            }
            this->preview_->frame_ = std::move(pixmap);
            this->manager_->requestRepaint();
        });
        QObject::connect(this->movie_, &QMovie::error, this, [this] {
            this->manager_->onSessionError(this->preview_.get());
        });

        QObject::connect(this->movie_, &QMovie::finished, this, [this] {
            if (this->wantsPlaying_)
            {
                QTimer::singleShot(0, this, [this] {
                    if (this->wantsPlaying_ &&
                        this->movie_->state() == QMovie::NotRunning)
                    {
                        this->movie_->start();
                    }
                });
            }
        });
    }

    void play(bool /*muted*/) override
    {
        this->wantsPlaying_ = true;
        this->setPlaying(true);

        if (this->started_)
        {
            this->movie_->setPaused(false);
            return;
        }

        if (this->preview_->imageData_.isEmpty())
        {
            this->manager_->loadImage(this->preview_);
            return;
        }

        this->buffer_->setData(this->preview_->imageData_);
        this->buffer_->open(QIODevice::ReadOnly);
        this->movie_->setDevice(this->buffer_);

        auto natural = this->preview_->mediaSize_;
        if (natural.isValid())
        {
            auto target = storedPixelSize(natural);
            if (target != natural)
            {
                this->movie_->setScaledSize(target);
            }
        }

        this->movie_->start();
        this->started_ = true;
    }

    void pause() override
    {
        this->wantsPlaying_ = false;
        this->setPlaying(false);
        if (this->started_)
        {
            this->movie_->setPaused(true);
        }
    }

    void onDataReady() override
    {
        if (this->wantsPlaying_ && !this->started_)
        {
            this->play(false);
        }
    }

private:
    QBuffer *buffer_;
    QMovie *movie_;
    bool started_ = false;
    bool wantsPlaying_ = false;
};

#ifdef TINYRINO_WITH_MULTIMEDIA

class MultimediaSession final : public MediaPlaybackSession
{
public:
    MultimediaSession(MediaPreviewPtr preview, MediaPreviewManager *manager)
        : MediaPlaybackSession(std::move(preview), manager)
        , player_(new QMediaPlayer(this))
        , audioOutput_(new QAudioOutput(this))
        , videoSink_(new QVideoSink(this))
    {
        this->audioOutput_->setVolume(0.6F);
        this->player_->setAudioOutput(this->audioOutput_);
        this->player_->setVideoSink(this->videoSink_);

        this->player_->setLoops(
            this->preview_->kind_ == Kind::Video ? QMediaPlayer::Infinite : 1);

        QObject::connect(
            this->videoSink_, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame &frame) {
                if (this->preview_->kind_ != Kind::Video || !frame.isValid())
                {
                    return;
                }
                auto image = frame.toImage();
                if (image.isNull())
                {
                    return;
                }
                auto naturalSize = image.size();
                const bool sizeChanged =
                    this->preview_->mediaSize_ != naturalSize;
                this->preview_->frame_ = toPreviewPixmap(std::move(image));
                this->preview_->mediaSize_ = naturalSize;
                if (sizeChanged)
                {
                    this->manager_->requestLayout();
                }
                this->manager_->requestRepaint();
            });
        QObject::connect(this->player_, &QMediaPlayer::positionChanged, this,
                         [this](qint64 position) {
                             this->preview_->position_ = position;
                             this->manager_->requestRepaint();
                         });
        QObject::connect(this->player_, &QMediaPlayer::durationChanged, this,
                         [this](qint64 duration) {
                             this->preview_->duration_ = duration;
                             if (duration > 0 && this->pendingSeek_ >= 0)
                             {
                                 this->player_->setPosition(this->pendingSeek_);
                                 this->pendingSeek_ = -1;
                             }
                             this->manager_->requestRepaint();
                         });
        QObject::connect(this->player_, &QMediaPlayer::mediaStatusChanged, this,
                         [this](QMediaPlayer::MediaStatus status) {
                             if (status == QMediaPlayer::EndOfMedia)
                             {
                                 this->wantsPlaying_ = false;
                                 this->preview_->position_ = 0;
                                 this->setPlaying(false);
                             }
                         });
        QObject::connect(
            this->player_, &QMediaPlayer::errorOccurred, this,
            [this](QMediaPlayer::Error error, const QString &errorString) {
                if (error == QMediaPlayer::NoError)
                {
                    return;
                }
                qCWarning(chatterinoApp)
                    << "Embedded media: can't play" << this->preview_->url()
                    << "from" << this->preview_->mediaFile_ << "-"
                    << errorString;
                this->manager_->onSessionError(this->preview_.get());
            });
    }

    void play(bool muted) override
    {
        this->wantsPlaying_ = true;
        this->audioOutput_->setMuted(muted);
        this->preview_->playbackError_ = false;
        this->preview_->errorText_.clear();
        this->setPlaying(true);

        if (this->preview_->mediaFile_.isEmpty())
        {
            this->manager_->loadMediaFile(this->preview_);
            return;
        }
        this->start();
    }

    void pause() override
    {
        this->wantsPlaying_ = false;
        if (this->sourceSet_)
        {
            this->player_->pause();
        }
        this->setPlaying(false);
    }

    void setMuted(bool muted) override
    {
        this->audioOutput_->setMuted(muted);
    }

    void seek(qint64 position) override
    {
        this->preview_->position_ = position;
        if (this->sourceSet_ && this->player_->duration() > 0)
        {
            this->player_->setPosition(position);
        }
        else
        {
            this->pendingSeek_ = position;
        }
        this->manager_->requestRepaint();
    }

    void onDataReady() override
    {
        if (this->wantsPlaying_)
        {
            this->start();
        }
    }

private:
    void start()
    {
        if (!this->sourceSet_)
        {
            this->player_->setSource(
                QUrl::fromLocalFile(this->preview_->mediaFile_));
            this->sourceSet_ = true;
        }
        this->player_->play();
    }

    QMediaPlayer *player_;
    QAudioOutput *audioOutput_;
    QVideoSink *videoSink_;
    bool sourceSet_ = false;
    bool wantsPlaying_ = false;
    qint64 pendingSeek_ = -1;
};

#endif

MediaPreviewManager &MediaPreviewManager::instance()
{
    auto &instance = managerInstance();
    if (!instance)
    {
        instance = new MediaPreviewManager;
    }
    return *instance;
}

MediaPreviewManager *MediaPreviewManager::existingInstance()
{
    return managerInstance().data();
}

MediaPreviewManager::MediaPreviewManager()
    : QObject(QCoreApplication::instance())
    , network_(new QNetworkAccessManager(this))
    , collectTimer_(new QTimer(this))
{
    initCacheDirectory();
    QThreadPool::globalInstance()->start([dir = cacheDirectory()] {
        trimCache(dir);
    });

    auto *s = getSettings();

    auto invalidateFilters = [this] {
        this->filtersDirty_ = true;
    };
    s->mediaPreviewWhitelist.connect(invalidateFilters, this->signalHolder_,
                                     false);
    s->mediaPreviewBlacklist.connect(invalidateFilters, this->signalHolder_,
                                     false);

    auto invalidateResolved = [this] {
        this->generation_++;
        this->cache_.clear();
    };
    s->mediaPreviewOpenGraph.connect(invalidateResolved, this->signalHolder_,
                                     false);
    s->mediaPreviewDirectMedia.connect(invalidateResolved, this->signalHolder_,
                                       false);
    s->mediaPreviewMaxSizeMb.connect(invalidateResolved, this->signalHolder_,
                                     false);

    auto reconcile = [this] {
        this->collectSessions();
    };
    s->mediaPreviewEnabled.connect(reconcile, this->signalHolder_, false);
    s->mediaPreviewVideo.connect(reconcile, this->signalHolder_, false);
    s->mediaPreviewAudio.connect(reconcile, this->signalHolder_, false);
    s->mediaPreviewAutoplayVideo.connect(reconcile, this->signalHolder_, false);
    s->mediaPreviewAutoplayAnimated.connect(reconcile, this->signalHolder_,
                                            false);

    if (auto *app = tryGetApp())
    {
        if (auto *streamerMode = app->getStreamerMode())
        {
            QObject::connect(streamerMode, &IStreamerMode::changed, this,
                             [this] {
                                 this->hovered_.reset();
                                 this->sessions_.clear();
                                 this->requestLayout();
                             });
        }
    }

    QObject::connect(qGuiApp, &QGuiApplication::applicationStateChanged, this,
                     [this](Qt::ApplicationState state) {
                         if (state != Qt::ApplicationActive)
                         {
                             this->stopAll();
                         }
                     });

    this->collectTimer_->setInterval(500);
    QObject::connect(this->collectTimer_, &QTimer::timeout, this,
                     &MediaPreviewManager::collectSessions);
    this->collectTimer_->start();
}

MediaPreviewManager::~MediaPreviewManager()
{
    this->hovered_.reset();
    this->sessions_.clear();
}

bool MediaPreviewManager::shouldPreview(const QString &url)
{
    auto *s = getSettings();
    if (!s->mediaPreviewEnabled)
    {
        return false;
    }
    if (!s->mediaPreviewOpenGraph && !s->mediaPreviewDirectMedia)
    {
        return false;
    }

    if (s->streamerModeHideLinkThumbnails)
    {
        auto *app = tryGetApp();
        if (app && app->getStreamerMode() &&
            app->getStreamerMode()->isEnabled())
        {
            return false;
        }
    }

    QUrl parsed(url);
    if (!parsed.isValid() ||
        (parsed.scheme() != u"http" && parsed.scheme() != u"https") ||
        parsed.host().isEmpty())
    {
        return false;
    }

    return instance().matchesFilters(parsed);
}

bool MediaPreviewManager::isKindEnabled(MediaPreview::Kind kind)
{
    switch (kind)
    {
        case Kind::Image:
            return true;
        case Kind::Video:
            return getSettings()->mediaPreviewVideo;
        case Kind::Audio:
            return getSettings()->mediaPreviewAudio;
        default:
            return false;
    }
}

bool MediaPreviewManager::canPlayMedia()
{
#ifdef TINYRINO_WITH_MULTIMEDIA
    return true;
#else
    return false;
#endif
}

bool MediaPreviewManager::previewsAtEnd()
{
    return getSettings()->mediaPreviewEnabled &&
           getSettings()->mediaPreviewAtEnd;
}

qint64 MediaPreviewManager::maxMediaBytes()
{
    return std::max<qint64>(1, getSettings()->mediaPreviewMaxSizeMb) * 1024 *
           1024;
}

MediaPreviewPtr MediaPreviewManager::get(const QString &url)
{
    if (auto it = this->cache_.find(url); it != this->cache_.end())
    {
        if (auto preview = it->lock())
        {
            return preview;
        }
    }

    if (this->cache_.size() > 2048)
    {
        this->cache_.removeIf([](const auto &it) {
            return it.value().expired();
        });
    }

    auto preview = std::make_shared<MediaPreview>(url, this->generation_);
    this->cache_.insert(url, preview);
    return preview;
}

void MediaPreviewManager::ensureResolving(const MediaPreviewPtr &preview)
{
    if (preview->resolveStarted_ || preview->isResolved())
    {
        return;
    }
    preview->resolveStarted_ = true;
    this->resolve(preview);
}

std::uint32_t MediaPreviewManager::generation() const
{
    return this->generation_;
}

bool MediaPreviewManager::matchesFilters(const QUrl &url)
{
    if (this->filtersDirty_)
    {
        this->rebuildFilters();
    }

    if (!this->whitelist_.isEmpty())
    {
        return matchesAny(this->whitelist_, url);
    }

    return !matchesAny(this->blacklist_, url);
}

void MediaPreviewManager::rebuildFilters()
{
    this->whitelist_ = parseFilterList(getSettings()->mediaPreviewWhitelist);
    this->blacklist_ = parseFilterList(getSettings()->mediaPreviewBlacklist);
    this->filtersDirty_ = false;
}

QNetworkRequest MediaPreviewManager::makeRequest(const QUrl &url) const
{
    static const QByteArray userAgent =
        QStringLiteral("tinyrino-embedded-media/%1")
            .arg(Version::instance().version())
            .toUtf8();

    QNetworkRequest request(url);
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setMaximumRedirectsAllowed(8);

    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);
    request.setTransferTimeout(REQUEST_TIMEOUT_MS);
    request.setRawHeader("User-Agent", userAgent);
    request.setRawHeader("Accept",
                         "text/html,application/xhtml+xml,image/webp,image/"
                         "gif,image/*,video/*,audio/*;q=0.9,*/*;q=0.8");
    return request;
}

void MediaPreviewManager::fetchData(const QString &url, bool wantData,
                                    FetchCallback callback)
{
    auto &pending = this->fetches_[url];
    const bool alreadyFetching = !pending.empty();
    pending.emplace_back(wantData, std::move(callback));
    if (alreadyFetching)
    {
        return;
    }

    struct CacheHit {
        bool hit = false;
        QString path;
        QByteArray data;
    };

    const auto maxBytes = maxMediaBytes();
    runAsync<CacheHit>(
        this,
        [url, maxBytes, wantData] {
            const auto path = findCachedDataFile(url);
            if (path.isEmpty())
            {
                return CacheHit{};
            }
            QFileInfo info(path);

            const bool converted = info.fileName().contains(u".converted.");
            if (!info.exists() || info.size() <= 0 ||
                (!converted && info.size() > maxBytes))
            {
                return CacheHit{};
            }

            QFile file(path);
            if (!file.open(QIODevice::ReadWrite))
            {
                return CacheHit{};
            }

            file.setFileTime(QDateTime::currentDateTime(),
                             QFileDevice::FileModificationTime);
            return CacheHit{
                .hit = true,
                .path = path,
                .data = wantData ? file.readAll() : QByteArray(),
            };
        },
        [this, url](CacheHit hit) {
            if (hit.hit)
            {
                this->finishFetch(url, true, hit.path, hit.data);
            }
            else
            {
                this->download(url);
            }
        });
}

void MediaPreviewManager::download(const QString &url)
{
    const auto maxBytes = maxMediaBytes();
    auto *reply = this->network_->get(this->makeRequest(QUrl(url)));

    QObject::connect(
        reply, &QNetworkReply::metaDataChanged, this, [reply, maxBytes] {
            auto length = reply->header(QNetworkRequest::ContentLengthHeader)
                              .toLongLong();
            if (length > maxBytes)
            {
                reply->abort();
            }
        });
    QObject::connect(reply, &QNetworkReply::downloadProgress, this,
                     [reply, maxBytes](qint64 received, qint64 /*total*/) {
                         if (received > maxBytes)
                         {
                             reply->abort();
                         }
                     });
    QObject::connect(reply, &QNetworkReply::finished, this, [this, reply, url] {
        reply->deleteLater();
        if (reply->error() != QNetworkReply::NoError)
        {
            this->finishFetch(url, false, {}, {});
            return;
        }

        const auto data = reply->readAll();
        const auto path = cacheDataPath(
            url, dataFileSuffix(contentTypeOf(reply), reply->url()));
        runAsync<bool>(
            this,
            [path, data] {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly))
                {
                    return false;
                }
                file.write(data);
                return file.commit();
            },
            [this, url, path, data](bool written) {
                this->finishFetch(url, true, written ? path : QString(), data);
            });
    });
}

void MediaPreviewManager::finishFetch(const QString &url, bool ok,
                                      const QString &path,
                                      const QByteArray &data)
{
    auto callbacks = this->fetches_.take(url);
    for (auto &[wantData, callback] : callbacks)
    {
        callback(ok, path, wantData ? data : QByteArray());
    }
}

void MediaPreviewManager::resolve(const MediaPreviewPtr &preview)
{
    std::weak_ptr<MediaPreview> weak = preview;
    const auto path = cacheMetaPath(preview->url());

    runAsync<CachedMeta>(
        this,
        [path] {
            return readCachedMeta(path);
        },
        [this, weak](CachedMeta meta) {
            auto preview = weak.lock();
            if (!preview || preview->isResolved())
            {
                return;
            }

            auto *s = getSettings();
            const bool viaOpenGraph =
                meta.kind == Kind::Image && meta.imageUrl != preview->url();

            bool usable = false;
            switch (meta.kind)
            {
                case Kind::None:
                    usable =
                        s->mediaPreviewOpenGraph && s->mediaPreviewDirectMedia;
                    break;
                case Kind::Image:
                    usable = viaOpenGraph ? s->mediaPreviewOpenGraph
                                          : s->mediaPreviewDirectMedia;
                    break;
                case Kind::Video:
                case Kind::Audio:
                    usable = s->mediaPreviewDirectMedia;
                    break;
                default:
                    break;
            }

            if (!usable)
            {
                this->resolveFromNetwork(preview);
                return;
            }

            preview->resolvedFromCache_ = true;
            switch (meta.kind)
            {
                case Kind::None:
                    this->finishWithNothing(*preview, false);
                    break;
                case Kind::Image:
                    this->finishWithImage(preview, meta.imageUrl, meta.title);
                    break;
                default:
                    this->finishWithMedia(preview, meta.kind, meta.title);
                    break;
            }
        });
}

void MediaPreviewManager::resolveFromNetwork(const MediaPreviewPtr &preview)
{
    std::weak_ptr<MediaPreview> weak = preview;
    auto *s = getSettings();

    if (!s->mediaPreviewDirectMedia)
    {
        if (s->mediaPreviewOpenGraph)
        {
            this->fetchOpenGraph(weak);
        }
        else
        {
            this->finishWithNothing(*preview, false);
        }
        return;
    }

    auto *reply = this->network_->head(this->makeRequest(QUrl(preview->url())));
    QObject::connect(
        reply, &QNetworkReply::finished, this, [this, reply, weak] {
            reply->deleteLater();
            auto preview = weak.lock();
            if (!preview || preview->isResolved())
            {
                return;
            }

            if (reply->error() == QNetworkReply::NoError)
            {
                auto kind = kindOfReply(reply);
                if (isMediaKind(kind))
                {
                    auto length =
                        reply->header(QNetworkRequest::ContentLengthHeader)
                            .toLongLong();
                    if (length > maxMediaBytes())
                    {
                        this->finishWithNothing(*preview, false);
                    }
                    else
                    {
                        this->finishWithMedia(preview, kind,
                                              fileNameFromDisposition(reply));
                    }
                    return;
                }
            }

            if (getSettings()->mediaPreviewOpenGraph)
            {
                this->fetchOpenGraph(weak);
            }
            else
            {
                this->finishWithNothing(*preview, false);
            }
        });
}

void MediaPreviewManager::fetchOpenGraph(
    const std::weak_ptr<MediaPreview> &weak)
{
    auto preview = weak.lock();
    if (!preview)
    {
        return;
    }

    auto *reply = this->network_->get(this->makeRequest(QUrl(preview->url())));
    auto scanner = std::make_shared<OpenGraphScanner>(QUrl(preview->url()));
    auto done = std::make_shared<bool>(false);

    auto finishScan = [this, weak, scanner, done] {
        if (*done)
        {
            return;
        }
        *done = true;

        auto preview = weak.lock();
        if (!preview || preview->isResolved())
        {
            return;
        }

        auto image = scanner->result();
        if (image.isEmpty())
        {
            this->finishWithNothing(*preview, true);
        }
        else
        {
            this->finishWithImage(preview, image, scanner->title());
        }
    };

    QObject::connect(
        reply, &QNetworkReply::metaDataChanged, this,
        [this, reply, weak, done] {
            if (*done)
            {
                return;
            }

            auto status =
                reply->attribute(QNetworkRequest::HttpStatusCodeAttribute)
                    .toInt();
            if (status >= 300 && status < 400)
            {
                return;
            }

            auto preview = weak.lock();
            if (!preview)
            {
                *done = true;
                reply->abort();
                return;
            }

            auto kind = kindOfReply(reply);
            if (isMediaKind(kind))
            {
                *done = true;
                auto length =
                    reply->header(QNetworkRequest::ContentLengthHeader)
                        .toLongLong();
                if (getSettings()->mediaPreviewDirectMedia &&
                    length <= maxMediaBytes())
                {
                    this->finishWithMedia(preview, kind,
                                          fileNameFromDisposition(reply));
                }
                else
                {
                    this->finishWithNothing(*preview, false);
                }
                reply->abort();
                return;
            }

            auto contentType = contentTypeOf(reply);
            if (status >= 400)
            {
                *done = true;

                const bool temporary = status >= 500 || status == 429;
                this->finishWithNothing(*preview, false, temporary);
                reply->abort();
            }
            else if (!contentType.isEmpty() && !contentType.contains(u"html") &&
                     !contentType.contains(u"xml"))
            {
                *done = true;
                this->finishWithNothing(*preview, true);
                reply->abort();
            }
        });

    QObject::connect(reply, &QNetworkReply::readyRead, this,
                     [reply, scanner, done, finishScan] {
                         if (*done)
                         {
                             return;
                         }
                         if (scanner->feed(reply->readAll()))
                         {
                             finishScan();
                             reply->abort();
                         }
                     });

    QObject::connect(reply, &QNetworkReply::finished, this,
                     [this, reply, weak, scanner, done, finishScan] {
                         reply->deleteLater();
                         if (*done)
                         {
                             return;
                         }

                         if (reply->error() != QNetworkReply::NoError)
                         {
                             *done = true;
                             if (auto preview = weak.lock())
                             {
                                 qCDebug(chatterinoApp)
                                     << "Embedded media: loading"
                                     << preview->url()
                                     << "failed:" << reply->errorString();

                                 this->finishWithNothing(*preview, false, true);
                             }
                             return;
                         }

                         scanner->feed(reply->readAll());
                         finishScan();
                     });
}

void MediaPreviewManager::finishWithMedia(const MediaPreviewPtr &preview,
                                          MediaPreview::Kind kind,
                                          const QString &title)
{
    if (kind == Kind::Image)
    {
        this->finishWithImage(preview, preview->url(), title);
        return;
    }

    preview->kind_ = kind;
    preview->title_ = title.isEmpty() ? titleForUrl(preview->url()) : title;
    qCDebug(chatterinoApp) << "Embedded media:" << preview->url() << "is"
                           << kindToString(kind) << preview->title_;
    if (!preview->resolvedFromCache_)
    {
        QThreadPool::globalInstance()->start(
            [path = cacheMetaPath(preview->url()), kind, title] {
                writeCachedMeta(path, kind, {}, title);
            });
    }
    this->requestLayout();
}

void MediaPreviewManager::finishWithImage(const MediaPreviewPtr &preview,
                                          const QString &imageUrl,
                                          const QString &title)
{
    preview->kind_ = Kind::Image;
    preview->title_ = title.isEmpty() ? titleForUrl(preview->url()) : title;
    preview->imageUrl_ = imageUrl;
    qCDebug(chatterinoApp) << "Embedded media:" << preview->url()
                           << "is the image" << imageUrl << preview->title_;
    if (!preview->resolvedFromCache_)
    {
        QThreadPool::globalInstance()->start(
            [path = cacheMetaPath(preview->url()), imageUrl, title] {
                writeCachedMeta(path, Kind::Image, imageUrl, title);
            });
    }

    this->loadImage(preview);
}

void MediaPreviewManager::finishWithNothing(MediaPreview &preview,
                                            bool definitive, bool temporary)
{
    preview.kind_ = Kind::None;
    qCDebug(chatterinoApp) << "Embedded media: no media for" << preview.url()
                           << (temporary ? "(will retry)" : "");

    if (temporary)
    {
        this->scheduleRetry(preview);
        return;
    }

    auto *s = getSettings();
    if (definitive && !preview.resolvedFromCache_ && s->mediaPreviewOpenGraph &&
        s->mediaPreviewDirectMedia)
    {
        QThreadPool::globalInstance()->start(
            [path = cacheMetaPath(preview.url())] {
                writeCachedMeta(path, Kind::None, {}, {});
            });
    }
}

void MediaPreviewManager::scheduleRetry(MediaPreview &preview)
{
    preview.retryAfter_ = Clock::now() + RETRY_DELAY;
    preview.retry_ = true;

    auto it = this->cache_.find(preview.url());
    if (it != this->cache_.end() && it->lock().get() == &preview)
    {
        this->cache_.erase(it);
    }
}

void MediaPreviewManager::dropPreview(MediaPreview &preview)
{
    preview.kind_ = Kind::None;
    preview.playing_ = false;

    if (this->findSession(&preview))
    {
        auto *key = &preview;
        QTimer::singleShot(0, this, [this, key] {
            if (this->hovered_.get() == key)
            {
                this->hovered_.reset();
            }
            this->sessions_.erase(key);
        });
    }
    this->requestLayout();
}

void MediaPreviewManager::loadImage(const MediaPreviewPtr &preview)
{
    if (preview->imageLoading_ || preview->imageUrl_.isEmpty())
    {
        return;
    }
    preview->imageLoading_ = true;

    std::weak_ptr<MediaPreview> weak = preview;
    this->fetchData(
        preview->imageUrl_, true,
        [this, weak](bool ok, const QString & /*path*/, QByteArray data) {
            auto preview = weak.lock();
            if (!preview)
            {
                return;
            }

            if (!ok || data.isEmpty())
            {
                preview->imageLoading_ = false;
                this->onImageFailed(*preview);
                return;
            }

            runAsync<DecodedImage>(
                this,
                [data] {
                    return decodeImage(data);
                },
                [this, weak, data](DecodedImage decoded) {
                    this->onImageDecoded(weak, decoded.image,
                                         decoded.naturalSize, decoded.animated,
                                         decoded.format, data);
                });
        });
}

void MediaPreviewManager::loadMediaFile(const MediaPreviewPtr &preview)
{
    if (!preview->mediaFile_.isEmpty() || preview->mediaLoading_)
    {
        return;
    }
    preview->mediaLoading_ = true;
    this->requestRepaint();

    std::weak_ptr<MediaPreview> weak = preview;
    this->fetchData(
        preview->url(), false,
        [this, weak](bool ok, const QString &path, QByteArray /*data*/) {
            auto preview = weak.lock();
            if (!preview)
            {
                return;
            }
            if (!ok || path.isEmpty())
            {
                preview->mediaLoading_ = false;
                this->dropPreview(*preview);
                return;
            }

#ifdef TINYRINO_WITH_MULTIMEDIA
            if (!isDecodable(path))
            {
                this->convertMedia(preview, path);
                return;
            }
#endif
            this->onMediaFileReady(preview, path);
        });
}

void MediaPreviewManager::onMediaFileReady(const MediaPreviewPtr &preview,
                                           const QString &path)
{
    preview->mediaLoading_ = false;
    preview->converting_ = false;
    preview->mediaFile_ = path;

#ifdef TINYRINO_WITH_MULTIMEDIA
    if (preview->posterRequested_ && preview->frame_.isNull())
    {
        this->posterQueue_.emplace_back(preview);
        if (!this->posterBusy_)
        {
            this->processNextPoster();
        }
    }
#endif

    if (auto *session = this->findSession(preview.get()))
    {
        session->onDataReady();
    }
    this->requestRepaint();
}

void MediaPreviewManager::failMediaFile(const MediaPreviewPtr &preview,
                                        const QString &errorText)
{
    preview->mediaLoading_ = false;
    preview->converting_ = false;
    preview->errorText_ = errorText;
    qCWarning(chatterinoApp)
        << "Embedded media: can't play" << preview->url() << "-" << errorText;

    if (this->findSession(preview.get()))
    {
        this->onSessionError(preview.get());
    }
    else
    {
        preview->playbackError_ = true;
    }
    this->requestRepaint();
}

#ifdef TINYRINO_WITH_MULTIMEDIA

void MediaPreviewManager::convertMedia(const MediaPreviewPtr &preview,
                                       const QString &path)
{
    const QFileInfo input(path);
    const auto format = input.suffix().toUpper();

    const auto ffmpeg = findFfmpeg();
    if (ffmpeg.isEmpty())
    {
        this->failMediaFile(
            preview, QStringLiteral("Unsupported format (%1)").arg(format));
        return;
    }

    preview->converting_ = true;
    this->requestRepaint();

    const bool video = preview->kind_ == Kind::Video;
    const auto output = input.absolutePath() + u'/' + input.completeBaseName() +
                        u".converted." + (video ? u"mp4" : u"m4a");
    const auto part = output + u".part";

    QStringList args{"-nostdin", "-y", "-v", "error", "-i", path};
    if (video)
    {
        args << "-map" << "0:v:0" << "-map" << "0:a:0?"

             << "-vf" << "scale=trunc(iw/2)*2:trunc(ih/2)*2"
             << "-c:v" << "libx264" << "-preset" << "veryfast" << "-crf"
             << "23" << "-pix_fmt" << "yuv420p" << "-c:a" << "aac" << "-b:a"
             << "128k" << "-movflags" << "+faststart" << "-f" << "mp4";
    }
    else
    {
        args << "-vn" << "-c:a" << "aac" << "-b:a" << "160k" << "-f"
             << "ipod";
    }
    args << part;

    qCDebug(chatterinoApp) << "Embedded media: converting" << path << "to"
                           << output;

    auto *process = new QProcess(this);
    process->setProgram(ffmpeg);
    process->setArguments(args);
    process->setProcessChannelMode(QProcess::MergedChannels);

    std::weak_ptr<MediaPreview> weak = preview;
    auto done = std::make_shared<bool>(false);
    auto finish = [this, weak, process, done, part, output,
                   format](bool success) {
        if (*done)
        {
            return;
        }
        *done = true;

        const auto log = QString::fromUtf8(process->readAll()).trimmed();
        process->deleteLater();

        if (success)
        {
            QFile::remove(output);
            success = QFile::rename(part, output);
        }
        if (!success)
        {
            QFile::remove(part);
            qCWarning(chatterinoApp)
                << "Embedded media: ffmpeg failed to convert" << format << "-"
                << log;
        }

        auto preview = weak.lock();
        if (!preview)
        {
            return;
        }
        if (success)
        {
            this->onMediaFileReady(preview, output);
        }
        else
        {
            this->failMediaFile(preview,
                                QStringLiteral("Can't convert %1").arg(format));
        }
    };

    QObject::connect(
        process, &QProcess::finished, this,
        [finish](int exitCode, QProcess::ExitStatus status) {
            finish(status == QProcess::NormalExit && exitCode == 0);
        });
    QObject::connect(process, &QProcess::errorOccurred, this,
                     [finish](QProcess::ProcessError error) {
                         if (error == QProcess::FailedToStart)
                         {
                             finish(false);
                         }
                     });

    QTimer::singleShot(3 * 60 * 1000, process, [process] {
        process->kill();
    });

    process->start();
}

#endif

void MediaPreviewManager::onImageDecoded(
    const std::weak_ptr<MediaPreview> &weak, QImage image, QSize naturalSize,
    bool animated, QString format, QByteArray data)
{
    auto preview = weak.lock();
    if (!preview)
    {
        return;
    }
    preview->imageLoading_ = false;

    if (image.isNull())
    {
        this->onImageFailed(*preview);
        return;
    }

    const bool firstLoad = preview->frame_.isNull();
    if (firstLoad)
    {
        preview->frame_ = QPixmap::fromImage(std::move(image));
    }
    preview->mediaSize_ = naturalSize;
    preview->animated_ = animated;
    preview->formatLabel_ = std::move(format);
    if (animated)
    {
        preview->imageData_ = std::move(data);
    }

    if (firstLoad)
    {
        this->requestLayout();
    }

    if (auto *session = this->findSession(preview.get()))
    {
        session->onDataReady();
    }
}

void MediaPreviewManager::onImageFailed(MediaPreview &preview)
{
    if (preview.frame_.isNull())
    {
        qCDebug(chatterinoApp) << "Embedded media: loading the image of"
                               << preview.url() << "failed";
        preview.kind_ = Kind::None;
        this->scheduleRetry(preview);
        return;
    }

    if (this->findSession(&preview))
    {
        this->onSessionError(&preview);
    }
    else
    {
        preview.playbackError_ = true;
    }
}

void MediaPreviewManager::requestRepaint()
{
    if (this->repaintQueued_)
    {
        return;
    }
    this->repaintQueued_ = true;
    QTimer::singleShot(0, this, [this] {
        this->repaintQueued_ = false;
        if (auto *app = tryGetApp())
        {
            app->getWindows()->repaintGifEmotes();
        }
    });
}

void MediaPreviewManager::requestLayout()
{
    if (this->layoutQueued_)
    {
        return;
    }
    this->layoutQueued_ = true;

    QTimer::singleShot(50, this, [this] {
        this->layoutQueued_ = false;
        if (auto *app = tryGetApp())
        {
            app->getWindows()->forceLayoutChannelViews();
        }
    });
}

bool MediaPreviewManager::isPlayable(const MediaPreview &preview)
{
    if (!getSettings()->mediaPreviewEnabled)
    {
        return false;
    }

    switch (preview.kind_)
    {
        case Kind::Image:
            return preview.animated_;
        case Kind::Video:
        case Kind::Audio:
            return canPlayMedia() && isKindEnabled(preview.kind_);
        default:
            return false;
    }
}

bool MediaPreviewManager::isAppActive()
{
    return QGuiApplication::applicationState() == Qt::ApplicationActive;
}

bool MediaPreviewManager::wantsAutoplay(const MediaPreview &preview)
{
    if (!isAppActive() || preview.playbackError_ || !isPlayable(preview))
    {
        return false;
    }

    switch (preview.kind_)
    {
        case Kind::Image:
            return getSettings()->mediaPreviewAutoplayAnimated;
        case Kind::Video:
            return getSettings()->mediaPreviewAutoplayVideo;
        default:
            return false;
    }
}

bool MediaPreviewManager::userMuted(const MediaPreview &preview)
{
    return preview.kind_ == Kind::Video &&
           !getSettings()->mediaPreviewVideoSound;
}

MediaPlaybackSession *MediaPreviewManager::findSession(
    const MediaPreview *preview) const
{
    auto it = this->sessions_.find(preview);
    if (it == this->sessions_.end())
    {
        return nullptr;
    }
    return it->second.get();
}

MediaPlaybackSession *MediaPreviewManager::createSession(
    const MediaPreviewPtr &preview)
{
    std::unique_ptr<MediaPlaybackSession> session;
    switch (preview->kind_)
    {
        case Kind::Image:
            session = std::make_unique<AnimatedImageSession>(preview, this);
            break;
#ifdef TINYRINO_WITH_MULTIMEDIA
        case Kind::Video:
        case Kind::Audio:
            session = std::make_unique<MultimediaSession>(preview, this);
            break;
#endif
        default:
            return nullptr;
    }

    auto *ptr = session.get();
    this->sessions_[preview.get()] = std::move(session);
    return ptr;
}

bool MediaPreviewManager::hasPlayerSession() const
{
    return std::ranges::any_of(this->sessions_, [](const auto &it) {
        return it.second->preview()->isPlayerKind();
    });
}

void MediaPreviewManager::stopPlayerSessions(const MediaPreview *except)
{
    std::erase_if(this->sessions_, [except](const auto &it) {
        return it.first != except && it.second->preview()->isPlayerKind();
    });
}

MediaPlaybackSession *MediaPreviewManager::userSession(
    const MediaPreviewPtr &preview)
{
    if (!preview->isPlayerKind() || !isPlayable(*preview))
    {
        return nullptr;
    }

    this->stopPlayerSessions(preview.get());

    auto *session = this->findSession(preview.get());
    if (!session)
    {
        session = this->createSession(preview);
    }
    if (!session)
    {
        return nullptr;
    }

    if (session->autoplay)
    {
        session->autoplay = false;
        session->setMuted(userMuted(*preview));
    }
    return session;
}

void MediaPreviewManager::togglePlayback(const MediaPreviewPtr &preview)
{
    if (!preview)
    {
        return;
    }

    auto *existing = this->findSession(preview.get());
    const bool wasAutoplay = existing && existing->autoplay;

    auto *session = this->userSession(preview);
    if (!session)
    {
        return;
    }

    if (wasAutoplay && preview->playing_)
    {
        this->requestRepaint();
        return;
    }

    if (preview->playing_)
    {
        session->pause();
    }
    else
    {
        session->play(userMuted(*preview));
    }
}

void MediaPreviewManager::seek(const MediaPreviewPtr &preview, double fraction)
{
    if (!preview)
    {
        return;
    }
    fraction = std::clamp(fraction, 0.0, 1.0);

    const bool existed = this->findSession(preview.get()) != nullptr;
    auto *session = this->userSession(preview);
    if (!session)
    {
        return;
    }

    if (preview->duration_ > 0)
    {
        session->seek(static_cast<qint64>(
            fraction * static_cast<double>(preview->duration_)));
    }

    if (!existed)
    {
        session->play(userMuted(*preview));
    }
}

std::size_t MediaPreviewManager::countAutoplaying(MediaPreview::Kind kind) const
{
    return static_cast<std::size_t>(
        std::ranges::count_if(this->sessions_, [kind](const auto &it) {
            return it.second->autoplay && it.second->preview()->kind_ == kind;
        }));
}

void MediaPreviewManager::notifyPainted(const MediaPreviewPtr &preview)
{
    preview->lastPainted_ = Clock::now();

    if (this->findSession(preview.get()) || !wantsAutoplay(*preview))
    {
        return;
    }

    auto alreadyQueued =
        std::ranges::any_of(this->autoplayQueue_, [&](const auto &weak) {
            return weak.lock() == preview;
        });
    if (!alreadyQueued)
    {
        this->autoplayQueue_.emplace_back(preview);
    }

    if (!this->autoplayQueued_)
    {
        this->autoplayQueued_ = true;
        QTimer::singleShot(0, this, &MediaPreviewManager::processAutoplayQueue);
    }
}

void MediaPreviewManager::processAutoplayQueue()
{
    this->autoplayQueued_ = false;
    auto queue = std::move(this->autoplayQueue_);
    this->autoplayQueue_.clear();

    for (const auto &weak : queue)
    {
        auto preview = weak.lock();
        if (!preview || this->findSession(preview.get()) ||
            !wantsAutoplay(*preview))
        {
            continue;
        }

        if (preview->isPlayerKind())
        {
            if (this->hasPlayerSession())
            {
                continue;
            }
        }
        else if (this->countAutoplaying(preview->kind_) >= MAX_AUTOPLAY_IMAGES)
        {
            continue;
        }

        auto *session = this->createSession(preview);
        if (!session)
        {
            continue;
        }
        session->autoplay = true;

        session->play(true);
    }
}

void MediaPreviewManager::setHovered(const MediaPreviewPtr &hovered)
{
    auto preview = hovered && isAppActive() && hovered->kind_ == Kind::Image &&
                           isPlayable(*hovered)
                       ? hovered
                       : nullptr;
    if (preview == this->hovered_)
    {
        return;
    }

    if (this->hovered_)
    {
        if (auto *session = this->findSession(this->hovered_.get()))
        {
            session->hovered = false;
            if (wantsAutoplay(*this->hovered_))
            {
                session->autoplay = true;
            }
            else
            {
                session->pause();
            }
        }
        this->hovered_.reset();
    }

    if (!preview)
    {
        return;
    }

    auto *session = this->findSession(preview.get());
    if (!session)
    {
        session = this->createSession(preview);
    }
    if (!session)
    {
        return;
    }

    this->hovered_ = preview;
    session->hovered = true;
    session->play(true);
}

void MediaPreviewManager::stopAll()
{
    this->hovered_.reset();
    this->autoplayQueue_.clear();

    std::erase_if(this->sessions_, [](const auto &it) {
        return it.second->autoplay || !it.second->preview()->isPlayerKind();
    });
    for (const auto &[key, session] : this->sessions_)
    {
        session->hovered = false;
        if (session->preview()->playing_)
        {
            session->pause();
        }
    }
    this->requestRepaint();
}

void MediaPreviewManager::onSessionError(MediaPreview *preview)
{
    preview->playbackError_ = true;
    preview->playing_ = false;
    this->requestRepaint();

    QTimer::singleShot(0, this, [this, preview] {
        if (this->hovered_.get() == preview)
        {
            this->hovered_.reset();
        }
        this->sessions_.erase(preview);
    });
}

void MediaPreviewManager::collectSessions()
{
    const auto now = Clock::now();

    if (!this->sessions_.empty())
    {
        this->requestRepaint();
    }

    if (this->hovered_ && !isPlayable(*this->hovered_))
    {
        this->hovered_.reset();
    }

    std::vector<const MediaPreview *> unused;
    for (const auto &[key, session] : this->sessions_)
    {
        const auto &preview = *session->preview();
        const bool visible = now - preview.lastPainted_ < VISIBILITY_TIMEOUT;

        if (!isPlayable(preview))
        {
            unused.push_back(key);
            continue;
        }

        if (preview.kind_ == Kind::Image)
        {
            if (session->hovered && this->hovered_.get() == key)
            {
                continue;
            }
            if (session->autoplay)
            {
                if (visible && wantsAutoplay(preview))
                {
                    continue;
                }
            }
            else if (visible &&
                     now - session->pausedSince < PAUSED_SESSION_TIMEOUT)
            {
                continue;
            }
            unused.push_back(key);
            continue;
        }

        if (session->autoplay)
        {
            if (!visible || !wantsAutoplay(preview))
            {
                unused.push_back(key);
            }
            continue;
        }

        if (visible)
        {
            continue;
        }
        if (preview.playing_)
        {
            session->pause();
            continue;
        }
        if (now - std::max(session->pausedSince, preview.lastPainted_) >
            USER_SESSION_TIMEOUT)
        {
            unused.push_back(key);
        }
    }

    for (const auto *key : unused)
    {
        this->sessions_.erase(key);
    }

    if (++this->collectTicks_ % 60 == 0)
    {
        for (auto it = this->cache_.begin(); it != this->cache_.end();)
        {
            auto preview = it->lock();
            if (!preview)
            {
                it = this->cache_.erase(it);
                continue;
            }
            if (!preview->imageData_.isEmpty() &&
                !this->findSession(preview.get()) &&
                now - preview->lastPainted_ > IMAGE_DATA_TIMEOUT)
            {
                preview->imageData_.clear();
            }
            ++it;
        }
    }
}

void MediaPreviewManager::requestPoster(const MediaPreviewPtr &preview)
{
#ifdef TINYRINO_WITH_MULTIMEDIA
    if (!preview || preview->posterRequested_ || preview->kind_ != Kind::Video)
    {
        return;
    }
    preview->posterRequested_ = true;

    if (preview->mediaFile_.isEmpty())
    {
        this->loadMediaFile(preview);
        return;
    }

    this->posterQueue_.emplace_back(preview);
    if (!this->posterBusy_)
    {
        this->processNextPoster();
    }
#else
    (void)preview;
#endif
}

#ifdef TINYRINO_WITH_MULTIMEDIA

void MediaPreviewManager::ensurePosterPlayer()
{
    if (this->posterPlayer_)
    {
        return;
    }

    this->posterPlayer_ = new QMediaPlayer(this);
    this->posterSink_ = new QVideoSink(this);
    this->posterPlayer_->setVideoSink(this->posterSink_);

    this->posterTimeout_ = new QTimer(this);
    this->posterTimeout_->setSingleShot(true);

    QObject::connect(this->posterSink_, &QVideoSink::videoFrameChanged, this,
                     &MediaPreviewManager::onPosterFrame);
    QObject::connect(
        this->posterPlayer_, &QMediaPlayer::durationChanged, this,
        [this](qint64 duration) {
            if (this->posterCurrent_ && this->posterCurrent_->duration_ == 0)
            {
                this->posterCurrent_->duration_ = duration;
            }
        });
    QObject::connect(this->posterPlayer_, &QMediaPlayer::errorOccurred, this,
                     [this](QMediaPlayer::Error error) {
                         if (error != QMediaPlayer::NoError)
                         {
                             this->finishPoster();
                         }
                     });
    QObject::connect(this->posterTimeout_, &QTimer::timeout, this,
                     &MediaPreviewManager::finishPoster);
}

void MediaPreviewManager::processNextPoster()
{
    this->posterCurrent_.reset();
    while (!this->posterQueue_.empty())
    {
        auto next = this->posterQueue_.front().lock();
        this->posterQueue_.pop_front();

        if (next && next->frame_.isNull() && !next->mediaFile_.isEmpty())
        {
            this->posterCurrent_ = std::move(next);
            break;
        }
    }

    if (!this->posterCurrent_)
    {
        this->posterBusy_ = false;
        if (this->posterPlayer_)
        {
            this->posterPlayer_->setSource({});
        }
        return;
    }

    this->posterBusy_ = true;
    this->ensurePosterPlayer();
    this->posterPlayer_->setSource(
        QUrl::fromLocalFile(this->posterCurrent_->mediaFile_));
    this->posterPlayer_->play();
    this->posterTimeout_->start(POSTER_TIMEOUT_MS);
}

void MediaPreviewManager::onPosterFrame(const QVideoFrame &frame)
{
    if (!this->posterCurrent_ || !frame.isValid())
    {
        return;
    }

    auto image = frame.toImage();
    if (image.isNull())
    {
        return;
    }

    if (this->posterCurrent_->frame_.isNull())
    {
        this->posterCurrent_->mediaSize_ = image.size();
        this->posterCurrent_->frame_ = toPreviewPixmap(std::move(image));
    }
    this->finishPoster();
}

void MediaPreviewManager::finishPoster()
{
    if (!this->posterCurrent_)
    {
        return;
    }

    this->posterTimeout_->stop();
    this->posterCurrent_.reset();
    this->requestLayout();

    QTimer::singleShot(0, this, [this] {
        this->posterPlayer_->stop();
        this->processNextPoster();
    });
}

#endif

}  // namespace chatterino
