from flask import Flask, render_template, request, redirect, url_for, session, flash, jsonify, send_from_directory
import mysql.connector
import bcrypt
import secrets
import requests as http_requests
from datetime import datetime
import os

app = Flask(__name__)
app.secret_key = secrets.token_hex(32)

DB_CONFIG = {
    'host': 'localhost',
    'user': 'scenechat',
    'password': 'XbSceneChat01!',
    'database': 'scenechat'
}

SUPERADMIN_USERNAME = 'admin'
SUPERADMIN_PASSWORD_HASH = '$2b$12$QqpCASh5Eil9rYbyHYYPN.hp6810Xrx5hrd1Sl9.YGSx6kwEnJKH2'

EMOJI_DIR     = '/opt/scenechat/emoji'
ADMIN_API_URL = 'http://127.0.0.1:8951/admin/send'

def get_db():
    return mysql.connector.connect(**DB_CONFIG)

def is_logged_in():
    return session.get('admin_logged_in', False)

def is_superadmin():
    return session.get('role') in ('superadmin', 'admin')

def is_moderator():
    return session.get('role') in ('superadmin', 'admin', 'moderator')

def require_superadmin(f):
    from functools import wraps
    @wraps(f)
    def decorated(*args, **kwargs):
        if not is_logged_in() or not is_superadmin():
            flash('Access denied')
            return redirect(url_for('dashboard'))
        return f(*args, **kwargs)
    return decorated

def require_moderator(f):
    from functools import wraps
    @wraps(f)
    def decorated(*args, **kwargs):
        if not is_logged_in() or not is_moderator():
            flash('Access denied')
            return redirect(url_for('login'))
        return f(*args, **kwargs)
    return decorated

# ---------------------------------------------------------------------------
#  Emoji static files
# ---------------------------------------------------------------------------
@app.route('/emoji/<name>.png')
def serve_emoji(name):
    return send_from_directory(EMOJI_DIR, f'{name}.png')

# ---------------------------------------------------------------------------
#  Auth
# ---------------------------------------------------------------------------
@app.route('/')
def index():
    if not is_logged_in():
        return redirect(url_for('login'))
    return redirect(url_for('dashboard'))

@app.route('/login', methods=['GET', 'POST'])
def login():
    if request.method == 'POST':
        username = request.form.get('username', '').strip()
        password = request.form.get('password', '').strip()

        if username == SUPERADMIN_USERNAME and bcrypt.checkpw(
            password.encode('utf-8'),
            SUPERADMIN_PASSWORD_HASH.encode('utf-8')
        ):
            session['admin_logged_in'] = True
            session['role'] = 'superadmin'
            session['display_name'] = 'SuperAdmin'
            return redirect(url_for('dashboard'))

        try:
            db = get_db()
            cursor = db.cursor()
            cursor.execute("""
                SELECT id, username, password_hash, role
                FROM users
                WHERE username = %s AND is_banned = 0
                AND role IN ('moderator', 'admin')
            """, (username,))
            user = cursor.fetchone()
            if user and bcrypt.checkpw(password.encode('utf-8'), user[2].encode('utf-8')):
                session['admin_logged_in'] = True
                session['role'] = user[3]
                session['display_name'] = user[1]
                session['user_id'] = user[0]
                return redirect(url_for('dashboard'))
            else:
                flash('Invalid credentials or insufficient permissions')
        except Exception as e:
            flash(f'Login error: {e}')
        finally:
            cursor.close()
            db.close()

    return render_template('login.html')

@app.route('/logout')
def logout():
    session.clear()
    return redirect(url_for('login'))

# ---------------------------------------------------------------------------
#  Dashboard
# ---------------------------------------------------------------------------
@app.route('/dashboard')
@require_moderator
def dashboard():
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT COUNT(*) FROM users")
        user_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM rooms")
        room_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM messages WHERE is_deleted = 0")
        message_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM users WHERE is_banned = 1")
        banned_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM messages WHERE is_deleted = 1")
        deleted_count = cursor.fetchone()[0]
        return render_template('dashboard.html',
            user_count=user_count,
            room_count=room_count,
            message_count=message_count,
            banned_count=banned_count,
            deleted_count=deleted_count
        )
    except Exception as e:
        flash(f'Database error: {e}')
        return render_template('dashboard.html')
    finally:
        cursor.close()
        db.close()

