#include "TextLinks.h"

#include "LinkSettings.h"

#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStringList>
#include <QUrl>

#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <climits>
#include <initializer_list>
#include <ranges>

namespace konsoll
{

namespace
{

constexpr int kMaxLinkLength = 1024;
// Words joined across single spaces before and after the pointed one ("my
// file name.txt" in ls output), longest first.
constexpr std::array<std::pair<int, int>, 8> kJoins{{{2, 2}, {2, 1}, {1, 2}, {1, 1}, {2, 0}, {0, 2}, {1, 0}, {0, 1}}};
constexpr char32_t kObjectReplacement = 0xFFFC; // a grapheme cluster's cell
constexpr std::u32string_view kOpeners = U"([{";
constexpr std::u32string_view kClosers = U")]}";
constexpr std::u32string_view kTrailingPunctuation = U".,;:!?";
constexpr std::u32string_view kQuotes = U"\"'`";

// How long a lookup is trusted, and how often the mount table is re-read.
constexpr qint64 kFileTtlMs = 2000;
constexpr qint64 kMountTtlMs = 5000;
constexpr qsizetype kMaxCachedPaths = 2048;
constexpr int kMaxSymlinks = 32;

struct Span {
    int start{0};
    int end{0};

    bool contains(int position) const { return position >= start && position < end; }
    int length() const { return end - start; }
};

bool isDelimiter(char32_t c)
{
    if (c == kWideSpacerChar) {
        return false;
    }
    // Controls, spaces, quotes, shell punctuation, box drawing (tree, tables).
    return c <= 0x20u || c == 0x7fu || c == 0xa0u || c == 0x3000u || c == kObjectReplacement ||
           (c >= 0x2500u && c <= 0x259fu) || std::u32string_view(U"\"'`<>|").contains(c);
}

// Position `i` belongs to a word: no delimiter, or a backslash-escaped
// space ("my\ file", as shells complete it).
bool isWordChar(std::u32string_view text, int i)
{
    const char32_t c = text[static_cast<std::size_t>(i)];
    return !isDelimiter(c) || (c == U' ' && i > 0 && text[static_cast<std::size_t>(i) - 1] == U'\\');
}

Span wordAt(std::u32string_view text, int position)
{
    Span span{.start = position, .end = position};
    const int size = static_cast<int>(text.size());
    while (span.start > 0 && position - span.start < kMaxLinkLength && isWordChar(text, span.start - 1)) {
        --span.start;
    }
    while (span.end < size && span.end - position < kMaxLinkLength && isWordChar(text, span.end)) {
        ++span.end;
    }
    return span;
}

// Drops what surrounds a link in prose: opening brackets before it, and
// punctuation or unbalanced closing brackets after it ("(see a.md).").
Span trimmed(std::u32string_view text, Span span)
{
    const auto at = [text](int i) { return text[static_cast<std::size_t>(i)]; };
    while (span.start < span.end && kOpeners.contains(at(span.start))) {
        ++span.start;
    }
    while (span.start < span.end) {
        const char32_t last = at(span.end - 1);
        const auto closer = kClosers.find(last);
        if (kTrailingPunctuation.contains(last)) {
            --span.end;
            continue;
        }
        if (closer == std::u32string_view::npos) {
            break;
        }
        const auto inside = text.substr(static_cast<std::size_t>(span.start), static_cast<std::size_t>(span.length()));
        if (std::ranges::count(inside, kOpeners[closer]) >= std::ranges::count(inside, last)) {
            break;
        }
        --span.end;
    }
    return span;
}

// The text of a span: wide-character spacers dropped, "\ " unescaped.
QString spanText(std::u32string_view text, Span span)
{
    QString out;
    out.reserve(span.length());
    std::ranges::for_each(std::views::iota(span.start, span.end), [&](int i) {
        const char32_t c = text[static_cast<std::size_t>(i)];
        const bool escape = c == U'\\' && i + 1 < span.end && text[static_cast<std::size_t>(i) + 1] == U' ';
        if (c != kWideSpacerChar && !escape) {
            out.append(QString::fromUcs4(&c, 1));
        }
    });
    return out;
}

bool isAsciiAlphanumeric(char32_t c)
{
    return (c >= U'0' && c <= U'9') || (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');
}

bool startsWithAsciiCaseless(std::u32string_view text, std::size_t at, std::u32string_view prefix)
{
    if (text.size() - at < prefix.size()) {
        return false;
    }
    return std::ranges::equal(text.substr(at, prefix.size()), prefix, [](char32_t a, char32_t b) {
        return (a >= U'A' && a <= U'Z' ? a + 32 : a) == b;
    });
}

// Where a URL starts in the word, if one does (also after "url=" or "(").
std::optional<int> urlStart(std::u32string_view text, Span word)
{
    static constexpr std::array<std::u32string_view, 6>
        kPrefixes{U"https://", U"http://", U"ftp://", U"file://", U"mailto:", U"www."};
    const auto starts = std::views::iota(word.start, word.end);
    const auto found = std::ranges::find_if(starts, [&](int i) {
        const auto at = static_cast<std::size_t>(i);
        return (i == word.start || !isAsciiAlphanumeric(text[at - 1])) &&
               std::ranges::any_of(kPrefixes, [&](std::u32string_view prefix) {
                   return startsWithAsciiCaseless(text, at, prefix);
               });
    });
    return found == starts.end() ? std::nullopt : std::optional<int>(*found);
}

bool isDigits(QStringView text)
{
    return !text.isEmpty() && std::ranges::all_of(text, [](QChar c) { return c.isDigit(); });
}

// "path:12", "path:12:5" (grep -n, compilers) and "path(12)",
// "path(12,5)" (MSVC, .NET): the path and its location.
TextLinkCandidate withLocation(const QString &text, Span span)
{
    TextLinkCandidate plain{.text = text, .start = span.start, .end = span.end};
    QStringView path(text);
    QList<QStringView> numbers;
    if (path.endsWith(u')')) {
        const qsizetype open = path.lastIndexOf(u'(');
        if (open > 0) {
            numbers = path.sliced(open + 1, path.size() - open - 2).split(u',');
            path = path.first(open);
        }
    } else {
        // Up to two trailing ":N" groups.
        const auto trailingNumber = [&path] {
            const qsizetype colon = path.lastIndexOf(u':');
            return colon > 0 && isDigits(path.sliced(colon + 1));
        };
        while (numbers.size() < 2 && trailingNumber()) {
            const qsizetype colon = path.lastIndexOf(u':');
            numbers.prepend(path.sliced(colon + 1));
            path = path.first(colon);
        }
    }
    if (numbers.isEmpty() || numbers.size() > 2 || !std::ranges::all_of(numbers, isDigits)) {
        return plain;
    }
    return {
        .text = path.toString(),
        .start = span.start,
        .end = span.end,
        .line = numbers.at(0).toInt(),
        .column = numbers.size() > 1 ? numbers.at(1).toInt() : 0
    };
}

// Cheap checks before a candidate costs an lstat(): no flags, numbers,
// bare punctuation ("..", "/", "=>") or URLs.
bool plausiblePath(const QString &path)
{
    if (path.isEmpty() || path.size() > kMaxLinkLength || path.startsWith(u'-') || path.contains(u"://")) {
        return false;
    }
    if (!std::ranges::any_of(path, [](QChar c) { return c.isLetterOrNumber(); })) {
        return false;
    }
    return !std::ranges::all_of(path, [](QChar c) { return c.isDigit() || QStringView(u".,:").contains(c); });
}

void appendPathCandidates(std::vector<TextLinkCandidate> &out, const QString &text, Span span)
{
    const auto add = [&out](TextLinkCandidate candidate) {
        const bool known = std::ranges::any_of(out, [&candidate](const TextLinkCandidate &other) {
            return other.text == candidate.text;
        });
        if (!known && plausiblePath(candidate.text)) {
            out.push_back(std::move(candidate));
        }
    };
    const TextLinkCandidate located = withLocation(text, span);
    add(located);
    add({.text = text, .start = span.start, .end = span.end});
    // git diff headers: "--- a/src/x.cpp", "+++ b/src/x.cpp".
    if (located.text.startsWith(QStringLiteral("a/")) || located.text.startsWith(QStringLiteral("b/"))) {
        TextLinkCandidate stripped = located;
        stripped.text.remove(0, 2);
        add(stripped);
    }
    // "--output=build/app", "KEY=path".
    const qsizetype equals = text.lastIndexOf(u'=');
    if (equals >= 0) {
        const int skipped = static_cast<int>(equals) + 1;
        add(withLocation(text.sliced(equals + 1), {.start = span.start + skipped, .end = span.end}));
    }
}

// The string between the quotes around `position` ('my file.txt' as GNU
// ls quotes it, "path" in a message).
std::optional<Span> quotedAt(std::u32string_view text, int position)
{
    int open = position - 1;
    while (open >= 0 && position - open < kMaxLinkLength && !kQuotes.contains(text[static_cast<std::size_t>(open)])) {
        --open;
    }
    if (open < 0 || !kQuotes.contains(text[static_cast<std::size_t>(open)])) {
        return std::nullopt;
    }
    const char32_t quote = text[static_cast<std::size_t>(open)];
    const auto close = text.find(quote, static_cast<std::size_t>(position));
    if (close == std::u32string_view::npos || static_cast<int>(close) - open > kMaxLinkLength) {
        return std::nullopt;
    }
    const Span span{.start = open + 1, .end = static_cast<int>(close)};
    return span.contains(position) ? std::optional<Span>(span) : std::nullopt;
}

// The word starting `count` single-space-separated words before (count < 0)
// or ending that many after the span, or nullopt.
std::optional<Span> joinedSpan(std::u32string_view text, Span word, int before, int after)
{
    const int size = static_cast<int>(text.size());
    Span span = word;
    const auto plainSpace = [text](int i) { return text[static_cast<std::size_t>(i)] == U' '; };
    const auto extendLeft = [&]() {
        if (span.start < 2 || !plainSpace(span.start - 1) ||
            isDelimiter(text[static_cast<std::size_t>(span.start) - 2])) {
            return false;
        }
        span.start = wordAt(text, span.start - 2).start;
        return true;
    };
    const auto extendRight = [&]() {
        if (span.end + 1 >= size || !plainSpace(span.end) ||
            isDelimiter(text[static_cast<std::size_t>(span.end) + 1])) {
            return false;
        }
        span.end = wordAt(text, span.end + 1).end;
        return true;
    };
    const auto left = std::views::iota(0, before);
    const auto right = std::views::iota(0, after);
    const bool joined = std::ranges::all_of(left, [&](int) { return extendLeft(); }) &&
                        std::ranges::all_of(right, [&](int) { return extendRight(); });
    return joined && span.length() <= kMaxLinkLength ? std::optional<Span>(span) : std::nullopt;
}

void appendJoinedCandidates(std::vector<TextLinkCandidate> &out, std::u32string_view text, Span word, int position)
{
    std::ranges::for_each(kJoins, [&](const std::pair<int, int> &join) {
        const std::optional<Span> span = joinedSpan(text, word, join.first, join.second);
        if (!span) {
            return;
        }
        const Span inner = trimmed(text, *span);
        if (inner.contains(position)) {
            appendPathCandidates(out, spanText(text, inner), inner);
        }
    });
}

bool hasSuffix(const QString &path, std::initializer_list<QStringView> suffixes)
{
    const QString suffix = QFileInfo(path).suffix().toLower();
    return !suffix.isEmpty() && std::ranges::contains(suffixes, QStringView(suffix));
}

// Bundles a folder link must not open (that would launch or install them).
bool isBundle(const QString &path)
{
    return hasSuffix(
        path,
        {u"app",
         u"bundle",
         u"framework",
         u"plugin",
         u"kext",
         u"prefpane",
         u"appex",
         u"xpc",
         u"saver",
         u"qlgenerator",
         u"mdimporter",
         u"workflow",
         u"action",
         u"pkg",
         u"mpkg"}
    );
}

// Files whose default application runs or installs them.
bool runsWhenOpened(const QString &path)
{
    return hasSuffix(path, {u"command", u"tool",   u"terminal", u"pkg",     u"mpkg",         u"workflow",   u"action",
                            u"jar",     u"webloc", u"inetloc",  u"fileloc", u"mobileconfig", u"app",        u"py",
                            u"pyw",     u"rb",     u"pl",       u"sh",      u"bash",         u"zsh",        u"csh",
                            u"ksh",     u"fish",   u"tcl",      u"php",     u"scpt",         u"applescript"});
}

// A GUI editor's command line, run by /bin/sh with the path and location
// as arguments (never parsed as shell code). The app's PATH is the system's,
// so the usual places of editor command-line tools are added.
bool runEditor(const QString &command, const QString &path, int line, int column)
{
    QString script = command;
    if (!script.contains(QStringLiteral("{file}"))) {
        script += QStringLiteral(" {file}");
    }
    script.replace(QStringLiteral("{file}"), QStringLiteral("\"$1\""))
        .replace(QStringLiteral("{line}"), QStringLiteral("\"$2\""))
        .replace(QStringLiteral("{column}"), QStringLiteral("\"$3\""));
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    const QString home = QDir::homePath();
    const QStringList extra{
        QStringLiteral("/opt/homebrew/bin"),
        QStringLiteral("/usr/local/bin"),
        home + QStringLiteral("/.local/bin"),
        home + QStringLiteral("/bin"),
        environment.value(QStringLiteral("PATH"), QStringLiteral("/usr/bin:/bin:/usr/sbin:/sbin"))
    };
    environment.insert(QStringLiteral("PATH"), extra.join(u':'));
    QProcess process;
    process.setProgram(QStringLiteral("/bin/sh"));
    process.setArguments(
        {QStringLiteral("-c"),
         script,
         QStringLiteral("konsoll"),
         path,
         QString::number(std::max(line, 1)),
         QString::number(std::max(column, 1))}
    );
    process.setProcessEnvironment(environment);
    process.setStandardOutputFile(QProcess::nullDevice());
    process.setStandardErrorFile(QProcess::nullDevice());
    return process.startDetached();
}

bool openWith(const QStringList &arguments)
{
    return QProcess::startDetached(QStringLiteral("/usr/bin/open"), arguments);
}

} // namespace

QString shellQuoted(const QString &text)
{
    QString quoted = text;
    quoted.replace(u'\'', QStringLiteral("'\\''"));
    return u'\'' + quoted + u'\'';
}

bool TextLinkCandidate::isUrl() const
{
    return text.contains(u"://") || text.startsWith(QStringLiteral("mailto:"), Qt::CaseInsensitive);
}

std::vector<TextLinkCandidate> textLinkCandidates(std::u32string_view text, int position)
{
    std::vector<TextLinkCandidate> out;
    if (position < 0 || position >= static_cast<int>(text.size())) {
        return out;
    }
    const bool onWord = isWordChar(text, position);
    const Span raw = onWord ? wordAt(text, position) : Span{.start = position, .end = position};
    if (onWord) {
        const std::optional<int> url = urlStart(text, raw);
        if (url) {
            const Span span = trimmed(text, {.start = *url, .end = raw.end});
            if (!span.contains(position)) {
                return out;
            }
            QString target = spanText(text, span);
            if (target.startsWith(QStringLiteral("www."), Qt::CaseInsensitive)) {
                target.prepend(QStringLiteral("https://"));
            }
            out.push_back({.text = target, .start = span.start, .end = span.end});
            return out;
        }
        const Span word = trimmed(text, raw);
        if (word.contains(position)) {
            appendPathCandidates(out, spanText(text, word), word);
        }
    }
    const std::optional<Span> quoted = quotedAt(text, position);
    if (quoted) {
        appendPathCandidates(out, spanText(text, *quoted), *quoted);
    }
    if (onWord) {
        appendJoinedCandidates(out, text, raw, position);
    }
    return out;
}

// --- FileLinkResolver ------------------------------------------------------

FileLinkResolver::FileLinkResolver()
{
    m_clock.start();
}

FileLinkResolver &FileLinkResolver::shared()
{
    static FileLinkResolver instance;
    return instance;
}

std::optional<mode_t> FileLinkResolver::probe(const QString &absolutePath)
{
    const qint64 now = m_clock.elapsed();
    const qint64 cached = m_files.value(absolutePath, -1);
    if (cached >= 0 && now - (cached >> 16) < kFileTtlMs) {
        const auto mode = static_cast<mode_t>(cached & 0xffff);
        return mode == 0 ? std::nullopt : std::optional<mode_t>(mode);
    }
    if (m_files.size() > kMaxCachedPaths) {
        m_files.clear();
        m_directories.clear();
    }
    refreshMounts(now);
    const std::optional<mode_t> mode = lookUp(absolutePath);
    m_files.insert(absolutePath, (now << 16) | static_cast<qint64>(mode.value_or(0)));
    return mode;
}

// The directory part is resolved once (cached), so a lookup in it costs one
// lstat() of the last component.
std::optional<mode_t> FileLinkResolver::lookUp(const QString &path)
{
    const qsizetype slash = path.lastIndexOf(u'/');
    const QString name = path.sliced(slash + 1);
    if (slash < 0 || name.isEmpty() || name == u"." || name == u"..") {
        const std::optional<Resolved> resolved = resolve(path);
        return resolved ? std::optional<mode_t>(resolved->second) : std::nullopt;
    }
    const std::optional<QString> directory = realDirectory(slash == 0 ? QStringLiteral("/") : path.first(slash));
    if (!directory) {
        return std::nullopt;
    }
    const QString full = *directory + u'/' + name;
    if (isOnSlowVolume(full)) {
        return std::nullopt;
    }
    struct stat info{};
    if (::lstat(QFile::encodeName(full).constData(), &info) != 0) {
        return std::nullopt;
    }
    if (!S_ISLNK(info.st_mode)) {
        return info.st_mode;
    }
    const std::optional<Resolved> resolved = resolve(full);
    return resolved ? std::optional<mode_t>(resolved->second) : std::nullopt;
}

// `directory` with its links resolved ("" for the root), when it is a
// directory on a fast volume.
std::optional<QString> FileLinkResolver::realDirectory(const QString &directory)
{
    const qint64 now = m_clock.elapsed();
    auto entry = m_directories.constFind(directory);
    if (entry == m_directories.cend() || now - entry->checkedAt >= kFileTtlMs) {
        const std::optional<Resolved> resolved = resolve(directory);
        const bool usable = resolved && S_ISDIR(resolved->second);
        // The root is kept as "/": an empty entry means unusable.
        const QString real = !usable ? QString{} : resolved->first.isEmpty() ? QStringLiteral("/") : resolved->first;
        entry = m_directories.insert(directory, {.realPath = real, .checkedAt = now});
    }
    if (entry->realPath.isEmpty()) {
        return std::nullopt;
    }
    return entry->realPath == u"/" ? QString{} : entry->realPath;
}

// Walks `path` one component at a time with lstat(), never touching a path
// on a slow volume: a symbolic link's target is checked like any other path
// before it is followed.
std::optional<FileLinkResolver::Resolved> FileLinkResolver::resolve(const QString &path) const
{
    QStringList pending = path.split(u'/', Qt::SkipEmptyParts);
    std::ranges::reverse(pending);
    QString resolved; // "" = the root
    mode_t mode = S_IFDIR;
    int links = 0;
    std::array<char, PATH_MAX> target{};
    while (!pending.isEmpty()) {
        const QString part = pending.takeLast();
        if (part == u"." || part == u"..") {
            resolved.truncate(part == u".." ? std::max<qsizetype>(resolved.lastIndexOf(u'/'), 0) : resolved.size());
            mode = S_IFDIR;
            continue;
        }
        const QString next = resolved + u'/' + part;
        struct stat info{};
        if (isOnSlowVolume(next) || ::lstat(QFile::encodeName(next).constData(), &info) != 0) {
            return std::nullopt;
        }
        if (!S_ISLNK(info.st_mode)) {
            resolved = next;
            mode = info.st_mode;
            continue;
        }
        const ssize_t length = ::readlink(QFile::encodeName(next).constData(), target.data(), target.size() - 1);
        if (++links > kMaxSymlinks || length <= 0) {
            return std::nullopt;
        }
        const QString destination = QFile::decodeName(QByteArray(target.data(), static_cast<qsizetype>(length)));
        if (destination.startsWith(u'/')) {
            resolved.clear();
        }
        QStringList parts = destination.split(u'/', Qt::SkipEmptyParts);
        std::ranges::reverse(parts);
        pending.append(parts);
    }
    return Resolved{resolved, mode};
}

bool FileLinkResolver::isOnSlowVolume(const QString &realPath) const
{
    return std::ranges::any_of(m_slowMounts, [&realPath](const QString &mount) {
        return realPath.startsWith(mount) &&
               (realPath.size() == mount.size() || realPath.at(mount.size()) == u'/' || mount.endsWith(u'/'));
    });
}

// getfsstat(MNT_NOWAIT) reads the kernel's mount table without asking any
// file system (a hung network share answers nothing).
void FileLinkResolver::refreshMounts(qint64 now)
{
    if (m_mountsCheckedAt >= 0 && now - m_mountsCheckedAt < kMountTtlMs) {
        return;
    }
    m_mountsCheckedAt = now;
    const int count = ::getfsstat(nullptr, 0, MNT_NOWAIT);
    if (count <= 0) {
        return;
    }
    // Room for mounts that appear between the two calls.
    std::vector<struct statfs> mounts(static_cast<std::size_t>(count) + 8);
    const int filled = ::getfsstat(mounts.data(), static_cast<int>(mounts.size() * sizeof(struct statfs)), MNT_NOWAIT);
    mounts.resize(static_cast<std::size_t>(std::max(filled, 0)));
    static constexpr std::array<std::string_view, 9>
        kSlowTypes{"autofs", "nfs", "smbfs", "afpfs", "webdav", "cifs", "ftp", "sshfs", "ftpfs"};
    m_slowMounts.clear();
    std::ranges::for_each(mounts, [this](const struct statfs &mount) {
        const std::string_view type(mount.f_fstypename);
        const bool slow =
            (mount.f_flags & MNT_LOCAL) == 0 || type.contains("fuse") || std::ranges::contains(kSlowTypes, type);
        if (slow) {
            m_slowMounts.push_back(QFile::decodeName(mount.f_mntonname));
        }
    });
}

// --- Opening -----------------------------------------------------------------

FileOpenAction fileOpenAction(const QString &path, mode_t mode, bool text, bool editorConfigured)
{
    if (S_ISDIR(mode)) {
        return isBundle(path) ? FileOpenAction::Reveal : FileOpenAction::Finder;
    }
    const bool risky = runsWhenOpened(path) || (mode & (S_IXUSR | S_IXGRP | S_IXOTH)) != 0;
    if (text) {
        if (editorConfigured) {
            return FileOpenAction::Editor;
        }
        return risky ? FileOpenAction::TextEditor : FileOpenAction::DefaultApp;
    }
    return risky ? FileOpenAction::Reveal : FileOpenAction::DefaultApp;
}

QString editorCommandLine(const QString &command, const QString &path, int line, int column)
{
    QString commandLine = command;
    if (!commandLine.contains(QStringLiteral("{file}"))) {
        commandLine += QStringLiteral(" {file}");
    }
    return commandLine.replace(QStringLiteral("{line}"), QString::number(std::max(line, 1)))
        .replace(QStringLiteral("{column}"), QString::number(std::max(column, 1)))
        .replace(QStringLiteral("{file}"), shellQuoted(path));
}

QString openFileLink(const QString &path, int line, int column, bool &opened)
{
    opened = false;
    const std::optional<mode_t> mode = FileLinkResolver::shared().probe(path);
    if (!mode) {
        return {};
    }
    const LinkSettings &settings = LinkSettings::shared();
    const bool regular = S_ISREG(*mode);
    // Only regular files are read for their type; a FIFO would block.
    const bool text = regular && QMimeDatabase().mimeTypeForFile(path).inherits(QStringLiteral("text/plain"));
    switch (fileOpenAction(path, *mode, text, !settings.editorCommand().isEmpty())) {
    case FileOpenAction::Finder:
        opened = openWith({QStringLiteral("-a"), QStringLiteral("Finder"), path});
        return {};
    case FileOpenAction::Reveal:
        opened = openWith({QStringLiteral("-R"), path});
        return {};
    case FileOpenAction::TextEditor:
        opened = openWith({QStringLiteral("-t"), path});
        return {};
    case FileOpenAction::Editor:
        if (settings.editorInTerminal()) {
            opened = true;
            return editorCommandLine(settings.editorCommand(), path, line, column);
        }
        opened = runEditor(settings.editorCommand(), path, line, column);
        return {};
    case FileOpenAction::DefaultApp:
        break;
    }
    opened = QDesktopServices::openUrl(QUrl::fromLocalFile(path));
    return {};
}

} // namespace konsoll
