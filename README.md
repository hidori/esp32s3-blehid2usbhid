# ESP32-S3 BLE HID to USB HID Bridge

BLE 接続キーボードを USB 接続で使用するためのブリッジです。

BIOS セットアップや OS インストール時にも Bluetooth 接続のキーボードを USB 接続で使用することができます。

## ディレクトリ構成

```text
.
├── .claude/                   # Claude Code の設定
├── .devcontainer/             # 開発用コンテナの設定
├── .github/                   # GitHub の設定
├── .gitignore                 # git の除外設定
├── .mcp.json                  # MCP サーバーの設定
├── AGENTS.md                  # コーディングエージェント指示書
├── CLAUDE.md                  # Claude Code 指示書
├── LICENSE                    # ライセンス条文
├── Makefile                   # 開発作業用のタスク定義
├── README.md                  # プロジェクトの主旨と概要（このファイル）
├── SPEC.md                    # プロジェクトターゲットの仕様書
├── TODO.md                    # プロジェクトの課題管理
├── arduino-cli.yaml           # arduino-cli の設定
├── compose.yaml               # Docker Compose の設定
├── docs/                      # 設定用 Web ページ
├── esp32s3-blehid2usbhid.ino  # スケッチ本体
├── mise.toml                  # mise の設定
└── renovate.json              # Renovate の設定
```

## ライセンス

このプロジェクトのライセンスは [LICENSE](LICENSE) を参照してください。
