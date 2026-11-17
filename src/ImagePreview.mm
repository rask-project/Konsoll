#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <QuickLookThumbnailing/QuickLookThumbnailing.h>

#include "ImagePreview.h"

#include <QDateTime>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QUrl>

#include <algorithm>
#include <atomic>

namespace konsoll
{

namespace
{

// The edge of a thumbnail when the view asks for no size, and the most it
// may ask for (pixels).
constexpr int kDefaultEdge = 512;
constexpr int kMaxEdge = 1024;
// QuickLook gets this long; a file it chews on longer shows no preview.
constexpr std::int64_t kTimeoutNs = 2'000'000'000;

// The waiter and QuickLook's completion handler agree on who keeps the
// image: whoever moves the state off Pending first.
constexpr int kPending = 0;
constexpr int kDone = 1;
constexpr int kAbandoned = 2;

QImage imageFromCGImage(CGImageRef cgImage)
{
    const auto width = static_cast<int>(CGImageGetWidth(cgImage));
    const auto height = static_cast<int>(CGImageGetHeight(cgImage));
    QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
    if (image.isNull()) {
        return {};
    }
    image.fill(Qt::transparent);
    CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef context = CGBitmapContextCreate(
        image.bits(),
        static_cast<size_t>(width),
        static_cast<size_t>(height),
        8,
        static_cast<size_t>(image.bytesPerLine()),
        space,
        static_cast<uint32_t>(kCGImageAlphaPremultipliedFirst) | static_cast<uint32_t>(kCGBitmapByteOrder32Host)
    );
    if (context != nullptr) {
        CGContextDrawImage(context, CGRectMake(0, 0, width, height), cgImage);
        CGContextRelease(context);
    }
    CGColorSpaceRelease(space);
    return image;
}

// A QuickLook thumbnail of `path` fitting `edge`, or a null image (no
// thumbnail for that file, or QuickLook took too long). Blocks the calling
// thread (the QML image loader's) until then.
QImage quickLookThumbnail(const QString &path, QSize edge)
{
    @autoreleasepool {
        NSURL *const url = [NSURL fileURLWithPath:path.toNSString()];
        QLThumbnailGenerationRequest *const request = [[QLThumbnailGenerationRequest alloc]
              initWithFileAtURL:url
                           size:CGSizeMake(edge.width(), edge.height())
                          scale:1.0
            representationTypes:QLThumbnailGenerationRequestRepresentationTypeThumbnail];
        dispatch_semaphore_t const done = dispatch_semaphore_create(0);
        // Block storage: it outlives this frame while the handler is pending.
        __block int state = kPending;
        __block CGImageRef result = nullptr;
        [[QLThumbnailGenerator sharedGenerator]
            generateBestRepresentationForRequest:request
                               completionHandler:^(QLThumbnailRepresentation *thumbnail, NSError *error) {
                                 CGImageRef image =
                                     thumbnail != nil && error == nil ? CGImageRetain(thumbnail.CGImage) : nullptr;
                                 result = image;
                                 int expected = kPending;
                                 if (std::atomic_ref<int>(state).compare_exchange_strong(expected, kDone)) {
                                     dispatch_semaphore_signal(done);
                                 } else if (image != nullptr) {
                                     CGImageRelease(image); // the waiter gave up
                                 }
                               }];
        bool finished = dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, kTimeoutNs)) == 0;
        if (!finished) {
            int expected = kPending;
            if (std::atomic_ref<int>(state).compare_exchange_strong(expected, kAbandoned)) {
                [[QLThumbnailGenerator sharedGenerator] cancelRequest:request];
            } else {
                // The handler won the race: its signal is on the way.
                dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
                finished = true;
            }
        }
        QImage image;
        if (finished && result != nullptr) {
            image = imageFromCGImage(result);
            CGImageRelease(result);
        }
        dispatch_release(done);
        [request release];
        return image;
    }
}

} // namespace

QString imagePreviewSource(const QString &path)
{
    const QMimeType type = QMimeDatabase().mimeTypeForFile(path, QMimeDatabase::MatchExtension);
    const QString name = type.name();
    const bool previewable = name.startsWith(QStringLiteral("image/")) || name.startsWith(QStringLiteral("video/")) ||
                             type.inherits(QStringLiteral("application/pdf"));
    if (!previewable) {
        return {};
    }
    const qint64 modified = QFileInfo(path).lastModified().toMSecsSinceEpoch();
    // Concatenated: the encoded path is full of "%2F", which arg() would
    // take for placeholders.
    return QStringLiteral("image://") + QLatin1StringView(kImagePreviewProvider) + u'/' +
           QString::fromLatin1(QUrl::toPercentEncoding(path)) + u'?' + QString::number(modified);
}

ImagePreviewProvider::ImagePreviewProvider() : QQuickImageProvider(QQuickImageProvider::Image) {}

QImage ImagePreviewProvider::requestImage(const QString &id, QSize *size, const QSize &requestedSize)
{
    const qsizetype query = id.lastIndexOf(u'?');
    const QString path = QUrl::fromPercentEncoding((query < 0 ? id : id.first(query)).toUtf8());
    const QSize edge = requestedSize.isEmpty()
                           ? QSize(kDefaultEdge, kDefaultEdge)
                           : requestedSize.boundedTo(QSize(kMaxEdge, kMaxEdge)).expandedTo(QSize(1, 1));
    QImage image = path.startsWith(u'/') ? quickLookThumbnail(path, edge) : QImage{};
    if (size != nullptr) {
        *size = image.size();
    }
    return image;
}

} // namespace konsoll
