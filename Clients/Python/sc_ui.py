"""
sc_ui.py -- SceneChat PySide6 client UI.
Dark theme matching the Xbox client and admin panel aesthetic.
"""

import sys
import os
import re
import urllib.request
from pathlib import Path
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QVBoxLayout, QHBoxLayout,
    QLabel, QLineEdit, QPushButton, QListWidget, QListWidgetItem,
    QTextEdit, QSplitter, QFrame, QStatusBar, QMessageBox,
    QScrollArea, QGridLayout, QDialog
)
from PySide6.QtCore import Qt, QSize, QTimer, Signal, Slot
from PySide6.QtGui import (
    QFont, QColor, QPalette, QTextCursor, QPixmap, QIcon,
    QTextCharFormat, QTextDocument
)

from sc_net import ChatWorker, load_creds, save_creds, clear_creds

# ---------------------------------------------------------------------------
#  Constants
# ---------------------------------------------------------------------------
COL_BG       = '#0a0a0a'
COL_SURFACE  = '#111111'
COL_BORDER   = '#222222'
COL_ACCENT   = '#39ff14'
COL_PURPLE   = '#8b5cf6'
COL_TEXT     = '#e0e0e0'
COL_MUTED    = '#555555'
COL_RED      = '#dc2626'
COL_ADMIN    = '#39ff14'

EMOJI_NAMES = [
    'smile','wink','laugh','cry','angry','sad','surprised','thinking',
    'cool','love_face','dead','party','question','thumbs_up','thumbs_down',
    'check','x','alert','heart','fireheart','star','skull','fire',
    'scenechat_sc','scenechat_softmod','scenechat_fire','scenechat_modchip',
    'scenechat_controller_chat','scenechat_ping','scenechat_lobby',
    'scenechat_dpad','scenechat_buttons','scenechat_devbuild',
]

EMOJI_CACHE = Path(__file__).parent / 'emoji_cache'

# ---------------------------------------------------------------------------
#  Stylesheet
# ---------------------------------------------------------------------------
APP_STYLE = f"""
QMainWindow, QWidget {{
    background: {COL_BG};
    color: {COL_TEXT};
    font-family: 'Segoe UI', 'SF Pro Display', 'Ubuntu', sans-serif;
    font-size: 13px;
}}
QLineEdit {{
    background: {COL_SURFACE};
    border: 1px solid {COL_BORDER};
    border-radius: 4px;
    color: {COL_TEXT};
    padding: 8px 12px;
    font-size: 13px;
}}
QLineEdit:focus {{
    border-color: {COL_ACCENT};
}}
QPushButton {{
    background: {COL_SURFACE};
    border: 1px solid {COL_BORDER};
    border-radius: 4px;
    color: {COL_TEXT};
    padding: 8px 16px;
    font-size: 13px;
}}
QPushButton:hover {{
    border-color: {COL_ACCENT};
    color: {COL_ACCENT};
}}
QPushButton#btn_primary {{
    background: {COL_ACCENT};
    color: #000;
    font-weight: bold;
    border: none;
}}
QPushButton#btn_primary:hover {{
    background: #44ff22;
    color: #000;
}}
QPushButton#btn_secondary {{
    background: {COL_PURPLE};
    color: #fff;
    border: none;
}}
QPushButton#btn_secondary:hover {{
    background: #9b6cff;
    color: #fff;
}}
QListWidget {{
    background: {COL_SURFACE};
    border: none;
    color: {COL_TEXT};
    outline: none;
}}
QListWidget::item {{
    padding: 10px 14px;
    border-bottom: 1px solid {COL_BG};
}}
QListWidget::item:hover {{
    background: #1a1a1a;
}}
QListWidget::item:selected {{
    background: #1a1a1a;
    color: {COL_ACCENT};
    border-left: 2px solid {COL_ACCENT};
}}
QTextEdit {{
    background: {COL_BG};
    border: none;
    color: {COL_TEXT};
    selection-background-color: #333;
    font-family: 'Consolas', 'Monaco', 'Courier New', monospace;
    font-size: 13px;
}}
QScrollBar:vertical {{
    background: {COL_BG};
    width: 6px;
    border-radius: 3px;
}}
QScrollBar::handle:vertical {{
    background: #333;
    border-radius: 3px;
}}
QScrollBar::handle:vertical:hover {{
    background: #555;
}}
QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical {{
    height: 0;
}}
QStatusBar {{
    background: {COL_SURFACE};
    border-top: 1px solid {COL_BORDER};
    color: {COL_MUTED};
    font-size: 11px;
}}
QSplitter::handle {{
    background: {COL_BORDER};
    width: 1px;
}}
QLabel#lbl_title {{
    color: {COL_ACCENT};
    font-size: 22px;
    font-weight: bold;
    letter-spacing: 2px;
}}
QLabel#lbl_sub {{
    color: {COL_PURPLE};
    font-size: 11px;
}}
QLabel#lbl_section {{
    color: {COL_MUTED};
    font-size: 10px;
    text-transform: uppercase;
    letter-spacing: 1px;
    padding: 8px 14px 4px 14px;
}}
QFrame#sidebar {{
    background: {COL_SURFACE};
    border-right: 1px solid {COL_BORDER};
}}
QFrame#topbar {{
    background: {COL_SURFACE};
    border-bottom: 1px solid {COL_BORDER};
}}
QDialog {{
    background: {COL_BG};
}}
"""

