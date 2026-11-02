import QtCore
import QtQuick
import QtQuick.Dialogs

FileDialog {
    id: dialog

    property var session: null
    property var focusTarget: null
    property bool saveSelection: false

    title: qsTr("Save Terminal Text")
    fileMode: FileDialog.SaveFile
    nameFilters: [qsTr("Text files (*.txt)"), qsTr("All files (*)")]
    defaultSuffix: "txt"
    currentFolder: StandardPaths.writableLocation(StandardPaths.DownloadLocation)

    onAccepted: {
        if (session)
            session.saveTextToFile(selectedFile, saveSelection);
        restoreFocus();
    }

    onRejected: restoreFocus()

    function restoreFocus() {
        const target = focusTarget;
        Qt.callLater(() => {
            if (target)
                target.forceActiveFocus();
            focusTarget = null;
        });
    }

    function openFor(targetSession, targetFocus) {
        session = targetSession;
        focusTarget = targetFocus;
        saveSelection = targetSession && (targetSession.selectionActive || targetSession.highlightActive);
        open();
    }
}
