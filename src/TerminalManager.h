#pragma once

#include "TerminalBackend.h"

#include <QElapsedTimer>
#include <QEvent>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QVariantList>
#include <QWindow>
#include <QtQml>

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

namespace konsoll
{

class TerminalLayoutNode final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool leaf READ isLeaf CONSTANT)
    Q_PROPERTY(int orientation READ orientation CONSTANT)
    Q_PROPERTY(QObject *first READ first NOTIFY layoutChanged)
    Q_PROPERTY(QObject *second READ second NOTIFY layoutChanged)
    Q_PROPERTY(QObject *session READ session CONSTANT)

public:
    explicit TerminalLayoutNode(TerminalBackend *session);
    TerminalLayoutNode(
        Qt::Orientation orientation,
        std::unique_ptr<TerminalLayoutNode> first,
        std::unique_ptr<TerminalLayoutNode> second
    );

    bool isLeaf() const { return m_session != nullptr; }
    int orientation() const { return static_cast<int>(m_orientation); }
    QObject *first() const { return m_first.get(); }
    QObject *second() const { return m_second.get(); }
    QObject *session() const { return m_session; }
    void notifyTreeChanged();

signals:
    void layoutChanged();

private:
    friend class TerminalManager;
    friend class TerminalWorkspaceModel;

    TerminalBackend *m_session{};
    std::unique_ptr<TerminalLayoutNode> m_first;
    std::unique_ptr<TerminalLayoutNode> m_second;
    quintptr m_orientation{static_cast<quintptr>(Qt::Horizontal)};
};

class TerminalWorkspaceModel final : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString title READ title NOTIFY titleChanged)
    Q_PROPERTY(QString customTitle READ customTitle NOTIFY titleChanged)
    // Why the tab wants a look while it is in the background: "activity"
    // (new output), "success" / "failure" (a command finished with that
    // exit status) or "bell"; empty when nothing happened or it is active.
    Q_PROPERTY(QString attention READ attention NOTIFY attentionChanged)
    // The last active pane's state for the tab icon: the ProcessKind name of
    // its job, whether it runs one, and its OSC 9 ; 4 progress
    // (Terminal::ProgressState, 0-100).
    Q_PROPERTY(QString processKind READ processKind NOTIFY statusChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY statusChanged)
    Q_PROPERTY(int progressState READ progressState NOTIFY statusChanged)
    Q_PROPERTY(int progressValue READ progressValue NOTIFY statusChanged)
    // The most urgent AI agent state among the workspace's panes (a pane
    // waiting for the user wins over one working): "working", "waiting",
    // "idle", "error" or empty, with that pane's agent name and message.
    Q_PROPERTY(QString agentState READ agentState NOTIFY statusChanged)
    Q_PROPERTY(QString agentName READ agentName NOTIFY statusChanged)
    Q_PROPERTY(QString agentMessage READ agentMessage NOTIFY statusChanged)
    Q_PROPERTY(QObject *rootNode READ rootNode NOTIFY layoutChanged)

public:
    TerminalWorkspaceModel(QString title, std::unique_ptr<TerminalLayoutNode> root);

    QString title() const;
    QString customTitle() const { return m_customTitle; }
    void setCustomTitle(QString title);
    // Attention levels, from least to most important; a higher one replaces
    // a lower one until the workspace is activated.
    enum class Attention : std::uint8_t {
        None,
        Activity,
        Success,
        Failure,
        Bell
    };

    QString attention() const;
    // Records `level` unless the workspace is active.
    void raiseAttention(Attention level);
    // New output: Activity, but not right after the workspace was left (the
    // focus change and resize make programs redraw).
    void markActivity();
    // The active workspace never holds attention.
    void setActive(bool active);
    QString processKind() const { return processKindName(m_processKind); }
    bool busy() const { return (m_flags & BusyFlag) != 0; }
    int progressState() const { return m_progressState; }
    int progressValue() const { return m_progressValue; }
    QString agentState() const { return agentStateName(m_agentState); }
    QString agentName() const { return m_agentName; }
    QString agentMessage() const { return m_agentMessage; }
    // Recomputes the status properties from the sessions in the tree.
    void refreshStatus();
    QObject *rootNode() const { return m_root.get(); }
    std::unique_ptr<TerminalLayoutNode> &root() { return m_root; }
    TerminalBackend *lastActiveSession() const { return m_lastActiveSession; }
    void setLastActiveSession(TerminalBackend *session);
    void notifySessionTitleChanged(TerminalBackend *session);

signals:
    void layoutChanged();
    void titleChanged();
    void attentionChanged();
    void statusChanged();

