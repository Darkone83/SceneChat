// MainWindow.xaml.cs -- Code-behind for the SceneChat WPF client.

using System.Text.RegularExpressions;
using System.Windows;
using System.Windows.Controls;
using System.Windows.Documents;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;
using SceneChatWPF.Net;

namespace SceneChatWPF.UI;

public partial class MainWindow : Window
{
    private ChatClient? _client;
    private int _currentRoomId = -1;
    private int _myUserId = 0;
    private readonly Dictionary<int, Paragraph> _msgParagraphs = new();
    private readonly Dictionary<int, UserInfo> _onlineUsers = new(); // msgId -> Paragraph
    private readonly Dictionary<int, string> _dmRooms = new(); // roomId -> displayName
    private readonly List<MailItem> _mails = new();
    private MailboxWindow? _mailboxWindow;
    private bool _doRegister;
    private string _pendingUser = "";
    private string _pendingPass = "";
    private string _pendingServer = "";

    // Colours matching App.xaml
    private static readonly SolidColorBrush BrAccent = new(Color.FromRgb(0x39, 0xFF, 0x14));
    private static readonly SolidColorBrush BrPurple = new(Color.FromRgb(0x8B, 0x5C, 0xF6));
    private static readonly SolidColorBrush BrMuted = new(Color.FromRgb(0x55, 0x55, 0x55));
    private static readonly SolidColorBrush BrText = new(Color.FromRgb(0xE0, 0xE0, 0xE0));
    private static readonly SolidColorBrush BrAdmin = new(Color.FromRgb(0x39, 0xFF, 0x14));
    private static readonly SolidColorBrush BrRed = new(Color.FromRgb(0xDC, 0x26, 0x26));

    // Emoji cache folder
    private static readonly string EmojiDir = System.IO.Path.Combine(
        AppDomain.CurrentDomain.BaseDirectory, "emoji_cache");

    public MainWindow()
    {
        InitializeComponent();
        ShowLogin();
        PrefillCreds();
    }

    // ── Creds prefill ─────────────────────────────────────────────────────────

    private void PrefillCreds()
    {
        var c = Creds.Load();
        if (c == null) return;
        TxtServer.Text = c.Server;
        TxtUsername.Text = c.Username;
        TxtPassword.Password = c.Password;
    }

    // ── View switching ────────────────────────────────────────────────────────

    private void ShowLogin()
    {
        LoginView.Visibility = Visibility.Visible;
        ChatView.Visibility = Visibility.Collapsed;
    }

    private void ShowChat()
    {
        LoginView.Visibility = Visibility.Collapsed;
        ChatView.Visibility = Visibility.Visible;
    }

    // ── Login / Register ──────────────────────────────────────────────────────

    private void BtnLogin_Click(object sender, RoutedEventArgs e) => StartConnect(register: false);
    private void BtnRegister_Click(object sender, RoutedEventArgs e) => StartConnect(register: true);

    private void StartConnect(bool register)
    {
        var server = TxtServer.Text.Trim();
        var username = TxtUsername.Text.Trim();
        var password = TxtPassword.Password;

        if (string.IsNullOrEmpty(server) || string.IsNullOrEmpty(username) || string.IsNullOrEmpty(password))
        { SetLoginStatus("All fields required."); return; }

        if (register && password.Length < 8)
        { SetLoginStatus("Password must be at least 8 characters."); return; }

        _doRegister = register;
        _pendingUser = username;
        _pendingPass = password;
        _pendingServer = server;

        SetLoginStatus("Connecting...", error: false);
        BtnLogin.IsEnabled = false;
        BtnRegister.IsEnabled = false;

        _ = ConnectAsync(server, username, password);
    }

