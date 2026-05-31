# SceneChat — Windows Client (WPF)

Native Windows desktop client for SceneChat. Connects to the same server
as the Xbox and Python clients using the identical SCCP protocol.

**Team Resurgent / Darkone83 — v1.3**

---

## Requirements

- Windows 10 or 11
- [.NET 8 SDK](https://dotnet.microsoft.com/download) (for building)
- Visual Studio 2022 with the `.NET desktop development` workload

---

## Building

Open `SceneChatWPF.sln` in Visual Studio 2022 and press **F5**, or from the
command line:

```
dotnet build
dotnet run --project SceneChatWPF
```

### Publish as a single self-contained exe

```
dotnet publish SceneChatWPF -c Release -r win-x64 --self-contained true
```

The output `SceneChat.exe` in `publish/` runs on any Windows 10/11 machine
with no .NET installation required.

---

## Features

- Full DH + ChaCha20-Poly1305 encrypted connection — same security as the Xbox client
- Login and registration with credential auto-save
- Multi-room chat with real-time messages
- **Direct messages** — right-click a user in the online panel (or double-click) to open a DM; active DMs appear in a dedicated sidebar section
- **Mailbox** — a standalone window: read, mark-read, reply, delete, and compose; mail is delivered on login
- **Compose to anyone** — send mail to a known user or type a username to mail an offline user
- Real-time deleted message sync — messages deleted by an admin disappear instantly
- Inline emoji rendering — type `:smile:` `:fire:` etc.
- Emoji picker
- Password protected rooms — dialog prompt on join, retry on wrong password
- ACL rooms — only visible to users with sufficient role
- Online users panel — live list of connected users and their current room, updates in real time

---

## Direct Messages and Mailbox

**DMs** open in the main chat view like any room. Open one by right-clicking a user in the online panel (Open DM) or double-clicking them. Active DM channels appear in the DIRECT MESSAGES section of the sidebar — click one to return to that conversation. DMs are delivered in real time to both participants.

**Mailbox** is for offline messaging. Click **Mailbox** in the top bar to open the mailbox window. New mail arrives when you log in. Compose to a known user via the online panel right-click menu (Send Mail), or use Compose in the mailbox window and type any username — the server resolves it, so you can mail offline users.

---

## Project structure

```
SceneChatWPF/
├── Crypto/
│   └── ScCrypto.cs          -- ChaCha20-Poly1305, HKDF (BCL only, no third-party)
├── Net/
│   ├── ChatClient.cs        -- Async TCP client, DH handshake, SCCP protocol (chat, DMs, mail)
│   ├── ScProtocol.cs        -- Packet type constants, string packing helpers
│   └── Creds.cs             -- JSON credential persistence
├── UI/
│   ├── MainWindow.xaml      -- Main layout: login view + chat view, DM list, mailbox button
│   ├── MainWindow.xaml.cs   -- Code-behind: event handling, message rendering, DM/mail wiring
│   ├── MailboxWindow.xaml         -- Standalone mailbox window
│   ├── MailboxWindow.xaml.cs
│   ├── ComposeMailWindow.xaml     -- Standalone compose-mail window
│   ├── ComposeMailWindow.xaml.cs
│   ├── EmojiPickerWindow.xaml
│   └── EmojiPickerWindow.xaml.cs
├── Resources/
│   ├── scenechat_logo.png   -- 256x256 logo (header + window icon)
│   └── scenechat.ico        -- Multi-size ICO for taskbar and exe
├── App.xaml                 -- Dark theme resource dictionary
└── App.xaml.cs
```

---

## Notes

- Emoji PNGs are downloaded from the admin panel (`http://server:8950/emoji/`)
  on first login and cached in `emoji_cache/` next to the exe.
  If the admin panel is unreachable, emoji render as `:token:` text.
- Credentials are saved to `creds.json` next to the exe for auto-login.
  Logging out clears this file and resets DM/mail state.
- Room access is enforced server-side. Rooms marked `*` require a password.
  Rooms not in your list are hidden by the server based on your role.
- DM rooms are private and never appear in the public room list.