# Godot MCP Server (module: godot_mcp)

MCP (Model Context Protocol) server bawaan editor Godot 4.8-dev (build fork arm64).
AI agent bisa mengontrol editor dan membuat game: scene, node, script, run game, screenshot, dan lain-lain.

## Fitur

- **Transport**: Streamable HTTP (`POST /mcp`) dan SSE (`GET /sse` + `POST /messages`) — bisa dipilih lewat setting `mcp/transport`
- **Jaringan**: pilihan `mcp/bind_mode` = LAN (`0.0.0.0`, IP di-generate otomatis) atau Localhost saja — URL siap pakai tampil di notifikasi & bisa disalin
- **Auto-start (default OFF)**: aktifkan `mcp/enabled` di Editor > Editor Settings; server aktif tanpa perlu buka project (Project Manager) dan di dalam project
- **Notifikasi Android**: "Server MCP hidup" + tombol **"Salin URL MCP"** (copy clipboard) dan **"Matikan server"** (foreground service → server tetap jalan saat editor di-background)
- **Token auth opsional** untuk akses LAN
- **26 tools**: project info, list/read/write file, project settings, scene tree, open/create/save scene, add/remove/rename/reparent node, get/set property (undoable), read/write/attach script, run/stop game, send input, screenshot (PNG), server info
- **Notifikasi MCP ke client**: `notifications/scene_changed`, `notifications/game_started`, `notifications/game_stopped`

## Konfigurasi (Editor > Editor Settings, cari "mcp")

| Setting | Default | Keterangan |
|---|---|---|
| `mcp/enabled` | `false` | aktif/tidak (server otomatis start setelah diaktifkan) |
| `mcp/transport` | `0` (Both) | Both (Streamable HTTP + SSE), Streamable HTTP only, SSE only |
| `mcp/bind_mode` | `0` (LAN) | LAN = akses dari perangkat lain (IP dibuat otomatis); Localhost = `127.0.0.1` saja |
| `mcp/port` | `8766` | port HTTP |
| `mcp/token` | (kosong) | jika diisi, client wajib kirim `Authorization: Bearer <token>` |

Perubahan setting diterapkan otomatis (server restart sendiri tanpa perlu buka project).

## Endpoint

```
POST /mcp        Streamable HTTP (json-rpc; Accept: text/event-stream juga didukung)
GET  /sse        SSE stream  (session id via header Mcp-Session-Id atau query ?sessionId=)
POST /messages   SSE transport (kiim request client → server)
GET  /           info server
```

## Cara pakai dari client

Semua client MCP standar bisa pakai. Contoh:

**RikkaHub (di tablet, localhost):**
```json
{
  "mcpServers": {
    "godot": {
      "type": "streamable_http",
      "url": "http://127.0.0.1:8766/mcp"
    }
  }
}
```

**RikkaHub / perangkat lain (LAN):** ganti `127.0.0.1` dengan IP tablet.

**OpenCode CLI** (`opencode.json`):
```json
{
  "mcp": {
    "godot": {
      "type": "remote",
      "url": "http://<ip-tablet>:8766/mcp",
      "enabled": true
    }
  }
}
```

**Codex CLI:**
```
codex mcp add godot --transport http --url http://<ip-tablet>:8766/mcp
```

**Claude CLI:**
```
claude mcp add --transport http godot http://<ip-tablet>:8766/mcp
# atau transport SSE:
claude mcp add --transport sse godot http://<ip-tablet>:8766/sse
```

Dengan token:
```
claude mcp add --transport http godot http://<ip-tablet>:8766/mcp --header "Authorization: Bearer <token>"
```

## Contoh dialog AI

> "Buat scene gameplay.tscn dengan root Node2D bernama Game, tambahkan Sprite2D dan child script player.gd" → AI memanggil `create_scene`, `add_node`, `write_script`, `attach_script`
> "Jalankan main scene" → `run_main_scene`; "tangkapan layar" → `screenshot`

## Catatan

- Screenshot game berjalan: pada editor Android, game berjalan di proses terpisah → screenshot diambil dari viewport editor/simulasi, bukan proses game.
- `send_input` kontrol berjalan untuk game yang dijalankan dalam proses yang sama (desktop). Di Android gunakan `action`-based input.
- Saat MCP dimatikan lewat notifikasi, server berhenti sampai dinyalakan lagi (dari Editor Settings `mcp/enabled` atau aplikasi di-restart).

## Download APK (build GitHub Actions)

Tiap build sukses membuat **GitHub Release per-build** berisi APK editor arm64 — klik link di halaman release = download APK langsung (bukan zip artifact):

```
https://github.com/hazn75206-sketch/godot-4.8/releases
```