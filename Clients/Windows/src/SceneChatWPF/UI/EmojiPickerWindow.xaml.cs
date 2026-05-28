using System.Windows;
using System.Windows.Controls;
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

    private const int CellSize = 32;
    private const int PerRow = 8;
    private const int RenderSize = 24;

    // Shared atlas -- loaded once
    private static BitmapSource? _atlas;

    private static BitmapSource GetAtlas()
    {
        if (_atlas != null) return _atlas;
        var uri = new Uri("pack://application:,,,/Resources/emoji_atlas.png");
        var bmp = new BitmapImage(uri);
        bmp.Freeze();
        _atlas = bmp;
        return _atlas;
    }

    public static CroppedBitmap GetEmojiSlice(int index)
    {
        int col = index % PerRow;
        int row = index / PerRow;
        var rect = new Int32Rect(col * CellSize, row * CellSize, CellSize, CellSize);
        var crop = new CroppedBitmap(GetAtlas(), rect);
        crop.Freeze();
        return crop;
    }

    public EmojiPickerWindow()
    {
        InitializeComponent();
        BuildGrid();
    }

    private void BuildGrid()
    {
        for (int i = 0; i < EmojiNames.Length; i++)
        {
            var name = EmojiNames[i];
            var idx = i;

            var img = new Image
            {
                Source = GetEmojiSlice(i),
                Width = RenderSize,
                Height = RenderSize
            };

            var btn = new Button
            {
                Content = img,
                Width = 36,
                Height = 36,
                Margin = new Thickness(2),
                Background = new SolidColorBrush(Color.FromRgb(0x1A, 0x1A, 0x1A)),
                BorderThickness = new Thickness(1),
                BorderBrush = new SolidColorBrush(Color.FromRgb(0x33, 0x33, 0x33)),
                ToolTip = $":{name}:",
                Tag = $":{name}:"
            };

            btn.Click += (_, _) => { EmojiSelected?.Invoke((string)btn.Tag); Close(); };
            EmojiPanel.Children.Add(btn);
        }
    }
}