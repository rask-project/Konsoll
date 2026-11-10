#include "Graphics.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QList>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <span>
#include <string_view>

namespace konsoll
{

namespace
{

// Decoded image memory kept per terminal before old images are evicted.
constexpr qsizetype kQuotaBytes = 320LL * 1024 * 1024;
// Largest image (in pixels) a command may create.
constexpr qint64 kMaxPixels = 10000LL * 10000;
// Largest compressed or file payload accepted.
constexpr qsizetype kMaxPayload = 400LL * 1024 * 1024;

// The key=value control data of a kitty graphics command.
class KittyControl
{
public:
    explicit KittyControl(const QByteArray &control)
    {
        std::ranges::for_each(control.split(','), [this](const QByteArray &pair) {
            if (pair.size() >= 2 && pair[1] == '=') {
                m_values.emplace_back(pair[0], pair.mid(2));
            }
        });
    }

    QByteArray value(char key) const
    {
        const auto found = std::ranges::find(m_values, key, &std::pair<char, QByteArray>::first);
        return found == m_values.end() ? QByteArray{} : found->second;
    }

    int number(char key, int fallback = 0) const
    {
        bool ok = false;
        const int parsed = value(key).toInt(&ok);
        return ok ? parsed : fallback;
    }

    std::uint32_t id(char key) const { return value(key).toUInt(); }

    char letter(char key, char fallback) const
    {
        const QByteArray v = value(key);
        return v.isEmpty() ? fallback : v.front();
    }

private:
    std::vector<std::pair<char, QByteArray>> m_values;
};

QByteArray inflate(const QByteArray &compressed)
{
    z_stream stream{};
    if (inflateInit(&stream) != Z_OK) {
        return {};
    }
    QByteArray out;
    std::array<char, 65536> chunk{};
    stream.next_in = reinterpret_cast<Bytef *>(const_cast<char *>(compressed.constData())); // zlib C API
    stream.avail_in = static_cast<uInt>(compressed.size());
    int status = Z_OK;
    while (status == Z_OK && out.size() < kMaxPayload) {
        stream.next_out = reinterpret_cast<Bytef *>(chunk.data());
        stream.avail_out = static_cast<uInt>(chunk.size());
        status = ::inflate(&stream, Z_NO_FLUSH);
        out.append(chunk.data(), static_cast<qsizetype>(chunk.size() - stream.avail_out));
    }
    inflateEnd(&stream);
    return status == Z_STREAM_END ? out : QByteArray{};
}

// t=s: a POSIX shared memory object, unlinked once read.
QByteArray readSharedMemory(const QByteArray &name, qsizetype size)
{
    const int fd = shm_open(name.constData(), O_RDONLY);
    if (fd < 0) {
        return {};
    }
    struct stat info{};
    QByteArray out;
    if (fstat(fd, &info) == 0 && info.st_size > 0) {
        const auto length = static_cast<std::size_t>(size > 0 ? std::min<qsizetype>(size, info.st_size) : info.st_size);
        void *const mapped = mmap(nullptr, length, PROT_READ, MAP_SHARED, fd, 0); // POSIX API
        if (mapped != MAP_FAILED) {
            out = QByteArray(static_cast<const char *>(mapped), static_cast<qsizetype>(length));
            munmap(mapped, length);
        }
    }
    close(fd);
    shm_unlink(name.constData());
    return out;
}

// t=f / t=t: a file (S= bytes from O=). Temporary files must live in a
// temporary directory and name the protocol, and are deleted after reading.
QByteArray readFile(const QString &path, bool temporary, qsizetype size, qsizetype offset)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.size() > kMaxPayload) {
        return {};
    }
    if (temporary) {
        const QString dir = info.canonicalPath();
        const bool inTemp = dir.startsWith(QDir::tempPath()) || dir.startsWith(QStringLiteral("/tmp")) ||
                            dir.startsWith(QStringLiteral("/private/tmp")) || dir.startsWith(QStringLiteral("/private/var/folders"));
        if (!inTemp || !info.fileName().contains(QStringLiteral("tty-graphics-protocol"))) {
            return {};
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly) || !file.seek(offset)) {
        return {};
    }
    QByteArray out = size > 0 ? file.read(size) : file.readAll();
    file.close();
    if (temporary) {
        QFile::remove(path);
    }
    return out;
}