# ---------------------------------------------------------------------------
#  Emoji helpers
# ---------------------------------------------------------------------------

def _emoji_path(name: str) -> Path:
    return EMOJI_CACHE / f'{name}.png'


def _ensure_emoji_cache(host: str, port: int = 8950):
    """Download emoji PNGs from the admin panel if not already cached."""
    EMOJI_CACHE.mkdir(exist_ok=True)
    for name in EMOJI_NAMES:
        p = _emoji_path(name)
        if not p.exists():
            try:
                url = f'http://{host}:{port}/emoji/{name}.png'
                urllib.request.urlretrieve(url, p)
            except Exception:
                pass  # silently skip -- render as text token if missing


def _render_content_html(text: str) -> str:
    """Replace :token: with inline <img> HTML for QTextEdit rich text."""
    def replace_token(m):
        name = m.group(1)
        p = _emoji_path(name)
        if p.exists():
            return f'<img src="{p.as_posix()}" width="18" height="18" style="vertical-align:middle;">'
        return m.group(0)
    escaped = (text
               .replace('&', '&amp;')
               .replace('<', '&lt;')
               .replace('>', '&gt;'))
    return re.sub(r':([a-zA-Z0-9_]+):', replace_token, escaped)

# ---------------------------------------------------------------------------
#  Emoji picker dialog
# ---------------------------------------------------------------------------

class EmojiPickerDialog(QDialog):
    emoji_selected = Signal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Emoji')
        self.setFixedSize(320, 200)
        layout = QGridLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(8, 8, 8, 8)

        col = 0
        row = 0
        for name in EMOJI_NAMES:
            btn = QPushButton()
            btn.setFixedSize(32, 32)
            btn.setToolTip(f':{name}:')
            btn.setStyleSheet(
                'QPushButton { background: #1a1a1a; border: 1px solid #222; border-radius: 4px; padding: 2px; }'
                'QPushButton:hover { border-color: #39ff14; }'
            )
            p = _emoji_path(name)
            if p.exists():
                btn.setIcon(QIcon(str(p)))
                btn.setIconSize(QSize(22, 22))
            else:
                btn.setText(':)')
                btn.setFont(QFont('Segoe UI', 8))

            token = f':{name}:'
            btn.clicked.connect(lambda checked, t=token: self._pick(t))
            layout.addWidget(btn, row, col)
            col += 1
            if col >= 10:
                col = 0
                row += 1

    def _pick(self, token: str):
        self.emoji_selected.emit(token)
        self.accept()

# ---------------------------------------------------------------------------
#  Login widget
# ---------------------------------------------------------------------------

