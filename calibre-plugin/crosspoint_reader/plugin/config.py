from calibre.utils.config import JSONConfig
from qt.core import (
    QCheckBox,
    QDialog,
    QDialogButtonBox,
    QFormLayout,
    QHBoxLayout,
    QLineEdit,
    QPlainTextEdit,
    QPushButton,
    QSpinBox,
    QVBoxLayout,
    QWidget,
)

from .log import get_log_text


PREFS = JSONConfig('plugins/crosspoint_reader')
PREFS.defaults['host'] = '192.168.4.1'
PREFS.defaults['port'] = 81
PREFS.defaults['path'] = '/'
PREFS.defaults['chunk_size'] = 2048
PREFS.defaults['debug'] = False
PREFS.defaults['fetch_metadata'] = False


class CrossPointConfigWidget(QWidget):
    def __init__(self):
        super().__init__()
        layout = QFormLayout(self)
        self.host = QLineEdit(self)
        self.port = QSpinBox(self)
        self.port.setRange(1, 65535)
        self.path = QLineEdit(self)
        self.chunk_size = QSpinBox(self)
        self.chunk_size.setRange(512, 65536)
        self.debug = QCheckBox('Enable debug logging', self)
        self.fetch_metadata = QCheckBox('Fetch metadata (slower device list)', self)

        self.host.setText(PREFS['host'])
        self.port.setValue(PREFS['port'])
        self.path.setText(PREFS['path'])
        self.chunk_size.setValue(PREFS['chunk_size'])
        self.debug.setChecked(PREFS['debug'])
        self.fetch_metadata.setChecked(PREFS['fetch_metadata'])

        layout.addRow('Host', self.host)
        layout.addRow('Port', self.port)
        layout.addRow('Upload path', self.path)
        layout.addRow('Chunk size', self.chunk_size)
        layout.addRow('', self.debug)
        layout.addRow('', self.fetch_metadata)

        self.log_view = QPlainTextEdit(self)
        self.log_view.setReadOnly(True)
        self.log_view.setPlaceholderText('Discovery log will appear here when debug is enabled.')
        self._refresh_logs()

        refresh_btn = QPushButton('Refresh Log', self)
        refresh_btn.clicked.connect(self._refresh_logs)
        log_layout = QHBoxLayout()
        log_layout.addWidget(refresh_btn)

        layout.addRow('Log', self.log_view)
        layout.addRow('', log_layout)

    def save(self):
        PREFS['host'] = self.host.text().strip() or PREFS.defaults['host']
        PREFS['port'] = int(self.port.value())
        PREFS['path'] = self.path.text().strip() or PREFS.defaults['path']
        PREFS['chunk_size'] = int(self.chunk_size.value())
        PREFS['debug'] = bool(self.debug.isChecked())
        PREFS['fetch_metadata'] = bool(self.fetch_metadata.isChecked())

    def _refresh_logs(self):
        self.log_view.setPlainText(get_log_text())


class CrossPointConfigDialog(QDialog):
    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle('CrossPoint Reader')
        self.widget = CrossPointConfigWidget()
        layout = QVBoxLayout(self)
        layout.addWidget(self.widget)
        buttons = QDialogButtonBox(QDialogButtonBox.StandardButton.Ok |
                                   QDialogButtonBox.StandardButton.Cancel)
        buttons.accepted.connect(self.accept)
        buttons.rejected.connect(self.reject)
        layout.addWidget(buttons)