# ---------------------------------------------------------------------------
#  Monitor
# ---------------------------------------------------------------------------
@app.route('/monitor')
@require_moderator
def monitor():
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT id, name, type FROM rooms ORDER BY type, name")
        rooms = cursor.fetchall()
        # Pass emoji names for the picker
        emoji_names = []
        if os.path.isdir(EMOJI_DIR):
            emoji_names = sorted([
                f[:-4] for f in os.listdir(EMOJI_DIR) if f.endswith('.png')
            ])
        return render_template('monitor.html', rooms=rooms, emoji_names=emoji_names)
    except Exception as e:
        flash(f'Database error: {e}')
        return render_template('monitor.html', rooms=[], emoji_names=[])
    finally:
        cursor.close()
        db.close()

@app.route('/api/messages/<int:room_id>')
@require_moderator
def api_messages(room_id):
    since_id = request.args.get('since', 0, type=int)
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("""
            SELECT m.id, u.username, m.content, m.sent_at, m.is_deleted
            FROM messages m
            JOIN users u ON m.user_id = u.id
            WHERE m.room_id = %s AND m.id > %s
            ORDER BY m.sent_at ASC
            LIMIT 100
        """, (room_id, since_id))
        messages = []
        for row in cursor.fetchall():
            messages.append({
                'id':        row[0],
                'username':  row[1],
                'content':   row[2] if not row[4] else '[deleted]',
                'timestamp': row[3].strftime('%H:%M:%S'),
                'deleted':   bool(row[4])
            })
        return jsonify(messages)
    except Exception as e:
        return jsonify([])
    finally:
        cursor.close()
        db.close()

# ---------------------------------------------------------------------------
#  Admin send message  (proxies to server.py internal API for live broadcast)
# ---------------------------------------------------------------------------
@app.route('/api/send_message', methods=['POST'])
@require_moderator
def api_send_message():
    room_id = request.form.get('room_id', type=int)
    content = request.form.get('content', '').strip()
    if not room_id or not content:
        return jsonify({'ok': False, 'msg': 'Missing room_id or content'})
    try:
        resp = http_requests.post(
            ADMIN_API_URL,
            json={'room_id': room_id, 'content': content},
            timeout=3
        )
        return jsonify(resp.json())
    except Exception as e:
        return jsonify({'ok': False, 'msg': str(e)})

# ---------------------------------------------------------------------------
#  Message delete
# ---------------------------------------------------------------------------
@app.route('/messages/delete/<int:message_id>')
@require_moderator
def delete_message(message_id):
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("UPDATE messages SET is_deleted = 1 WHERE id = %s", (message_id,))
        db.commit()
        return jsonify({'status': 'ok'})
    except Exception as e:
        return jsonify({'status': 'error', 'message': str(e)})
    finally:
        cursor.close()
        db.close()

# ---------------------------------------------------------------------------
#  Users
# ---------------------------------------------------------------------------
@app.route('/users')
@require_moderator
def users():
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("""
            SELECT id, username, created_at, is_banned, token_expiry, role
            FROM users ORDER BY created_at DESC
        """)
        users = cursor.fetchall()
        return render_template('users.html', users=users)
    except Exception as e:
        flash(f'Database error: {e}')
        return render_template('users.html', users=[])
    finally:
        cursor.close()
        db.close()

@app.route('/users/ban/<int:user_id>')
@require_moderator
def ban_user(user_id):
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("UPDATE users SET is_banned = 1 WHERE id = %s", (user_id,))
        db.commit()
        flash('User banned')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('users'))

