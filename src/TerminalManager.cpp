#include "TerminalManager.h"

#include "WindowRegistry.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace konsoll
{

namespace
{
// How much a pane's agent state wants the user, for the workspace's icon.
int agentUrgency(AgentState state)
{
    switch (state) {
    case AgentState::Waiting:
        return 4;
    case AgentState::Error:
        return 3;
    case AgentState::Working:
        return 2;
    case AgentState::Idle:
        return 1;
    case AgentState::None:
        break;
    }
    return 0;
}

// Removes the matching objects from `objects` (keeping the order of the rest)
// and destroys them on the next event-loop turn instead of right away. QML may
// still hold them on the current JS stack -- e.g. the tab's close handler that
// started this -- and if the GC runs before that handler returns (the model
// change below recreates delegates and allocates), it would mark freed
// memory. Ownership passes to Qt's deferred deletion.
template <typename T, typename Pred>
void eraseDeferred(std::vector<std::unique_ptr<T>> &objects, const QObject *receiver, Pred pred)
{
    auto const removed =
        std::ranges::stable_partition(objects, [&pred](const std::unique_ptr<T> &object) { return !pred(object); });
    std::ranges::for_each(removed, [receiver](std::unique_ptr<T> &object) {
        QObject::disconnect(object.get(), nullptr, receiver, nullptr);
        object.release()->deleteLater();
    });
    objects.erase(removed.begin(), removed.end());
}
} // namespace

TerminalLayoutNode::TerminalLayoutNode(TerminalBackend *session) : m_session(session) {}

TerminalLayoutNode::TerminalLayoutNode(
    Qt::Orientation orientation,
    std::unique_ptr<TerminalLayoutNode> first,
    std::unique_ptr<TerminalLayoutNode> second
)
    : m_first(std::move(first))
    , m_second(std::move(second))
    , m_orientation(static_cast<quintptr>(orientation))
{}

void TerminalLayoutNode::notifyTreeChanged()
{
    std::vector<TerminalLayoutNode *> pending;
    pending.push_back(this);
    while (!pending.empty()) {
        auto *node = pending.back();
        pending.pop_back();
        node->layoutChanged();
        if (node->m_second) {
            pending.push_back(node->m_second.get());
        }
        if (node->m_first) {
            pending.push_back(node->m_first.get());
        }
    }
}

TerminalWorkspaceModel::TerminalWorkspaceModel(QString title, std::unique_ptr<TerminalLayoutNode> root)
    : m_root(std::move(root))
    , m_title(std::move(title))
{
    m_inactiveTimer.start();
    connect(this, &TerminalWorkspaceModel::layoutChanged, this, &TerminalWorkspaceModel::refreshStatus);
}

QString TerminalWorkspaceModel::title() const
{
    if (!m_customTitle.isEmpty()) {
        return m_customTitle;
    }
    if (m_lastActiveSession == nullptr) {
        return m_title;
    }
    return m_lastActiveSession->displayTitle();
}

void TerminalWorkspaceModel::setCustomTitle(QString title)
{
    title = title.trimmed();
    if (m_customTitle == title) {
        return;
    }
    m_customTitle = std::move(title);
    emit titleChanged();
}

QString TerminalWorkspaceModel::attention() const
{
    switch (m_attention) {
    case Attention::Activity:
        return QStringLiteral("activity");
    case Attention::Success:
        return QStringLiteral("success");
    case Attention::Failure:
        return QStringLiteral("failure");
    case Attention::Bell:
        return QStringLiteral("bell");
    case Attention::None:
        break;
    }
    return {};
}

void TerminalWorkspaceModel::raiseAttention(Attention level)
{
    if ((m_flags & ActiveFlag) != 0 || level <= m_attention) {
        return;
    }
    m_attention = level;
    emit attentionChanged();
}

void TerminalWorkspaceModel::markActivity()
{
    constexpr qint64 kRedrawGraceMs = 2000;
    if (m_attention == Attention::None && (m_flags & ActiveFlag) == 0 && m_inactiveTimer.elapsed() >= kRedrawGraceMs) {
        raiseAttention(Attention::Activity);
    }
}

void TerminalWorkspaceModel::setActive(bool active)
{
    const bool wasActive = (m_flags & ActiveFlag) != 0;
    if (active == wasActive) {
        return;
    }
    m_flags = static_cast<unsigned char>(active ? (m_flags | ActiveFlag) : (m_flags & ~ActiveFlag));
    if (!active) {
        m_inactiveTimer.start();
        return;
    }
    if (m_attention != Attention::None) {
        m_attention = Attention::None;
        emit attentionChanged();
    }
}

void TerminalWorkspaceModel::refreshStatus()
{
    std::vector<TerminalBackend *> sessions;
    std::vector<TerminalLayoutNode *> pending;
    if (m_root) {
        pending.push_back(m_root.get());
    }
    while (!pending.empty()) {
        TerminalLayoutNode *node = pending.back();
        pending.pop_back();
        if (node->m_session != nullptr) {
            sessions.push_back(node->m_session);
        }
        std::ranges::for_each(std::array{node->m_second.get(), node->m_first.get()}, [&pending](auto *child) {
            if (child != nullptr) {
                pending.push_back(child);
            }
        });
    }
    // The icon follows the pane the user worked in last (else the first):
    // switching panes switches the icon.
    const auto last = std::ranges::find(sessions, m_lastActiveSession);
    const TerminalBackend *session = last != sessions.end() ? *last : (sessions.empty() ? nullptr : sessions.front());
    const bool busy = session != nullptr && (session->hasForegroundProcess() || session->commandRunning());
    const ProcessKind kind = session != nullptr ? session->processKindValue() : ProcessKind::None;
    const int progressState = session != nullptr ? session->progressState() : 0;
    const int progressValue = session != nullptr ? session->progressValue() : 0;
    const auto flags = static_cast<unsigned char>(busy ? (m_flags | BusyFlag) : (m_flags & ~BusyFlag));
    // The agent shown is the one that most wants the user, in any pane.
    const auto agent = std::ranges::max_element(sessions, {}, [](const TerminalBackend *candidate) {
        return agentUrgency(candidate->agentStateValue());
    });
    const AgentState agentState = agent != sessions.end() ? (*agent)->agentStateValue() : AgentState::None;
    const QString agentName = agent != sessions.end() ? (*agent)->agentName() : QString{};
    const QString agentMessage = agent != sessions.end() ? (*agent)->agentMessage() : QString{};

    if (kind == m_processKind && progressState == m_progressState && progressValue == m_progressValue &&
        flags == m_flags && agentState == m_agentState && agentName == m_agentName && agentMessage == m_agentMessage) {
        return;
    }
    m_agentState = agentState;
    m_agentName = agentName;
    m_agentMessage = agentMessage;
    m_processKind = kind;
    m_progressState = static_cast<unsigned char>(progressState);
    m_progressValue = progressValue;
    m_flags = flags;
    emit statusChanged();
}

void TerminalWorkspaceModel::setLastActiveSession(TerminalBackend *session)
{
    if (m_lastActiveSession == session) {
        return;
    }
    m_lastActiveSession = session;
    emit titleChanged();
    refreshStatus();
}

void TerminalWorkspaceModel::notifySessionTitleChanged(TerminalBackend *session)
{
    if (m_lastActiveSession == session) {
        emit titleChanged();
    }
}

TerminalManager::TerminalManager(QObject *parent) : QObject(parent)
{
    WindowRegistry::instance().add(this);
    if (QCoreApplication *application = QCoreApplication::instance()) {
        application->installEventFilter(this);
    }
}

TerminalManager::~TerminalManager()
{
    std::ranges::for_each(m_sessions, [](const auto &session) {
        while (!session->close()) {
            QThread::msleep(25);
        }
    });
    WindowRegistry::instance().remove(this);
}

void TerminalManager::setWindow(QObject *window)
{
    if (m_window == window) {
        return;
    }
    m_window = window;
    emit windowChanged();
}

bool TerminalManager::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Control || keyEvent->key() == Qt::Key_Meta) {
            // Only the manager whose window is focused commits its activation
            // history; with several windows, each manager sees the same key
            // release, and only the focused one should act on it.
            if (qobject_cast<QWindow *>(m_window) == QGuiApplication::focusWindow()) {
                commitActivationHistory();
            }
        }
    }
    return QObject::eventFilter(watched, event);
}