    private async Task ConnectAsync(string server, string username, string password)
    {
        _client?.DisposeAsync();
        _client = new ChatClient();

        _client.OnConnected += () => Dispatcher.Invoke(() =>
        {
            SetStatus("Connected — authenticating...");
            if (_doRegister)
                _ = _client.SendRegisterAsync(username, password);
            else
                _ = _client.SendLoginAsync(username, password);
        });

        _client.OnAuthOk += (userId, uname, token) => Dispatcher.Invoke(() =>
        {
            _myUserId = userId;
            if (_doRegister && userId == 0)
            {
                _doRegister = false;
                BtnLogin.IsEnabled = true;
                BtnRegister.IsEnabled = true;
                SetLoginStatus("Registered! Please log in.", error: false);
                SetStatus("Registered — please log in");
                return;
            }
            var display = string.IsNullOrEmpty(uname) ? username : uname;
            TxtLoggedInUser.Text = display;
            Creds.Save(server, username, password);
            ShowChat();
            SetStatus($"Logged in as {display}");
        });

        _client.OnAuthFail += msg => Dispatcher.Invoke(() =>
        {
            SetLoginStatus(msg);
            BtnLogin.IsEnabled = true;
            BtnRegister.IsEnabled = true;
        });

        _client.OnRoomList += rooms => Dispatcher.Invoke(() =>
        {
            RoomListBox.Items.Clear();
            foreach (var r in rooms)
            {
                var label = (r.PasswordFlag > 0 ? "* " : "") +
                            (r.Type == 1 ? "[voice] " : "# ") + r.Name;
                RoomListBox.Items.Add(new RoomItem(r.Id, label, r.Name, r.Type, r.PasswordFlag));
            }
            // No auto-join -- user selects from list
            RefreshUserList(); // Re-resolve room names now that rooms are loaded
        });

        _client.OnJoinFail += reason => Dispatcher.Invoke(() =>
        {
            if (reason == "Wrong password")
            {
                var pw = Microsoft.VisualBasic.Interaction.InputBox(
                    "Wrong password. Try again:", "Password Required", "");
                if (!string.IsNullOrEmpty(pw) && _client != null)
                    _ = _client.JoinRoomAsync(_currentRoomId, pw);
            }
            else SetChatStatus($"Join failed: {reason}");
        });

        _client.OnUserList += users => Dispatcher.Invoke(() =>
        {
            _onlineUsers.Clear();
            foreach (var u in users) _onlineUsers[u.UserId] = u;
            RefreshUserList();
        });

        _client.OnUserJoin += user => Dispatcher.Invoke(() =>
        {
            _onlineUsers[user.UserId] = user;
            RefreshUserList();
        });

        _client.OnUserLeave += (userId, _) => Dispatcher.Invoke(() =>
        {
            _onlineUsers.Remove(userId);
            RefreshUserList();
        });

        _client.OnRoomJoined += (roomId, name, history) => Dispatcher.Invoke(() =>
        {
            _currentRoomId = roomId;
            // Update own presence room
            if (_myUserId > 0 && _onlineUsers.TryGetValue(_myUserId, out var me))
            {
                _onlineUsers[_myUserId] = me with { RoomId = roomId };
                RefreshUserList();
            }
            // DM rooms display "@ username"; regular rooms "# name"
            string display = name.StartsWith("DM:") ? name.Substring(3) : name;
            bool isDm = _dmRooms.ContainsKey(roomId) || name.StartsWith("DM:");
            TxtRoomName.Text = isDm ? $"@ {display}" : $"# {name}";
            FeedBox.Document.Blocks.Clear();
            _msgParagraphs.Clear();
            foreach (var m in history)
                AppendMessage(m.Username, m.Content, m.Timestamp, m.MsgId);
            SetStatus(isDm ? $"@{display}" : $"#{name}");
        });

        _client.OnDmRoom += (roomId, name) => Dispatcher.Invoke(() =>
        {
            string uname = name.StartsWith("DM:") ? name.Substring(3) : name;
            if (!_dmRooms.ContainsKey(roomId))
            {
                _dmRooms[roomId] = uname;
                DmListBox.Items.Add(new DmItem(roomId, uname));
            }
        });

        _client.OnMailList += mails => Dispatcher.Invoke(() =>
        {
            _mails.AddRange(mails);
            if (_mails.Count > 0)
                SetStatus($"{_mails.Count} message(s) in mailbox");
            if (_mailboxWindow != null && _mailboxWindow.IsVisible)
                _mailboxWindow.SetMails(_mails);
        });

        _client.OnMessage += msg => Dispatcher.Invoke(() =>
        {
            if (msg.RoomId == _currentRoomId)
                AppendMessage(msg.Username, msg.Content, msg.Timestamp, msg.MsgId);
        });

        _client.OnMsgDelete += (roomId, msgId) => Dispatcher.Invoke(() =>
        {
            if (roomId == _currentRoomId && _msgParagraphs.TryGetValue(msgId, out var para))
            {
                para.Inlines.Clear();
                para.Inlines.Add(new Run("[deleted]") { Foreground = BrMuted, FontStyle = FontStyles.Italic });
            }
        });

        _client.OnError += err => Dispatcher.Invoke(() => SetStatus($"Error: {err}"));

        _client.OnDisconnected += reason => Dispatcher.Invoke(() =>
        {
            SetLoginStatus(reason ?? "Disconnected");
            BtnLogin.IsEnabled = true;
            BtnRegister.IsEnabled = true;
            ShowLogin();
            SetStatus("Disconnected");
        });

        try
        {
            await _client.ConnectAsync(server);
        }
        catch (Exception ex)
        {
            Dispatcher.Invoke(() =>
            {
                SetLoginStatus($"Connection failed: {ex.Message}");
                BtnLogin.IsEnabled = true;
                BtnRegister.IsEnabled = true;
            });
        }
    }

