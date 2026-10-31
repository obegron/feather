import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.kirigami as Kirigami
import org.kde.feather

PlasmoidItem {
    id: root
    readonly property var radio: bridge.snapshot
    property bool confirmationPending: false
    property bool suppressWarning: false
    property bool settingsOpen: false
    property double now: Date.now() / 1000
    readonly property bool timed: !!radio.owned && Number(radio.deadline) > 0
    readonly property int secondsLeft: timed ? Math.max(0, Math.ceil(Number(radio.deadline) - now)) : 0
    readonly property int minutesLeft: Math.ceil(secondsLeft / 60)
    readonly property real remainingFraction: timed && Number(radio.activeDuration) > 0 ? Math.min(1, secondsLeft / Number(radio.activeDuration)) : 0
    readonly property string countdown: Math.floor(secondsLeft / 60) + ":" + (secondsLeft % 60 < 10 ? "0" : "") + secondsLeft % 60
    readonly property string remaining: root.timed ? i18n("%1 remaining", root.countdown) : i18n("Until turned off")
    readonly property string stateText: ({"off": i18n("Off"), "on": i18n("On"), "starting": i18n("Turning on…"), "stopping": i18n("Turning off…"), "external": i18n("In use by another app"), "unavailable": i18n("Unavailable"), "error": i18n("Couldn’t connect")})[radio.state] || i18n("Checking…")
    readonly property bool canToggle: !confirmationPending && !radio.busy && (radio.owned || radio.state === "off" || radio.state === "error")
    Plasmoid.title: i18n("Feather")
    Plasmoid.icon: "document-send"
    Plasmoid.status: radio.owned || radio.busy ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.PassiveStatus
    toolTipMainText: i18n("Feather — AirDrop radio")
    toolTipSubText: stateText + (radio.state === "on" ? " · " + remaining : "")
    preferredRepresentation: compactRepresentation
    compactRepresentation: QQC2.ToolButton {
        id: trayButton
        Accessible.name: i18n("Feather, %1", root.stateText) + (root.radio.owned ? ", " + root.remaining : "")
        onClicked: root.expanded = !root.expanded
        contentItem: Item {
            implicitWidth: Kirigami.Units.iconSizes.smallMedium
            implicitHeight: implicitWidth
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
                property real fraction: root.timed ? root.remainingFraction : 1
                property color ringColor: Kirigami.Theme.highlightColor
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
            Rectangle {
                visible: root.timed
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                width: Math.max(minuteBadge.implicitWidth + 4, parent.width * 0.48)
                height: Math.max(minuteBadge.implicitHeight, parent.height * 0.45)
                radius: height / 2
                color: Kirigami.Theme.highlightColor
                QQC2.Label {
                    id: minuteBadge
                    anchors.centerIn: parent
                    text: root.minutesLeft > 0 ? String(root.minutesLeft) : "<1"
                    color: Kirigami.Theme.highlightedTextColor
                    font.pixelSize: Math.max(8, Math.min(trayButton.width, trayButton.height) * 0.3)
                    font.bold: true
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
        Layout.preferredWidth: Kirigami.Units.gridUnit * 20
        Layout.minimumHeight: Kirigami.Units.gridUnit * 14
        Layout.preferredHeight: Kirigami.Units.gridUnit * 17
        contentWidth: availableWidth
        clip: true
        ColumnLayout {
            width: panel.availableWidth
            spacing: Kirigami.Units.largeSpacing
            // Padding belongs to the content so the tray popup and standalone
            // window use the same layout without a separate floating dialog.
            QQC2.Pane {
                Layout.fillWidth: true
                padding: Kirigami.Units.largeSpacing
                contentItem: ColumnLayout {
                    spacing: Kirigami.Units.largeSpacing
                    RowLayout {
                        Kirigami.Icon { source: "document-send"; implicitWidth: Kirigami.Units.iconSizes.medium; implicitHeight: implicitWidth }
                        ColumnLayout {
                            spacing: 0
                            Kirigami.Heading { text: i18n("Feather"); level: 2 }
                            QQC2.Label { text: i18n("AirDrop radio"); opacity: 0.7 }
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
                    ColumnLayout {
                        visible: !root.confirmationPending && !root.settingsOpen
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing
                        QQC2.Label { text: root.radio.owned ? i18n("Next time, turn off after") : i18n("Turn off after"); opacity: 0.7 }
                        QQC2.Slider {
                            id: timeoutSlider
                            Layout.fillWidth: true
                            from: 0; to: 2; stepSize: 1
                            snapMode: QQC2.Slider.SnapAlways
                            value: Number(root.radio.duration) === 600 ? 0 : Number(root.radio.duration) === 1800 ? 1 : 2
                            Accessible.name: i18n("Radio timeout")
                            Accessible.description: [i18n("10 minutes"), i18n("30 minutes"), i18n("Until turned off")][Math.round(value)]
                            onMoved: bridge.setDuration([600, 1800, 0][Math.round(value)])
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            QQC2.Label { text: i18n("10 min"); font.bold: Math.round(timeoutSlider.value) === 0 }
                            Item { Layout.fillWidth: true }
                            QQC2.Label { text: i18n("30 min"); font.bold: Math.round(timeoutSlider.value) === 1 }
                            Item { Layout.fillWidth: true }
                            QQC2.Label { text: i18n("Until off"); font.bold: Math.round(timeoutSlider.value) === 2 }
                        }
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
                        QQC2.Button { text: i18n("Network settings"); icon.name: "network-wireless"; onClicked: bridge.openNetworkSettings() }
                        QQC2.Label {
                            text: i18n("If Wi-Fi doesn’t reconnect, use Network settings. File sharing is coming later.")
                            wrapMode: Text.WordWrap; Layout.fillWidth: true; opacity: 0.7
                        }
                        QQC2.CheckBox { id: details; text: i18n("Diagnostics") }
                        QQC2.Label {
                            visible: details.checked
                            text: root.radio.detail || i18n("Radio powered by Owly. This preview controls the radio only.")
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
                        text: i18n("Preview · radio controls only")
                        opacity: 0.6; font: Kirigami.Theme.smallFont
                    }
                }
            }
        }
    }
}