QVariantList TerminalManager::workspaces() const
{
    QVariantList list;
    list.reserve(static_cast<qsizetype>(m_workspaces.size()));
    std::ranges::transform(m_workspaces, std::back_inserter(list), [](const auto &workspace) {
        return QVariant::fromValue(static_cast<QObject *>(workspace.get()));
    });
    return list;
}

int TerminalManager::activeWorkspaceIndex() const
{
    for (std::size_t index = 0; index < m_workspaces.size(); ++index) {
        if (findLeaf(static_cast<TerminalLayoutNode *>(m_workspaces[index]->rootNode()), m_activeSession) != nullptr) {
            return static_cast<int>(index);
        }
    }
    return -1;
}

void TerminalManager::start()
{
    if (m_workspaces.empty()) {
        addWorkspace();
    }
}

void TerminalManager::addWorkspace()
{
    createWorkspace(QString{});
}

void TerminalManager::addNamedWorkspace(const QString &title)
{
    createWorkspace(title);
}

void TerminalManager::createWorkspace(const QString &customTitle)
{
    const QString workingDirectory = (m_activeSession != nullptr) ? m_activeSession->currentDirectory() : QString{};
    TerminalBackend *session = createSession();
    auto workspace = std::make_unique<TerminalWorkspaceModel>(
        QStringLiteral("Workspace %1").arg(m_nextWorkspaceNumber++),
        std::make_unique<TerminalLayoutNode>(session)
    );
    workspace->setCustomTitle(customTitle);
    m_workspaces.push_back(std::move(workspace));
    emit workspacesChanged();
    emit activeWorkspaceChanged();
    setActiveSession(session, true);
    session->start(workingDirectory);
}

void TerminalManager::renameWorkspace(QObject *workspaceObject, const QString &title)
{
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    if (workspace == nullptr) {
        return;
    }
    const auto it =
        std::ranges::find_if(m_workspaces, [workspace](const auto &candidate) { return candidate.get() == workspace; });
    if (it != m_workspaces.end()) {
        workspace->setCustomTitle(title);
    }
}

void TerminalManager::moveWorkspace(int fromIndex, int toIndex)
{
    const int count = static_cast<int>(m_workspaces.size());
    if (fromIndex < 0 || fromIndex >= count || toIndex < 0 || toIndex >= count || fromIndex == toIndex) {
        return;
    }

    const auto first = m_workspaces.begin();
    if (fromIndex < toIndex) {
        std::rotate(first + fromIndex, first + fromIndex + 1, first + toIndex + 1);
    } else {
        std::rotate(first + toIndex, first + fromIndex, first + fromIndex + 1);
    }
    emit workspacesChanged();
    emit activeWorkspaceChanged();
}

TerminalBackend *TerminalManager::createSession()
{
    auto session = std::make_unique<TerminalBackend>();
    auto *sessionPtr = session.get();
    connectSession(sessionPtr);
    m_sessions.push_back(std::move(session));
    return sessionPtr;
}