class LoginWidget(QWidget):
    sig_login    = Signal(str, str, str)   # server, username, password
    sig_register = Signal(str, str, str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._build_ui()

    def _build_ui(self):
        outer = QVBoxLayout(self)
        outer.setAlignment(Qt.AlignCenter)

        card = QFrame()
        card.setFixedWidth(380)
        card.setStyleSheet(f'QFrame {{ background: {COL_SURFACE}; border: 1px solid {COL_BORDER}; border-radius: 8px; }}')

        vlay = QVBoxLayout(card)
        vlay.setContentsMargins(32, 32, 32, 32)
        vlay.setSpacing(0)

        # Title
        title = QLabel('SCENECHAT')
        title.setObjectName('lbl_title')
        title.setAlignment(Qt.AlignCenter)
        sub = QLabel('Team Resurgent / Darkone83')
        sub.setObjectName('lbl_sub')
        sub.setAlignment(Qt.AlignCenter)
        vlay.addWidget(title)
        vlay.addWidget(sub)
        vlay.addSpacing(28)

        # Server
        vlay.addWidget(self._label('SERVER'))
        self.inp_server = QLineEdit()
        self.inp_server.setPlaceholderText('hostname or IP')
        vlay.addWidget(self.inp_server)
        vlay.addSpacing(12)

        # Username
        vlay.addWidget(self._label('USERNAME'))
        self.inp_user = QLineEdit()
        vlay.addWidget(self.inp_user)
        vlay.addSpacing(12)

        # Password
        vlay.addWidget(self._label('PASSWORD'))
        self.inp_pass = QLineEdit()
        self.inp_pass.setEchoMode(QLineEdit.Password)
        self.inp_pass.returnPressed.connect(self._on_login)
        vlay.addWidget(self.inp_pass)
        vlay.addSpacing(20)

        # Buttons
        btn_row = QHBoxLayout()
        self.btn_login = QPushButton('LOGIN')
        self.btn_login.setObjectName('btn_primary')
        self.btn_login.clicked.connect(self._on_login)
        self.btn_reg = QPushButton('REGISTER')
        self.btn_reg.setObjectName('btn_secondary')
        self.btn_reg.clicked.connect(self._on_register)
        btn_row.addWidget(self.btn_login)
        btn_row.addWidget(self.btn_reg)
        vlay.addLayout(btn_row)

        self.lbl_status = QLabel('')
        self.lbl_status.setAlignment(Qt.AlignCenter)
        self.lbl_status.setStyleSheet(f'color: {COL_RED}; font-size: 12px; padding-top: 8px;')
        vlay.addWidget(self.lbl_status)

        outer.addWidget(card)

    def _label(self, text):
        l = QLabel(text)
        l.setStyleSheet(f'color: {COL_MUTED}; font-size: 10px; letter-spacing: 1px; margin-bottom: 4px;')
        return l

    def prefill(self, creds: dict):
        self.inp_server.setText(creds.get('server', ''))
        self.inp_user.setText(creds.get('username', ''))
        self.inp_pass.setText(creds.get('password', ''))

    def set_status(self, msg: str, error: bool = True):
        color = COL_RED if error else COL_ACCENT
        self.lbl_status.setStyleSheet(f'color: {color}; font-size: 12px; padding-top: 8px;')
        self.lbl_status.setText(msg)

    def set_busy(self, busy: bool):
        self.btn_login.setEnabled(not busy)
        self.btn_reg.setEnabled(not busy)
        if busy:
            self.lbl_status.setStyleSheet(f'color: {COL_MUTED}; font-size: 12px; padding-top: 8px;')
            self.lbl_status.setText('Connecting...')

    def _on_login(self):
        self.sig_login.emit(
            self.inp_server.text().strip(),
            self.inp_user.text().strip(),
            self.inp_pass.text()
        )

    def _on_register(self):
        self.sig_register.emit(
            self.inp_server.text().strip(),
            self.inp_user.text().strip(),
            self.inp_pass.text()
        )

# ---------------------------------------------------------------------------
#  Chat widget
# ---------------------------------------------------------------------------

class ChatWidget(QWidget):
    sig_join_room   = Signal(int)
    sig_send        = Signal(int, str)
    sig_logout      = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_room    = None
        self._current_room_name = ''
        self._username        = ''
        self._rooms           = {}   # id -> (name, type)
        self._build_ui()

    def _build_ui(self):
        root = QVBoxLayout(self)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        # ── Top bar ──────────────────────────────────────────────────────────
        topbar = QFrame()
        topbar.setObjectName('topbar')
        topbar.setFixedHeight(52)
        tlay = QHBoxLayout(topbar)
        tlay.setContentsMargins(16, 0, 16, 0)

        self.lbl_room_name = QLabel('SceneChat')
        self.lbl_room_name.setStyleSheet(f'color: {COL_ACCENT}; font-size: 15px; font-weight: bold;')
        tlay.addWidget(self.lbl_room_name)
        tlay.addStretch()

        self.lbl_user = QLabel('')
        self.lbl_user.setStyleSheet(f'color: {COL_PURPLE}; font-size: 12px;')
        tlay.addWidget(self.lbl_user)

        btn_logout = QPushButton('Logout')
        btn_logout.setFixedWidth(70)
        btn_logout.setStyleSheet(f'QPushButton {{ background: none; border: 1px solid #333; color: {COL_MUTED}; border-radius: 4px; padding: 4px 10px; font-size: 11px; }} QPushButton:hover {{ border-color: {COL_RED}; color: {COL_RED}; }}')
        btn_logout.clicked.connect(self.sig_logout)
        tlay.addWidget(btn_logout)
        root.addWidget(topbar)

        # ── Main splitter ────────────────────────────────────────────────────
        splitter = QSplitter(Qt.Horizontal)
        splitter.setHandleWidth(1)

        # Sidebar
        sidebar = QFrame()
        sidebar.setObjectName('sidebar')
        sidebar.setFixedWidth(200)
        slay = QVBoxLayout(sidebar)
        slay.setContentsMargins(0, 0, 0, 0)
        slay.setSpacing(0)

        rooms_lbl = QLabel('ROOMS')
        rooms_lbl.setObjectName('lbl_section')
        slay.addWidget(rooms_lbl)

        self.room_list = QListWidget()
        self.room_list.setStyleSheet(f'QListWidget {{ background: {COL_SURFACE}; }} QListWidget::item {{ padding: 10px 14px; font-size: 13px; }}')
        self.room_list.itemClicked.connect(self._on_room_clicked)
        slay.addWidget(self.room_list)
        splitter.addWidget(sidebar)

        # Message area
        msg_area = QWidget()
        mlay = QVBoxLayout(msg_area)
        mlay.setContentsMargins(0, 0, 0, 0)
        mlay.setSpacing(0)

        self.feed = QTextEdit()
        self.feed.setReadOnly(True)
        self.feed.setAcceptRichText(True)
        mlay.addWidget(self.feed)

        # Input bar
        inputbar = QFrame()
        inputbar.setFixedHeight(54)
        inputbar.setStyleSheet(f'QFrame {{ background: {COL_SURFACE}; border-top: 1px solid {COL_BORDER}; }}')
        ilay = QHBoxLayout(inputbar)
        ilay.setContentsMargins(12, 8, 12, 8)
        ilay.setSpacing(8)

        self.inp_msg = QLineEdit()
        self.inp_msg.setPlaceholderText('Message...')
        self.inp_msg.returnPressed.connect(self._on_send)
        ilay.addWidget(self.inp_msg)

        self.btn_emoji = QPushButton('😊')
        self.btn_emoji.setFixedSize(36, 36)
        self.btn_emoji.setStyleSheet('QPushButton { background: #1a1a1a; border: 1px solid #333; border-radius: 4px; font-size: 16px; } QPushButton:hover { border-color: #39ff14; }')
        self.btn_emoji.clicked.connect(self._on_emoji)
        ilay.addWidget(self.btn_emoji)

        btn_send = QPushButton('Send')
        btn_send.setObjectName('btn_primary')
        btn_send.setFixedWidth(70)
        btn_send.clicked.connect(self._on_send)
        ilay.addWidget(btn_send)
        mlay.addWidget(inputbar)

        splitter.addWidget(msg_area)
        splitter.setStretchFactor(0, 0)
        splitter.setStretchFactor(1, 1)
        root.addWidget(splitter)

    def set_user(self, username: str):
        self._username = username
        self.lbl_user.setText(username)

    def set_rooms(self, rooms: list):
        self._rooms = {}
        self.room_list.clear()
        for room_id, name, rtype in rooms:
            self._rooms[room_id] = (name, rtype)
            prefix = '🔊 ' if rtype == 1 else '# '
            item = QListWidgetItem(prefix + name)
            item.setData(Qt.UserRole, room_id)
            self.room_list.addItem(item)

    def show_history(self, room_id: int, name: str, history: list):
        self._current_room      = room_id
        self._current_room_name = name
        self.lbl_room_name.setText(f'# {name}')
        self.feed.clear()
        for msg in history:
            self._append_message(msg['username'], msg['content'], msg['ts'])

    def append_message(self, room_id: int, username: str, content: str, ts: str):
        if room_id != self._current_room:
            return
        self._append_message(username, content, ts)

    def _append_message(self, username: str, content: str, ts: str):
        cursor = self.feed.textCursor()
        cursor.movePosition(QTextCursor.End)

        is_admin = username == '[Admin]'
        ucolor   = COL_ADMIN if is_admin else COL_PURPLE

        html = (
            f'<span style="color:{COL_MUTED}; font-size:11px;">{ts}</span>&nbsp;'
            f'<span style="color:{ucolor}; font-weight:bold;">{username}</span>&nbsp;&nbsp;'
            f'<span style="color:{COL_TEXT};">{_render_content_html(content)}</span><br>'
        )
        cursor.insertHtml(html)
        self.feed.verticalScrollBar().setValue(
            self.feed.verticalScrollBar().maximum()
        )

    def _on_room_clicked(self, item):
        room_id = item.data(Qt.UserRole)
        if room_id != self._current_room:
            self.sig_join_room.emit(room_id)

    def _on_send(self):
        if self._current_room is None:
            return
        text = self.inp_msg.text().strip()
        if not text:
            return
        self.inp_msg.clear()
        self.sig_send.emit(self._current_room, text)

    def _on_emoji(self):
        dlg = EmojiPickerDialog(self)
        dlg.emoji_selected.connect(self._insert_emoji)
        dlg.exec()

    def _insert_emoji(self, token: str):
        self.inp_msg.setText(self.inp_msg.text() + token)
        self.inp_msg.setFocus()

# ---------------------------------------------------------------------------
#  Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('SceneChat')
        self.setMinimumSize(900, 600)
        self.resize(1100, 700)

        self._worker    = None
        self._do_register = False
        self._username  = ''
        self._server    = ''

        self._login_widget = LoginWidget()
        self._chat_widget  = ChatWidget()

        self._stack = QWidget()
        self._stack_lay = QVBoxLayout(self._stack)
        self._stack_lay.setContentsMargins(0, 0, 0, 0)
        self.setCentralWidget(self._stack)

        self._show_login()

        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        self.status_bar.showMessage('Not connected')

        # Wire login signals
        self._login_widget.sig_login.connect(self._on_login)
        self._login_widget.sig_register.connect(self._on_register)

        # Wire chat signals
        self._chat_widget.sig_join_room.connect(self._on_join_room)
        self._chat_widget.sig_send.connect(self._on_send_message)
        self._chat_widget.sig_logout.connect(self._on_logout)

        # Prefill saved creds
        creds = load_creds()
        if creds:
            self._login_widget.prefill(creds)

    # ── View switching ────────────────────────────────────────────────────────

    def _show_login(self):
        self._clear_stack()
        self._stack_lay.addWidget(self._login_widget)
        self._login_widget.show()

    def _show_chat(self):
        self._clear_stack()
        self._stack_lay.addWidget(self._chat_widget)
        self._chat_widget.show()

    def _clear_stack(self):
        for i in reversed(range(self._stack_lay.count())):
            w = self._stack_lay.itemAt(i).widget()
            if w:
                w.hide()
                self._stack_lay.removeWidget(w)

    # ── Login / register ──────────────────────────────────────────────────────

    def _on_login(self, server: str, username: str, password: str):
        if not server or not username or not password:
            self._login_widget.set_status('All fields required')
            return
        self._do_register = False
        self._username    = username
        self._password    = password
        self._server      = server
        self._start_worker(server, username, password)

    def _on_register(self, server: str, username: str, password: str):
        if not server or not username or not password:
            self._login_widget.set_status('All fields required')
            return
        if len(password) < 8:
            self._login_widget.set_status('Password must be at least 8 characters')
            return
        self._do_register = True
        self._username    = username
        self._password    = password
        self._server      = server
        self._start_worker(server, username, password)

    def _start_worker(self, server: str, username: str, password: str):
        self._login_widget.set_busy(True)

        if self._worker:
            self._worker.send_disconnect()
            self._worker.quit()
            self._worker.wait()

        self._worker = ChatWorker(self)
        self._worker.configure(server)
        self._worker.sig_connected.connect(lambda: self._on_worker_connected(username, password))
        self._worker.sig_auth_ok.connect(self._on_auth_ok)
        self._worker.sig_auth_fail.connect(self._on_auth_fail)
        self._worker.sig_room_list.connect(self._on_room_list)
        self._worker.sig_room_joined.connect(self._on_room_joined)
        self._worker.sig_message.connect(self._on_message)
        self._worker.sig_error.connect(self._on_error)
        self._worker.sig_disconnected.connect(self._on_disconnected)
        self._worker.start()
        self.status_bar.showMessage(f'Connecting to {server}...')

    @Slot()
    def _on_worker_connected(self, username: str, password: str):
        self.status_bar.showMessage('Connected — authenticating...')
        if self._do_register:
            self._worker.send_register(username, password)
        else:
            self._worker.send_login(username, password)

    @Slot(int, str, str)
    def _on_auth_ok(self, user_id: int, username: str, token: str):
        # Registration success -- user_id=0, username='' 
        if user_id == 0 and not username:
            self._login_widget.set_busy(False)
            self._login_widget.set_status('Registered! Please log in.', error=False)
            self.status_bar.showMessage('Registered — please log in')
            return
        display = username if username else self._username
        self._chat_widget.set_user(display)
        save_creds(self._server, self._username, self._password)
        EMOJI_CACHE.mkdir(exist_ok=True)
        QTimer.singleShot(500, lambda: _ensure_emoji_cache(self._server))
        self._show_chat()
        self.status_bar.showMessage(f'Logged in as {display}')

    @Slot(str)
    def _on_auth_fail(self, msg: str):
        self._login_widget.set_busy(False)
        self._login_widget.set_status(msg)
        self.status_bar.showMessage('Authentication failed')

    @Slot(list)
    def _on_room_list(self, rooms: list):
        self._chat_widget.set_rooms(rooms)
        # Auto-join first text room
        for room_id, name, rtype in rooms:
            if rtype == 0:
                self._worker.send_join_room(room_id)
                break

    @Slot(int, str, list)
    def _on_room_joined(self, room_id: int, name: str, history: list):
        self._chat_widget.show_history(room_id, name, history)
        self.status_bar.showMessage(f'#{name}')

    @Slot(int, str, str, str)
    def _on_message(self, room_id: int, username: str, content: str, ts: str):
        self._chat_widget.append_message(room_id, username, content, ts)

    @Slot(str)
    def _on_error(self, msg: str):
        self.status_bar.showMessage(f'Error: {msg}')

    @Slot(str)
    def _on_disconnected(self, reason: str):
        self._login_widget.set_busy(False)
        self._login_widget.set_status(reason if reason else 'Disconnected', error=True)
        self._show_login()
        self.status_bar.showMessage('Disconnected')

    def _on_join_room(self, room_id: int):
        if self._worker:
            self._worker.send_join_room(room_id)

    def _on_send_message(self, room_id: int, content: str):
        if self._worker:
            self._worker.send_message(room_id, content)

    def _on_logout(self):
        clear_creds()
        if self._worker:
            self._worker.send_disconnect()
            self._worker.quit()
            self._worker.wait()
            self._worker = None
        self._login_widget.set_busy(False)
        self._login_widget.set_status('')
        self._show_login()
        self.status_bar.showMessage('Logged out')

    def closeEvent(self, event):
        if self._worker:
            self._worker.send_disconnect()
            self._worker.quit()
            self._worker.wait()
        event.accept()

# ---------------------------------------------------------------------------
#  Entry point
# ---------------------------------------------------------------------------

def main():
    app = QApplication(sys.argv)
    app.setStyleSheet(APP_STYLE)
    app.setApplicationName('SceneChat')

    # Set dark palette for any widgets not covered by stylesheet
    palette = QPalette()
    palette.setColor(QPalette.Window,          QColor(COL_BG))
    palette.setColor(QPalette.WindowText,      QColor(COL_TEXT))
    palette.setColor(QPalette.Base,            QColor(COL_SURFACE))
    palette.setColor(QPalette.AlternateBase,   QColor('#0f0f0f'))
    palette.setColor(QPalette.Text,            QColor(COL_TEXT))
    palette.setColor(QPalette.Button,          QColor(COL_SURFACE))
    palette.setColor(QPalette.ButtonText,      QColor(COL_TEXT))
    palette.setColor(QPalette.Highlight,       QColor('#333333'))
    palette.setColor(QPalette.HighlightedText, QColor(COL_ACCENT))
    app.setPalette(palette)

    win = MainWindow()
    win.show()
    sys.exit(app.exec())