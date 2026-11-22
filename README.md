# ESP32-S3 BLE HID to USB HID Bridge

Bluetooth 接続のキーボードを USB 接続で使用するためのブリッジです。

BIOS セットアップや OS インストール時にも Bluetooth 接続のキーボードを USB 接続で使用することができます。

## 動作環境

- 設定用 Web ページ
  - WebHID に対応したブラウザ（Chrome、Edge など）が使える PC
  - Linux では udev の設定が必要
  - Mac ではアクセサリの接続の許可が必要
- ブリッジ
  - M5Stack AtomS3 Lite
  - M5Stack AtomS3U
- キーボード
  - Bluetooth LE に対応したキーボード
  - Bluetooth Classic のキーボードは使えない

## 使用方法

1. ブリッジを PC につなぐ
2. [設定用 Web ページ](https://hidori.github.io/esp32s3-blehid2usbhid/)を開き、ブリッジに接続する
3. Bluetooth 接続のキーボードをペアリングできる状態にしてから、ページで検索し、一覧から選んでペアリングする。パスキーが表示されたら、Bluetooth 接続のキーボードで入力して Enter キーを押す
4. 登録が済むと、Bluetooth 接続のキーボードのキー入力が、USB キーボードの入力として PC に送られる。次からは、ブリッジと Bluetooth 接続のキーボードの電源を入れるだけでつながる

- 登録が済んだブリッジは、BIOS/UEFI の設定画面や、OS をインストールしている途中の PC でも使える（ペアリングと設定は、OS の動いている PC で行う）
- 登録の削除と、ブリッジの再起動も、設定用 Web ページで行う
- ブリッジの状態は、RGB LED で分かる（[SPEC.md](SPEC.md) の「表示」を参照）

## 開発環境

- IDE
  - Arduino IDE
- ボードマネージャ
  - M5Stack
- 依存ライブラリ
  - Adafruit TinyUSB
  - EspBle

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
├── renovate.json              # Renovate の設定
└── src/                       # スケッチの各部（責務ごとのフォルダに分けたもの）
```

## ライセンス

このプロジェクトのライセンスは [LICENSE](LICENSE) を参照してください。