// Raw RGB / RGBA pixels into an image (copied, so the image owns them).
QImage rawImage(const QByteArray &data, int width, int height, int format)
{
    const int bytesPerPixel = format == 24 ? 3 : 4;
    if (width <= 0 || height <= 0 || static_cast<qint64>(width) * height > kMaxPixels ||
        data.size() < static_cast<qsizetype>(width) * height * bytesPerPixel) {
        return {};
    }
    const QImage view(reinterpret_cast<const uchar *>(data.constData()), width, height, static_cast<qsizetype>(width) * bytesPerPixel,
                      format == 24 ? QImage::Format_RGB888 : QImage::Format_RGBA8888);
    return view.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
}

QByteArray kittyReply(std::uint32_t id, std::uint32_t number, std::uint32_t placement, const QByteArray &message)
{
    QByteArray out = "\x1b_Gi=" + QByteArray::number(id);
    if (number != 0) {
        out += ",I=" + QByteArray::number(number);
    }
    if (placement != 0) {
        out += ",p=" + QByteArray::number(placement);
    }
    return out + ";" + message + "\x1b\\";
}

bool intersects(const GraphicsPlacement &placement, long long line, int column)
{
    return line >= placement.anchor && line < placement.anchor + placement.rows && column >= placement.column &&
           column < placement.column + placement.columns;
}

} // namespace

const GraphicsImage *GraphicsStore::image(std::uint32_t id) const
{
    const auto found = m_images.find(id);
    return found == m_images.end() ? nullptr : &found->second;
}

std::uint32_t GraphicsStore::idForNumber(std::uint64_t number) const
{
    // The newest image with this number.
    const GraphicsImage *best = nullptr;
    std::uint32_t bestId = 0;
    std::ranges::for_each(m_images, [&](const auto &entry) {
        if (entry.second.number == number && (best == nullptr || entry.second.serial > best->serial)) {
            best = &entry.second;
            bestId = entry.first;
        }
    });
    return bestId;
}

std::uint32_t GraphicsStore::storeImage(std::uint32_t id, std::uint64_t number, QImage image)
{
    if (id == 0) {
        id = static_cast<std::uint32_t>(m_nextId++);
    }
    // Re-transmitting an id replaces the image and its placements.
    std::erase_if(m_placements, [id](const GraphicsPlacement &placement) { return placement.imageId == id; });
    m_images[id] = GraphicsImage{
        .pixels = std::make_shared<const QImage>(std::move(image)),
        .serial = m_nextSerial++,
        .number = number,
    };
    evict();
    touch();
    return id;
}

void GraphicsStore::evict()
{
    const auto total = [this] {
        qsizetype bytes = 0;
        std::ranges::for_each(m_images, [&bytes](const auto &entry) { bytes += entry.second.pixels->sizeInBytes(); });
        return bytes;
    };
    // Oldest first, unplaced images before placed ones.
    while (m_images.size() > 1 && total() > kQuotaBytes) {
        const auto placed = [this](std::uint32_t id) {
            return std::ranges::any_of(m_placements, [id](const GraphicsPlacement &p) { return p.imageId == id; });
        };
        auto victim = m_images.begin();
        std::ranges::for_each(m_images, [&](const auto &entry) {
            const auto &[id, image] = entry;
            const bool better = placed(victim->first) && !placed(id);
            const bool older = placed(victim->first) == placed(id) && image.serial < victim->second.serial;
            if (better || older) {
                victim = m_images.find(id);
            }
        });
        const std::uint32_t id = victim->first;
        std::erase_if(m_placements, [id](const GraphicsPlacement &p) { return p.imageId == id; });
        m_images.erase(victim);
    }
}