void TerminalManager::connectSession(TerminalBackend *sessionPtr)
{
    // The close is queued to whichever window owns the session by then: its
    // workspace may move to another window in between.
    const auto scheduleClose = [sessionPtr]() {
        QTimer::singleShot(0, QCoreApplication::instance(), [sessionPtr]() {
            WindowRegistry::instance().closeSession(sessionPtr);
        });
    };
    connect(sessionPtr, &TerminalBackend::processExited, this, [scheduleClose](int) { scheduleClose(); });
    connect(sessionPtr, &TerminalBackend::startFailed, this, [scheduleClose](const QString &) { scheduleClose(); });
    connect(sessionPtr, &TerminalBackend::workerThreadStopped, this, scheduleClose);
    // Tab indicators: the workspace holding the session records why it
    // wants attention and refreshes its icon state.
    const auto raise = [this, sessionPtr](TerminalWorkspaceModel::Attention level) {
        if (TerminalWorkspaceModel *workspace = workspaceOf(sessionPtr)) {
            workspace->raiseAttention(level);
        }
    };
    connect(sessionPtr, &TerminalBackend::commandFinished, this, [raise](int exitCode) {
        raise(exitCode > 0 ? TerminalWorkspaceModel::Attention::Failure : TerminalWorkspaceModel::Attention::Success);
    });
    connect(sessionPtr, &TerminalBackend::bellRang, this, [raise]() {
        raise(TerminalWorkspaceModel::Attention::Bell);
    });
    connect(sessionPtr, &TerminalBackend::outputActivity, this, [this, sessionPtr]() {
        if (TerminalWorkspaceModel *workspace = workspaceOf(sessionPtr)) {
            workspace->markActivity();
        }
    });
    const auto refreshStatus = [this, sessionPtr]() {
        if (TerminalWorkspaceModel *workspace = workspaceOf(sessionPtr)) {
            workspace->refreshStatus();
        }
    };
    connect(sessionPtr, &TerminalBackend::processKindChanged, this, refreshStatus);
    connect(sessionPtr, &TerminalBackend::foregroundProcessChanged, this, refreshStatus);
    connect(sessionPtr, &TerminalBackend::commandRunningChanged, this, refreshStatus);
    connect(sessionPtr, &TerminalBackend::progressChanged, this, refreshStatus);
    // An agent waiting for the user, done with its turn or failed flags its
    // background tab like a bell or a finished command.
    connect(sessionPtr, &TerminalBackend::agentStatusChanged, this, [refreshStatus, raise, sessionPtr]() {
        refreshStatus();
        switch (sessionPtr->agentStateValue()) {
        case AgentState::Waiting:
            raise(TerminalWorkspaceModel::Attention::Bell);
            break;
        case AgentState::Idle:
            raise(TerminalWorkspaceModel::Attention::Success);
            break;
        case AgentState::Error:
            raise(TerminalWorkspaceModel::Attention::Failure);
            break;
        case AgentState::Working:
        case AgentState::None:
            break;
        }
    });
    const auto notifyWorkspaceTitle = [this, sessionPtr]() {
        std::ranges::for_each(m_workspaces, [sessionPtr](const auto &workspace) {
            workspace->notifySessionTitleChanged(sessionPtr);
        });
    };
    connect(sessionPtr, &TerminalBackend::displayTitleChanged, this, notifyWorkspaceTitle);
}

bool TerminalManager::moveWorkspaceTo(QObject *workspaceObject, QObject *targetObject, int index)
{
    auto *target = qobject_cast<TerminalManager *>(targetObject);
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    if (target == nullptr || workspace == nullptr) {
        return false;
    }
    if (target == this) {
        const auto it =
            std::ranges::find_if(m_workspaces, [workspace](const auto &item) { return item.get() == workspace; });
        if (it == m_workspaces.end()) {
            return false;
        }
        const int count = static_cast<int>(m_workspaces.size());
        moveWorkspace(static_cast<int>(std::distance(m_workspaces.begin(), it)), std::clamp(index, 0, count - 1));
        return true;
    }
    auto detached = detachWorkspace(workspace);
    if (!detached) {
        return false;
    }
    target->adoptWorkspace(std::move(*detached), index);
    if (m_workspaces.empty()) {
        emit windowShouldClose();
    }
    return true;
}

std::optional<TerminalManager::DetachedWorkspace> TerminalManager::detachWorkspace(TerminalWorkspaceModel *workspace)
{
    const auto it =
        std::ranges::find_if(m_workspaces, [workspace](const auto &candidate) { return candidate.get() == workspace; });
    if (it == m_workspaces.end()) {
        return std::nullopt;
    }

    DetachedWorkspace detached;
    detached.workspace = std::move(*it);
    m_workspaces.erase(it);

    // The sessions in the workspace's tree move with it, still running.
    auto *root = detached.workspace->root().get();
    const auto moved = std::ranges::stable_partition(m_sessions, [this, root](const auto &session) {
        return findLeaf(root, session.get()) == nullptr;
    });
    std::ranges::move(moved, std::back_inserter(detached.sessions));
    m_sessions.erase(moved.begin(), moved.end());

    bool wasActive = false;
    std::ranges::for_each(detached.sessions, [this, &wasActive](const auto &session) {
        disconnect(session.get(), nullptr, this, nullptr);
        std::erase(m_activationHistory, session.get());
        wasActive = wasActive || session.get() == m_activeSession;
    });
    m_activationHistoryCursor = 0;

    if (wasActive) {
        setActiveSession(nextActiveSession(nullptr), false);
    }
    emit workspacesChanged();
    emit activeWorkspaceChanged();
    return detached;
}

void TerminalManager::adoptWorkspace(DetachedWorkspace detached, int index)
{
    std::ranges::for_each(detached.sessions, [this](auto &session) {
        connectSession(session.get());
        m_sessions.push_back(std::move(session));
    });
    auto *workspace = detached.workspace.get();
    const auto position = std::clamp(
        static_cast<std::ptrdiff_t>(index),
        std::ptrdiff_t{0},
        static_cast<std::ptrdiff_t>(m_workspaces.size())
    );
    m_workspaces.insert(m_workspaces.begin() + position, std::move(detached.workspace));
    emit workspacesChanged();
    emit activeWorkspaceChanged();
    workspace->refreshStatus();
    setActiveSession(firstValidSessionIn(workspace), true);
}

