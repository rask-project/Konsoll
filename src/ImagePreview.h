#pragma once

#include <QImage>
#include <QQuickImageProvider>
#include <QSize>
#include <QString>

namespace konsoll
{

// Cmd+hover previews of image, PDF and video files named in the output.
// Nothing happens without Cmd: the view asks once the pointer rests on a
// file link, and the thumbnail comes from macOS QuickLook
// (QLThumbnailGenerator, whose generators run in system processes), on the
// QML image loader's thread, never the GUI's.

// The image provider's name: sources are image://konsoll-preview/<id>.
inline constexpr auto kImagePreviewProvider = "konsoll-preview";

// The image source previewing `path` (a local file already checked by
// FileLinkResolver), or empty when its type has no preview (by extension:
// images, PDF, video). The modification time is part of it, so an edited
// file is not shown from the image cache.
QString imagePreviewSource(const QString &path);

// Serves image://konsoll-preview/ (registered on the QML engine by main()).
class ImagePreviewProvider final : public QQuickImageProvider
{
public:
    ImagePreviewProvider();

    QImage requestImage(const QString &id, QSize *size, const QSize &requestedSize) override;
};

} // namespace konsoll
