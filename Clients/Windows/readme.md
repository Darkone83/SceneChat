# SceneChat — Windows Client (WPF)

Native Windows desktop client for SceneChat. Connects to the same server
as the Xbox and Python clients using the identical SCCP protocol.

**Team Resurgent / Darkone83**

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

## Project structure

```
SceneChatWPF/
├── Crypto/
│   └── ScCrypto.cs          -- ChaCha20-Poly1305, HKDF (BCL only, no third-party)
├── Net/
│   ├── ChatClient.cs        -- Async TCP client, DH handshake, SCCP protocol
│   ├── ScProtocol.cs        -- Packet type constants, string packing helpers
│   └── Creds.cs             -- JSON credential persistence
├── UI/
│   ├── MainWindow.xaml      -- Main layout: login view + chat view
│   ├── MainWindow.xaml.cs   -- Code-behind: event handling, message rendering
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
  Logging out clears this file.