KittyResult GraphicsStore::handleKitty(const QByteArray &command, const GraphicsContext &context)
{
    const qsizetype separator = command.indexOf(';');
    const QByteArray control = separator < 0 ? command : command.left(separator);
    const QByteArray payload = separator < 0 ? QByteArray{} : command.mid(separator + 1);
    const KittyControl keys(control);

    // Chunked transmission: m=1 chunks append; the first chunk's control
    // data applies to the whole image.
    const bool more = keys.number('m') == 1;
    if (!m_pending.control.isEmpty() || more) {
        if (m_pending.control.isEmpty()) {
            m_pending.control = control;
        }
        if (m_pending.payload.size() + payload.size() <= kMaxPayload) {
            m_pending.payload += payload;
        }
        if (more) {
            return {};
        }
        const Pending complete = std::exchange(m_pending, Pending{});
        return runKitty(complete.control, complete.payload, context);
    }
    return runKitty(control, payload, context);
}

KittyResult GraphicsStore::runKitty(const QByteArray &control, const QByteArray &payload, const GraphicsContext &context)
{
    const KittyControl keys(control);
    switch (keys.letter('a', 't')) {
    case 't':
    case 'q':
        return transmit(control, payload, context, false);
    case 'T':
        return transmit(control, payload, context, true);
    case 'p': {
        const std::uint32_t id = keys.id('i') != 0 ? keys.id('i') : idForNumber(keys.id('I'));
        return place(control, id, context);
    }
    case 'd':
        remove(control, context);
        return {};
    default: {
        // Animation (f, a, c) is not supported.
        KittyResult result;
        if ((keys.id('i') != 0 || keys.id('I') != 0) && keys.number('q') < 2) {
            result.reply = kittyReply(keys.id('i'), keys.id('I'), 0, "EINVAL:animation is not supported");
        }
        return result;
    }
    }
}

KittyResult GraphicsStore::transmit(const QByteArray &control, const QByteArray &payload, const GraphicsContext &context,
                                    bool display)
{
    const KittyControl keys(control);
    const std::uint32_t requestedId = keys.id('i');
    const std::uint32_t number = keys.id('I');
    const int quiet = keys.number('q');
    const bool query = keys.letter('a', 't') == 'q';
    const bool answer = requestedId != 0 || number != 0;
    const auto fail = [&](const char *error) {
        KittyResult result;
        if (answer && quiet < 2) {
            result.reply = kittyReply(requestedId, number, keys.id('p'), error);
        }
        return result;
    };

    QByteArray data = QByteArray::fromBase64(payload);
    const char medium = keys.letter('t', 'd');
    if (medium == 'f' || medium == 't') {
        data = readFile(QString::fromUtf8(data), medium == 't', keys.number('S'), keys.number('O'));
    } else if (medium == 's') {
        data = readSharedMemory(data, keys.number('S'));
    }
    if (data.isEmpty()) {
        return fail("ENODATA:no image data");
    }
    if (keys.letter('o', 0) == 'z') {
        data = inflate(data);
        if (data.isEmpty()) {
            return fail("EINVAL:zlib decompression failed");
        }
    }
    const int format = keys.number('f', 32);
    QImage image = format == 100 ? QImage::fromData(data, "PNG")
                                 : rawImage(data, keys.number('s'), keys.number('v'), format);
    if (image.isNull() || static_cast<qint64>(image.width()) * image.height() > kMaxPixels) {
        return fail("EBADPNG:could not decode the image");
    }
    if (query) {
        return fail("OK"); // validated, not stored
    }
    const std::uint32_t id = storeImage(requestedId, number, std::move(image));
    KittyResult result = display ? place(control, id, context) : KittyResult{};
    if (answer && quiet == 0 && result.reply.isEmpty()) {
        result.reply = kittyReply(id, number, keys.id('p'), "OK");
    }
    return result;
}

