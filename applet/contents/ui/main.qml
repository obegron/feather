import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.kirigami as Kirigami
import io.github.obegron.feather

PlasmoidItem {
    id: root
    readonly property var radio: bridge.snapshot
    property bool confirmationPending: false
    property bool suppressWarning: false
    property bool settingsOpen: false
    property double now: Date.now() / 1000
    readonly property bool timerUnknown: radio.timerKnown === false
    readonly property bool timed: !!radio.owned && Number(radio.deadline) > 0 && !timerUnknown
    readonly property int secondsLeft: timed ? Math.max(0, Math.ceil(Number(radio.deadline) - now)) : 0
    // An installed update may talk to a session service started before
    // activeDuration existed. Capture an estimate once, never on each tick.
    property double observedDeadline: 0
    property real fallbackDuration: 0
    readonly property real ringDuration: Number(radio.activeDuration) > 0 ? Number(radio.activeDuration) : fallbackDuration
    readonly property real remainingFraction: timed && ringDuration > 0 ? Math.max(0, Math.min(1, secondsLeft / ringDuration)) : 0
    readonly property string countdown: Math.floor(secondsLeft / 60) + ":" + (secondsLeft % 60 < 10 ? "0" : "") + secondsLeft % 60
    readonly property string remaining: timerUnknown ? i18n("Timer status unknown") : root.timed && root.secondsLeft === 0 && Number(radio.transferCount) > 0 ? i18n("Finishing transfers") : root.timed ? i18n("%1 remaining", root.countdown) : i18n("Until turned off")
    readonly property string stateText: ({"off": i18n("Off"), "on": i18n("On"), "starting": i18n("Turning on…"), "stopping": i18n("Turning off…"), "external": i18n("In use by another app"), "unavailable": i18n("Unavailable"), "error": i18n("Couldn’t connect")})[radio.state] || i18n("Checking…")
    readonly property bool canToggle: !confirmationPending && !radio.busy && !(Number(radio.transferCount) > 0) && (radio.owned || radio.state === "off" || radio.state === "error")
    Plasmoid.title: i18n("Feather")
    Plasmoid.icon: "document-send"
    Plasmoid.status: radio.owned || radio.busy ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.PassiveStatus
    toolTipMainText: i18n("Feather — AirDrop radio")
    toolTipSubText: (Number(radio.transferCount) > 0 ? i18n("%1 active transfers", radio.transferCount) + " · " : "") + stateText + (radio.state === "on" ? " · " + remaining : "")
    function shareDrop(drop) {
        if (!drop.hasUrls || drop.urls.length === 0) { drop.accepted = false; return }
        const urls = []
        for (let i = 0; i < drop.urls.length; ++i) {
            const url = drop.urls[i].toString()
            if (!url.startsWith("file:")) { drop.accepted = false; return }
            urls.push(url)
        }
        bridge.sendFiles(urls)
        drop.accept(Qt.CopyAction)
    }
    Plasmoid.contextualActions: [
        PlasmaCore.Action { text: i18n("Send files…"); icon.name: "document-send"; onTriggered: bridge.pickFiles() },
        PlasmaCore.Action { text: i18n("Receive files"); icon.name: "download"; onTriggered: bridge.receiveFiles() }
    ]
    preferredRepresentation: compactRepresentation
    compactRepresentation: QQC2.ToolButton {
        id: trayButton
        Accessible.name: i18n("Feather, %1", root.stateText) + (root.radio.owned ? ", " + root.remaining : "")
        onClicked: root.expanded = !root.expanded
        QQC2.Menu {
            id: trayMenu
            QQC2.MenuItem { text: i18n("Send files…"); icon.name: "document-send"; onTriggered: bridge.pickFiles() }
            QQC2.MenuItem { text: i18n("Receive files"); icon.name: "download"; onTriggered: bridge.receiveFiles() }
            QQC2.MenuSeparator {}
            QQC2.MenuItem { text: i18n("Settings"); icon.name: "configure"; onTriggered: { root.settingsOpen = true; root.expanded = true } }
        }
        MouseArea {
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: function(mouse) { trayMenu.popup(mouse.x, mouse.y) }
        }
        contentItem: Item {
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
            Rectangle {
                anchors.fill: parent
                radius: Kirigami.Units.smallSpacing
                visible: trayDrop.containsDrag
                color: Kirigami.Theme.highlightColor
                opacity: 0.2
            }
            DropArea {
                id: trayDrop
                anchors.fill: parent
                onEntered: function(drag) { drag.accepted = drag.hasUrls }
                onDropped: function(drop) { root.shareDrop(drop) }
            }
            Kirigami.Icon {
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height) * (root.radio.owned ? 0.65 : 0.9)
                height: width
                source: "document-send"
                opacity: root.radio.owned ? 1 : 0.65
                active: !!root.radio.owned
            }
            Canvas {
                id: countdownRing
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height)
                height: width
                visible: !!root.radio.owned
                property real fraction: root.timerUnknown ? 0 : root.timed ? root.remainingFraction : 1
                property color ringColor: Kirigami.Theme.positiveTextColor
                property color trackColor: Kirigami.Theme.textColor
                onFractionChanged: requestPaint()
                onRingColorChanged: requestPaint()
                onTrackColorChanged: requestPaint()
                onVisibleChanged: requestPaint()
                onWidthChanged: requestPaint()
                onHeightChanged: requestPaint()
                onPaint: {
                    const ctx = getContext("2d")
                    ctx.clearRect(0, 0, width, height)
                    const stroke = Math.max(2, width / 12)
                    const radius = Math.max(0, (width - stroke) / 2)
                    ctx.lineWidth = stroke
                    ctx.globalAlpha = 0.2
                    ctx.strokeStyle = trackColor
                    ctx.beginPath()
                    ctx.arc(width / 2, height / 2, radius, 0, 2 * Math.PI)
                    ctx.stroke()
                    ctx.globalAlpha = 1
                    ctx.strokeStyle = ringColor
                    ctx.lineCap = "round"
                    ctx.beginPath()
                    ctx.arc(width / 2, height / 2, radius, -Math.PI / 2, -Math.PI / 2 + 2 * Math.PI * fraction)
                    ctx.stroke()
                }
            }

        }
    }
    onExpandedChanged: {
        if (root.expanded) { root.now = Date.now() / 1000; bridge.refresh() }
        else { root.confirmationPending = false; root.suppressWarning = false }
    }
    Bridge {
        id: bridge
        onChanged: {
            const snapshot = bridge.snapshot
            const deadline = Number(snapshot.deadline) || 0
            if (deadline !== root.observedDeadline) {
                root.observedDeadline = deadline
                root.fallbackDuration = deadline > 0 ? Math.max(1, deadline - Date.now() / 1000, Number(snapshot.duration) || 0) : 0
            }
        }
        onWarningRequired: {
            root.suppressWarning = false
            root.confirmationPending = true
            root.settingsOpen = false
        }
    }
    Timer { interval: 1000; repeat: true; running: true; onTriggered: root.now = Date.now() / 1000 }
    fullRepresentation: QQC2.ScrollView {
        id: panel
        Layout.minimumWidth: Kirigami.Units.gridUnit * 18
        Layout.preferredWidth: Kirigami.Units.gridUnit * 18
        Layout.minimumHeight: Kirigami.Units.gridUnit * 9
        Layout.preferredHeight: Kirigami.Units.gridUnit * 11
        contentWidth: availableWidth
        clip: true
        QQC2.Menu {
            id: panelMenu
            QQC2.MenuItem { text: i18n("Send files…"); icon.name: "document-send"; onTriggered: bridge.pickFiles() }
            QQC2.MenuItem { text: i18n("Receive files"); icon.name: "download"; onTriggered: bridge.receiveFiles() }
        }
        MouseArea {
            parent: panel
            anchors.fill: parent
            acceptedButtons: Qt.RightButton
            onClicked: function(mouse) { panelMenu.popup(mouse.x, mouse.y) }
        }
        ColumnLayout {
            width: panel.availableWidth
            spacing: Kirigami.Units.largeSpacing
            // Padding belongs to the content so the tray popup and standalone
            // window use the same layout without a separate floating dialog.
            QQC2.Pane {
                Layout.fillWidth: true
                padding: Kirigami.Units.largeSpacing
                DropArea {
                    parent: panel
                    anchors.fill: parent
                    onEntered: function(drag) { drag.accepted = drag.hasUrls }
                    onDropped: function(drop) { root.shareDrop(drop) }
                }
                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    RowLayout {
                        Kirigami.Icon { source: "document-send"; implicitWidth: Kirigami.Units.iconSizes.medium; implicitHeight: implicitWidth }
                        ColumnLayout {
                            spacing: 0
                            Kirigami.Heading { text: i18n("Feather"); level: 2 }
                            QQC2.Label { text: root.radio.receiving ? i18n("Ready to receive") : i18n("AirDrop"); opacity: 0.7 }
                        }
                        Item { Layout.fillWidth: true }
                        QQC2.ToolButton {
                            icon.name: root.settingsOpen ? "go-previous" : "configure"
                            Accessible.name: root.settingsOpen ? i18n("Back") : i18n("Settings")
                            QQC2.ToolTip.text: Accessible.name
                            QQC2.ToolTip.visible: hovered
                            onClicked: { root.settingsOpen = !root.settingsOpen; root.confirmationPending = false }
                        }
                    }
                    RowLayout {
                        visible: !root.confirmationPending && !root.settingsOpen
                        ColumnLayout {
                            Layout.fillWidth: true
                            QQC2.Label { text: root.stateText; font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                            QQC2.Label { visible: root.radio.state === "on"; text: root.remaining; opacity: 0.7 }
                        }
                        QQC2.BusyIndicator { visible: !!root.radio.busy; running: visible; implicitWidth: Kirigami.Units.iconSizes.smallMedium; implicitHeight: implicitWidth }
                        QQC2.Switch {
                            id: radioSwitch
                            checked: !!root.radio.owned
                            enabled: root.canToggle
                            Accessible.name: i18n("AirDrop radio")
                            onClicked: {
                                checked = Qt.binding(function() { return !!root.radio.owned })
                                root.radio.owned ? bridge.disable() : bridge.enable()
                            }
                        }
                    }
                    RowLayout {
                        visible: !root.confirmationPending && !root.settingsOpen && (!root.radio.owned || root.timed)
                        Layout.fillWidth: true
                        enabled: !root.radio.busy && (!root.radio.owned || root.radio.canRenew)
                        QQC2.Button {
                            text: root.radio.owned ? i18n("+5 min") : i18n("5 min")
                            Layout.fillWidth: true
                            checkable: !root.radio.owned
                            checked: !root.radio.owned && Number(root.radio.duration) === 300
                            onClicked: root.radio.owned ? bridge.addTime(300) : bridge.setDuration(300)
                        }
                        QQC2.Button {
                            text: root.radio.owned ? i18n("+10 min") : i18n("10 min")
                            Layout.fillWidth: true
                            checkable: !root.radio.owned
                            checked: !root.radio.owned && Number(root.radio.duration) === 600
                            onClicked: root.radio.owned ? bridge.addTime(600) : bridge.setDuration(600)
                        }
                        QQC2.Button {
                            text: "∞"
                            font.pixelSize: Kirigami.Units.iconSizes.smallMedium
                            Accessible.name: i18n("Until turned off")
                            QQC2.ToolTip.text: i18n("Until turned off")
                            QQC2.ToolTip.visible: hovered
                            Layout.fillWidth: true
                            checkable: !root.radio.owned
                            checked: !root.radio.owned && Number(root.radio.duration) === 0
                            onClicked: root.radio.owned ? bridge.addTime(0) : bridge.setDuration(0)
                        }
                    }
                    ColumnLayout {
                        visible: !!root.radio.owned && !root.confirmationPending && !root.settingsOpen
                        Layout.fillWidth: true
                        Repeater {
                            model: root.radio.transfers || []
                            delegate: RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                Kirigami.Icon { source: modelData.direction === "incoming" ? "download" : "upload"; implicitWidth: Kirigami.Units.iconSizes.small; implicitHeight: implicitWidth }
                                QQC2.Label {
                                    text: modelData.state === "stopping" ? i18n("Finishing transfer cleanup…") : modelData.state === "approval" ? i18n("Awaiting approval: %1", modelData.name) : modelData.direction === "incoming" ? i18n("Receiving from %1", modelData.name) : i18n("Sending to %1", modelData.name)
                                    textFormat: Text.PlainText; elide: Text.ElideRight; Layout.fillWidth: true
                                }
                            }
                        }
                        QQC2.Label { visible: !!root.radio.transferError; text: root.radio.transferError || ""; textFormat: Text.PlainText; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                    }
                    ColumnLayout {
                        visible: root.confirmationPending
                        onVisibleChanged: { if (visible) cancelButton.forceActiveFocus() }
                        Layout.fillWidth: true
                        QQC2.Label { text: i18n("Turn on AirDrop radio?"); font.bold: true; wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        QQC2.Label {
                            text: i18n("Intel Wi-Fi disconnects while AirDrop is on. You may need to reconnect afterward.")
                            wrapMode: Text.WordWrap
                            Layout.fillWidth: true
                        }
                        QQC2.CheckBox {
                            text: i18n("Don’t ask again")
                            checked: root.suppressWarning
                            onToggled: root.suppressWarning = checked
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Item { Layout.fillWidth: true }
                            QQC2.Button { id: cancelButton; text: i18n("Cancel"); onClicked: { root.confirmationPending = false; radioSwitch.forceActiveFocus() } }
                            QQC2.Button {
                                text: i18n("Turn on")
                                icon.name: "network-wireless"
                                onClicked: { root.confirmationPending = false; bridge.confirmEnable(root.suppressWarning) }
                            }
                        }
                    }
                    ColumnLayout {
                        visible: root.settingsOpen
                        Layout.fillWidth: true
                        QQC2.Label { visible: !!root.radio.showWarning; text: i18n("Wi-Fi warning is enabled"); opacity: 0.7 }
                        QQC2.Button {
                            visible: !root.radio.showWarning
                            text: i18n("Show Wi-Fi warning again")
                            icon.name: "dialog-warning"
                            onClicked: bridge.resetWarning()
                        }
                        QQC2.Button { visible: !!root.radio.rememberedRecipient; text: i18n("Forget saved device"); icon.name: "edit-clear"; onClicked: bridge.forgetRecipient() }
                        QQC2.Button { text: i18n("Network settings"); icon.name: "network-wireless"; onClicked: bridge.openNetworkSettings() }
                        QQC2.Label {
                            text: i18n("If Wi-Fi doesn’t reconnect, use Network settings. Received files are saved to Downloads.")
                            wrapMode: Text.WordWrap; Layout.fillWidth: true; opacity: 0.7
                        }
                        QQC2.CheckBox { id: details; text: i18n("Diagnostics") }
                        QQC2.Label {
                            visible: details.checked
                            text: root.radio.detail || i18n("Transfers powered by Owly.")
                            wrapMode: Text.WrapAnywhere; Layout.fillWidth: true
                        }
                    }
                    RowLayout {
                        visible: !root.confirmationPending && (!!root.radio.detail || root.radio.state === "unavailable")
                        Layout.fillWidth: true
                        QQC2.Label { text: i18n("Connection needs attention"); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        QQC2.ToolButton {
                            icon.name: "view-refresh"; Accessible.name: i18n("Retry")
                            QQC2.ToolTip.text: Accessible.name; QQC2.ToolTip.visible: hovered
                            onClicked: bridge.refresh()
                        }
                        QQC2.ToolButton {
                            icon.name: "dialog-information"; Accessible.name: i18n("Details")
                            QQC2.ToolTip.text: Accessible.name; QQC2.ToolTip.visible: hovered
                            onClicked: { root.settingsOpen = true; details.checked = true }
                        }
                    }
                    QQC2.Label {
                        visible: !root.settingsOpen && !root.confirmationPending
                        text: i18n("Drop files here to share")
                        opacity: 0.6; font: Kirigami.Theme.smallFont
                    }
                }
            }
        }
    }
}