bool TerminalManager::movePane(QObject *sessionObject, QObject *targetObject, QObject *targetSessionObject, int edge)
{
    auto *target = qobject_cast<TerminalManager *>(targetObject);
    TerminalBackend *session = ownedSession(sessionObject);
    TerminalBackend *targetSession = target == nullptr ? nullptr : target->ownedSession(targetSessionObject);
    TerminalWorkspaceModel *workspace = targetSession == nullptr ? nullptr : target->workspaceOf(targetSession);
    // Checked up front: once extracted, the pane must have somewhere to go.
    if (session == nullptr || workspace == nullptr || session == targetSession || edge < PaneEdgeLeft ||
        edge > PaneEdgeBottom) {
        return false;
    }
    // Rearranged within one workspace, QML hears of the final tree only.
    auto leaf = extractPane(session, workspace != workspaceOf(session));
    if (!leaf) {
        return false;
    }
    handOverSession(session, target);

    // Extracting the pane can restructure its own tree, never the target leaf.
    auto *slot = target->findNodeSlot(workspace->root(), targetSession);
    std::unique_ptr<TerminalLayoutNode> first = std::move(*slot);
    std::unique_ptr<TerminalLayoutNode> second = std::move(leaf);
    if (edge == PaneEdgeLeft || edge == PaneEdgeTop) {
        std::swap(first, second);
    }
    const bool sideBySide = edge == PaneEdgeLeft || edge == PaneEdgeRight;
    *slot = std::make_unique<TerminalLayoutNode>(
        sideBySide ? Qt::Horizontal : Qt::Vertical,
        std::move(first),
        std::move(second)
    );
    workspace->root()->notifyTreeChanged();
    emit workspace->layoutChanged();
    finishPaneMove(target, session);
    return true;
}

bool TerminalManager::movePaneToWorkspace(QObject *sessionObject, QObject *targetObject, int index)
{
    auto *target = qobject_cast<TerminalManager *>(targetObject);
    TerminalBackend *session = ownedSession(sessionObject);
    TerminalWorkspaceModel *workspace = session == nullptr ? nullptr : workspaceOf(session);
    if (target == nullptr || workspace == nullptr) {
        return false;
    }
    if (workspace->root()->isLeaf()) {
        // The pane already is a workspace of its own: move that, keeping its
        // name. In place, `index` counts the workspace itself.
        if (target == this) {
            const auto it =
                std::ranges::find_if(m_workspaces, [workspace](const auto &item) { return item.get() == workspace; });
            const int from = static_cast<int>(std::distance(m_workspaces.begin(), it));
            index = index > from ? index - 1 : index;
        }
        return moveWorkspaceTo(workspace, target, index);
    }
    auto leaf = extractPane(session);
    if (!leaf) {
        return false;
    }
    handOverSession(session, target);
    target->insertPaneWorkspace(std::move(leaf), index);
    finishPaneMove(target, session);
    return true;
}