    // ── Message rendering ─────────────────────────────────────────────────────

    private void AppendMessage(string username, string content, string ts, int msgId = 0)
    {
        var para = new Paragraph { Margin = new Thickness(0, 2, 0, 2) };

        // Timestamp
        para.Inlines.Add(new Run(ts + "  ")
        { Foreground = BrMuted, FontSize = 11 });

        // Username
        bool isAdmin = username == "[Admin]";
        para.Inlines.Add(new Run(username + "  ")
        { Foreground = isAdmin ? BrAdmin : BrPurple, FontWeight = FontWeights.Bold });

        // Content -- parse :emoji: tokens
        AppendContentInlines(para, content);

        FeedBox.Document.Blocks.Add(para);
        if (msgId != 0) _msgParagraphs[msgId] = para;
        FeedBox.ScrollToEnd();
    }

    private void AppendContentInlines(Paragraph para, string content)
    {
        var regex = new Regex(@":([a-zA-Z0-9_]+):");
        int pos = 0;
        foreach (Match m in regex.Matches(content))
        {
            if (m.Index > pos)
                para.Inlines.Add(new Run(content[pos..m.Index]) { Foreground = BrText });

            var name = m.Groups[1].Value;
            var index = Array.IndexOf(EmojiPickerWindow.EmojiNames, name);
            if (index >= 0)
            {
                var img = new Image
                {
                    Source = EmojiPickerWindow.GetEmojiSlice(index),
                    Width = 18,
                    Height = 18
                };
                para.Inlines.Add(new InlineUIContainer(img)
                { BaselineAlignment = BaselineAlignment.Center });
                para.Inlines.Add(new Run(" "));
            }
            else
            {
                para.Inlines.Add(new Run(m.Value) { Foreground = BrText });
            }
            pos = m.Index + m.Length;
        }
        if (pos < content.Length)
            para.Inlines.Add(new Run(content[pos..]) { Foreground = BrText });
    }

    // ── Online users ──────────────────────────────────────────────────────────

    private void RefreshUserList()
    {
        OnlineListBox.Items.Clear();
        foreach (var u in _onlineUsers.Values)
        {
            // Try to find room name
            string roomName = "?";
            foreach (RoomItem ri in RoomListBox.Items)
            {
                if (ri.Id == u.RoomId) { roomName = ri.RawName; break; }
            }
            OnlineListBox.Items.Add(new UserListItem(u.UserId, u.Username,
                $"● {u.Username}  #{roomName}"));
        }
    }

