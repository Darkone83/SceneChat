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
    QScrollArea, QGridLayout, QDialog, QMenu, QInputDialog,
    QPlainTextEdit, QComboBox
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
    sig_open_dm     = Signal(int, str)    # user_id, username
    sig_mail_user   = Signal(int, str)    # user_id, username (compose to)
    sig_open_mailbox = Signal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._current_room    = None
        self._current_room_name = ''
        self._username        = ''
        self._rooms           = {}   # id -> (name, type)
        self._msg_ids         = []   # msg_id per appended message (0=unknown)
        self._pending_room    = None  # room_id being joined (for password retry)
        self._users           = {}   # user_id -> (username, room_id)
        self._dm_rooms        = {}   # room_id -> display_name
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

        self.btn_mailbox = QPushButton('Mailbox')
        self.btn_mailbox.setFixedWidth(80)
        self.btn_mailbox.setStyleSheet(f'QPushButton {{ background: none; border: 1px solid #333; color: {COL_ACCENT}; border-radius: 4px; padding: 4px 10px; font-size: 11px; }} QPushButton:hover {{ border-color: {COL_ACCENT}; }}')
        self.btn_mailbox.clicked.connect(self.sig_open_mailbox)
        tlay.addWidget(self.btn_mailbox)

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

        # DM section
        dm_lbl = QLabel('DIRECT MESSAGES')
        dm_lbl.setObjectName('lbl_section')
        slay.addWidget(dm_lbl)

        self.dm_list = QListWidget()
        self.dm_list.setStyleSheet(f'QListWidget {{ background: {COL_SURFACE}; }} QListWidget::item {{ padding: 8px 14px; font-size: 13px; }}')
        self.dm_list.setMaximumHeight(150)
        self.dm_list.itemClicked.connect(self._on_dm_clicked)
        slay.addWidget(self.dm_list)

        online_lbl = QLabel('ONLINE')
        online_lbl.setObjectName('lbl_section')
        slay.addWidget(online_lbl)

        self.user_list = QListWidget()
        self.user_list.setStyleSheet(f'QListWidget {{ background: {COL_SURFACE}; }} QListWidget::item {{ padding: 6px 14px; font-size: 12px; }}')
        self.user_list.setMaximumHeight(200)
        self.user_list.setContextMenuPolicy(Qt.CustomContextMenu)
        self.user_list.customContextMenuRequested.connect(self._on_user_menu)
        self.user_list.itemDoubleClicked.connect(self._on_user_dblclick)
        slay.addWidget(self.user_list)
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
        self._pending_room = None
        self.room_list.clear()
        for entry in rooms:
            room_id, name, rtype = entry[0], entry[1], entry[2]
            pw_flag = entry[3] if len(entry) > 3 else 0
            self._rooms[room_id] = (room_id, name, rtype, pw_flag)
            prefix = '[voice] ' if rtype == 1 else '# '
            lock   = '* ' if pw_flag else ''
            item = QListWidgetItem(lock + prefix + name)
            item.setData(Qt.UserRole, room_id)
            self.room_list.addItem(item)
        # Refresh user list now that room names are available
        self._refresh_user_list()

    def _refresh_user_list(self):
        self.user_list.clear()
        for uid, (username, room_id) in self._users.items():
            ridx = self._rooms.get(room_id)
            rname = ridx[1] if ridx else '?'
            item = QListWidgetItem(f'\u25cf {username}  #{rname}')
            item.setForeground(QColor(COL_ACCENT))
            item.setData(Qt.UserRole, uid)
            item.setData(Qt.UserRole + 1, username)
            self.user_list.addItem(item)

    def update_user_list(self, users: list):
        self._users = {uid: (username, room_id) for uid, username, room_id in users}
        self._refresh_user_list()

    def add_user(self, user_id: int, username: str, room_id: int):
        self._users[user_id] = (username, room_id)
        self._refresh_user_list()

    def remove_user(self, user_id: int):
        self._users.pop(user_id, None)
        self._refresh_user_list()

    def update_user_room(self, user_id: int, room_id: int):
        if user_id in self._users:
            username, _ = self._users[user_id]
            self._users[user_id] = (username, room_id)
            self._refresh_user_list()

    def _on_user_menu(self, pos):
        item = self.user_list.itemAt(pos)
        if not item:
            return
        uid   = item.data(Qt.UserRole)
        uname = item.data(Qt.UserRole + 1)
        if uid is None or uid == self.parent_user_id():
            return
        menu = QMenu(self)
        act_dm   = menu.addAction(f'Open DM with {uname}')
        act_mail = menu.addAction(f'Send mail to {uname}')
        chosen = menu.exec(self.user_list.mapToGlobal(pos))
        if chosen == act_dm:
            self.sig_open_dm.emit(uid, uname)
        elif chosen == act_mail:
            self.sig_mail_user.emit(uid, uname)

    def _on_user_dblclick(self, item):
        uid   = item.data(Qt.UserRole)
        uname = item.data(Qt.UserRole + 1)
        if uid is not None and uid != self.parent_user_id():
            self.sig_open_dm.emit(uid, uname)

    def parent_user_id(self):
        return getattr(self, '_my_user_id', 0)

    def set_my_user_id(self, uid):
        self._my_user_id = uid

    def _on_dm_clicked(self, item):
        room_id = item.data(Qt.UserRole)
        if room_id is not None and room_id != self._current_room:
            self._pending_room = room_id
            self.sig_join_room.emit(room_id)

    def add_dm_room(self, room_id: int, display_name: str):
        if room_id not in self._dm_rooms:
            self._dm_rooms[room_id] = display_name
            item = QListWidgetItem(f'@ {display_name}')
            item.setData(Qt.UserRole, room_id)
            self.dm_list.addItem(item)
        # Register as a room so show_history can title it
        self._rooms[room_id] = (room_id, display_name, 2, 0)

    def get_room(self, room_id: int):
        return self._rooms.get(room_id)

    def get_pending_room(self):
        return self._pending_room

    def show_history(self, room_id: int, name: str, history: list):
        self._current_room      = room_id
        self._current_room_name = name
        self.lbl_room_name.setText(f'# {name}')
        self.feed.clear()
        self._msg_ids = []
        for msg in history:
            self._append_message(msg['username'], msg['content'], msg['ts'], msg.get('msg_id', 0))

    def append_message(self, room_id: int, username: str, content: str, ts: str, msg_id: int = 0):
        if room_id != self._current_room:
            return
        self._append_message(username, content, ts, msg_id)

    def _append_message(self, username: str, content: str, ts: str, msg_id: int = 0):
        self._msg_ids.append(msg_id)
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
        self._pending_room = room_id
        if room_id != self._current_room:
            self.sig_join_room.emit(room_id)

    def delete_message(self, room_id: int, msg_id: int):
        """Find message by msg_id and replace content with [deleted]."""
        if room_id != self._current_room or msg_id == 0:
            return
        if msg_id not in self._msg_ids:
            return
        idx    = self._msg_ids.index(msg_id)
        doc    = self.feed.document()
        block  = doc.findBlockByNumber(idx)
        if not block.isValid():
            return
        cursor = QTextCursor(block)
        cursor.select(QTextCursor.LineUnderCursor)
        cursor.insertHtml(
            f'<span style="color:{COL_MUTED}; font-style:italic;">[deleted]</span><br>'
        )

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
#  Compose Mail dialog
# ---------------------------------------------------------------------------

