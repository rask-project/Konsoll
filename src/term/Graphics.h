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
    // kPlacementNatural: pixel size; kPlacementScaled: c= / r= stretch it;
    // kPlacementVirtual: U=1, drawn only where Unicode placeholders are.
    short scaled{0};
    short offsetX{0}; // pixels inside the first cell
    short offsetY{0};
};

inline constexpr short kPlacementNatural = 0;
inline constexpr short kPlacementScaled = 1;
inline constexpr short kPlacementVirtual = 2;

// One animation frame (kitty a=f): its pixels and how long it shows (ms).
struct GraphicsFrame {
    std::shared_ptr<const QImage> pixels;
    std::uint64_t serial{0};
    std::int64_t gap{40};
};

// `pixels` / `serial` are the frame on screen; `frames` holds every frame
// (the transmitted image is frame 1).
struct GraphicsImage {
    std::shared_ptr<const QImage> pixels;
    std::uint64_t serial{0}; // unique per decoded frame, for texture caches
    std::uint64_t number{0}; // kitty image number (I=)
    std::vector<GraphicsFrame> frames;
    std::int64_t current{0};  // index of the frame shown
    std::int64_t elapsed{0};  // ms the current frame has shown
    std::int64_t running{0};  // 1 while the animation plays (a=a,s=3 / s=2)
    std::int64_t loops{0};    // loops left; 0 = forever
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

// Where an iTerm2 inline image went: the cells it covers (0 when nothing
// was placed).
struct ItermResult {
    int columns{0};
    int rows{0};
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
    // iTerm2 OSC 1337 File= (and MultipartFile / FilePart / FileEnd): `args`
    // are the File= key=value pairs, `base64` the file. Only inline=1 files
    // are shown. `screenColumns` / `screenRows` size the N% dimensions.
    ItermResult placeItermFile(const QByteArray &args, const QByteArray &base64, const GraphicsContext &context,
                               int screenColumns, int screenRows);

    // Deletes placements whose top row is in [first, last] on the main
    // (alternate = 0) or alternate screen.
    void erasePlacements(int alternate, long long first, long long last);
    void clearAlternate();
    // After a reflow: main-screen placements anchored at base + i move to
    // anchors[i]; ones past the end keep their distance from the last line.
    void remapAnchors(long long base, const std::vector<long long> &anchors);
    void clear();

    const std::vector<GraphicsPlacement> &placements() const { return m_placements; }
    // The virtual (U=1) placement Unicode placeholders of image `imageId`
    // (and `placementId`, 0 = any) refer to.
    const GraphicsPlacement *virtualPlacement(std::uint32_t imageId, std::uint32_t placementId) const;
    // Animations: whether any plays, and advancing them by `ms`. Returns
    // whether a frame changed.
    bool animating() const;
    bool advanceAnimations(int ms);
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
    KittyResult addFrame(const QByteArray &control, const QByteArray &payload);
    void controlAnimation(const QByteArray &control);
    void composeFrames(const QByteArray &control);
    void showFrame(GraphicsImage &image, std::int64_t index);
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