TerminalBackend *TerminalManager::ownedSession(const QObject *session) const
{
    // By address, without qobject_cast: QML may hand over a stale pointer.
    const auto it =
        std::ranges::find_if(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    return it == m_sessions.end() ? nullptr : it->get();
}

TerminalWorkspaceModel *TerminalManager::workspaceOf(TerminalBackend *session) const
{
    const auto it = std::ranges::find_if(m_workspaces, [this, session](const auto &workspace) {
        return findLeaf(workspace->root().get(), session) != nullptr;
    });
    return it == m_workspaces.end() ? nullptr : it->get();
}

std::unique_ptr<TerminalLayoutNode> TerminalManager::extractPane(TerminalBackend *session, bool notify)
{
    TerminalWorkspaceModel *workspace = workspaceOf(session);
    if (workspace == nullptr) {
        return nullptr;
    }
    auto &root = workspace->root();
    if (root->m_session == session) {
        // The workspace's only pane: the workspace goes with it.
        std::unique_ptr<TerminalLayoutNode> leaf = std::move(root);
        eraseDeferred(m_workspaces, this, [workspace](const auto &candidate) { return candidate.get() == workspace; });
        emit workspacesChanged();
        return leaf;
    }
    auto *parentSlot = findParentSlot(root, session);
    if (parentSlot == nullptr) {
        return nullptr;
    }
    // The split holding the pane collapses into the pane's sibling.
    std::unique_ptr<TerminalLayoutNode> parent = std::move(*parentSlot);
    const bool isFirst = parent->m_first && parent->m_first->m_session == session;
    std::unique_ptr<TerminalLayoutNode> leaf = std::move(isFirst ? parent->m_first : parent->m_second);
    *parentSlot = std::move(isFirst ? parent->m_second : parent->m_first);
    // QML may still bind to the split until the layout signals below are
    // handled, so it goes on the next event-loop turn.
    parent.release()->deleteLater();

    if (workspace->lastActiveSession() == session) {
        workspace->setLastActiveSession(firstSession(root.get()));
    }
    if (notify) {
        root->notifyTreeChanged();
        emit workspace->layoutChanged();
    }
    return leaf;
}

void TerminalManager::handOverSession(TerminalBackend *session, TerminalManager *target)
{
    if (target == this) {
        return;
    }
    const auto it =
        std::ranges::find_if(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    if (it == m_sessions.end()) {
        return;
    }
    std::unique_ptr<TerminalBackend> owned = std::move(*it);
    m_sessions.erase(it);
    disconnect(session, nullptr, this, nullptr);
    std::erase(m_activationHistory, session);
    m_activationHistoryCursor = 0;
    if (m_activeSession == session) {
        setActiveSession(nextActiveSession(nullptr), false);
    }
    target->connectSession(session);
    target->m_sessions.push_back(std::move(owned));
}

void TerminalManager::insertPaneWorkspace(std::unique_ptr<TerminalLayoutNode> leaf, int index)
{
    auto workspace = std::make_unique<TerminalWorkspaceModel>(
        QStringLiteral("Workspace %1").arg(m_nextWorkspaceNumber++),
        std::move(leaf)
    );
    const auto position = std::clamp(
        static_cast<std::ptrdiff_t>(index),
        std::ptrdiff_t{0},
        static_cast<std::ptrdiff_t>(m_workspaces.size())
    );
    m_workspaces.insert(m_workspaces.begin() + position, std::move(workspace));
    emit workspacesChanged();
}

void TerminalManager::finishPaneMove(TerminalManager *target, TerminalBackend *session)
{
    emit activeWorkspaceChanged();
    if (target != this) {
        emit target->activeWorkspaceChanged();
    }
    target->setActiveSession(session, true);
    if (target != this && m_workspaces.empty()) {
        emit windowShouldClose();
    }
}

void TerminalManager::splitActive(int orientation)
{
    if ((m_activeSession == nullptr) ||
        (orientation != static_cast<int>(Qt::Horizontal) && orientation != static_cast<int>(Qt::Vertical))) {
        return;
    }
    const int workspaceIndex = activeWorkspaceIndex();
    if (workspaceIndex < 0) {
        return;
    }

    auto &root = m_workspaces[static_cast<std::size_t>(workspaceIndex)]->root();
    auto *leafSlot = findNodeSlot(root, m_activeSession);
    if (leafSlot == nullptr) {
        return;
    }

    const QString workingDirectory = m_activeSession->currentDirectory();
    auto newSession = createSession();
    auto oldLeaf = std::move(*leafSlot);
    auto newLeaf = std::make_unique<TerminalLayoutNode>(newSession);
    *leafSlot = std::make_unique<TerminalLayoutNode>(
        static_cast<Qt::Orientation>(orientation),
        std::move(oldLeaf),
        std::move(newLeaf)
    );
    auto &workspace = m_workspaces[static_cast<std::size_t>(workspaceIndex)];
    workspace->root()->notifyTreeChanged();
    emit workspace->layoutChanged();
    emit workspacesChanged();
    emit activeWorkspaceChanged();
    setActiveSession(newSession, true);
    newSession->start(workingDirectory);
}

void TerminalManager::closeWorkspace(QObject *workspaceObject)
{
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    const auto it =
        std::ranges::find_if(m_workspaces, [workspace](const auto &candidate) { return candidate.get() == workspace; });
    if ((workspace == nullptr) || it == m_workspaces.end()) {
        return;
    }
    std::vector<TerminalBackend *> removedSessions;
    std::ranges::for_each(m_sessions, [this, &removedSessions, workspace](const auto &candidate) {
        if (findLeaf(workspace->root().get(), candidate.get())) {
            removedSessions.push_back(candidate.get());
        }
    });
    const bool wasActive = std::ranges::find(removedSessions, m_activeSession) != removedSessions.end();
    bool allStopped = true;
    std::ranges::for_each(removedSessions, [&allStopped](TerminalBackend *session) {
        if (!session->close()) {
            allStopped = false;
        }
    });
    if (!allStopped) {
        return;
    }
    for (TerminalBackend *session : removedSessions) {
        pruneSession(session);
        removeSessionFromLayouts(session);
        eraseDeferred(m_sessions, this, [session](const auto &candidate) { return candidate.get() == session; });
    }
    eraseDeferred(m_workspaces, this, [workspace](const auto &candidate) { return candidate.get() == workspace; });
    if (wasActive && m_workspaces.empty()) {
        // Closing the last workspace empties the window, which closes below.
        setActiveSession(nullptr, false);
    } else if (wasActive) {
        TerminalBackend *nextSession = mostRecentlyActiveSession();
        if (nextSession == nullptr) {
            const auto nextWorkspace = std::ranges::find_if(m_workspaces, [this](const auto &candidate) {
                return findLeaf(candidate->root().get(), candidate->lastActiveSession()) != nullptr;
            });
            if (nextWorkspace != m_workspaces.end()) {
                nextSession = (*nextWorkspace)->lastActiveSession();
            } else {
                nextSession = firstSession(m_workspaces.front()->root().get());
            }
        }
        setActiveSession(nextSession, true);
    }
    emit workspacesChanged();
    emit activeWorkspaceChanged();
    if (m_sessions.empty()) {
        emit windowShouldClose();
    }
}

bool TerminalManager::workspaceHasRunningCommand(QObject *workspaceObject) const
{
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    if (workspace == nullptr) {
        return false;
    }
    return std::ranges::any_of(m_sessions, [this, workspace](const auto &session) {
        return session->commandRunning() && findLeaf(workspace->root().get(), session.get()) != nullptr;
    });
}

bool TerminalManager::workspaceNeedsCloseConfirmation(QObject *workspaceObject) const
{
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    if (workspace == nullptr) {
        return false;
    }
    const auto sessionsInWorkspace = std::ranges::count_if(m_sessions, [this, workspace](const auto &session) {
        return findLeaf(workspace->root().get(), session.get()) != nullptr;
    });
    return sessionsInWorkspace > 1 || workspaceHasRunningCommand(workspace);
}

bool TerminalManager::anySessionHasRunningCommand() const
{
    return std::ranges::any_of(m_sessions, [](const auto &session) { return session->commandRunning(); });
}

bool TerminalManager::anySessionHasActiveProcess() const
{
    return std::ranges::any_of(m_sessions, [](const auto &session) {
        return session->commandRunning() || session->hasForegroundProcess();
    });
}

int TerminalManager::sessionCount() const
{
    return static_cast<int>(m_sessions.size());
}

bool TerminalManager::hasSession(const QObject *session) const
{
    return std::ranges::any_of(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
}

void TerminalManager::closeSession(QObject *sessionObject)
{
    // Match by raw pointer without qobject_cast: `sessionObject` may be a raw
    // pointer captured by an earlier queued callback (processExited →
    // closeSession), so it can dangle after the session is destroyed, and
    // qobject_cast would read freed memory.
    const auto it = std::ranges::find_if(m_sessions, [sessionObject](const auto &candidate) {
        return candidate.get() == sessionObject;
    });

    if (it == m_sessions.end()) {
        return;
    }

    auto *session = it->get();
    const bool wasActive = m_activeSession == session;
    if (!session->close()) {
        return;
    }

    const auto activeWorkspace = std::ranges::find_if(m_workspaces, [this, session](const auto &workspace) {
        return findLeaf(workspace->root().get(), session) != nullptr;
    });

    TerminalWorkspaceModel *closingWorkspace = activeWorkspace == m_workspaces.end() ? nullptr : activeWorkspace->get();
    pruneSession(session);
    removeSessionFromLayouts(session);

    TerminalBackend *nextInClosingWorkspace = firstValidSessionIn(closingWorkspace);
    std::ranges::for_each(m_workspaces, [this, session](const auto &workspace) {
        if (workspace->lastActiveSession() == session) {
            workspace->setLastActiveSession(firstSession(workspace->root().get()));
        }
    });

    eraseDeferred(m_sessions, this, [session](const auto &candidate) { return candidate.get() == session; });
    eraseDeferred(m_workspaces, this, [](const auto &workspace) { return workspace->root() == nullptr; });

    if (wasActive) {
        setActiveSession(nextActiveSession(nextInClosingWorkspace), false);
    }

    emit workspacesChanged();
    emit activeWorkspaceChanged();

    if (m_sessions.empty()) {
        emit windowShouldClose();
    }
}

QVariantList TerminalManager::workspaceSessions(QObject *workspaceObject) const
{
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    const bool owned =
        std::ranges::any_of(m_workspaces, [workspace](const auto &item) { return item.get() == workspace; });
    QVariantList list;
    if (!owned || workspace->root() == nullptr) {
        return list;
    }
    // Depth-first, first child before second: the panes' on-screen order.
    std::vector<const TerminalLayoutNode *> pending{workspace->root().get()};
    while (!pending.empty()) {
        const TerminalLayoutNode *node = pending.back();
        pending.pop_back();
        if (node->m_session != nullptr) {
            list.append(QVariant::fromValue(static_cast<QObject *>(node->m_session)));
        }
        if (node->m_second) {
            pending.push_back(node->m_second.get());
        }
        if (node->m_first) {
            pending.push_back(node->m_first.get());
        }
    }
    return list;
}

void TerminalManager::forgetSession(TerminalBackend *session)
{
    disconnect(session, nullptr, this, nullptr);
    std::erase(m_activationHistory, session);
    m_activationHistoryCursor = 0;
}

std::unique_ptr<TerminalBackend> TerminalManager::releaseSession(QObject *sessionObject)
{
    TerminalBackend *session = ownedSession(sessionObject);
    if (session == nullptr) {
        return nullptr;
    }
    std::unique_ptr<TerminalLayoutNode> leaf = extractPane(session);
    if (!leaf) {
        return nullptr;
    }
    // The pane's view may still bind to its node until the layout signals
    // are handled.
    leaf.release()->deleteLater();
    const auto it =
        std::ranges::find_if(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    std::unique_ptr<TerminalBackend> owned = std::move(*it);
    m_sessions.erase(it);
    forgetSession(session);
    if (m_activeSession == session) {
        setActiveSession(nextActiveSession(nullptr), false);
    }
    emit activeWorkspaceChanged();
    if (m_workspaces.empty()) {
        addWorkspace();
    }
    return owned;
}

std::vector<std::unique_ptr<TerminalBackend>>
TerminalManager::releaseWorkspace(QObject *workspaceObject, QString &title)
{
    auto *workspace = qobject_cast<TerminalWorkspaceModel *>(workspaceObject);
    const bool owned =
        std::ranges::any_of(m_workspaces, [workspace](const auto &item) { return item.get() == workspace; });
    if (!owned) {
        return {};
    }
    // detachWorkspace returns the sessions in creation order; keep the
    // panes' order instead.
    const QVariantList order = workspaceSessions(workspace);
    auto detached = detachWorkspace(workspace);
    if (!detached) {
        return {};
    }
    title = detached->workspace->customTitle();
    detached->workspace.release()->deleteLater();
    std::vector<std::unique_ptr<TerminalBackend>> sessions;
    sessions.reserve(detached->sessions.size());
    std::ranges::for_each(order, [&detached, &sessions](const QVariant &entry) {
        const QObject *session = entry.value<QObject *>();
        const auto it = std::ranges::find_if(detached->sessions, [session](const auto &candidate) {
            return candidate.get() == session;
        });
        if (it != detached->sessions.end() && *it) {
            sessions.push_back(std::move(*it));
        }
    });
    if (m_workspaces.empty()) {
        addWorkspace();
    }
    return sessions;
}

void TerminalManager::adoptSession(std::unique_ptr<TerminalBackend> session, const QString &title)
{
    if (!session) {
        return;
    }
    TerminalBackend *sessionPtr = session.get();
    connectSession(sessionPtr);
    m_sessions.push_back(std::move(session));
    insertPaneWorkspace(std::make_unique<TerminalLayoutNode>(sessionPtr), static_cast<int>(m_workspaces.size()));
    TerminalWorkspaceModel *workspace = m_workspaces.back().get();
    workspace->setCustomTitle(title);
    workspace->refreshStatus();
    emit activeWorkspaceChanged();
    setActiveSession(sessionPtr, true);
}

TerminalBackend *TerminalManager::firstValidSessionIn(TerminalWorkspaceModel *workspace) const
{
    if (workspace == nullptr || workspace->root() == nullptr) {
        return nullptr;
    }
    TerminalBackend *session = workspace->lastActiveSession();
    if (findLeaf(workspace->root().get(), session) == nullptr) {
        session = firstSession(workspace->root().get());
    }
    return session;
}

TerminalBackend *TerminalManager::nextActiveSession(TerminalBackend *fallback) const
{
    if (fallback != nullptr) {
        return fallback;
    }
    TerminalBackend *next = mostRecentlyActiveSession();
    if (next == nullptr) {
        for (const auto &workspace : m_workspaces) {
            next = firstValidSessionIn(workspace.get());
            if (next != nullptr) {
                break;
            }
        }
    }
    return next;
}

void TerminalManager::activateSession(QObject *sessionObject)
{
    auto *session = qobject_cast<TerminalBackend *>(sessionObject);
    if ((session != nullptr) &&
        std::ranges::any_of(m_sessions, [session](const auto &item) { return item.get() == session; })) {
        setActiveSession(session, true);
    }
}

void TerminalManager::setActiveWorkspace(int index)
{
    if (index < 0 || index >= static_cast<int>(m_workspaces.size())) {
        return;
    }

    auto *workspace = m_workspaces[static_cast<std::size_t>(index)].get();

    TerminalBackend *session = firstValidSessionIn(workspace);

    if (session != nullptr) {
        setActiveSession(session, true);
    }
}

void TerminalManager::navigateActivationHistory(int direction)
{
    const qsizetype historySize = static_cast<qsizetype>(m_activationHistory.size());
    if (historySize < 2) {
        return;
    }

    const qsizetype step = direction < 0 ? -1 : 1;
    m_activationHistoryCursor = (m_activationHistoryCursor + step + historySize) % historySize;
    setActiveSession(m_activationHistory.at(static_cast<std::size_t>(m_activationHistoryCursor)), false);
}

void TerminalManager::routeKeyPress(QObject *sessionObject, int key, const QString &text, int modifiers)
{
    auto *session = qobject_cast<TerminalBackend *>(sessionObject);
    if ((session != nullptr) && session == m_activeSession) {
        session->keyPress(key, text, modifiers);
    }
}

void TerminalManager::routeKeyEvent(QObject *sessionObject, int key, const QString &text, int modifiers, int eventType)
{
    auto *session = qobject_cast<TerminalBackend *>(sessionObject);
    if ((session != nullptr) && session == m_activeSession) {
        session->keyEvent(key, text, modifiers, eventType);
    }
}

void TerminalManager::routeTextInput(QObject *sessionObject, const QString &text)
{
    auto *session = qobject_cast<TerminalBackend *>(sessionObject);
    if ((session != nullptr) && session == m_activeSession) {
        session->writeText(text);
    }
}

TerminalLayoutNode *TerminalManager::findLeaf(TerminalLayoutNode *node, TerminalBackend *session) const
{
    if ((node == nullptr) || (session == nullptr)) {
        return nullptr;
    }

    std::vector<TerminalLayoutNode *> pending;
    pending.push_back(node);
    while (!pending.empty()) {
        auto *current = pending.back();
        pending.pop_back();
        if (current->isLeaf()) {
            if (current->m_session == session) {
                return current;
            }
            continue;
        }

        if (current->m_second) {
            pending.push_back(current->m_second.get());
        }

        if (current->m_first) {
            pending.push_back(current->m_first.get());
        }
    }
    return nullptr;
}

TerminalBackend *TerminalManager::firstSession(TerminalLayoutNode *node) const
{
    if (node == nullptr) {
        return nullptr;
    }

    std::vector<TerminalLayoutNode *> pending;
    pending.push_back(node);

    while (!pending.empty()) {
        auto *current = pending.back();
        pending.pop_back();
        if (current->isLeaf()) {
            return current->m_session;
        }

        if (current->m_second) {
            pending.push_back(current->m_second.get());
        }

        if (current->m_first) {
            pending.push_back(current->m_first.get());
        }
    }

    return nullptr;
}

TerminalBackend *TerminalManager::mostRecentlyActiveSession() const
{
    const auto it = std::ranges::find_if(m_activationHistory, [this](const TerminalBackend *session) {
        return std::ranges::any_of(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    });
    return it == m_activationHistory.end() ? nullptr : *it;
}

std::unique_ptr<TerminalLayoutNode> *
TerminalManager::findNodeSlot(std::unique_ptr<TerminalLayoutNode> &node, TerminalBackend *session) const
{
    if (!node) {
        return nullptr;
    }

    std::vector<std::unique_ptr<TerminalLayoutNode> *> pending;
    pending.push_back(&node);

    while (!pending.empty()) {
        auto *slot = pending.back();
        pending.pop_back();
        if (!*slot) {
            continue;
        }

        if ((*slot)->m_session == session) {
            return slot;
        }

        if ((*slot)->m_second) {
            pending.push_back(&(*slot)->m_second);
        }

        if ((*slot)->m_first) {
            pending.push_back(&(*slot)->m_first);
        }
    }

    return nullptr;
}

std::unique_ptr<TerminalLayoutNode> *
TerminalManager::findParentSlot(std::unique_ptr<TerminalLayoutNode> &node, TerminalBackend *session) const
{
    std::vector<std::unique_ptr<TerminalLayoutNode> *> pending;
    if (node) {
        pending.push_back(&node);
    }
    while (!pending.empty()) {
        auto *slot = pending.back();
        pending.pop_back();
        auto *current = slot->get();
        if (current == nullptr || current->isLeaf()) {
            continue;
        }
        const auto holds = [session](const std::unique_ptr<TerminalLayoutNode> &child) {
            return child && child->m_session == session;
        };
        if (holds(current->m_first) || holds(current->m_second)) {
            return slot;
        }
        pending.push_back(&current->m_second);
        pending.push_back(&current->m_first);
    }
    return nullptr;
}

void TerminalManager::setActiveSession(TerminalBackend *session, bool record)
{
    std::ranges::for_each(m_workspaces, [this, session](const auto &workspace) {
        const bool holdsSession = session != nullptr && findLeaf(workspace->root().get(), session) != nullptr;
        if (holdsSession) {
            workspace->setLastActiveSession(session);
        }
        workspace->setActive(holdsSession);
    });

    if (m_activeSession == session) {
        if (record && (session != nullptr)) {
            recordActivation(session);
        }
        return;
    }

    m_activeSession = session;
    if (record && (session != nullptr)) {
        recordActivation(session);
    }

    emit activeSessionChanged();
    emit activeWorkspaceChanged();
}

void TerminalManager::recordActivation(TerminalBackend *session)
{
    std::erase(m_activationHistory, session);
    m_activationHistory.insert(m_activationHistory.begin(), session);
    m_activationHistoryCursor = 0;
}

void TerminalManager::pruneSession(TerminalBackend *session)
{
    std::erase(m_activationHistory, session);
    m_activationHistoryCursor = 0;

    if (m_activeSession == session) {
        m_activeSession = nullptr;
        emit activeSessionChanged();
        emit activeWorkspaceChanged();
    }
}

void TerminalManager::removeSessionFromLayouts(TerminalBackend *session)
{
    struct Frame {
        std::unique_ptr<TerminalLayoutNode> *slot;
        std::size_t entered; // 0 / 1, pointer-sized: no padding
    };
    const auto enter = [session](Frame &frame, std::vector<Frame> &stack) {
        frame.entered = 1;
        auto *node = (*frame.slot).get();
        if (node == nullptr) {
            return;
        }

        if (node->m_session != nullptr) {
            if (node->m_session == session) {
                (*frame.slot).reset();
            }
            return;
        }

        if (node->m_second) {
            stack.push_back({.slot = &node->m_second, .entered = 0});
        }

        if (node->m_first) {
            stack.push_back({.slot = &node->m_first, .entered = 0});
        }
    };

    const auto exit = [](Frame &frame) {
        auto *node = (*frame.slot).get();
        if (node == nullptr || node->m_session != nullptr) {
            return;
        }

        if (node->m_first == nullptr) {
            *frame.slot = std::move(node->m_second);
        } else if (node->m_second == nullptr) {
            *frame.slot = std::move(node->m_first);
        }
    };

    for (const auto &workspace : m_workspaces) {
        std::vector<Frame> stack;
        stack.push_back({.slot = &workspace->root(), .entered = 0});

        while (!stack.empty()) {
            auto &frame = stack.back();
            if (frame.entered != 0) {
                exit(frame);
                stack.pop_back();
                continue;
            }
            enter(frame, stack);
        }

        if (workspace->root()) {
            workspace->root()->notifyTreeChanged();
        }

        emit workspace->layoutChanged();
    }
}

void TerminalManager::commitActivationHistory()
{
    if (m_activationHistoryCursor > 0 &&
        m_activationHistoryCursor < static_cast<qsizetype>(m_activationHistory.size())) {
        recordActivation(m_activationHistory.at(static_cast<std::size_t>(m_activationHistoryCursor)));
    }
}

QJsonArray TerminalManager::serializeState() const
{
    QJsonArray workspaces;
    for (const auto &workspace : m_workspaces) {
        QJsonObject workspaceJson;
        workspaceJson[QStringLiteral("customTitle")] = workspace->customTitle();
        workspaceJson[QStringLiteral("layout")] = serializeLayout(workspace->root().get());
        workspaces.append(workspaceJson);
    }
    return workspaces;
}

QJsonObject TerminalManager::serializeLayout(TerminalLayoutNode *root) const
{
    if (root == nullptr) {
        return QJsonObject();
    }

    std::vector<QJsonObject> pool;
    struct Link {
        int self;
        int first;
        int second;
    };
    std::vector<Link> links;
    struct Work {
        TerminalLayoutNode *node;
        std::ptrdiff_t index; // pointer-sized: no padding
    };
    std::vector<Work> pending;

    pool.emplace_back();
    pending.push_back({root, 0});

    while (!pending.empty()) {
        const Work work = pending.back();
        pending.pop_back();
        const auto index = static_cast<int>(work.index);
        if (work.node->m_session != nullptr) {
            QJsonObject leaf;
            leaf[QStringLiteral("d")] = work.node->m_session->currentDirectory();
            pool[index] = std::move(leaf);
        } else {
            const int firstIndex = static_cast<int>(pool.size());
            pool.emplace_back();
            const int secondIndex = static_cast<int>(pool.size());
            pool.emplace_back();
            QJsonObject split;
            split[QStringLiteral("o")] = work.node->orientation();
            pool[index] = std::move(split);
            links.push_back({index, firstIndex, secondIndex});
            pending.push_back({work.node->m_second.get(), secondIndex});
            pending.push_back({work.node->m_first.get(), firstIndex});
        }
    }

    for (const Link &link : links) {
        pool[static_cast<std::size_t>(link.self)][QStringLiteral("a")] = pool[static_cast<std::size_t>(link.first)];
        pool[static_cast<std::size_t>(link.self)][QStringLiteral("b")] = pool[static_cast<std::size_t>(link.second)];
    }

    return pool.front();
}

void TerminalManager::restoreState(const QJsonArray &state)
{
    m_workspaces.clear();
    m_sessions.clear();
    m_activationHistory.clear();
    m_activeSession = nullptr;
    m_nextWorkspaceNumber = 1;

    for (const QJsonValue workspaceValue : state) {
        const QJsonObject workspaceJson = workspaceValue.toObject();
        auto root = buildLayoutNode(workspaceJson[QStringLiteral("layout")].toObject());
        auto workspace = std::make_unique<TerminalWorkspaceModel>(
            QStringLiteral("Workspace %1").arg(m_nextWorkspaceNumber++),
            std::move(root)
        );
        workspace->setCustomTitle(workspaceJson[QStringLiteral("customTitle")].toString());
        m_workspaces.push_back(std::move(workspace));
    }

    if (!m_workspaces.empty()) {
        setActiveSession(firstSession(m_workspaces.front()->root().get()), false);
    }

    emit workspacesChanged();
    emit activeWorkspaceChanged();
}

namespace
{

bool isSplitNode(const QJsonObject &json)
{
    return json.contains(QStringLiteral("a")) && json.contains(QStringLiteral("b"));
}

} // namespace

std::unique_ptr<TerminalLayoutNode> TerminalManager::buildLayoutNode(const QJsonObject &rootJson)
{
    struct Frame {
        QJsonObject json;
        std::unique_ptr<TerminalLayoutNode> *slot;
        std::size_t entered; // 0 / 1, pointer-sized: no padding
        std::unique_ptr<TerminalLayoutNode> first;
        std::unique_ptr<TerminalLayoutNode> second;
    };

    std::unique_ptr<TerminalLayoutNode> result;
    std::deque<Frame> stack;
    stack.push_back({rootJson, &result, 0, {}, {}});

    while (!stack.empty()) {
        Frame &frame = stack.back();
        if (frame.entered == 0) {
            frame.entered = 1;
            const QJsonObject &json = frame.json;
            if (isSplitNode(json)) {
                const QJsonObject firstJson = json[QStringLiteral("a")].toObject();
                const QJsonObject secondJson = json[QStringLiteral("b")].toObject();
                stack.push_back({secondJson, &frame.second, 0, {}, {}});
                stack.push_back({firstJson, &frame.first, 0, {}, {}});
            } else {
                TerminalBackend *session = createSession();
                session->start(json[QStringLiteral("d")].toString());
                *frame.slot = std::make_unique<TerminalLayoutNode>(session);
            }
            continue;
        }
        if (isSplitNode(frame.json)) {
            if (frame.first && frame.second) {
                *frame.slot = std::make_unique<TerminalLayoutNode>(
                    static_cast<Qt::Orientation>(frame.json[QStringLiteral("o")].toInt()),
                    std::move(frame.first),
                    std::move(frame.second)
                );
                stack.pop_back();
            }
        } else {
            stack.pop_back();
        }
    }

    return result;
}

} // namespace konsoll