    private void SetChatStatus(string msg) => SetStatus(msg);

    // ── Emoji download ────────────────────────────────────────────────────────

    private static readonly string[] EmojiNames =
    {
        "smile","wink","laugh","cry","angry","sad","surprised","thinking",
        "cool","love_face","dead","party","question","thumbs_up","thumbs_down",
        "check","x","alert","heart","fireheart","star","skull","fire",
        "scenechat_sc","scenechat_softmod","scenechat_fire","scenechat_modchip",
        "scenechat_controller_chat","scenechat_ping","scenechat_lobby",
        "scenechat_dpad","scenechat_buttons","scenechat_devbuild",
    };

    private static async Task DownloadEmojiAsync(string host, int port = 8950)
    {
        using var http = new System.Net.Http.HttpClient { Timeout = TimeSpan.FromSeconds(5) };
        foreach (var name in EmojiNames)
        {
            var dest = System.IO.Path.Combine(EmojiDir, $"{name}.png");
            if (System.IO.File.Exists(dest)) continue;
            try
            {
                var bytes = await http.GetByteArrayAsync($"http://{host}:{port}/emoji/{name}.png");
                await System.IO.File.WriteAllBytesAsync(dest, bytes);
            }
            catch { /* skip missing emoji silently */ }
        }
    }

    // ── Room list ─────────────────────────────────────────────────────────────

