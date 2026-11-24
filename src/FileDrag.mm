#import <AppKit/AppKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include "FileDrag.h"

#include <QCoreApplication>
#include <QCursor>
#include <QDebug>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMouseEvent>
#include <QPointer>
#include <QWindow>

#include <utility>

// The drag's source: copies only, and gives the window the mouse release
// AppKit keeps during the drag, so the view does not stay "pressed".
@interface KonsollFileDragSource : NSObject <NSDraggingSource> {
    QPointer<QWindow> _window;
}
- (instancetype)initWithWindow:(QWindow *)window;
@end

@implementation KonsollFileDragSource

- (instancetype)initWithWindow:(QWindow *)window
{
    self = [super init];
    if (self != nil) {
        _window = window;
    }
    return self;
}

- (NSDragOperation)draggingSession:(NSDraggingSession *)session
    sourceOperationMaskForDraggingContext:(NSDraggingContext)context
{
    Q_UNUSED(session);
    Q_UNUSED(context);
    return NSDragOperationCopy;
}

- (void)draggingSession:(NSDraggingSession *)session
           endedAtPoint:(NSPoint)screenPoint
              operation:(NSDragOperation)operation
{
    Q_UNUSED(session);
    Q_UNUSED(screenPoint);
    Q_UNUSED(operation);
    if (QWindow *const window = _window.data()) {
        const QPointF global = QCursor::pos();
        const QPointF local = window->mapFromGlobal(global);
        QMouseEvent release(
            QEvent::MouseButtonRelease,
            local,
            local,
            global,
            Qt::LeftButton,
            Qt::NoButton,
            QGuiApplication::keyboardModifiers()
        );
        QCoreApplication::sendEvent(window, &release);
    }
    // The session does not keep its source: it kept itself (startDrag).
    [self release];
}

@end

// Names the promised file and has it written where it was dropped.
@interface KonsollFilePromiseDelegate : NSObject <NSFilePromiseProviderDelegate> {
    NSString *_fileName;
    konsoll::FilePromiseFulfil _fulfil;
}
- (instancetype)initWithFileName:(NSString *)fileName fulfil:(konsoll::FilePromiseFulfil)fulfil;
@end

@implementation KonsollFilePromiseDelegate

- (instancetype)initWithFileName:(NSString *)fileName fulfil:(konsoll::FilePromiseFulfil)fulfil
{
    self = [super init];
    if (self != nil) {
        _fileName = [fileName copy];
        _fulfil = std::move(fulfil);
    }
    return self;
}

- (void)dealloc
{
    [_fileName release];
    [super dealloc];
}

- (NSString *)filePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider fileNameForType:(NSString *)fileType
{
    Q_UNUSED(filePromiseProvider);
    Q_UNUSED(fileType);
    return _fileName;
}

- (void)filePromiseProvider:(NSFilePromiseProvider *)filePromiseProvider
          writePromiseToURL:(NSURL *)url
          completionHandler:(void (^)(NSError *errorOrNil))completionHandler
{
    Q_UNUSED(filePromiseProvider);
    // Called once; released when the transfer ends.
    void (^const handler)(NSError *) = [completionHandler copy];
    NSString *const path = [[url path] copy];
    konsoll::FilePromiseFulfil fulfil = _fulfil;
    dispatch_async(dispatch_get_main_queue(), ^{
      const QString localPath = QString::fromNSString(path);
      [path release];
      if (!fulfil) {
          handler([NSError errorWithDomain:NSCocoaErrorDomain code:NSUserCancelledError userInfo:nil]);
          [handler release];
          return;
      }
      fulfil(localPath, [handler](const QString &error) {
          if (error.isEmpty()) {
              handler(nil);
          } else {
              handler([NSError errorWithDomain:NSCocoaErrorDomain
                                          code:NSFileWriteUnknownError
                                      userInfo:@{NSLocalizedDescriptionKey : error.toNSString()}]);
          }
          [handler release];
      });
    });
}

@end

