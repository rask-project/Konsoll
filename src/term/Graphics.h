#pragma once

#include <QByteArray>
#include <QImage>
#include <QRect>

#include <cstdint>
#include <map>
#include <memory>
#include <vector>

namespace konsoll
{

// Where a new image lands: the cursor, as an absolute line (see
// Screen::lineOrigin) and column, plus the cell size in pixels.
struct GraphicsContext {
    long long anchor{0};
    int column{0};
    int cellWidth{8};
    int cellHeight{16};
    int alternate{0}; // 1 when the alternate screen is active
    long long top{0}; // absolute line of screen row 0
};

// An image shown on the screen. `anchor` is the absolute line of its top
// row, so it scrolls with the text; `columns` x `rows` cells it covers.
struct GraphicsPlacement {
    long long anchor{0};
    std::uint32_t imageId{0};
    std::uint32_t placementId{0};
    QRect source; // pixels of the image to show
    int column{0};
    int columns{1};
    int rows{1};
    int z{0};
    short alternate{0};
    short scaled{0}; // 1 when c= / r= set the size: the image stretches to it
    short offsetX{0}; // pixels inside the first cell
    short offsetY{0};
};

struct GraphicsImage {
    std::shared_ptr<const QImage> pixels;
    std::uint64_t serial{0}; // unique per decoded image, for texture caches
    std::uint64_t number{0}; // kitty image number (I=)
};

// The result of a kitty graphics command: the reply for the PTY and how far
// the cursor moves (cells) after a placement.
struct KittyResult {
    QByteArray reply;
    int cursorColumns{0};
    int cursorRows{0};
    int moveCursor{0}; // 1 when the cursor moves past the image
    int placed{0};     // 1 when the command placed an image
};

// Images and placements of one terminal: the kitty graphics protocol (APC G)
// and sixel (DCS q) both end up here.
class GraphicsStore
{
public:
    // `command` is an APC body after its 'G'.
    KittyResult handleKitty(const QByteArray &command, const GraphicsContext &context);
    // Places a decoded sixel image at the context's cursor; returns the
    // rows it covers.
    int addSixel(const QImage &image, const GraphicsContext &context);

    // Deletes placements whose top row is in [first, last] on the main
    // (alternate = 0) or alternate screen.
    void erasePlacements(int alternate, long long first, long long last);
    void clearAlternate();
    void clear();

    const std::vector<GraphicsPlacement> &placements() const { return m_placements; }
    const GraphicsImage *image(std::uint32_t id) const;
    // Increments on every change, so renderers know to rebuild.
    std::uint64_t generation() const { return m_generation; }

private:
    struct Pending {
        QByteArray control;
        QByteArray payload;
    };

    KittyResult runKitty(const QByteArray &control, const QByteArray &payload, const GraphicsContext &context);
    KittyResult transmit(const QByteArray &control, const QByteArray &payload, const GraphicsContext &context,
                         bool display);
    KittyResult place(const QByteArray &control, std::uint32_t imageId, const GraphicsContext &context);
    void remove(const QByteArray &control, const GraphicsContext &context);
    std::uint32_t storeImage(std::uint32_t id, std::uint64_t number, QImage image);
    std::uint32_t idForNumber(std::uint64_t number) const;
    void evict();
    void touch() { ++m_generation; }

    std::map<std::uint32_t, GraphicsImage> m_images;
    std::vector<GraphicsPlacement> m_placements;
    Pending m_pending;
    std::uint64_t m_generation{0};
    std::uint64_t m_nextSerial{1};
    std::uint64_t m_nextId{0x7f000000u}; // ids the terminal assigns (32-bit)
};

// Decodes sixel data (the DCS body after 'q'). `transparent` is P2 = 1:
// pixels no sixel set stay transparent instead of taking color register 0.
QImage decodeSixel(const QByteArray &data, bool transparent);

} // namespace konsoll
