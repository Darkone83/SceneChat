from flask import Flask, render_template, request, redirect, url_for, session, flash, jsonify, send_from_directory
import mysql.connector
import bcrypt
import secrets
import requests as http_requests
from datetime import datetime
import os
import subprocess
import tempfile
from flask import Response

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
    """Get a DB connection with auto-reconnect on stale connection."""
    for attempt in range(3):
        try:
            conn = mysql.connector.connect(**DB_CONFIG)
            conn.ping(reconnect=True, attempts=3, delay=1)
            return conn
        except mysql.connector.Error as e:
            app.logger.warning(f"DB connect attempt {attempt+1} failed: {e}")
            if attempt == 2:
                raise
            import time; time.sleep(1)

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

def require_admin(f):
    from functools import wraps
    @wraps(f)
    def decorated(*args, **kwargs):
        if not is_logged_in() or session.get('role') not in ('admin', 'superadmin'):
            flash('Access denied')
            return redirect(url_for('dashboard'))
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
        # Get room_id before marking deleted so we can broadcast
        cursor.execute("SELECT room_id FROM messages WHERE id = %s", (message_id,))
        row = cursor.fetchone()
        if not row:
            return jsonify({'status': 'error', 'message': 'Message not found'})
        room_id = row[0]
        cursor.execute("UPDATE messages SET is_deleted = 1 WHERE id = %s", (message_id,))
        db.commit()
        # Notify connected clients via internal bridge
        try:
            import urllib.request, json as _json
            req_body = _json.dumps({'room_id': room_id, 'msg_id': message_id}).encode()
            req = urllib.request.Request(
                'http://127.0.0.1:8951/admin/delete',
                data=req_body,
                headers={'Content-Type': 'application/json'},
                method='POST'
            )
            urllib.request.urlopen(req, timeout=2)
        except Exception as be:
            app.logger.error(f"Bridge notify failed: {be}")
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
            SELECT id, username, created_at, is_banned, token_expiry, role,
                   last_seen, last_room
            FROM users ORDER BY created_at DESC
        """)
        users = cursor.fetchall()
        return render_template('users.html', users=users)
    except Exception as e:
        import traceback
        flash(f'Database error: {e} -- {traceback.format_exc()}')
        return render_template('users.html', users=[])
    finally:
        try: cursor.close()
        except: pass
        try: db.close()
        except: pass

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
        cursor.execute("SELECT id, name, type, created_at, password_hash, access_level FROM rooms ORDER BY type, name")
        rooms = cursor.fetchall()
        return render_template('rooms.html', rooms=rooms)
    except Exception as e:
        flash(f'Database error: {e}')
        return render_template('rooms.html', rooms=[])
    finally:
        cursor.close()
        db.close()

@app.route('/rooms/create', methods=['POST'])
@require_moderator
def create_room():
    name        = request.form.get('name', '').strip()
    room_type   = request.form.get('type', 'text')
    password    = request.form.get('password', '').strip()
    access_level = request.form.get('access_level', 'public')
    caller_role = session.get('role', 'user')

    # Mods can only create public rooms
    if caller_role == 'moderator' and access_level != 'public':
        access_level = 'public'

    if not name:
        flash('Room name required')
        return redirect(url_for('rooms'))
    try:
        db = get_db()
        cursor = db.cursor()
        password_hash = None
        if password:
            password_hash = bcrypt.hashpw(password.encode(), bcrypt.gensalt(12)).decode()
        cursor.execute(
            "INSERT INTO rooms (name, type, password_hash, access_level) VALUES (%s, %s, %s, %s)",
            (name, room_type, password_hash, access_level)
        )
        db.commit()
        flash(f'Room #{name} created')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('rooms'))

@app.route('/rooms/set_acl/<int:room_id>', methods=['POST'])
@require_moderator
def set_room_acl(room_id):
    access_level = request.form.get('access_level', 'public')
    caller_role  = session.get('role', 'user')
    # Mods can only toggle between public and moderator
    if caller_role == 'moderator' and access_level == 'admin':
        flash('Moderators cannot set admin-only access')
        return redirect(url_for('rooms'))
    try:
        db = get_db()
        cursor = db.cursor()
        # Mods cannot change admin-level rooms
        if caller_role == 'moderator':
            cursor.execute("SELECT access_level FROM rooms WHERE id = %s", (room_id,))
            row = cursor.fetchone()
            if row and row[0] == 'admin':
                flash('Moderators cannot modify admin-only rooms')
                return redirect(url_for('rooms'))
        cursor.execute(
            "UPDATE rooms SET access_level = %s WHERE id = %s",
            (access_level, room_id)
        )
        db.commit()
        flash(f'Room access level updated to {access_level}')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        cursor.close()
        db.close()
    return redirect(url_for('rooms'))

@app.route('/rooms/set_password/<int:room_id>', methods=['POST'])
@require_admin
def set_room_password(room_id):
    password = request.form.get('password', '').strip()
    try:
        db = get_db()
        cursor = db.cursor()
        if password:
            password_hash = bcrypt.hashpw(password.encode(), bcrypt.gensalt(12)).decode()
        else:
            password_hash = None
        cursor.execute(
            "UPDATE rooms SET password_hash = %s WHERE id = %s",
            (password_hash, room_id)
        )
        db.commit()
        flash('Room password updated' if password else 'Room password removed')
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

# ---------------------------------------------------------------------------
#  Maintenance
# ---------------------------------------------------------------------------

@app.route('/maintenance')
@require_moderator
def maintenance():
    try:
        db     = get_db()
        cursor = db.cursor()

        # Table stats
        stats = {}
        for table in ('users', 'rooms', 'messages', 'mailbox'):
            try:
                cursor.execute(f"SELECT COUNT(*) FROM `{table}`")
                stats[table] = cursor.fetchone()[0]
            except Exception:
                stats[table] = 'N/A'

        cursor.execute("SELECT COUNT(*) FROM messages WHERE is_deleted = 1")
        stats['deleted_messages'] = cursor.fetchone()[0]

        cursor.execute("SELECT COUNT(*) FROM messages WHERE is_deleted = 0")
        stats['active_messages'] = cursor.fetchone()[0]

        # List backups
        backup_dir = '/opt/scenechat/backups'
        os.makedirs(backup_dir, exist_ok=True)
        backups = []
        for f in sorted(os.listdir(backup_dir), reverse=True):
            if f.endswith('.sql'):
                path = os.path.join(backup_dir, f)
                size = os.path.getsize(path)
                mtime = datetime.fromtimestamp(os.path.getmtime(path)).strftime('%Y-%m-%d %H:%M')
                backups.append({'name': f, 'size': size, 'mtime': mtime})

        return render_template('maintenance.html', stats=stats, backups=backups)
    except Exception as e:
        flash(f'Error: {e}')
        return render_template('maintenance.html', stats={}, backups=[])
    finally:
        try: cursor.close()
        except: pass
        try: db.close()
        except: pass


@app.route('/maintenance/backup', methods=['POST'])
@require_moderator
def db_backup():
    backup_dir = '/opt/scenechat/backups'
    os.makedirs(backup_dir, exist_ok=True)
    filename = f"scenechat_{datetime.now().strftime('%Y%m%d_%H%M%S')}.sql"
    filepath = os.path.join(backup_dir, filename)
    try:
        result = subprocess.run([
            'mysqldump',
            '-h', DB_CONFIG['host'],
            '-u', DB_CONFIG['user'],
            f"-p{DB_CONFIG['password']}",
            DB_CONFIG['database']
        ], capture_output=True, text=True, timeout=60)
        if result.returncode != 0:
            flash(f'Backup failed: {result.stderr}')
            return redirect(url_for('maintenance'))
        with open(filepath, 'w') as f:
            f.write(result.stdout)
        flash(f'Backup saved: {filename}')
    except Exception as e:
        flash(f'Backup error: {e}')
    return redirect(url_for('maintenance'))


@app.route('/maintenance/backup/download/<filename>')
@require_moderator
def db_backup_download(filename):
    backup_dir = '/opt/scenechat/backups'
    safe = os.path.basename(filename)
    if not safe.endswith('.sql'):
        return 'Invalid file', 400
    return send_from_directory(backup_dir, safe, as_attachment=True)


@app.route('/maintenance/backup/delete/<filename>', methods=['POST'])
@require_moderator
def db_backup_delete(filename):
    backup_dir = '/opt/scenechat/backups'
    safe = os.path.basename(filename)
    if not safe.endswith('.sql'):
        flash('Invalid file')
        return redirect(url_for('maintenance'))
    try:
        os.remove(os.path.join(backup_dir, safe))
        flash(f'Deleted backup: {safe}')
    except Exception as e:
        flash(f'Error: {e}')
    return redirect(url_for('maintenance'))


@app.route('/maintenance/restore', methods=['POST'])
@require_moderator
def db_restore():
    f = request.files.get('sql_file')
    if not f or not f.filename.endswith('.sql'):
        flash('Please upload a valid .sql file')
        return redirect(url_for('maintenance'))
    try:
        with tempfile.NamedTemporaryFile(suffix='.sql', delete=False) as tmp:
            f.save(tmp.name)
            result = subprocess.run([
                'mysql',
                '-h', DB_CONFIG['host'],
                '-u', DB_CONFIG['user'],
                f"-p{DB_CONFIG['password']}",
                DB_CONFIG['database']
            ], stdin=open(tmp.name), capture_output=True, text=True, timeout=120)
            os.unlink(tmp.name)
        if result.returncode != 0:
            flash(f'Restore failed: {result.stderr}')
        else:
            flash('Database restored successfully')
    except Exception as e:
        flash(f'Restore error: {e}')
    return redirect(url_for('maintenance'))


@app.route('/maintenance/purge/deleted', methods=['POST'])
@require_moderator
def purge_deleted_messages():
    try:
        db     = get_db()
        cursor = db.cursor()
        cursor.execute("DELETE FROM messages WHERE is_deleted = 1")
        db.commit()
        flash(f'Purged {cursor.rowcount} deleted messages')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        try: cursor.close()
        except: pass
        try: db.close()
        except: pass
    return redirect(url_for('maintenance'))


@app.route('/maintenance/purge/old', methods=['POST'])
@require_moderator
def purge_old_messages():
    days = int(request.form.get('days', 30))
    try:
        db     = get_db()
        cursor = db.cursor()
        cursor.execute(
            "DELETE FROM messages WHERE sent_at < NOW() - INTERVAL %s DAY",
            (days,)
        )
        db.commit()
        flash(f'Purged {cursor.rowcount} messages older than {days} days')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        try: cursor.close()
        except: pass
        try: db.close()
        except: pass
    return redirect(url_for('maintenance'))


@app.route('/maintenance/purge/inactive', methods=['POST'])
@require_moderator
def purge_inactive_users():
    days = int(request.form.get('days', 90))
    try:
        db     = get_db()
        cursor = db.cursor()
        # Find users to purge first
        cursor.execute("""
            SELECT id FROM users
            WHERE role = 'user'
            AND (last_seen IS NULL OR last_seen < NOW() - INTERVAL %s DAY)
            AND username != 'scene_bot'
        """, (days,))
        user_ids = [row[0] for row in cursor.fetchall()]
        if user_ids:
            fmt = ','.join(['%s'] * len(user_ids))
            # Delete messages first to satisfy FK constraint
            cursor.execute(f"DELETE FROM messages WHERE user_id IN ({fmt})", user_ids)
            cursor.execute(f"DELETE FROM users WHERE id IN ({fmt})", user_ids)
            db.commit()
        flash(f'Purged {len(user_ids)} inactive users (>{days} days)')
    except Exception as e:
        flash(f'Error: {e}')
    finally:
        try: cursor.close()
        except: pass
        try: db.close()
        except: pass
    return redirect(url_for('maintenance'))


# ---------------------------------------------------------------------------
#  Online Users
# ---------------------------------------------------------------------------
@app.route('/online')
@require_moderator
def online_users():
    return render_template('online.html')

@app.route('/api/online')
@require_moderator
def api_online_users():
    import urllib.request, json as _json
    try:
        req = urllib.request.Request('http://127.0.0.1:8951/admin/online')
        resp = urllib.request.urlopen(req, timeout=2)
        return resp.read(), 200, {'Content-Type': 'application/json'}
    except Exception as e:
        return _json.dumps({'users': [], 'error': str(e)}), 200, {'Content-Type': 'application/json'}

if __name__ == '__main__':
    app.run(host='0.0.0.0', port=8950, debug=False)