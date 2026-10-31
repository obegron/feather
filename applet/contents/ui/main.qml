import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as QQC2
import org.kde.plasma.plasmoid
import org.kde.plasma.core as PlasmaCore
import org.kde.kirigami as Kirigami
import org.kde.feather

PlasmoidItem {
    id: root
    readonly property var state: bridge.snapshot
    property double now: Date.now() / 1000
    readonly property string remaining: state.deadline > 0 ? i18n("About %1 min remaining", Math.max(0, Math.ceil((state.deadline - now) / 60))) : i18n("Until turned off")
    readonly property string stateText: ({"off": i18n("Radio off"), "on": i18n("Radio on"), "starting": i18n("Turning radio on…"), "stopping": i18n("Turning radio off…"), "external": i18n("Another application is using the radio"), "unavailable": i18n("Service unavailable"), "error": i18n("Radio action failed")})[state.state] || i18n("Checking radio…")
    Plasmoid.title: i18n("Feather")
    Plasmoid.icon: "network-wireless"
    Plasmoid.status: state.state === "on" || state.busy ? PlasmaCore.Types.ActiveStatus : PlasmaCore.Types.PassiveStatus
    toolTipMainText: i18n("Feather — AirDrop radio")
    toolTipSubText: stateText + (state.state === "on" ? " · " + remaining : "")
    preferredRepresentation: compactRepresentation
    compactRepresentation: QQC2.ToolButton {
        icon.name: "network-wireless"
        Accessible.name: i18n("Feather, %1", root.stateText)
        onClicked: root.expanded = !root.expanded
    }
    onExpandedChanged: if (expanded) bridge.refresh()
    Bridge {
        id: bridge
        onWarningRequired: {
            suppress.checked = false
            warning.open()
        }
    }
    Timer { interval: 1000; repeat: true; running: true; onTriggered: root.now = Date.now() / 1000 }
    fullRepresentation: ColumnLayout {
        Layout.minimumWidth: Kirigami.Units.gridUnit * 19
        Layout.preferredWidth: Kirigami.Units.gridUnit * 22
        Layout.minimumHeight: implicitHeight
        spacing: Kirigami.Units.smallSpacing
        Kirigami.Heading { text: i18n("Feather"); Layout.fillWidth: true }
        QQC2.Label { text: i18n("AirDrop radio · Developer preview"); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        QQC2.Label { visible: root.state.state === "unavailable"; text: i18n("Install feather-session and owlyd, including their D-Bus service files, then refresh status."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        QQC2.Label { visible: !!root.state.detail && root.state.state !== "unavailable"; text: i18n("The last action reported a problem. See technical details."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        QQC2.Label { text: root.stateText; wrapMode: Text.WordWrap; Layout.fillWidth: true; Accessible.name: text }
        QQC2.BusyIndicator { running: !!root.state.busy; visible: running; Layout.alignment: Qt.AlignHCenter }
        QQC2.Label { visible: root.state.state === "on"; text: root.remaining; Layout.fillWidth: true }
        QQC2.Button {
            text: root.state.owned ? i18n("Turn off") : i18n("Turn on")
            icon.name: root.state.owned ? "network-disconnect" : "network-connect"
            Layout.fillWidth: true
            enabled: !root.state.busy && (root.state.owned || root.state.state === "off" || root.state.state === "error")
            onClicked: root.state.owned ? bridge.disable() : bridge.enable()
        }
        QQC2.Label { text: i18n("Wi-Fi is unavailable while AirDrop radio is on. File sharing is not available in this version."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        QQC2.Label { text: i18n("Off means the radio lease ended. Wi-Fi reconnection is not verified; KWallet connections may need manual recovery."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
        QQC2.Button { text: i18n("Open network settings"); icon.name: "configure"; onClicked: bridge.openNetworkSettings() }
        QQC2.Label { text: i18n("Turn off radio after (next activation)"); Layout.fillWidth: true; wrapMode: Text.WordWrap }
        QQC2.ComboBox {
            model: [i18n("10 minutes"), i18n("30 minutes"), i18n("Until turned off")]
            currentIndex: root.state.duration === 600 ? 0 : root.state.duration === 1800 ? 1 : 2
            Accessible.name: i18n("Turn off radio after, next activation")
            onActivated: bridge.setDuration([600, 1800, 0][currentIndex])
            Layout.fillWidth: true
        }
        QQC2.Button { text: i18n("Show activation warning again"); enabled: !root.state.showWarning; onClicked: bridge.resetWarning() }
        QQC2.Button { text: i18n("Refresh status"); onClicked: bridge.refresh() }
        QQC2.CheckBox { id: details; text: i18n("Technical details") }
        QQC2.Label { visible: details.checked; text: root.state.detail || i18n("Owly owns radio control and recovery. Install and start owlyd on the system bus if unavailable."); wrapMode: Text.WrapAnywhere; Layout.fillWidth: true; selectByMouse: true }
    }
    QQC2.Dialog {
        id: warning
        title: i18n("Turn on AirDrop radio?")
        modal: true
        anchors.centerIn: parent
        width: Math.min(parent.width, Kirigami.Units.gridUnit * 24)
        onOpened: cancel.forceActiveFocus()
        contentItem: ColumnLayout {
            QQC2.Label { text: i18n("Your Wi-Fi connection may disconnect while the radio is on (Intel AX200 will disconnect). Owly will attempt to restore it when turned off. This preview lacks Owly’s user-session reconnection helper: KWallet Wi-Fi may require manual recovery in network settings."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
            QQC2.CheckBox { id: suppress; text: i18n("Don’t show again") }
        }
        footer: QQC2.DialogButtonBox {
            QQC2.Button { id: cancel; text: i18n("Cancel"); QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.RejectRole }
            QQC2.Button { text: i18n("Turn on"); QQC2.DialogButtonBox.buttonRole: QQC2.DialogButtonBox.AcceptRole }
            onAccepted: { warning.close(); bridge.confirmEnable(suppress.checked) }
            onRejected: warning.close()
        }
    }
}
