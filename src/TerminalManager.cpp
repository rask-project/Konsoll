#include "TerminalManager.h"

#include <QCoreApplication>
#include <QKeyEvent>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <iterator>
#include <ranges>
#include <utility>

namespace konsoll
{

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
    emit layoutChanged();
    if (m_first) {
        m_first->notifyTreeChanged();
    }
    if (m_second) {
        m_second->notifyTreeChanged();
    }
}

TerminalWorkspaceModel::TerminalWorkspaceModel(QString title, std::unique_ptr<TerminalLayoutNode> root)
    : m_root(std::move(root))
    , m_title(std::move(title))
{}

QString TerminalWorkspaceModel::title() const
{
    if (!m_customTitle.isEmpty()) {
        return m_customTitle;
    }
    if (m_lastActiveSession == nullptr) {
        return m_title;
    }
    const QString directory = m_lastActiveSession->currentDirectory();
    return directory.isEmpty() ? m_lastActiveSession->title() : directory;
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

void TerminalWorkspaceModel::setAlerting(bool value)
{
    if (m_alerting == value) {
        return;
    }
    m_alerting = value;
    emit alertingChanged();
}

void TerminalWorkspaceModel::setLastActiveSession(TerminalBackend *session)
{
    if (m_lastActiveSession == session) {
        return;
    }
    m_lastActiveSession = session;
    emit titleChanged();
}

void TerminalWorkspaceModel::notifySessionTitleChanged(TerminalBackend *session)
{
    if (m_lastActiveSession == session) {
        emit titleChanged();
    }
}

TerminalManager *TerminalManager::s_instance = nullptr;

TerminalManager *TerminalManager::instance()
{
    return s_instance;
}

TerminalManager::TerminalManager(QObject *parent) : QObject(parent)
{
    s_instance = this;
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
    if (s_instance == this) {
        s_instance = nullptr;
    }
}

bool TerminalManager::eventFilter(QObject *watched, QEvent *event)
{
    if (event->type() == QEvent::KeyRelease) {
        const auto *keyEvent = static_cast<QKeyEvent *>(event);
        if (keyEvent->key() == Qt::Key_Control || keyEvent->key() == Qt::Key_Meta) {
            commitActivationHistory();
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
    const QString workingDirectory = (m_activeSession != nullptr) ? m_activeSession->currentDirectory() : QString{};
    TerminalBackend *session = createSession();
    auto workspace = std::make_unique<TerminalWorkspaceModel>(
        QStringLiteral("Workspace %1").arg(m_nextWorkspaceNumber++),
        std::make_unique<TerminalLayoutNode>(session)
    );
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
    const auto it = std::ranges::find_if(m_workspaces, [workspace](const auto &candidate) {
        return candidate.get() == workspace;
    });
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
    connect(sessionPtr, &TerminalBackend::processExited, this, [this, sessionPtr](int) {
        QTimer::singleShot(0, this, [this, sessionPtr]() { closeSession(sessionPtr); });
    });
    const auto scheduleClose = [this, sessionPtr]() {
        QTimer::singleShot(0, this, [this, sessionPtr]() { closeSession(sessionPtr); });
    };
    connect(sessionPtr, &TerminalBackend::startFailed, this, [scheduleClose](const QString &) { scheduleClose(); });
    connect(sessionPtr, &TerminalBackend::workerThreadStopped, this, scheduleClose);
    connect(sessionPtr, &TerminalBackend::commandFinished, this, [this, sessionPtr]() {
        const auto workspace = std::ranges::find_if(m_workspaces, [this, sessionPtr](const auto &item) {
            return findLeaf(item->root().get(), sessionPtr) != nullptr;
        });
        if (workspace != m_workspaces.end()) {
            const int workspaceIndex = static_cast<int>(std::distance(m_workspaces.begin(), workspace));
            (*workspace)->setAlerting(activeWorkspaceIndex() != workspaceIndex);
        }
    });
    const auto notifyWorkspaceTitle = [this, sessionPtr]() {
        std::ranges::for_each(m_workspaces, [sessionPtr](const auto &workspace) {
            workspace->notifySessionTitleChanged(sessionPtr);
        });
    };
    connect(sessionPtr, &TerminalBackend::titleChanged, this, notifyWorkspaceTitle);
    connect(sessionPtr, &TerminalBackend::currentDirectoryChanged, this, notifyWorkspaceTitle);
    m_sessions.push_back(std::move(session));
    return sessionPtr;
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
    if (m_workspaces.size() == 1 && m_sessions.size() > 1) {
        auto *root = workspace->root().get();
        auto *session = (findLeaf(root, m_activeSession) != nullptr) ? m_activeSession : firstSession(root);
        closeSession(session);
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
        std::erase_if(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    }
    m_workspaces.erase(it);
    if (wasActive && !m_workspaces.empty()) {
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
        emit quitRequested();
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

bool TerminalManager::anySessionHasRunningCommand() const
{
    return std::ranges::any_of(m_sessions, [](const auto &session) {
        return session->commandRunning();
    });
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
    TerminalWorkspaceModel *closingWorkspace =
        activeWorkspace == m_workspaces.end() ? nullptr : activeWorkspace->get();
    pruneSession(session);
    removeSessionFromLayouts(session);
    TerminalBackend *nextInClosingWorkspace = nullptr;
    if (closingWorkspace != nullptr && closingWorkspace->root() != nullptr) {
        nextInClosingWorkspace = closingWorkspace->lastActiveSession();
        if (findLeaf(closingWorkspace->root().get(), nextInClosingWorkspace) == nullptr) {
            nextInClosingWorkspace = firstSession(closingWorkspace->root().get());
        }
    }
    std::ranges::for_each(m_workspaces, [this, session](const auto &workspace) {
        if (workspace->lastActiveSession() == session) {
            workspace->setLastActiveSession(firstSession(workspace->root().get()));
        }
    });
    std::erase_if(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    std::erase_if(m_workspaces, [](const auto &workspace) { return workspace->root() == nullptr; });

    if (wasActive) {
        TerminalBackend *next = nextInClosingWorkspace;
        if (next == nullptr) {
            next = mostRecentlyActiveSession();
        }
        if (next == nullptr) {
            for (const auto &workspace : m_workspaces) {
                next = workspace->lastActiveSession();
                if (findLeaf(workspace->root().get(), next) == nullptr) {
                    next = firstSession(workspace->root().get());
                }
                if (next != nullptr) {
                    break;
                }
            }
        }
        setActiveSession(next, false);
    }
    emit workspacesChanged();
    emit activeWorkspaceChanged();
    if (m_sessions.empty()) {
        emit quitRequested();
    }
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
    workspace->setAlerting(false);
    TerminalBackend *session = workspace->lastActiveSession();
    if (findLeaf(workspace->root().get(), session) == nullptr) {
        session = firstSession(workspace->root().get());
    }
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
    if (node->isLeaf()) {
        return node->session() == session ? node : nullptr;
    }
    if (auto *found = findLeaf(static_cast<TerminalLayoutNode *>(node->first()), session)) {
        return found;
    }
    return findLeaf(static_cast<TerminalLayoutNode *>(node->second()), session);
}

TerminalBackend *TerminalManager::firstSession(TerminalLayoutNode *node) const
{
    if (node == nullptr) {
        return nullptr;
    }
    if (node->isLeaf()) {
        return static_cast<TerminalBackend *>(node->session());
    }
    if (auto *first = firstSession(static_cast<TerminalLayoutNode *>(node->first()))) {
        return first;
    }
    return firstSession(static_cast<TerminalLayoutNode *>(node->second()));
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
    if (node->m_session == session) {
        return &node;
    }
    if (auto *found = findNodeSlot(node->m_first, session)) {
        return found;
    }
    return findNodeSlot(node->m_second, session);
}

void TerminalManager::setActiveSession(TerminalBackend *session, bool record)
{
    std::ranges::for_each(m_workspaces, [this, session](const auto &workspace) {
        if (findLeaf(workspace->root().get(), session)) {
            workspace->setAlerting(false);
            workspace->setLastActiveSession(session);
        }
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
    const auto removeNode = [session](auto &&self, std::unique_ptr<TerminalLayoutNode> &node) -> void {
        if (!node) {
            return;
        }
        if (node->m_session) {
            if (node->m_session == session) {
                node.reset();
            }
            return;
        }
        self(self, node->m_first);
        self(self, node->m_second);
        if (!node->m_first) {
            node = std::move(node->m_second);
        } else if (!node->m_second) {
            node = std::move(node->m_first);
        }
    };
    for (const auto &workspace : m_workspaces) {
        removeNode(removeNode, workspace->root());
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

} // namespace konsoll
