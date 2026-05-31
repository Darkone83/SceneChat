// MailboxWindow.xaml.cs -- standalone mailbox: list, read pane, reply, delete, compose.

using System;
using System.Collections.Generic;
using System.Windows;
using System.Windows.Controls;
using SceneChatWPF.Net;

namespace SceneChatWPF.UI;

public partial class MailboxWindow : Window
{
    private readonly List<MailItem> _mails = new();

    public event Action<int>? OnReadMail;       // mailId
    public event Action<int>? OnDeleteMail;     // mailId
    public event Action<string>? OnComposeReply;   // sender to reply to
    public event Action? OnComposeNew;

    public MailboxWindow()
    {
        InitializeComponent();
    }

    public void SetMails(IEnumerable<MailItem> mails)
    {
        _mails.Clear();
        _mails.AddRange(mails);
        MailListBox.Items.Clear();
        foreach (var m in _mails)
            MailListBox.Items.Add(m);
        if (_mails.Count > 0)
            MailListBox.SelectedIndex = 0;
        else
        {
            TxtFrom.Text = "";
            TxtTimestamp.Text = "";
            TxtBody.Text = "";
        }
    }

    private MailItem? Current =>
        MailListBox.SelectedItem as MailItem;

    private void MailListBox_SelectionChanged(object sender, SelectionChangedEventArgs e)
    {
        var m = Current;
        if (m == null) return;
        TxtFrom.Text = $"From: {m.Sender}";
        TxtTimestamp.Text = m.Timestamp;
        TxtBody.Text = m.Body;
        OnReadMail?.Invoke(m.MailId);
    }

    private void BtnReply_Click(object sender, RoutedEventArgs e)
    {
        var m = Current;
        if (m != null)
            OnComposeReply?.Invoke(m.Sender);
    }

    private void BtnDelete_Click(object sender, RoutedEventArgs e)
    {
        var m = Current;
        if (m == null) return;
        OnDeleteMail?.Invoke(m.MailId);
        int idx = MailListBox.SelectedIndex;
        _mails.Remove(m);
        MailListBox.Items.Remove(m);
        if (_mails.Count > 0)
            MailListBox.SelectedIndex = Math.Min(idx, _mails.Count - 1);
        else
        {
            TxtFrom.Text = ""; TxtTimestamp.Text = ""; TxtBody.Text = "";
        }
    }

    private void BtnCompose_Click(object sender, RoutedEventArgs e)
        => OnComposeNew?.Invoke();
}