pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Effects
import QtQuick.Layouts

// Shown in a pane running an AI agent Konsoll can connect but has not
// (AgentIntegrations.connectOffer): one click adds the agent's hooks, so its
// state shows in the tab and Konsoll notifies when it needs you.
Rectangle {
    id: root

    property var session: null
    // Set after Connect: the confirmation (or error) replaces the question.
    property string result: ""
    property bool failed: false
    property bool dismissed: false

    // Re-evaluated when the integrations change (AgentIntegrations.agents).
    readonly property string offer: {
        const agents = AgentIntegrations.agents;
        return agents && root.session && root.session.agentState === "" ? AgentIntegrations.connectOffer(root.session.foregroundProcess) : "";
    }
    readonly property string agentName: AgentIntegrations.agentName(root.offer !== "" ? root.offer : shownAgent)
    // The agent the confirmation is about (the offer is gone once connected).
    property string shownAgent: ""

    visible: !dismissed && (offer !== "" || result !== "")

    width: Math.min(460, parent ? parent.width - 24 : 460)
    implicitHeight: content.implicitHeight + 24
    radius: 8

    color: Qt.rgba(appPalette.window.r, appPalette.window.g, appPalette.window.b, 0.94)
    border.color: appPalette.mid
    layer.enabled: true

    layer.effect: MultiEffect {
        shadowEnabled: true
        shadowColor: appPalette.shadow
        shadowOpacity: 0.4
        shadowBlur: 0.35
        blurMax: 16
        shadowVerticalOffset: 3
    }

    SystemPalette {
        id: appPalette
        colorGroup: SystemPalette.Active
    }

    // A new agent in the pane may be offered again.
    onOfferChanged: {
        if (offer !== "")
            dismissed = false;
    }

    Timer {
        id: hideTimer
        interval: 6000
        onTriggered: {
            root.result = "";
            root.dismissed = true;
        }
    }

    ColumnLayout {
        id: content

        anchors.fill: parent
        anchors.margins: 12
        spacing: 8

        Label {
            Layout.fillWidth: true
            text: root.result !== "" ? root.result : qsTr("Connect %1 to Konsoll?").arg(root.agentName)
            font.bold: root.result === ""
            color: root.failed ? "#ff453a" : appPalette.windowText
            wrapMode: Text.WordWrap
        }

        Label {
            Layout.fillWidth: true
            visible: root.result === ""
            text: qsTr("The tab shows what it is doing, and Konsoll notifies you when it needs you or finishes. It applies the next time %1 starts.").arg(root.agentName)
            color: appPalette.windowText
            opacity: 0.75
            font.pointSize: 11
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.alignment: Qt.AlignRight
            visible: root.result === ""
            spacing: 8

            Button {
                text: qsTr("Not Now")
                onClicked: {
                    AgentIntegrations.declineOffer(root.offer);
                    root.dismissed = true;
                }
            }

            Button {
                text: qsTr("Connect")
                highlighted: true
                onClicked: {
                    const agent = root.offer;
                    const error = AgentIntegrations.install(agent);
                    root.shownAgent = agent;
                    root.failed = error !== "";
                    root.result = error !== "" ? error : qsTr("%1 is connected. Restart it to apply; Settings ▸ AI Agents can remove it.").arg(AgentIntegrations.agentName(agent));
                    hideTimer.restart();
                }
            }
        }
    }
}