    private void RoomListBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (RoomListBox.SelectedItem is RoomItem room && room.Id != _currentRoomId)
        {
            if (room.PasswordFlag > 0)
            {
                var pw = Microsoft.VisualBasic.Interaction.InputBox(
                    $"Enter password for {room.RawName}:", "Password Required", "");
                if (!string.IsNullOrEmpty(pw))
                    _ = _client?.JoinRoomAsync(room.Id, pw);
            }
            else
                _ = _client?.JoinRoomAsync(room.Id);
        }
    }

    // ── DMs and mailbox ───────────────────────────────────────────────────────

    private void DmListBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        if (DmListBox.SelectedItem is DmItem dm && dm.Id != _currentRoomId)
            _ = _client?.JoinRoomAsync(dm.Id);
    }

    private void OnlineListBox_MouseDoubleClick(object sender, System.Windows.Input.MouseButtonEventArgs e)
    {
        if (OnlineListBox.SelectedItem is UserListItem u && u.UserId != _myUserId)
            _ = _client?.SendDmOpenAsync(u.UserId);
    }

    private void MenuOpenDm_Click(object sender, RoutedEventArgs e)
    {
        if (OnlineListBox.SelectedItem is UserListItem u && u.UserId != _myUserId)
            _ = _client?.SendDmOpenAsync(u.UserId);
    }

    private void MenuSendMail_Click(object sender, RoutedEventArgs e)
    {
        if (OnlineListBox.SelectedItem is UserListItem u && u.UserId != _myUserId)
            OpenComposeMail(u.UserId, u.Username);
    }

    private void BtnMailbox_Click(object sender, RoutedEventArgs e)
    {
        if (_mailboxWindow == null || !_mailboxWindow.IsLoaded)
        {
            _mailboxWindow = new MailboxWindow { Owner = this };
            _mailboxWindow.OnReadMail += mailId => _ = _client?.SendMailReadAsync(mailId);
            _mailboxWindow.OnDeleteMail += mailId =>
            {
                _ = _client?.SendMailDeleteAsync(mailId);
                _mails.RemoveAll(m => m.MailId == mailId);
            };
            _mailboxWindow.OnComposeReply += sender2 => OpenComposeMail(0, sender2);
            _mailboxWindow.OnComposeNew += () => OpenComposeMail(0, "");
        }
        _mailboxWindow.SetMails(_mails);
        _mailboxWindow.Show();
        _mailboxWindow.Activate();
    }

    private void OpenComposeMail(int recipientId, string recipientName)
    {
        var dlg = new ComposeMailWindow(recipientId, recipientName) { Owner = this };
        dlg.OnSend += (rid, rname, body) =>
        {
            if (rid != 0)
                _ = _client?.SendMailAsync(rid, body);
            else
                _ = _client?.SendMailAsync(0, $"TO:{rname}\n{body}");
            SetStatus($"Mail sent to {rname}");
        };
        dlg.Show();
    }

    // ── Input bar ─────────────────────────────────────────────────────────────

    private void TxtMessage_KeyDown(object sender, KeyEventArgs e)
    {
        if (e.Key == Key.Enter) SendMessage();
    }

    private void BtnSend_Click(object sender, RoutedEventArgs e) => SendMessage();

    private void SendMessage()
    {
        var text = TxtMessage.Text.Trim();
        if (string.IsNullOrEmpty(text) || _currentRoomId < 0 || _client == null) return;
        TxtMessage.Clear();
        _ = _client.SendMessageAsync(_currentRoomId, text);
    }

    // ── Emoji picker ──────────────────────────────────────────────────────────

    private void BtnEmoji_Click(object sender, RoutedEventArgs e)
    {
        var picker = new EmojiPickerWindow();
        picker.Owner = this;
        picker.EmojiSelected += token =>
        {
            TxtMessage.Text += token;
            TxtMessage.Focus();
            TxtMessage.CaretIndex = TxtMessage.Text.Length;
        };
        picker.ShowDialog();
    }

    // ── Logout ────────────────────────────────────────────────────────────────

    private async void BtnLogout_Click(object sender, RoutedEventArgs e)
    {
        Creds.Clear();
        if (_client != null)
        {
            await _client.DisconnectAsync();
            await _client.DisposeAsync();
            _client = null;
        }
        _currentRoomId = -1;
        FeedBox.Document.Blocks.Clear();
        RoomListBox.Items.Clear();
        // Clear DM/mail state so a different account doesn't see stale data
        DmListBox.Items.Clear();
        _dmRooms.Clear();
        _mails.Clear();
        _onlineUsers.Clear();
        OnlineListBox.Items.Clear();
        if (_mailboxWindow != null) { _mailboxWindow.Close(); _mailboxWindow = null; }
        TxtLoginStatus.Text = "";
        BtnLogin.IsEnabled = true;
        BtnRegister.IsEnabled = true;
        ShowLogin();
        SetStatus("Logged out");
    }

    // ── Status helpers ────────────────────────────────────────────────────────

    private void SetStatus(string msg) => TxtStatus.Text = msg;

    private void SetLoginStatus(string msg, bool error = true)
    {
        TxtLoginStatus.Foreground = error ? BrRed
            : new SolidColorBrush(Color.FromRgb(0x55, 0x55, 0x55));
        TxtLoginStatus.Text = msg;
    }

    // ── Window close ─────────────────────────────────────────────────────────

    protected override async void OnClosed(EventArgs e)
    {
        if (_client != null)
        {
            await _client.DisconnectAsync();
            await _client.DisposeAsync();
        }
        base.OnClosed(e);
    }
}

// ── Room list item view model ──────────────────────────────────────────────────

public class RoomItem
{
    public int Id { get; }
    public string DisplayName { get; }  // Full label with prefix
    public string RawName { get; }  // Room name only for lookups
    public int Type { get; }
    public int PasswordFlag { get; }

    public RoomItem(int id, string displayName, string rawName, int type, int passwordFlag = 0)
    { Id = id; DisplayName = displayName; RawName = rawName; Type = type; PasswordFlag = passwordFlag; }

    public override string ToString() => DisplayName;
}

public class UserListItem
{
    public int UserId { get; }
    public string Username { get; }
    public string Display { get; }
    public UserListItem(int userId, string username, string display)
    { UserId = userId; Username = username; Display = display; }
    public override string ToString() => Display;
}

public class DmItem
{
    public int Id { get; }
    public string DisplayName { get; }
    public string Username { get; }
    public DmItem(int id, string username)
    { Id = id; Username = username; DisplayName = "@ " + username; }
    public override string ToString() => DisplayName;
}