KittyResult GraphicsStore::place(const QByteArray &control, std::uint32_t imageId, const GraphicsContext &context)
{
    const KittyControl keys(control);
    KittyResult result;
    const GraphicsImage *const stored = image(imageId);
    if (stored == nullptr) {
        if (keys.number('q') < 2 && (keys.id('i') != 0 || keys.id('I') != 0)) {
            result.reply = kittyReply(keys.id('i'), keys.id('I'), keys.id('p'), "ENOENT:no such image");
        }
        return result;
    }
    const QRect whole(0, 0, stored->pixels->width(), stored->pixels->height());
    QRect source(keys.number('x'), keys.number('y'), keys.number('w'), keys.number('h'));
    source.setWidth(source.width() > 0 ? source.width() : whole.width() - source.x());
    source.setHeight(source.height() > 0 ? source.height() : whole.height() - source.y());
    source = source.intersected(whole);
    if (source.isEmpty()) {
        return result;
    }
    GraphicsPlacement placement{
        .anchor = context.anchor,
        .imageId = imageId,
        .placementId = keys.id('p'),
        .source = source,
        .column = context.column,
        .columns = keys.number('c'),
        .rows = keys.number('r'),
        .z = keys.number('z'),
        .alternate = static_cast<short>(context.alternate),
        .scaled = static_cast<short>(keys.number('c') > 0 || keys.number('r') > 0 ? 1 : 0),
        .offsetX = static_cast<short>(std::clamp(keys.number('X'), 0, std::min(context.cellWidth - 1, 0x7fff))),
        .offsetY = static_cast<short>(std::clamp(keys.number('Y'), 0, std::min(context.cellHeight - 1, 0x7fff))),
    };
    const auto cells = [](int pixels, int cell) { return std::max(1, (pixels + cell - 1) / std::max(cell, 1)); };
    placement.columns = placement.columns > 0 ? placement.columns : cells(source.width() + placement.offsetX, context.cellWidth);
    placement.rows = placement.rows > 0 ? placement.rows : cells(source.height() + placement.offsetY, context.cellHeight);
    if (placement.placementId != 0) {
        std::erase_if(m_placements, [&placement](const GraphicsPlacement &p) {
            return p.imageId == placement.imageId && p.placementId == placement.placementId;
        });
    }
    m_placements.push_back(placement);
    touch();
    result.cursorColumns = placement.columns;
    result.cursorRows = placement.rows;
    result.moveCursor = keys.number('C') == 1 ? 0 : 1;
    result.placed = 1;
    if ((keys.id('i') != 0 || keys.id('I') != 0) && keys.number('q') == 0) {
        result.reply = kittyReply(imageId, keys.id('I'), placement.placementId, "OK");
    }
    return result;
}

void GraphicsStore::remove(const QByteArray &control, const GraphicsContext &context)
{
    const KittyControl keys(control);
    const char spec = keys.letter('d', 'a');
    const bool freeData = spec >= 'A' && spec <= 'Z';
    const auto lower = static_cast<char>(freeData ? spec - 'A' + 'a' : spec);
    const std::uint32_t id = lower == 'n' ? idForNumber(keys.id('I')) : keys.id('i');
    const std::uint32_t placementId = keys.id('p');
    const long long row = context.top + keys.number('y') - 1;
    const int column = keys.number('x') - 1;
    const auto matches = [&](const GraphicsPlacement &p) {
        switch (lower) {
        case 'i':
        case 'n':
            return p.imageId == id && (placementId == 0 || p.placementId == placementId);
        case 'c':
            return intersects(p, context.anchor, context.column);
        case 'p':
            return intersects(p, row, column);
        case 'x':
            return column >= p.column && column < p.column + p.columns;
        case 'y':
            return row >= p.anchor && row < p.anchor + p.rows;
        case 'z':
            return p.z == keys.number('z');
        case 'r':
            return p.imageId >= keys.id('x') && p.imageId <= keys.id('y');
        default: // 'a'
            return true;
        }
    };
    std::vector<std::uint32_t> affected;
    std::ranges::for_each(m_placements, [&](const GraphicsPlacement &p) {
        if (matches(p)) {
            affected.push_back(p.imageId);
        }
    });
    std::erase_if(m_placements, matches);
    if (freeData) {
        if ((lower == 'i' || lower == 'n') && id != 0) {
            affected.push_back(id);
        }
        std::ranges::for_each(affected, [this](std::uint32_t imageId) {
            const bool placed = std::ranges::any_of(m_placements, [imageId](const GraphicsPlacement &p) {
                return p.imageId == imageId;
            });
            if (!placed) {
                m_images.erase(imageId);
            }
        });
    }
    touch();
}

