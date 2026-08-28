# X68K Tab

[**English README → README_en.md**](README_en.md)

<p align="center">
  <img src="./x68t.gif" alt="X68K Tab running on M5Stack Tab5" width="760">
</p>

<p align="center">
  <b>X68000 Emulator for M5Stack Tab5 / ESP32-P4</b><br>
  Developed by Layer812 &nbsp;|&nbsp; Based on PX68K &nbsp;|&nbsp; 68000 core: Musashi
</p>

**X68K Tab** は、M5Stack Tab5（ESP32-P4）で動作するポータブル X68000 エミュレータです。
PX68K / Musashi をベースに、ESP32-P4 の **2つのHP CPU + LP Core**、PSRAM、MIPI-DSI、USB Host、microSD、内蔵オーディオを使う構成へ再設計しています。

もともとは、昔の X68000 **PANIC** データを M5Stack Tab5 で再生したくなり、専用プレイヤー [**PanicPlayerTab5**](https://github.com/Layer812/PanicPlayerTab5) を作っていました。
PANIC を再生するために必要な X68000 互換環境を少しずつ実装していったのですが、問題がひとつありました。
**肝心の PANIC データが、まったく手元にありません……**

ということで（？）、**まだ見ぬ PANIC データのために** CPU、画面、音、I/O、メモリ転送をひたすら詰めていった結果……
**CPU1・LP Core・CPU0 が非同期に協調して動く、マルチノード協調型の X68000 エミュレータができました。**

> [!IMPORTANT]
> まだすべての X68000 ソフトが完全互換で動くわけではありません。表示・音・USB機器などで問題を見つけたら、再現方法やログと一緒に **優しく教えてください**。Issue歓迎です。

---

## まず試す — M5Burner

- [M5Burner 公式ページ](https://docs.m5stack.com/ja/uiflow/m5burner/intro)
- [M5Stack ダウンロード](https://docs.m5stack.com/ja/download)

M5Burner の **Share Burn** で Share Code を入力してください。

| バージョン | Share Code | 用途 |
|---|---|---|
| **Latest / Production** | `qUbdr77ZmhX8Esgo` | **通常はこちらを推奨** |
| Previous public build | `pfDbZl26Z3MsI3wP` | 直前の公開版・比較用 |
| Older public build | `aFmGCMA3FSvzcW5H` | 旧版との比較・相性確認用 (SFXVI早い)|
| Legacy public build | `xX5zvurDW6xMacAK` | さらに古い公開版 |

古い Share Code も残します。新しい版で相性問題が出たときや、以前の挙動と比較したいときに選べます。

動作デモ:

- [X68K Tab 動作デモ（X / @Layer812）](https://x.com/layer812/status/2089625598687891632)

---

## 必要なもの

- **M5Stack Tab5** — [公式ドキュメント](https://docs.m5stack.com/ja/core/Tab5)
- **microSD カード**
- 利用権を持つ X68000 ソフトウェア / ディスクイメージ
  - FDD: `XDF`, `DIM`
  - HDD: `HDS`

あると便利:

- USB キーボード
- USB Joypad / Gamepad
- USB マウス

Tab5 のタッチ UI だけでも操作できます。

---

## Build 6.15 Production — 主な特徴

更新履歴と機能説明はここへまとめました。現在の Production 版で実際に残っている構成を中心にしています。

| 領域 | Production 構成 |
|---|---|
| **68000** | Musashi + TCM / Internal SRAM dispatch cache + 実測済み hot-path / inline fast path |
| **CPU1** | 68000、割り込み、タイマ、DMAイベントなど **X68000 のゲスト時間軸**を優先。ホスト側の一時的な遅延と切り離して進行 |
| **CPU0** | 画面合成、LCD、YM2151、最終音声mix、USB、SD / Flash / HostFS、ホストUIなど最終出力側を担当 |
| **LP Core** | Async Brokerとして通知 / ACK / dirty / workset / metadata を処理。**guest memoryへ直接アクセスさせない**構成 |
| **CPU間通信** | no-wait / latest-wins を基本にした event journal / shadow / mailbox 型パイプライン |
| **Graphics** | CPU0 compositor、GRP8 paired-page shared-scroll。PIE / XespV / PPA / DMA を描画・ブロック処理に活用 |
| **LCD** | Managed Double-FB + dirty tile / partial update + `refresh_done` 同期 + latest-frame 優先 |
| **Audio** | CPU1 の guest-timed ADPCM + CPU0 YM2151 / final mix。vgmM5系 backend、44.1kHz 合成ドメイン |
| **Storage** | microSD / Flash / HostFS、XDF / DIM / HDS、Human68k 起動 |
| **Input** | USB Keyboard / Joypad / Mouse + Touch UI |
| **PANIC** | PanicPlayer を統合。Human68k 起動後の PANIC 自動起動にも対応 |

内部の多数の実験ビルド名や計測履歴は README には載せず、公開版では **6.15 Production** としてまとめています。

---

## ESP32-P4 マルチコア構成

<p align="center">
  <img src="./x68ktab_emulation_block_ja.png" alt="X68K Tab エミュレーションブロック構成" width="1100">
</p>

X68K Tab では「全部を1つのエミュレーションループで処理する」のではなく、**ゲスト時間・非同期仲介・ホスト出力**を分けています。CPU1が publish した状態変化は Ordered Shadow / Event Journal を通り、LP Core は軽量な制御・通知、CPU0 は最終的な画面・音声・I/Oを担当します。

- **HP CPU1 — Guest Time Domain**  
  X68000側の時間を進めることを最優先。68000、LSI状態、割り込み、タイマ、guest-side DMA / ADPCM event などを処理します。ホスト処理の都合でguest timeを不用意に止めないことが設計上の中心です。

- **HP CPU0 — Host Processing**  
  画面合成、LCD、FM、最終音声mix、USB、SD、HostFS、Touch UIなど、Tab5側の周辺処理と最終出力を引き受けます。

- **LP Core — Async Broker**  
  `Notify / ACK / dirty / workset / metadata` のような軽量メッセージを処理します。LP側からX68000の大きなguest memoryを直接読みに行かず、HP Core間の非同期協調を補助します。

重要なのは、**CPU0 や LP Core の都合で CPU1 の guest timeline を必要以上に止めないこと**です。

---

## PANIC Player

X68K Tab は [PanicPlayerTab5](https://github.com/Layer812/PanicPlayerTab5) から生まれました。
現在も PANIC Player を統合しており、`.PAN` データを選択して再生できます。

昔の HDD、MO、CD-R、バックアップなどに PANIC データやファイル一覧が残っていたら、情報だけでも歓迎です。

---

## CGROM / ROM / Human68k

オリジナル X68000 の CGROM dump をそのまま再配布するのではなく、[`build_cgrom.py`](build_cgrom.py) を使い、**再配布可能なフォントから互換 CGROM データを生成する方式**を採用しています。
使用するフォント自身のライセンスに従ってください。

ゲーム、OS、ROM、ディスクイメージなどの権利は各権利者に帰属します。利用権を持つデータ、または適切な条件で公開されているデータをご利用ください。

ソースからビルドする場合は、必要な Human68k 関連ファイルを各自で適切な条件に従って用意してください。詳細は [`LICENSE_SHARP_X68000.txt`](LICENSE_SHARP_X68000.txt) を参照してください。

---

## ソースからビルド

現在の開発は ESP-IDF / M5Unified を中心に行っています。
Production firmware は ESP32-P4 向けに最適化されているため、まず M5Burner 版での利用をおすすめします。

リポジトリのビルド設定・必要ファイルは今後変わる可能性があります。ソースをビルドする場合は、リポジトリ内の設定とコメントを参照してください。

---

## Credits / License

X68K Tab は多くの先人の仕事の上に成り立っています。ありがとうございます。

- [PX68K](https://github.com/hissorii/px68k)
- [Musashi](https://github.com/kstenerud/Musashi)
- [vgmM5](https://github.com/Layer812/vgmM5)
- [M5Stack](https://docs.m5stack.com/ja/core/Tab5)
- Espressif ESP32-P4 / ESP-IDF

ライセンスと第三者コードの詳細:

- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
- [`LICENSE_SHARP_X68000.txt`](LICENSE_SHARP_X68000.txt)
- [`LICENSE_PANIC_X.txt`](LICENSE_PANIC_X.txt)
- [`PANIC_V1.38_NOTICE.txt`](PANIC_V1.38_NOTICE.txt)

**X68K Tab は個人による非公式プロジェクトであり、SHARP、M5Stack その他の各社による公式製品・承認プロジェクトではありません。**
