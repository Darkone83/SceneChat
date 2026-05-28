using System.Windows;
using System.Windows.Controls;
using System.Windows.Input;
using System.Windows.Media;
using System.Windows.Media.Imaging;

namespace SceneChatWPF.UI;

public partial class EmojiPickerWindow : Window
{
    public event Action<string>? EmojiSelected;

    public static readonly string[] EmojiNames =
    {
        "smile","wink","laugh","cry","angry","sad","surprised","thinking",
        "cool","love_face","dead","party","question","thumbs_up","thumbs_down",
        "check","x","alert","heart","fireheart","star","skull","fire",
        "scenechat_sc","scenechat_softmod","scenechat_fire","scenechat_modchip",
        "scenechat_controller_chat","scenechat_ping","scenechat_lobby",
        "scenechat_dpad","scenechat_buttons","scenechat_devbuild",
    };

    private static readonly Dictionary<string, BitmapImage> _cache = new();

    public static BitmapImage? GetEmojiImage(string name)
    {
        if (_cache.TryGetValue(name, out var cached)) return cached;

        var path = System.IO.Path.Combine(
            AppDomain.CurrentDomain.BaseDirectory,
            "Resources", "emoji", $"{name}.png");

        if (!System.IO.File.Exists(path)) return null;

        var bmp = new BitmapImage(new Uri(path, UriKind.Absolute));
        bmp.Freeze();
        _cache[name] = bmp;
        return bmp;
    }

    public static ImageSource? GetEmojiSlice(int index)
    {
        if (index < 0 || index >= EmojiNames.Length) return null;
        return GetEmojiImage(EmojiNames[index]);
    }

    public EmojiPickerWindow()
    {
        InitializeComponent();
        BuildGrid();
    }

    private void BuildGrid()
    {
        var normal = new SolidColorBrush(Color.FromRgb(0x1A, 0x1A, 0x1A));
        var hover = new SolidColorBrush(Color.FromRgb(0x39, 0xFF, 0x14));
        var border = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x33));

        foreach (var name in EmojiNames)
        {
            var img = GetEmojiImage(name);
            var token = $":{name}:";

            // Use Border + Image directly -- avoids WPF Button ControlTemplate
            // foreground/opacity inheritance that was tinting the image black
            var image = new Image
            {
                Width = 24,
                Height = 24,
                Stretch = Stretch.Uniform,
                Source = img
            };

            var cell = new Border
            {
                Width = 36,
                Height = 36,
                Margin = new Thickness(2),
                Background = normal,
                BorderThickness = new Thickness(1),
                BorderBrush = border,
                CornerRadius = new CornerRadius(4),
                Child = image,
                Cursor = Cursors.Hand,
                ToolTip = token
            };

            var capToken = token;
            cell.MouseEnter += (_, _) => cell.BorderBrush = hover;
            cell.MouseLeave += (_, _) => cell.BorderBrush = border;
            cell.MouseLeftButtonUp += (_, _) =>
            {
                EmojiSelected?.Invoke(capToken);
                Close();
            };

            EmojiPanel.Children.Add(cell);
        }
    }
}