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
                RoomListBox.Items.Add(new RoomItem(r.Id, r.Name, r.Type));
            // Auto-join first text room
            var first = rooms.FirstOrDefault(r => r.Type == 0);
            if (first != null) _ = _client.JoinRoomAsync(first.Id);
        });

        _client.OnRoomJoined += (roomId, name, history) => Dispatcher.Invoke(() =>
        {
            _currentRoomId = roomId;
            TxtRoomName.Text = $"# {name}";
            FeedBox.Document.Blocks.Clear();
            foreach (var m in history)
                AppendMessage(m.Username, m.Content, m.Timestamp);
            SetStatus($"#{name}");
        });

        _client.OnMessage += msg => Dispatcher.Invoke(() =>
        {
            if (msg.RoomId == _currentRoomId)
                AppendMessage(msg.Username, msg.Content, msg.Timestamp);
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

    private void AppendMessage(string username, string content, string ts)
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
            _ = _client?.JoinRoomAsync(room.Id);
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
    public string Name { get; }
    public int Type { get; }
    public string DisplayName => (Type == 1 ? "🔊 " : "# ") + Name;

    public RoomItem(int id, string name, int type)
    { Id = id; Name = name; Type = type; }
}