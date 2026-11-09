pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic
import "." as Controls

Rectangle {
    id: strip

    required property TerminalManager sessionManager

    property var tabs: []
    property int currentIndex: -1
    property real baseSafeTop: 40
    property real leadingInset: 0
    property real trailingInset: 0
    property real minimumTabWidth: 88
    property var windowTranslucency: null
    property bool tabPointerPressed: false
    property int dragSourceIndex: -1
    property int dragTargetIndex: -1
    property var dragWorkspace: null
    property bool dragWorkspaceActive: false
    property real dragPointerX: 0
    property real dragPointerY: 0
    property real dragGrabOffsetX: 0
    property real dragGrabOffsetY: 0

    readonly property real rowHeight: 34
    readonly property real tabSpacing: 6
    readonly property real firstRowWidth: Math.max(0, width - leadingInset - trailingInset - 54)
    readonly property real followingRowWidth: Math.max(0, width - trailingInset - 16)
    readonly property int firstRowCapacity: Math.max(1, Math.floor((firstRowWidth + tabSpacing) / (minimumTabWidth + tabSpacing)))
    readonly property int followingRowCapacity: Math.max(1, Math.floor((followingRowWidth + tabSpacing) / (minimumTabWidth + tabSpacing)))
    readonly property int rowCount: tabs.length <= firstRowCapacity ? 1 : 1 + Math.ceil((tabs.length - firstRowCapacity) / followingRowCapacity)
    readonly property real requiredHeight: Math.max(40, baseSafeTop) + (rowCount - 1) * rowHeight
    readonly property int visualCurrentIndex: dragSourceIndex < 0 ? currentIndex : visualIndex(currentIndex)

    signal activateRequested(int index)
    signal closeRequested(var workspace)
    signal reorderRequested(int fromIndex, int toIndex)
    signal addRequested

    onWindowTranslucencyChanged: updateWindowMovable()
    Component.onCompleted: updateWindowMovable()

    // The strip's background is native Liquid Glass (drawn behind the Qt
    // content); a tinted fill here would wash it out. Without glass, fall
    // back to the flat translucent fill.
    color: stripGlass.supported ? "transparent" : Qt.rgba(systemPalette.window.r, systemPalette.window.g, systemPalette.window.b, 0.50)

    GlassEffect {
        id: stripGlass

        anchors.fill: parent
        visible: supported
        cornerRadius: 0
    }

    SystemPalette {
        id: systemPalette
        colorGroup: SystemPalette.Active
    }

    function tabRow(index) {
        if (index < firstRowCapacity)
            return 0;
        return 1 + Math.floor((index - firstRowCapacity) / followingRowCapacity);
    }

    function firstIndexInRow(row) {
        return row === 0 ? 0 : firstRowCapacity + (row - 1) * followingRowCapacity;
    }

    function itemCountInRow(row) {
        const firstIndex = firstIndexInRow(row);
        const capacity = row === 0 ? firstRowCapacity : followingRowCapacity;
        return Math.min(capacity, tabs.length - firstIndex);
    }

    function widthInRow(row) {
        return row === 0 ? firstRowWidth : followingRowWidth;
    }

    function tabWidth(index) {
        const row = tabRow(index);
        const firstIndex = firstIndexInRow(row);
        const count = itemCountInRow(row);
        const rowWidth = widthInRow(row) - tabSpacing * (count - 1);
        const minimumWidth = Math.min(minimumTabWidth, rowWidth / count);
        let preferredWidthSum = 0;
        let preferredExcessSum = 0;

        for (let rowIndex = firstIndex; rowIndex < firstIndex + count; ++rowIndex) {
            const preferredWidth = rowIndex === visualCurrentIndex ? 250 : 150;
            preferredWidthSum += preferredWidth;
            preferredExcessSum += preferredWidth - minimumWidth;
        }

        const preferredWidth = index === visualCurrentIndex ? 250 : 150;
        if (preferredWidthSum <= rowWidth)
            return preferredWidth;

        const availableExcess = rowWidth - minimumWidth * count;
        const scale = Math.min(1, availableExcess / preferredExcessSum);
        return minimumWidth + (preferredWidth - minimumWidth) * scale;
    }

    function tabX(index) {
        const row = tabRow(index);
        const firstIndex = firstIndexInRow(row);
        let x = row === 0 ? leadingInset + 8 : 8;

        for (let rowIndex = firstIndex; rowIndex < index; ++rowIndex)
            x += tabWidth(rowIndex) + tabSpacing;

        return x;
    }

    function tabY(index) {
        const firstRowY = (Math.max(40, baseSafeTop) - rowHeight) / 2;
        return firstRowY + tabRow(index) * rowHeight;
    }

    function visualIndex(modelIndex) {
        if (dragSourceIndex < 0)
            return modelIndex;
        const order = [];
        for (let index = 0; index < tabs.length; ++index) {
            if (index !== dragSourceIndex)
                order.push(index);
        }
        order.splice(dragTargetIndex, 0, dragSourceIndex);
        return order.indexOf(modelIndex);
    }

    function nearestTabIndex(x, y) {
        let nearest = -1;
        let nearestDistance = Number.POSITIVE_INFINITY;
        for (let index = 0; index < tabs.length; ++index) {
            const dx = x - tabX(index) - tabWidth(index) / 2;
            const dy = y - tabY(index) - rowHeight / 2;
            const distance = dx * dx + dy * dy;
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearest = index;
            }
        }
        return nearest;
    }

    function updateWindowMovable() {
        let overTab = false;
        for (let index = 0; index < tabRepeater.count; ++index) {
            const tab = tabRepeater.itemAt(index);
            if (tab && tab.pointerOverTab) {
                overTab = true;
                break;
            }
        }
        if (windowTranslucency)
            windowTranslucency.setWindowMovable(dragSourceIndex < 0 && !tabPointerPressed && !overTab);
    }

    function beginTabDrag(index, sceneX, sceneY) {
        if (windowTranslucency)
            windowTranslucency.setWindowMovable(false);
        const point = strip.mapFromItem(null, sceneX, sceneY);
        dragTargetIndex = index;
        dragSourceIndex = index;
        dragWorkspace = tabs[index];
        dragWorkspaceActive = index === currentIndex;
        dragPointerX = point.x;
        dragPointerY = point.y;
        dragGrabOffsetX = point.x - tabX(index);
        dragGrabOffsetY = point.y - tabY(index);
        updateWindowMovable();
    }

    function moveTabDrag(sceneX, sceneY) {
        const point = strip.mapFromItem(null, sceneX, sceneY);
        dragPointerX = point.x;
        dragPointerY = point.y;
        const targetIndex = nearestTabIndex(point.x, point.y);
        if (targetIndex >= 0 && targetIndex !== dragTargetIndex)
            dragTargetIndex = targetIndex;
    }

    function finishTabDrag(sceneX, sceneY) {
        if (dragSourceIndex < 0)
            return;
        const point = strip.mapFromItem(null, sceneX, sceneY);
        const fromIndex = dragSourceIndex;
        const toIndex = nearestTabIndex(point.x, point.y);
        dragSourceIndex = -1;
        dragTargetIndex = -1;
        dragWorkspace = null;
        updateWindowMovable();
        if (toIndex >= 0)
            reorderRequested(fromIndex, toIndex);
    }

    function cancelTabDrag() {
        dragSourceIndex = -1;
        dragTargetIndex = -1;
        dragWorkspace = null;
        updateWindowMovable();
    }

    Repeater {
        id: tabRepeater
        model: strip.tabs

        delegate: TerminalTabButton {
            id: tabButton

            required property int index
            required property var modelData

            readonly property int layoutIndex: strip.visualIndex(index)

            x: strip.tabX(layoutIndex)
            y: strip.tabY(layoutIndex)

            width: strip.tabWidth(layoutIndex)
            opacity: index === strip.dragSourceIndex ? 0 : 1

            Behavior on x {
                NumberAnimation {
                    duration: 100
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on y {
                NumberAnimation {
                    duration: 100
                    easing.type: Easing.OutCubic
                }
            }

            workspace: modelData
            active: index === strip.currentIndex
            tabStrip: strip
            sessionManager: strip.sessionManager

            onClicked: {
                if (!dragged)
                    strip.activateRequested(index);
            }
            onCloseRequested: strip.closeRequested(workspace)
            onDragStarted: (sceneX, sceneY) => strip.beginTabDrag(index, sceneX, sceneY)
            onDragMoved: (sceneX, sceneY) => strip.moveTabDrag(sceneX, sceneY)
            onDragFinished: (sceneX, sceneY) => strip.finishTabDrag(sceneX, sceneY)
            onDragCanceled: strip.cancelTabDrag()
        }
    }

    Controls.TerminalTabButton {
        visible: strip.dragSourceIndex >= 0
        enabled: false

        x: strip.dragPointerX - strip.dragGrabOffsetX
        y: strip.dragPointerY - strip.dragGrabOffsetY
        z: 2

        width: strip.dragTargetIndex >= 0 ? strip.tabWidth(strip.dragTargetIndex) : 0
        opacity: 0.9

        workspace: strip.dragWorkspace
        active: strip.dragWorkspaceActive
        dragging: false
        dragged: false
        sessionManager: strip.sessionManager
    }

    Controls.ToolButton {
        id: addButton

        Accessible.name: qsTr("New tab")

        y: (Math.max(40, strip.baseSafeTop) - height) / 2
        z: 1

        width: 30
        height: 30
        anchors.right: strip.right
        anchors.rightMargin: strip.trailingInset + 8

        icon.name: "add"
        display: AbstractButton.IconOnly

        onClicked: strip.addRequested()
    }
}
