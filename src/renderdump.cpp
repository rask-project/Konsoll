#include "renderdump.h"

#include "TerminalBackend.h"
#include "TerminalRenderer.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QQuickWindow>
#include <QThread>

#include <iostream>
#include <memory>
#include <print>

namespace konsoll
{

int runRenderDump(std::span<char *> args)
{
    if (args.size() < 5) {
        std::println(std::cerr, "usage: konsoll --render-dump ROWS COLS OUT.png");
        return 2;
    }
    const int rows = std::max(QByteArray(args[2]).toInt(), 2);
    const int cols = std::max(QByteArray(args[3]).toInt(), 2);
    const QString out = QString::fromLocal8Bit(args[4]);

    QFile input;
    if (!input.open(stdin, QIODevice::ReadOnly)) {
        return 1;
    }
    const QByteArray bytes = input.readAll();

    QQuickWindow window;
    window.setColor(QColor(0x14, 0x14, 0x14));
    auto renderer = std::make_unique<TerminalRenderer>();
    renderer->setParentItem(window.contentItem());
    renderer->setFont(QFont(QStringLiteral("Menlo"), 14));
    const qreal cellWidth = renderer->cellWidth();
    const qreal cellHeight = renderer->cellHeight();

    TerminalBackend backend;
    backend.setSystemColors(QColor(0x14, 0x14, 0x14), QColor(0xe0, 0xe0, 0xe0));
    backend.setCellPixelSize(static_cast<int>(cellWidth), static_cast<int>(cellHeight));
    backend.startReplay(cols, rows, bytes);
    renderer->setSession(&backend);
    renderer->setSize(QSizeF(cols * cellWidth, rows * cellHeight));
    window.resize(static_cast<int>(cols * cellWidth), static_cast<int>(rows * cellHeight));
    // Not shown: grabWindow() renders an unexposed window offscreen with the
    // platform's graphics API (the offscreen platform falls back to the
    // software renderer, which cannot draw the cell rectangles).

    // Let the coalesced frame timer fire and the scene graph render.
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < 400) {
        QCoreApplication::processEvents();
        QThread::msleep(10);
    }
    const QImage frame = window.grabWindow();
    renderer.reset();
    if (frame.isNull() || !frame.save(out)) {
        std::println(std::cerr, "render-dump: could not grab or save the frame");
        return 1;
    }
    return 0;
}

} // namespace konsoll
