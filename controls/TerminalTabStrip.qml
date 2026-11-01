pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls.Basic

Item {
    id: strip

    property var tabs: []
    property int currentIndex: -1
    property real baseSafeTop: 40
    property real leadingInset: 0
    property real trailingInset: 0
    property real minimumTabWidth: 88

    readonly property real rowHeight: 34
    readonly property real tabSpacing: 6
    readonly property real firstRowWidth: Math.max(0, width - leadingInset - trailingInset - 54)
    readonly property real followingRowWidth: Math.max(0, width - trailingInset - 16)
    readonly property int firstRowCapacity: Math.max(1, Math.floor((firstRowWidth + tabSpacing) / (minimumTabWidth + tabSpacing)))
    readonly property int followingRowCapacity: Math.max(1, Math.floor((followingRowWidth + tabSpacing) / (minimumTabWidth + tabSpacing)))
    readonly property int rowCount: tabs.length <= firstRowCapacity ? 1 : 1 + Math.ceil((tabs.length - firstRowCapacity) / followingRowCapacity)
    readonly property real requiredHeight: Math.max(40, baseSafeTop) + (rowCount - 1) * rowHeight

    signal activateRequested(int index)
    signal closeRequested(var workspace)
    signal addRequested

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
            const preferredWidth = rowIndex === currentIndex ? 250 : 150;
            preferredWidthSum += preferredWidth;
            preferredExcessSum += preferredWidth - minimumWidth;
        }

        const preferredWidth = index === currentIndex ? 250 : 150;
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

    Repeater {
        model: strip.tabs

        delegate: TerminalTabButton {
            required property int index
            required property var modelData

            x: strip.tabX(index)
            y: strip.tabY(index)
            width: strip.tabWidth(index)

            workspace: modelData
            active: index === strip.currentIndex
            onClicked: strip.activateRequested(index)
            onCloseRequested: strip.closeRequested(workspace)
        }
    }

    ToolButton {
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

        background: Rectangle {
            radius: width / 2
            color: addButton.hovered ? Qt.rgba(systemPalette.windowText.r, systemPalette.windowText.g, systemPalette.windowText.b, 0.12) : "transparent"
        }

        onClicked: strip.addRequested()
    }
}
