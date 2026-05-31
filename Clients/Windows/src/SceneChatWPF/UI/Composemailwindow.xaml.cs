// ComposeMailWindow.xaml.cs -- compose a mail message.
// If recipientId != 0 the To field is pre-filled and locked (known user).
// If recipientId == 0 the user types a username (resolved server-side).

using System;
using System.Windows;

namespace SceneChatWPF.UI;

public partial class ComposeMailWindow : Window
{
    private readonly int _recipientId;

    /// <summary>recipientId (0=by name), recipientName, body</summary>
    public event Action<int, string, string>? OnSend;

    public ComposeMailWindow(int recipientId = 0, string recipientName = "")
    {
        InitializeComponent();
        _recipientId = recipientId;
        if (!string.IsNullOrEmpty(recipientName))
        {
            TxtTo.Text = recipientName;
            if (recipientId != 0)
                TxtTo.IsReadOnly = true;
        }
        TxtBody.Focus();
    }

    private void BtnSend_Click(object sender, RoutedEventArgs e)
    {
        var to = TxtTo.Text.Trim();
        var body = TxtBody.Text.Trim();
        if (string.IsNullOrEmpty(to) || string.IsNullOrEmpty(body))
            return;
        OnSend?.Invoke(_recipientId, to, body);
        Close();
    }

    private void BtnCancel_Click(object sender, RoutedEventArgs e) => Close();
}