# X68K Tab

[**English README → README_en.md**](README_en.md)

<p align="center">
  <img src="./x68t.gif" alt="X68K Tab running on M5Stack Tab5" width="760">
</p>

<p align="center">
  <b>X68000 Emulator for M5Stack Tab5 / ESP32-P4</b><br>
  Developed by Layer812 &nbsp;|&nbsp; Based on PX68K &nbsp;|&nbsp; 68000 core: Musashi
</p>

## X68K Tab とは

**X68K Tab** は、M5Stack Tab5（ESP32-P4）で動作するポータブル X68000 エミュレータです。

PX68K / Musashi をベースに、ESP32-P4 の 2つの HP CPU、LP Core、PSRAM、MIPI-DSI、USB Host、microSD、内蔵オーディオを活用する構成へ再設計しています。

もともとは、昔の X68000 **PANIC** データを M5Stack Tab5 で再生したくなり、専用プレイヤー [PanicPlayerTab5](https://github.com/Layer812/PanicPlayerTab5) を作っていました。

PANIC を再生するために必要な X68000 互換環境を少しずつ実装していったのですが、

**肝心の PANIC データを１個しか持っていませんでした.....**  
というわけで(?)、まだ見ぬ PANIC データのために CPU、画面、音、I/O、メモリ転送を詰めていった結果、CPU1・LP Core・CPU0 が協調して動く X68000 エミュレータになりました。

> [!IMPORTANT]
> すべての X68000 ソフトウェアの完全互換を保証するものではありません。  
> 表示、音声、入力、USB機器、ディスクなどで問題を見つけたら、再現方法とシリアルログを添えて Issue で教えてください。

---

## Current Production

現在の Production 版では、**68000 guest CPU を 12 MHz、X68000 peripheral time domain を exact 10 MHz** としています。

### リリース方針

このビルドを、X68K Tab の**現在の正式リリース版**とします。

ここまでで基本機能・速度・音声・表示・入力・ストレージを一旦 Production としてまとめました。今後は大きく仕様を動かすことよりも、実際の X68000 ソフトウェアや周辺機器で見つかった**互換性問題、回帰、不具合の修正**を中心に進める予定です。

もちろん、互換性や安定性のために必要な改善は引き続き行いますが、現在の 12 MHz / exact 10 MHz / no-wait 構成を基準点として扱います。

| 項目 | Production 構成 |
| --- | --- |
| Guest CPU | **12 MHz** |
| Peripheral domain | **exact 10 MHz** |
| Guest RAM | **12 MiB** |
| CPU1 | 68000 / guest device time を優先。host-side の都合で待たせない |
| CPU0 | video / LCD / YM2151 / final audio mix / USB / storage / host UI |
| Inter-core | no-wait / latest-wins を基本とする非同期パイプライン |
| Graphics | sparse dirty update、必要箇所だけを current Front FB へ反映 |
| Audio | guest-timed ADPCM + CPU0 YM2151 / final mix |
| Storage | microSD / Flash / HostFS、XDF / DIM / HDS |
| Input | USB Keyboard / Joypad / Mouse + Touch UI |
| PANIC | PanicPlayer 統合、PANIC データ選択・起動に対応 |

内部で使用した多数の計測ビルド名や A/B テスト名は、公開 README には掲載しません。

### Turbo

画面上の `TURBO` ボタンは3段階です。

| 表示 | モード | Audio source rate | Video |
| --- | --- | ---: | --- |
| BLACK | NORMAL | 44.1 kHz | 通常 |
| GREEN | TURBO | 22.05 kHz | adaptive 15 / 20 / 24 fps |
| RED | RED TURBO | 11.025 kHz | stronger host-work shedding / MAX 24 fps |

Turbo は **guest CPU clock を変更しません**。68000 は常に 12 MHz、peripheral domain は exact 10 MHz のままです。

---

## まず試す — M5Burner

- [M5Burner 公式ページ](https://docs.m5stack.com/en/uiflow/m5burner/intro)
- [M5Stack ダウンロード](https://docs.m5stack.com/en/download)

M5Burner の **Share Burn** で Share Code を入力してください。

| バージョン | Share Code | 用途 |
| --- | --- | --- |
| **Latest / Production Release** | `KJL8QIk1H35dtT7O` | **今回の正式リリース版 / 通常はこちらを推奨** |
| Previous public build | `wUgYltOEbYF7mrBn` | 直前の公開版・比較用 |
| Older public build | `qUbdr77ZmhX8Esgo` | 比較・互換確認用 |
| Older public build | `pfDbZl26Z3MsI3wP` | 比較・互換確認用 |
| Older public build | `aFmGCMA3FSvzcW5H` | 比較・互換確認用 |
| Legacy public build | `xX5zvurDW6xMacAK` | 旧公開イメージ |

> 新しい M5Burner イメージを公開する場合は、付属の公開フォルダ作成スクリプトの第2引数に新しい Share Code を渡すと、日本語・英語 README の Latest 行を同時に更新できます。

---

## 必要なもの

- [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5)
- microSD カード
- 正当に利用できる X68000 ソフトウェア / ディスクイメージ
  - FDD: `XDF`, `DIM`
  - HDD: `HDS`

### 入力機器

- [**M5Stack Tab5用キーボード**](https://www.switch-science.com/products/11257)
- USB キーボード
- USB Joypad / Gamepad
- USB マウス
- 画面上のタッチ UI / バーチャルキーボード / Joypad

外付け入力機器がなくても、タッチ UI から基本操作ができます。

---

## Display / MULTISCAN

X68K Tab は guest 側の CRTC 状態から **15 kHz / 24 kHz / 31 kHz 系モードを自動判定**し、Tab5 の 1280×720 LCD へ変換して表示します。

X68000 の走査周波数そのものを外部へ出力するものではありません。Tab5 の LCD は固定出力で、guest video mode を内部で変換します。

---

## Audio

CPU1 は X68000 側の guest-timed audio event を進め、CPU0 が YM2151 波形生成、ADPCM との final mix、speaker 出力を担当します。

host-side の一時的な表示負荷で guest timeline を止めないことを優先し、audio reserve が不足した場合は画面側の host work を抑える Audio Guard を使用します。

---

## ESP32-P4 マルチコア構成

<p align="center">
  <img src="./x68ktab_emulation_block_ja.png" alt="X68K Tab ESP32-P4 multi-core architecture" width="1100">
</p>

- **HP CPU1 — Guest Time Domain**  
  68000、割り込み、タイマ、guest-side DMA、CRTC、audio event など X68000 側の時間を優先して進めます。

- **HP CPU0 — Host Processing**  
  画面合成、LCD、YM2151、final audio mix、USB、SD / Flash / HostFS、Touch UI を担当します。

- **LP Core — Lightweight Broker**  
  一部の軽量 notification / metadata / broker 処理を補助します。guest memory を直接大規模に走査する役割にはしません。

中心となる原則は、**host-side の一時的な遅れを理由に CPU1 の guest timeline を不必要に止めないこと**です。

---

## PANIC Player

X68K Tab は [PanicPlayerTab5](https://github.com/Layer812/PanicPlayerTab5) から発展したプロジェクトです。PANIC Player 機能は現在も統合されています。

### PANICデータを探しています

昔の X68000 メディアがお手元にありましたら、

- `.PAN` ファイル
- PANIC データ入りの LZH / ZIP
- MO / HDD / CD-R のバックアップ
- BBS のファイル一覧
- README / DOC
- 覚えているファイル名

などの情報を歓迎します。

著作権等の理由でデータそのものを共有できない場合は、**ファイル名やディレクトリ一覧だけでも大変助かります。**

---

## ROM / CGROM / Human68k

本リポジトリには、SHARP X68000 のオリジナル ROM dump、Human68k のディスクイメージ、ユーザー所有の X68000 ソフトウェアを含めません。

CGROM については、オリジナル CGROM dump を再配布するのではなく、`build_cgrom.py` を用意しています。使用するフォントや生成物については、それぞれのライセンス・権利条件に従ってください。

Human68k 関連ファイルを必要とする場合も、適用される条件に従って各自で正当に用意してください。

詳細:

- `LICENSE_SHARP_X68000.txt`
- `LICENSE_PANIC_X.txt`
- `PANIC_V1.38_NOTICE.txt`
- `THIRD_PARTY_NOTICES.md`

---

## Source Build

開発・実機確認は ESP32-P4 / M5Stack Tab5 を中心に行っています。

現在の Production 基準:

- ESP-IDF 5.5.x 系
- ESP32-P4 360 MHz
- PSRAM 32 MiB / 200 MHz
- Flash QIO / 80 MHz
- M5Unified / M5GFX
- Musashi
- PX68K

ソースツリーには PlatformIO / ESP-IDF 用の設定を含みます。ローカルに必要な ROM / OS / disk image 等はリポジトリへ追加しないでください。

---

## Credits / License

X68K Tab は、多くのエミュレータ、ハードウェア研究、OSS の成果の上に成り立っています。

- PX68K
- Musashi
- vgmM5
- M5Stack / M5Unified / M5GFX
- Espressif ESP32-P4 / ESP-IDF

各ライセンス・third-party notice はリポジトリ内の文書を確認してください。

X68K Tab は個人による非公式プロジェクトです。SHARP、M5Stack その他各権利者の公式・公認・スポンサー製品ではありません。
