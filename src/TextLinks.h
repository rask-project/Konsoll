#pragma once

#include <QElapsedTimer>
#include <QHash>
#include <QString>

#include <sys/types.h>

#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace konsoll
{

// Links found in the text of a terminal line that carries no OSC 8
// hyperlink: URLs, and paths of local files ("docs/RELEASE.md", grep's
// "src/main.cpp:42:", ls output). Nothing is scanned as output arrives: the
// view asks for the link under the pointer only while Cmd is held, so the
// cost is one line of text and a few cached lstat() calls per pointer move.

// The text handed to textLinkCandidates has one code point per cell; a wide
// character's trailing cell is this placeholder (part of the word, dropped
// from the link text).
inline constexpr char32_t kWideSpacerChar = 0;

// A possible link over a position of the text. `start`/`end` (exclusive)
// are positions in the text; `text` is a URL (always with its scheme) or a
// path as written (escapes removed), with its `path:line:column` location
// split off (0 = none).
struct TextLinkCandidate {
    QString text;
    int start{0};
    int end{0};
    int line{0};
    int column{0};

    bool isUrl() const;
};

// The candidates covering `position`, most likely first: a URL alone, or
// paths to look up on disk (the word, its location-free form, git's a/ b/
// prefix stripped, a quoted string, words joined across single spaces).
std::vector<TextLinkCandidate> textLinkCandidates(std::u32string_view text, int position);

// Looks paths up on disk for links, cheaply and never on a slow volume:
// network shares, automounts and FUSE file systems (sshfs) are skipped
// without being touched, symbolic links included (each one's target is
// checked before it is followed). Results are cached for a short time.
class FileLinkResolver
{
public:
    // The one instance, shared by every session.
    static FileLinkResolver &shared();

    // The st_mode of the file `absolutePath` names, or nullopt when it does
    // not exist or lies on a slow volume.
    std::optional<mode_t> probe(const QString &absolutePath);

    FileLinkResolver(const FileLinkResolver &) = delete;
    FileLinkResolver &operator=(const FileLinkResolver &) = delete;
    FileLinkResolver(FileLinkResolver &&) = delete;
    FileLinkResolver &operator=(FileLinkResolver &&) = delete;
    ~FileLinkResolver() = default;

private:
    FileLinkResolver();

    // A path with its symbolic links resolved, and the st_mode of what it
    // names.
    using Resolved = std::pair<QString, mode_t>;
    struct DirectoryEntry {
        QString realPath; // empty: missing, not a directory, or slow
        qint64 checkedAt{0};
    };

    std::optional<mode_t> lookUp(const QString &path);
    std::optional<QString> realDirectory(const QString &directory);
    std::optional<Resolved> resolve(const QString &path) const;
    bool isOnSlowVolume(const QString &realPath) const;
    void refreshMounts(qint64 now);

    // probe() results: (time << 16) | st_mode, st_mode 0 when missing.
    QHash<QString, qint64> m_files;
    QHash<QString, DirectoryEntry> m_directories;
    // Mount points of slow volumes (see the class comment).
    std::vector<QString> m_slowMounts;
    QElapsedTimer m_clock;
    qint64 m_mountsCheckedAt{-1};
};

// How a local file link opens.
enum class FileOpenAction : unsigned char {
    DefaultApp, // the file's default application
    Finder,     // a folder: shown in the Finder
    Reveal,     // selected in the Finder, not opened (apps, installers)
    TextEditor, // the default text editor (open -t): scripts, never run
    Editor      // the editor command of Settings > Terminal
};

// Decides how to open the file at `path` with `mode` (st_mode). Opening
// never runs anything: an executable, a script or an installer goes to a
// text editor or is only revealed. `text`: the file holds text.
FileOpenAction fileOpenAction(const QString &path, mode_t mode, bool text, bool editorConfigured);

// `text` single-quoted for zsh, bash and fish ('it'\''s').
QString shellQuoted(const QString &text);

// The editor command of Settings > Terminal with its {file}, {line} and
// {column} placeholders filled in for a shell that types it (the path
// single-quoted); "{file}" is appended when the command has none.
QString editorCommandLine(const QString &command, const QString &path, int line, int column);

// Opens the local file link `path` (line/column 0 = none) as
// fileOpenAction decides. A command for a terminal editor is not run here:
// it is returned so that the caller runs it in a new tab; empty otherwise.
// `opened` reports whether a program was started.
QString openFileLink(const QString &path, int line, int column, bool &opened);

} // namespace konsoll