class ComposeMailDialog(QDialog):
    """Compose a mail message. recipient may be pre-filled (from a user) or
    chosen by typing a username (offline send)."""
    sig_send = Signal(int, str, str)   # recipient_id (0=by name), recipient_name, body

    def __init__(self, recipient_id=0, recipient_name='', parent=None):
        super().__init__(parent)
        self._recipient_id = recipient_id
        self.setWindowTitle('Compose Mail')
        self.setMinimumSize(420, 320)
        self.setModal(False)

        lay = QVBoxLayout(self)
        lay.setContentsMargins(16, 16, 16, 16)
        lay.setSpacing(10)

        to_lbl = QLabel('To:')
        to_lbl.setStyleSheet(f'color: {COL_MUTED}; font-size: 12px;')
        lay.addWidget(to_lbl)

        self.inp_to = QLineEdit()
        self.inp_to.setPlaceholderText('username')
        if recipient_name:
            self.inp_to.setText(recipient_name)
            if recipient_id:
                self.inp_to.setReadOnly(True)
        lay.addWidget(self.inp_to)

        msg_lbl = QLabel('Message:')
        msg_lbl.setStyleSheet(f'color: {COL_MUTED}; font-size: 12px;')
        lay.addWidget(msg_lbl)

        self.inp_body = QPlainTextEdit()
        self.inp_body.setPlaceholderText('Type your message...')
        lay.addWidget(self.inp_body)

        btnrow = QHBoxLayout()
        btnrow.addStretch()
        btn_cancel = QPushButton('Cancel')
        btn_cancel.clicked.connect(self.reject)
        btnrow.addWidget(btn_cancel)
        btn_send = QPushButton('Send')
        btn_send.setObjectName('btn_primary')
        btn_send.clicked.connect(self._on_send)
        btnrow.addWidget(btn_send)
        lay.addLayout(btnrow)

    def _on_send(self):
        to_name = self.inp_to.text().strip()
        body    = self.inp_body.toPlainText().strip()
        if not to_name or not body:
            return
        self.sig_send.emit(self._recipient_id, to_name, body)
        self.accept()