private:
    static constexpr unsigned char ActiveFlag = 1u;
    static constexpr unsigned char BusyFlag = 2u;

    std::unique_ptr<TerminalLayoutNode> m_root;
    TerminalBackend *m_lastActiveSession{};
    QString m_title;
    QString m_customTitle;
    QString m_agentName;
    QString m_agentMessage;
    QElapsedTimer m_inactiveTimer; // since the workspace was left
    int m_progressValue{};
    unsigned char m_progressState{};
    // One byte for both (-Wpadded): 18 kinds, 5 agent states.
    ProcessKind m_processKind : 5 {ProcessKind::None};
    AgentState m_agentState : 3 {AgentState::None};
    Attention m_attention{Attention::None};
    unsigned char m_flags{};
};

class TerminalManager : public QObject
{
    Q_OBJECT

    QML_NAMED_ELEMENT(TerminalManager)

    Q_PROPERTY(QObject *window READ window WRITE setWindow NOTIFY windowChanged)
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(QObject *activeSession READ activeSession NOTIFY activeSessionChanged)
    Q_PROPERTY(int activeWorkspaceIndex READ activeWorkspaceIndex NOTIFY activeWorkspaceChanged)

public:
    // The side of a target pane a dragged pane lands on.
    enum PaneEdge : quint8 {
        PaneEdgeLeft,
        PaneEdgeTop,
        PaneEdgeRight,
        PaneEdgeBottom
    };
    Q_ENUM(PaneEdge)

    explicit TerminalManager(QObject *parent = nullptr);
    ~TerminalManager() override;

    QObject *window() const { return m_window; }
    void setWindow(QObject *window);

    QVariantList workspaces() const;
    QObject *activeSession() const { return m_activeSession; }
    int activeWorkspaceIndex() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void addWorkspace();
    // A new tab titled `title`, its shell in `directory`, running `command`
    // (New Agent in Worktree).
    Q_INVOKABLE void addWorkspaceRunning(const QString &title, const QString &directory, const QString &command);
    Q_INVOKABLE void addNamedWorkspace(const QString &title);
    Q_INVOKABLE void renameWorkspace(QObject *workspace, const QString &title);
    Q_INVOKABLE void closeWorkspace(QObject *workspace);
    Q_INVOKABLE bool workspaceHasRunningCommand(QObject *workspace) const;
    // Same rule as closing a window: confirm when the workspace holds more
    // than one session (split panes) or any of them is running a command.
    Q_INVOKABLE bool workspaceNeedsCloseConfirmation(QObject *workspace) const;
    Q_INVOKABLE bool anySessionHasRunningCommand() const;
    Q_INVOKABLE bool anySessionHasActiveProcess() const;
    Q_INVOKABLE int sessionCount() const;
    // Compares by address only, so `session` may dangle.
    bool hasSession(const QObject *session) const;
    // Shows the session whose token is `token` (a clicked notification):
    // activates its pane and raises the window. False when not here.
    bool activateSessionWithToken(const QString &token);
    // Panes whose agent waits for the user (a question or a permission).
    int agentsWaitingCount() const;
    // The session in this window whose token is `token`, or null.
    TerminalBackend *sessionWithToken(const QString &token) const;
    // The session with that pane number (TerminalBackend::paneId), or null.
    TerminalBackend *sessionWithPaneId(int paneId) const;
    // Splits the pane of `session` (Qt::Orientation) and runs `command` in an
    // interactive shell in the new pane, leaving the focus where it is (an
    // agent asks for it while the user may be typing). Null when `session`
    // is not in this window.
    TerminalBackend *
    openPaneBeside(TerminalBackend *session, int orientation, const QString &command, const QString &directory = {});
    Q_INVOKABLE void moveWorkspace(int fromIndex, int toIndex);
    // Hands `workspace` and its running sessions to the manager of another
    // window, inserted at `index`; the shells keep running. A source window
    // left without workspaces emits windowShouldClose.
    Q_INVOKABLE bool moveWorkspaceTo(QObject *workspace, QObject *target, int index);
    // Moves the pane of `session` beside the pane of `targetSession`, which
    // may belong to another window's manager (`target`), splitting it on
    // `edge` (a PaneEdge). The shell keeps running; a workspace left without
    // panes goes away, and a window left without workspaces closes.
    Q_INVOKABLE bool movePane(QObject *session, QObject *target, QObject *targetSession, int edge);
    // Moves the pane of `session` into a workspace of its own in `target`,
    // inserted at `index`. A workspace's only pane moves the workspace.
    Q_INVOKABLE bool movePaneToWorkspace(QObject *session, QObject *target, int index);
    Q_INVOKABLE void closeSession(QObject *session);
    // The sessions of `workspace`'s panes, in layout order.
    Q_INVOKABLE QVariantList workspaceSessions(QObject *workspace) const;
    // Takes the pane of `session` out of this window, still running, for the
    // background commands (BackgroundJobs). A window left without workspaces
    // gets a fresh one instead of closing. Null when it is not owned here.
    std::unique_ptr<TerminalBackend> releaseSession(QObject *session);
    // Takes every session of `workspace` out of this window, like
    // releaseSession; `title` receives the workspace's custom name.
    std::vector<std::unique_ptr<TerminalBackend>> releaseWorkspace(QObject *workspace, QString &title);
    // Shows a running session (from the background) as a new workspace at the
    // end of the tabs, named `title` when not empty, and activates it.
    void adoptSession(std::unique_ptr<TerminalBackend> session, const QString &title);
    Q_INVOKABLE void activateSession(QObject *session);
    // The pane running an AI agent that text from `session` would go to: one
    // in the same tab first, then any in the window; null when none runs.
    Q_INVOKABLE QObject *agentSessionFor(QObject *session) const;
    // Pastes `text` into that agent's pane (as its next prompt, not sent)
    // and makes it the active pane. False when no agent runs.
    Q_INVOKABLE bool sendToAgent(QObject *session, const QString &text);
    Q_INVOKABLE void setActiveWorkspace(int index);
    Q_INVOKABLE void splitActive(int orientation);
    Q_INVOKABLE void navigateActivationHistory(int direction);
    Q_INVOKABLE void routeKeyPress(QObject *session, int key, const QString &text, int modifiers);
    // routeKeyPress with an event type: press (1), repeat (2), release (3).
    Q_INVOKABLE void routeKeyEvent(QObject *session, int key, const QString &text, int modifiers, int eventType);
    Q_INVOKABLE void routeTextInput(QObject *session, const QString &text);