int GraphicsStore::addSixel(const QImage &image, const GraphicsContext &context)
{
    const std::uint32_t id = storeImage(0, 0, image.convertToFormat(QImage::Format_RGBA8888_Premultiplied));
    const auto cells = [](int pixels, int cell) { return std::max(1, (pixels + cell - 1) / std::max(cell, 1)); };
    m_placements.push_back(GraphicsPlacement{
        .anchor = context.anchor,
        .imageId = id,
        .placementId = 0,
        .source = image.rect(),
        .column = context.column,
        .columns = cells(image.width(), context.cellWidth),
        .rows = cells(image.height(), context.cellHeight),
        .z = 0,
        .alternate = static_cast<short>(context.alternate),
        .scaled = 0,
        .offsetX = 0,
        .offsetY = 0,
    });
    touch();
    return m_placements.back().rows;
}

void GraphicsStore::erasePlacements(int alternate, long long first, long long last)
{
    const auto before = m_placements.size();
    std::erase_if(m_placements, [&](const GraphicsPlacement &p) {
        return p.alternate == alternate && p.anchor >= first && p.anchor <= last;
    });
    if (m_placements.size() != before) {
        touch();
    }
}

void GraphicsStore::remapAnchors(long long base, const std::vector<long long> &anchors)
{
    if (anchors.empty()) {
        return;
    }
    const auto last = static_cast<long long>(anchors.size()) - 1;
    std::ranges::for_each(m_placements, [&](GraphicsPlacement &placement) {
        if (placement.alternate != 0) {
            return;
        }
        const long long index = placement.anchor - base;
        if (index >= 0 && index <= last) {
            placement.anchor = anchors[static_cast<std::size_t>(index)];
        } else if (index > last) {
            placement.anchor = anchors.back() + (index - last);
        }
    });
    touch();
}

void GraphicsStore::clearAlternate()
{
    const auto before = m_placements.size();
    std::erase_if(m_placements, [](const GraphicsPlacement &p) { return p.alternate != 0; });
    if (m_placements.size() != before) {
        touch();
    }
}

void GraphicsStore::clear()
{
    m_placements.clear();
    m_images.clear();
    m_pending = {};
    touch();
}

// --- Sixel -----------------------------------------------------------------

namespace
{

// VT340 default color registers 0-15.
constexpr std::array<QRgb, 16> kSixelDefaults{
    0xff000000, 0xff3333cc, 0xffcc2121, 0xff33cc33, 0xffcc33cc, 0xff33cccc, 0xffcccc33, 0xff878787,
    0xff424242, 0xff545499, 0xff994242, 0xff549954, 0xff995499, 0xff549999, 0xff999954, 0xffcccccc,
};

constexpr int kMaxSixelSize = 4096;

class SixelDecoder
{
public:
    explicit SixelDecoder(bool transparent) : m_transparent(transparent ? 1 : 0)
    {
        std::ranges::copy(kSixelDefaults, m_palette.begin());
    }

    void feed(const QByteArray &data)
    {
        std::ranges::for_each(data, [this](char c) { step(static_cast<unsigned char>(c)); });
        flushCommand();
    }

    QImage image() const
    {
        const int width = std::max(m_width, m_rasterWidth);
        const int height = std::max(m_height, m_rasterHeight);
        if (width <= 0 || height <= 0) {
            return {};
        }
        QImage out(width, height, QImage::Format_ARGB32);
        out.fill(m_transparent != 0 ? QColor(Qt::transparent).rgba() : m_palette[0]);
        std::ranges::for_each(m_pixels, [&out](const auto &pixel) {
            const auto [x, y, color] = pixel;
            out.setPixel(x, y, color);
        });
        return out;
    }

private:
    struct Pixel {
        int x;
        int y;
        QRgb color;
    };

