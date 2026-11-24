#pragma once

#include <QString>

#include <functional>

class QWindow;

namespace konsoll
{

// Dragging files out of a pane to the Finder (or any app taking files),
// started from the mouse drag being handled in `window`. Each returns false
// when no drag could start (not the Cocoa platform, no mouse event).

// A local file or folder, as itself.
bool startFileDrag(QWindow *window, const QString &localPath);

// A file that does not exist here yet (one on an ssh host): a file promise.
// Where it is dropped, the target names the path for it, and `fulfil` is
// called on the GUI thread with that path and the callback to call once
// it is written there (with an error message, empty on success).
using FilePromiseDone = std::function<void(const QString &error)>;
using FilePromiseFulfil = std::function<void(const QString &localPath, const FilePromiseDone &done)>;
bool startFilePromiseDrag(QWindow *window, const QString &fileName, bool directory, FilePromiseFulfil fulfil);

} // namespace konsoll
