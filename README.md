# ESP32-S3 BLE HID to USB HID Bridge

BLE 接続キーボードを USB 接続で使用するためのブリッジです。

## ディレクトリ構成

```text
.
├── .claude/
│   └── settings.json          # Claude Code のプロジェクト設定
├── .github/
│   └── workflows/
│       └── ci.yaml            # GitHub Actions のワークフロー
├── .gitignore                 # git の除外設定
├── AGENTS.md                  # コーディングエージェント向けの指示
├── CLAUDE.md                  # Claude Code 向けの指示
├── LICENSE
├── Makefile                   # 作業用のタスク定義
├── README.md                  # プロジェクトの主旨と概要
├── arduino-cli.yaml           # arduino-cli の設定
├── esp32s3-blehid2usbhid.ino  # スケッチ本体
├── mise.toml                  # mise の設定
└── renovate.json              # Renovate の設定
```

## ドキュメント

- [AGENTS.md](AGENTS.md) — コーディングエージェント向けの指示

## ライセンス

このプロジェクトのライセンスは MIT です。詳細は [LICENSE](LICENSE) を参照してください。