    void step(unsigned char c)
    {
        if (c >= '0' && c <= '9') {
            m_number = std::min((m_number * 10) + (c - '0'), 99999);
            m_hasNumber = 1;
            return;
        }
        if (c == ';') {
            m_params.push_back(m_hasNumber != 0 ? m_number : 0);
            m_number = 0;
            m_hasNumber = 0;
            return;
        }
        flushCommand();
        m_command = 0;
        if (c == '#' || c == '"' || c == '!') {
            m_command = c;
        } else if (c == '$') {
            m_x = 0;
        } else if (c == '-') {
            m_x = 0;
            m_y += 6;
        } else if (c >= '?' && c <= '~') {
            draw(c - '?', std::exchange(m_repeat, 1));
        }
    }

    // Ends the pending #, " or ! command once its parameters are complete.
    void flushCommand()
    {
        if (m_command == 0) {
            return;
        }
        if (m_hasNumber != 0 || !m_params.empty()) {
            m_params.push_back(m_hasNumber != 0 ? m_number : 0);
        }
        m_number = 0;
        m_hasNumber = 0;
        const std::vector<int> params = std::exchange(m_params, {});
        const auto at = [&params](std::size_t i) { return i < params.size() ? params[i] : 0; };
        if (m_command == '!') {
            m_repeat = std::clamp(at(0), 1, kMaxSixelSize);
        } else if (m_command == '"') {
            m_rasterWidth = std::clamp(at(2), 0, kMaxSixelSize);
            m_rasterHeight = std::clamp(at(3), 0, kMaxSixelSize);
        } else if (m_command == '#') {
            defineColor(at(0), params.size() >= 5 ? at(1) : 0, at(2), at(3), at(4));
        }
        m_command = 0;
    }

    void defineColor(int index, int space, int a, int b, int c)
    {
        m_color = std::clamp(index, 0, 255);
        const auto percent = [](int v) { return std::clamp(v, 0, 100) * 255 / 100; };
        if (space == 2) {
            m_palette[static_cast<std::size_t>(m_color)] = qRgb(percent(a), percent(b), percent(c));
        } else if (space == 1) {
            // DEC HLS: hue 0 is blue, 120 red, 240 green.
            m_palette[static_cast<std::size_t>(m_color)] = QColor::fromHsl((a + 240) % 360, percent(c), percent(b)).rgb();
        }
    }

    void draw(int bits, int count)
    {
        const int run = std::min(count, kMaxSixelSize - m_x);
        if (m_y >= kMaxSixelSize || run <= 0) {
            return;
        }
        const QRgb color = m_palette[static_cast<std::size_t>(m_color)];
        std::ranges::for_each(std::views::iota(0, 6), [&](int bit) {
            if ((bits & (1 << bit)) == 0 || m_y + bit >= kMaxSixelSize) {
                return;
            }
            std::ranges::for_each(std::views::iota(m_x, m_x + run),
                                  [&](int x) { m_pixels.push_back(Pixel{.x = x, .y = m_y + bit, .color = color}); });
            m_height = std::max(m_height, m_y + bit + 1);
        });
        m_x += run;
        m_width = std::max(m_width, m_x);
    }

    std::array<QRgb, 256> m_palette{};
    std::vector<Pixel> m_pixels;
    std::vector<int> m_params;
    int m_color{0};
    int m_x{0};
    int m_y{0};
    int m_width{0};
    int m_height{0};
    int m_rasterWidth{0};
    int m_rasterHeight{0};
    int m_number{0};
    int m_repeat{1};
    int m_command{0};
    int m_hasNumber{0};
    int m_transparent{0};
};

} // namespace

QImage decodeSixel(const QByteArray &data, bool transparent)
{
    SixelDecoder decoder(transparent);
    decoder.feed(data);
    return decoder.image();
}

} // namespace konsoll