@app.route('/users/unban/<int:user_id>')
@require_moderator
def unban_user(user_id):
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("UPDATE users SET is_banned = 0 WHERE id = %s", (user_id,))
        db.commit()
        flash('User unbanned')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('users'))

@app.route('/users/delete/<int:user_id>')
@require_superadmin
def delete_user(user_id):
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("DELETE FROM messages WHERE user_id = %s", (user_id,))
        cursor.execute("DELETE FROM users WHERE id = %s", (user_id,))
        db.commit()
        flash('User deleted')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('users'))

@app.route('/users/role/<int:user_id>', methods=['POST'])
@require_superadmin
def set_role(user_id):
    role = request.form.get('role', 'user')
    if role not in ('user', 'moderator', 'admin'):
        flash('Invalid role')
        return redirect(url_for('users'))
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("UPDATE users SET role = %s WHERE id = %s", (role, user_id))
        db.commit()
        flash(f'Role updated to {role}')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('users'))

# ---------------------------------------------------------------------------
#  Rooms
# ---------------------------------------------------------------------------
@app.route('/rooms')
@require_superadmin
def rooms():
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT id, name, type, created_at FROM rooms ORDER BY type, name")
        rooms = cursor.fetchall()
        return render_template('rooms.html', rooms=rooms)
    except Exception as e:
        flash(f'Database error: {e}')
        return render_template('rooms.html', rooms=[])
    finally:
        cursor.close()
        db.close()

@app.route('/rooms/create', methods=['POST'])
@require_superadmin
def create_room():
    name = request.form.get('name', '').strip()
    room_type = request.form.get('type', 'text')
    if not name:
        flash('Room name required')
        return redirect(url_for('rooms'))
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("INSERT INTO rooms (name, type) VALUES (%s, %s)", (name, room_type))
        db.commit()
        flash(f'Room #{name} created')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('rooms'))

@app.route('/rooms/delete/<int:room_id>')
@require_superadmin
def delete_room(room_id):
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("DELETE FROM messages WHERE room_id = %s", (room_id,))
        cursor.execute("DELETE FROM rooms WHERE id = %s", (room_id,))
        db.commit()
        flash('Room deleted')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('rooms'))

# ---------------------------------------------------------------------------
#  Dashboard stats API (for auto-refresh)
# ---------------------------------------------------------------------------
@app.route('/api/dashboard_stats')
@require_moderator
def api_dashboard_stats():
    try:
        db = get_db()
        cursor = db.cursor()
        cursor.execute("SELECT COUNT(*) FROM users")
        user_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM rooms")
        room_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM messages WHERE is_deleted = 0")
        message_count = cursor.fetchone()[0]
        cursor.execute("SELECT COUNT(*) FROM users WHERE is_banned = 1")
        banned_count = cursor.fetchone()[0]
        return jsonify({
            'user_count':    user_count,
            'room_count':    room_count,
            'message_count': message_count,
            'banned_count':  banned_count
        })
    except Exception as e:
        return jsonify({'error': str(e)})
    finally:
        cursor.close()
        db.close()

# ---------------------------------------------------------------------------
#  Log viewer
# ---------------------------------------------------------------------------
LOG_PATH = '/opt/scenechat/logs/scenechat.log'

@app.route('/logs')
@require_moderator
def logs():
    return render_template('logs.html')

@app.route('/api/logs')
@require_moderator
def api_logs():
    lines  = request.args.get('lines', 200, type=int)
    filter_ = request.args.get('filter', '').strip().lower()
    try:
        with open(LOG_PATH, 'r') as f:
            all_lines = f.readlines()
        # Most recent first
        all_lines = all_lines[-lines:]
        if filter_:
            all_lines = [l for l in all_lines if filter_ in l.lower()]
        return jsonify({'lines': [l.rstrip() for l in reversed(all_lines)]})
    except FileNotFoundError:
        return jsonify({'lines': ['Log file not found: ' + LOG_PATH]})
    except Exception as e:
        return jsonify({'lines': [f'Error reading log: {e}']})

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8950, debug=False)