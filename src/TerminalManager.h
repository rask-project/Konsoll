#pragma once

#include "TerminalBackend.h"

#include <QEvent>
#include <QObject>
#include <QVariantList>
#include <QWindow>
#include <QtQml>

#include <memory>
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
    TerminalLayoutNode(Qt::Orientation orientation,
                       std::unique_ptr<TerminalLayoutNode> first,
                       std::unique_ptr<TerminalLayoutNode> second);

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
    Q_PROPERTY(bool alerting READ alerting NOTIFY alertingChanged)
    Q_PROPERTY(QObject *rootNode READ rootNode NOTIFY layoutChanged)

public:
    TerminalWorkspaceModel(QString title, std::unique_ptr<TerminalLayoutNode> root);

    QString title() const;
    QString customTitle() const { return m_customTitle; }
    void setCustomTitle(QString title);
    bool alerting() const { return m_alerting; }
    void setAlerting(bool value);
    QObject *rootNode() const { return m_root.get(); }
    std::unique_ptr<TerminalLayoutNode> &root() { return m_root; }
    TerminalBackend *lastActiveSession() const { return m_lastActiveSession; }
    void setLastActiveSession(TerminalBackend *session);
    void notifySessionTitleChanged(TerminalBackend *session);

signals:
    void layoutChanged();
    void titleChanged();
    void alertingChanged();

private:
    std::unique_ptr<TerminalLayoutNode> m_root;
    TerminalBackend *m_lastActiveSession{};
    QString m_title;
    QString m_customTitle;
    bool m_alerting{};
};

class TerminalManager : public QObject
{
    Q_OBJECT

    QML_ELEMENT

    Q_PROPERTY(QObject *window READ window WRITE setWindow)
    Q_PROPERTY(QVariantList workspaces READ workspaces NOTIFY workspacesChanged)
    Q_PROPERTY(QObject *activeSession READ activeSession NOTIFY activeSessionChanged)
    Q_PROPERTY(int activeWorkspaceIndex READ activeWorkspaceIndex NOTIFY activeWorkspaceChanged)

public:
    explicit TerminalManager(QObject *parent = nullptr);
    ~TerminalManager() override;

    QObject *window() const { return m_window; }
    void setWindow(QObject *window);

    QVariantList workspaces() const;
    QObject *activeSession() const { return m_activeSession; }
    int activeWorkspaceIndex() const;

    Q_INVOKABLE void start();
    Q_INVOKABLE void addWorkspace();
    Q_INVOKABLE void renameWorkspace(QObject *workspace, const QString &title);
    Q_INVOKABLE void closeWorkspace(QObject *workspace);
    Q_INVOKABLE bool workspaceHasRunningCommand(QObject *workspace) const;
    Q_INVOKABLE bool anySessionHasRunningCommand() const;
    Q_INVOKABLE bool anySessionHasActiveProcess() const;
    Q_INVOKABLE int sessionCount() const;
    Q_INVOKABLE void moveWorkspace(int fromIndex, int toIndex);
    Q_INVOKABLE void closeSession(QObject *session);
    Q_INVOKABLE void activateSession(QObject *session);
    Q_INVOKABLE void setActiveWorkspace(int index);
    Q_INVOKABLE void splitActive(int orientation);
    Q_INVOKABLE void navigateActivationHistory(int direction);
    Q_INVOKABLE void routeKeyPress(QObject *session, int key, const QString &text, int modifiers);
    Q_INVOKABLE void routeTextInput(QObject *session, const QString &text);

signals:
    void workspacesChanged();
    void activeWorkspaceChanged();
    void activeSessionChanged();
    void quitRequested();

private:
    TerminalBackend *createSession();
    TerminalLayoutNode *findLeaf(TerminalLayoutNode *node, TerminalBackend *session) const;
    TerminalBackend *firstSession(TerminalLayoutNode *node) const;
    TerminalBackend *mostRecentlyActiveSession() const;
    TerminalBackend *firstValidSessionIn(TerminalWorkspaceModel *workspace) const;
    TerminalBackend *nextActiveSession(TerminalBackend *fallback) const;
    std::unique_ptr<TerminalLayoutNode> *findNodeSlot(
        std::unique_ptr<TerminalLayoutNode> &node, TerminalBackend *session) const;
    void setActiveSession(TerminalBackend *session, bool record);
    void recordActivation(TerminalBackend *session);
    void pruneSession(TerminalBackend *session);
    void removeSessionFromLayouts(TerminalBackend *session);
    void commitActivationHistory();

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