# ---------------------------------------------------------------------------
#  Mailbox window
# ---------------------------------------------------------------------------

class MailboxDialog(QDialog):
    """Standalone mailbox window: list of mail, read pane, delete, compose."""
    sig_read    = Signal(int)        # mail_id
    sig_delete  = Signal(int)        # mail_id
    sig_compose = Signal()           # request compose dialog

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setWindowTitle('Mailbox')
        self.setMinimumSize(640, 420)
        self.setModal(False)
        self._mails = []   # list of dicts

        lay = QVBoxLayout(self)
        lay.setContentsMargins(12, 12, 12, 12)
        lay.setSpacing(8)

        # Toolbar
        tools = QHBoxLayout()
        title = QLabel('Mailbox')
        title.setStyleSheet(f'color: {COL_ACCENT}; font-size: 15px; font-weight: bold;')
        tools.addWidget(title)
        tools.addStretch()
        btn_compose = QPushButton('Compose')
        btn_compose.setObjectName('btn_primary')
        btn_compose.clicked.connect(self.sig_compose)
        tools.addWidget(btn_compose)
        lay.addLayout(tools)

        # Split: list on left, read pane on right
        split = QSplitter(Qt.Horizontal)

        self.list = QListWidget()
        self.list.setMinimumWidth(220)
        self.list.currentRowChanged.connect(self._on_row)
        split.addWidget(self.list)

        right = QWidget()
        rlay = QVBoxLayout(right)
        rlay.setContentsMargins(8, 0, 0, 0)
        self.lbl_from = QLabel('')
        self.lbl_from.setStyleSheet(f'color: {COL_PURPLE}; font-weight: bold;')
        rlay.addWidget(self.lbl_from)
        self.lbl_ts = QLabel('')
        self.lbl_ts.setStyleSheet(f'color: {COL_MUTED}; font-size: 11px;')
        rlay.addWidget(self.lbl_ts)
        self.body = QTextEdit()
        self.body.setReadOnly(True)
        rlay.addWidget(self.body)
        delrow = QHBoxLayout()
        delrow.addStretch()
        self.btn_reply = QPushButton('Reply')
        self.btn_reply.clicked.connect(self._on_reply)
        delrow.addWidget(self.btn_reply)
        self.btn_delete = QPushButton('Delete')
        self.btn_delete.clicked.connect(self._on_delete)
        delrow.addWidget(self.btn_delete)
        rlay.addLayout(delrow)
        split.addWidget(right)
        split.setStretchFactor(0, 0)
        split.setStretchFactor(1, 1)
        lay.addWidget(split)

        self.sig_reply_to = None  # set when reply pressed -- read by MainWindow

    def set_mails(self, mails: list):
        self._mails = list(mails)
        self.list.clear()
        for m in self._mails:
            item = QListWidgetItem(f"{m['sender']}  ({m['ts']})")
            self.list.addItem(item)
        if self._mails:
            self.list.setCurrentRow(0)
        else:
            self.lbl_from.setText('')
            self.lbl_ts.setText('')
            self.body.clear()

    def _on_row(self, row: int):
        if row < 0 or row >= len(self._mails):
            return
        m = self._mails[row]
        self.lbl_from.setText(f"From: {m['sender']}")
        self.lbl_ts.setText(m['ts'])
        self.body.setPlainText(m['body'])
        self.sig_read.emit(m['mail_id'])

    def _current_mail(self):
        row = self.list.currentRow()
        if 0 <= row < len(self._mails):
            return self._mails[row]
        return None

    def _on_delete(self):
        m = self._current_mail()
        if not m:
            return
        self.sig_delete.emit(m['mail_id'])
        # Remove locally
        row = self.list.currentRow()
        self._mails.pop(row)
        self.list.takeItem(row)
        if self._mails:
            self.list.setCurrentRow(min(row, len(self._mails) - 1))
        else:
            self.lbl_from.setText(''); self.lbl_ts.setText(''); self.body.clear()

    def _on_reply(self):
        m = self._current_mail()
        if not m:
            return
        self.sig_reply_to = m['sender']
        self.sig_compose.emit()