    QJsonArray serializeState() const;
    Q_INVOKABLE void restoreState(const QJsonArray &state);

signals:
    void windowChanged();
    void workspacesChanged();
    void activeWorkspaceChanged();
    void activeSessionChanged();
    void windowShouldClose();

private:
    struct DetachedWorkspace {
        std::unique_ptr<TerminalWorkspaceModel> workspace;
        std::vector<std::unique_ptr<TerminalBackend>> sessions;
    };

    TerminalBackend *createSession();
    void connectSession(TerminalBackend *session);
    std::optional<DetachedWorkspace> detachWorkspace(TerminalWorkspaceModel *workspace);
    void adoptWorkspace(DetachedWorkspace detached, int index);
    TerminalBackend *ownedSession(const QObject *session) const;
    TerminalWorkspaceModel *workspaceOf(TerminalBackend *session) const;
    // Splits the pane of `session` with a new, not yet started session.
    TerminalBackend *splitSession(TerminalBackend *session, int orientation);
    // Tab attention and a desktop notification for an agent's new state.
    void onAgentStatusChanged(TerminalBackend *session, AgentState previous);
    // Whether the user is looking at `session`: the active pane of the
    // active window of the frontmost app.
    bool sessionInView(const TerminalBackend *session) const;
    // Takes the pane of `session` out of its workspace's tree. `notify` false
    // leaves the layout signals to the caller, which reshapes the same tree.
    std::unique_ptr<TerminalLayoutNode> extractPane(TerminalBackend *session, bool notify = true);
    void handOverSession(TerminalBackend *session, TerminalManager *target);
    void insertPaneWorkspace(std::unique_ptr<TerminalLayoutNode> leaf, int index);
    void finishPaneMove(TerminalManager *target, TerminalBackend *session);
    // Drops a session that left this window from the activation history.
    void forgetSession(TerminalBackend *session);
    TerminalBackend *createWorkspace(const QString &customTitle, const QString &directory = {});
    TerminalLayoutNode *findLeaf(TerminalLayoutNode *node, TerminalBackend *session) const;
    TerminalBackend *firstSession(TerminalLayoutNode *node) const;
    TerminalBackend *mostRecentlyActiveSession() const;
    TerminalBackend *firstValidSessionIn(TerminalWorkspaceModel *workspace) const;
    TerminalBackend *nextActiveSession(TerminalBackend *fallback) const;
    std::unique_ptr<TerminalLayoutNode> *
    findNodeSlot(std::unique_ptr<TerminalLayoutNode> &node, TerminalBackend *session) const;
    // The slot holding the split whose direct child is the leaf of `session`.
    std::unique_ptr<TerminalLayoutNode> *
    findParentSlot(std::unique_ptr<TerminalLayoutNode> &node, TerminalBackend *session) const;
    void setActiveSession(TerminalBackend *session, bool record);
    void recordActivation(TerminalBackend *session);
    void pruneSession(TerminalBackend *session);
    void removeSessionFromLayouts(TerminalBackend *session);
    void commitActivationHistory();
    QJsonObject serializeLayout(TerminalLayoutNode *root) const;
    std::unique_ptr<TerminalLayoutNode> buildLayoutNode(const QJsonObject &rootJson);

    std::vector<std::unique_ptr<TerminalBackend>> m_sessions;
    std::vector<std::unique_ptr<TerminalWorkspaceModel>> m_workspaces;
    std::vector<TerminalBackend *> m_activationHistory;
    TerminalBackend *m_activeSession{};
    QObject *m_window{};
    qsizetype m_activationHistoryCursor{};
    qsizetype m_nextWorkspaceNumber{1};

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
};

} // namespace konsoll