namespace konsoll
{

namespace
{

constexpr CGFloat kIconSize = 32;

NSView *viewOf(QWindow *window)
{
    static const bool cocoa = QGuiApplication::platformName() == QLatin1String("cocoa");
    return cocoa && window != nullptr ? reinterpret_cast<NSView *>(window->winId()) : nil;
}

// The last left-button press or drag AppKit delivered: Qt hands mouse
// events to QML after AppKit's dispatch, when [NSApp currentEvent] may be
// another event already (Qt's own drags keep the last mouse event too).
struct LastMouseEvent {
    void remember(NSEvent *event) const
    {
        [event retain];
        [current release];
        current = event;
    }
    mutable NSEvent *current{nil};
};
const LastMouseEvent s_lastMouseEvent{};

// Starts the drag of `writer` with `icon` under the pointer, from the
// mouse event being handled.
bool startDrag(QWindow *window, id<NSPasteboardWriting> writer, NSImage *icon)
{
    NSView *const view = viewOf(window);
    NSEvent *const event = s_lastMouseEvent.current;
    if (view == nil || event == nil || [event window] != [view window] || ([NSEvent pressedMouseButtons] & 1u) == 0) {
        qWarning() << "Konsoll: no mouse drag to start the file drag from";
        return false;
    }
    const NSPoint point = [view convertPoint:[event locationInWindow] fromView:nil];
    [icon setSize:NSMakeSize(kIconSize, kIconSize)];
    NSDraggingItem *const item = [[NSDraggingItem alloc] initWithPasteboardWriter:writer];
    [item setDraggingFrame:NSMakeRect(point.x - (kIconSize / 2), point.y - (kIconSize / 2), kIconSize, kIconSize)
                  contents:icon];
    // Released when the drag ends (draggingSession:endedAtPoint:operation:).
    KonsollFileDragSource *const source = [[KonsollFileDragSource alloc] initWithWindow:window];
    NSDraggingSession *const session = [view beginDraggingSessionWithItems:@[ item ] event:event source:source];
    [item release];
    if (session == nil) {
        [source release];
        return false;
    }
    [session setAnimatesToStartingPositionsOnCancelOrFail:YES];
    return true;
}

} // namespace

void trackMouseForFileDrags()
{
    static const bool cocoa = QGuiApplication::platformName() == QLatin1String("cocoa");
    static bool installed = false;
    if (!cocoa || installed) {
        return;
    }
    installed = true;
    // Never removed: it lives as long as the application.
    [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskLeftMouseDown | NSEventMaskLeftMouseDragged
                                          handler:^NSEvent *(NSEvent *event) {
                                            s_lastMouseEvent.remember(event);
                                            return event;
                                          }];
}

bool startFileDrag(QWindow *window, const QString &localPath)
{
    NSString *const path = localPath.toNSString();
    return startDrag(window, [NSURL fileURLWithPath:path], [[NSWorkspace sharedWorkspace] iconForFile:path]);
}

bool startFilePromiseDrag(QWindow *window, const QString &fileName, bool directory, FilePromiseFulfil fulfil)
{
    UTType *type = UTTypeFolder;
    if (!directory) {
        const QString suffix = QFileInfo(fileName).suffix();
        UTType *const bySuffix = suffix.isEmpty() ? nil : [UTType typeWithFilenameExtension:suffix.toNSString()];
        type = bySuffix != nil ? bySuffix : UTTypeData;
    }
    KonsollFilePromiseDelegate *const delegate =
        [[KonsollFilePromiseDelegate alloc] initWithFileName:fileName.toNSString() fulfil:std::move(fulfil)];
    NSFilePromiseProvider *const provider = [[NSFilePromiseProvider alloc] initWithFileType:[type identifier]
                                                                                   delegate:delegate];
    // The provider's delegate is weak: its userInfo keeps it.
    [provider setUserInfo:delegate];
    [delegate release];
    const bool started = startDrag(window, provider, [[NSWorkspace sharedWorkspace] iconForContentType:type]);
    [provider release];
    return started;
}

} // namespace konsoll