# ---------------------------------------------------------------------------
#  Main window
# ---------------------------------------------------------------------------

class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle('SceneChat v1.3')
        self.setMinimumSize(900, 600)
        self.resize(1100, 700)

        self._worker    = None
        self._my_user_id = 0
        self._do_register = False
        self._username  = ''
        self._server    = ''
        self._mailbox_win = None
        self._mails       = []

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
        self._chat_widget.sig_open_dm.connect(self._on_open_dm)
        self._chat_widget.sig_mail_user.connect(self._on_mail_user)
        self._chat_widget.sig_open_mailbox.connect(self._on_open_mailbox)

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
        self._worker.sig_msg_delete.connect(self._on_msg_delete)
        self._worker.sig_join_fail.connect(self._on_join_fail)
        self._worker.sig_user_list.connect(self._on_user_list)
        self._worker.sig_user_join.connect(self._on_user_join)
        self._worker.sig_user_leave.connect(self._on_user_leave)
        self._worker.sig_dm_room.connect(self._on_dm_room)
        self._worker.sig_mail_list.connect(self._on_mail_list)
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
        self._my_user_id = user_id
        # Registration success -- user_id=0, username='' 
        if user_id == 0 and not username:
            self._login_widget.set_busy(False)
            self._login_widget.set_status('Registered! Please log in.', error=False)
            self.status_bar.showMessage('Registered — please log in')
            return
        display = username if username else self._username
        self._chat_widget.set_user(display)
        self._chat_widget.set_my_user_id(user_id)
        # Fresh login -- clear any leftover DM/mail state
        self._mails = []
        self._chat_widget._dm_rooms = {}
        self._chat_widget.dm_list.clear()
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
        # No auto-join -- user selects from room list

    @Slot(int, str, list)
    def _on_room_joined(self, room_id: int, name: str, history: list):
        self._chat_widget.show_history(room_id, name, history)
        # Update own room in presence list
        if self._my_user_id:
            self._chat_widget.update_user_room(self._my_user_id, room_id)
        self.status_bar.showMessage(f'#{name}')

    @Slot(int, str, str, str, int)
    def _on_message(self, room_id: int, username: str, content: str, ts: str, msg_id: int):
        self._chat_widget.append_message(room_id, username, content, ts, msg_id)

    @Slot(int, int)
    def _on_msg_delete(self, room_id: int, msg_id: int):
        self._chat_widget.delete_message(room_id, msg_id)

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
            # Check if room is password protected
            room = self._chat_widget.get_room(room_id)
            if room and room[3]:  # password_flag
                from PySide6.QtWidgets import QInputDialog, QLineEdit
                pw, ok = QInputDialog.getText(
                    self, "Password Required",
                    f"Enter password for #{room[1]}:",
                    QLineEdit.Password
                )
                if ok:
                    self._worker.send_join_room(room_id, pw)
            else:
                self._worker.send_join_room(room_id)

    @Slot(str)
    def _on_join_fail(self, reason: str):
        from PySide6.QtWidgets import QInputDialog, QLineEdit, QMessageBox
        if reason == "Wrong password":
            # Get current room and retry with new password
            room_id = self._chat_widget.get_pending_room()
            if room_id is not None:
                room = self._chat_widget.get_room(room_id)
                pw, ok = QInputDialog.getText(
                    self, "Wrong Password",
                    f"Wrong password for #{room[1] if room else room_id}. Try again:",
                    QLineEdit.Password
                )
                if ok and self._worker:
                    self._worker.send_join_room(room_id, pw)
        else:
            self.status_bar.showMessage(f'Join failed: {reason}')

    @Slot(list)
    def _on_user_list(self, users: list):
        self._chat_widget.update_user_list(users)
        self.status_bar.showMessage(f'{len(users)} user(s) online')

    @Slot(int, str, int)
    def _on_user_join(self, user_id: int, username: str, room_id: int):
        self._chat_widget.add_user(user_id, username, room_id)

    @Slot(int, str)
    def _on_open_dm(self, user_id: int, username: str):
        if self._worker:
            self._worker.send_dm_open(user_id)
            self.status_bar.showMessage(f'Opening DM with {username}...')

    def _on_dm_room(self, room_id: int, display_name: str):
        # display_name comes as "DM:username" from server -- strip prefix
        name = display_name[3:] if display_name.startswith('DM:') else display_name
        self._chat_widget.add_dm_room(room_id, name)

    def _on_mail_user(self, user_id: int, username: str):
        dlg = ComposeMailDialog(recipient_id=user_id, recipient_name=username, parent=self)
        dlg.sig_send.connect(self._do_send_mail)
        dlg.show()

    def _on_open_mailbox(self):
        if self._mailbox_win is None:
            self._mailbox_win = MailboxDialog(self)
            self._mailbox_win.sig_read.connect(self._on_mail_read)
            self._mailbox_win.sig_delete.connect(self._on_mail_delete)
            self._mailbox_win.sig_compose.connect(self._on_compose_from_mailbox)
        self._mailbox_win.set_mails(self._mails)
        self._mailbox_win.show()
        self._mailbox_win.raise_()
        self._mailbox_win.activateWindow()

    def _on_compose_from_mailbox(self):
        reply_to = ''
        if self._mailbox_win and self._mailbox_win.sig_reply_to:
            reply_to = self._mailbox_win.sig_reply_to
            self._mailbox_win.sig_reply_to = None
        dlg = ComposeMailDialog(recipient_id=0, recipient_name=reply_to, parent=self)
        dlg.sig_send.connect(self._do_send_mail)
        dlg.show()

    def _do_send_mail(self, recipient_id: int, recipient_name: str, body: str):
        if not self._worker:
            return
        if recipient_id != 0:
            self._worker.send_mail(recipient_id, body)
        else:
            # send by name -- encode TO:name\nbody for the server to resolve
            self._worker.send_mail(0, f'TO:{recipient_name}\n{body}')
        self.status_bar.showMessage(f'Mail sent to {recipient_name}')

    def _on_mail_list(self, mails: list):
        # Accumulate mail and refresh the mailbox window if open
        self._mails.extend(mails)
        if self._mails:
            self.status_bar.showMessage(f'{len(self._mails)} message(s) in mailbox')
        if self._mailbox_win and self._mailbox_win.isVisible():
            self._mailbox_win.set_mails(self._mails)

    def _on_mail_read(self, mail_id: int):
        if self._worker:
            self._worker.send_mail_read(mail_id)

    def _on_mail_delete(self, mail_id: int):
        if self._worker:
            self._worker.send_mail_delete(mail_id)
        self._mails = [m for m in self._mails if m['mail_id'] != mail_id]

    def _on_user_leave(self, user_id: int, username: str):
        self._chat_widget.remove_user(user_id)

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
        # Reset DM/mail state so a different account doesn't see stale data
        self._mails = []
        if self._mailbox_win:
            self._mailbox_win.close()
            self._mailbox_win = None
        self._chat_widget._dm_rooms = {}
        self._chat_widget.dm_list.clear()
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