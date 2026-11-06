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
    m_window = window;
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

    std::erase_if(m_sessions, [session](const auto &candidate) { return candidate.get() == session; });
    std::erase_if(m_workspaces, [](const auto &workspace) { return workspace->root() == nullptr; });

    if (wasActive) {
        setActiveSession(nextActiveSession(nextInClosingWorkspace), false);
    }

    emit workspacesChanged();
    emit activeWorkspaceChanged();

    if (m_sessions.empty()) {
        emit windowShouldClose();
    }
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
    workspace->setAlerting(false);

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
    struct Frame {
        std::unique_ptr<TerminalLayoutNode> *slot;
        bool entered;
    };
    const auto enter = [session](Frame &frame, std::vector<Frame> &stack) {
        frame.entered = true;
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
            stack.push_back({.slot = &node->m_second, .entered = false});
        }

        if (node->m_first) {
            stack.push_back({.slot = &node->m_first, .entered = false});
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
        stack.push_back({.slot = &workspace->root(), .entered = false});

        while (!stack.empty()) {
            auto &frame = stack.back();
            if (frame.entered) {
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
        int index;
    };
    std::vector<Work> pending;

    pool.emplace_back();
    pending.push_back({root, 0});

    while (!pending.empty()) {
        const Work work = pending.back();
        pending.pop_back();
        const int index = work.index;
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
        bool entered;
        std::unique_ptr<TerminalLayoutNode> first;
        std::unique_ptr<TerminalLayoutNode> second;
    };

    std::unique_ptr<TerminalLayoutNode> result;
    std::deque<Frame> stack;
    stack.push_back({rootJson, &result, false, {}, {}});

    while (!stack.empty()) {
        Frame &frame = stack.back();
        if (!frame.entered) {
            frame.entered = true;
            const QJsonObject &json = frame.json;
            if (isSplitNode(json)) {
                const QJsonObject firstJson = json[QStringLiteral("a")].toObject();
                const QJsonObject secondJson = json[QStringLiteral("b")].toObject();
                stack.push_back({secondJson, &frame.second, false, {}, {}});
                stack.push_back({firstJson, &frame.first, false, {}, {}});
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
