# X68K Tab

**X68K Tab** は、M5Stack Tab5（ESP32-P4）上で SHARP X68000 環境を動作させることを目的とした、ポータブル X68000 エミュレータ／実験プロジェクトです。

PX68K をベースに、ESP32-P4 のデュアルコア、広い PSRAM、LCD DMA、専用ピクセル処理系、タッチパネル、USB Host、内蔵オーディオなど、Tab5 が持つハードウェアをできるだけ直接活用する方向で移植・最適化しています。

単に「PX68K を ESP32-P4 でコンパイルする」ことが目標ではありません。

X68000 の **ゲスト時間軸を守る処理** と、画面・音声・入力・ストレージなどの **ホスト側処理** を分離し、さらに描画、メモリ転送、FM 音源生成などを ESP32-P4 向けに最適化することで、組み込み機器上でも X68000 らしい動作感を維持しながら高い実行効率を狙っています。

> [!IMPORTANT]
> X68K Tab は現在も開発中です。
> 互換性、速度、音声、描画、USB 周辺機器などは継続して改善しています。
> 現時点では「完成した汎用 X68000 エミュレータ」というより、実機互換性と ESP32-P4 固有最適化を同時に進めている開発版です。

---

## 特徴

- M5Stack Tab5 / ESP32-P4 専用設計
- X68000 / MC68000 系エミュレーション
- PX68K 系コードをベースにした移植
- ESP32-P4 デュアルコアを利用した役割分担
- CPU1：X68000 のゲスト時間軸、68000 実行、割り込み・タイマ等を担当
- CPU0：画面、FM 音源、最終音声ミックス、USB、タッチ、LCD、ストレージ等を担当
- CPU 間は SPM / Mailbox を意識した非同期通信
- ESP32-P4 のピクセル処理／ベクトル処理を使った描画高速化
- LCD DMA を使った画面転送
- vgmM5 系 YM2151 バックエンドによる FM 音源生成
- ADPCM 対応
- USB キーボード / マウス / JoyPAD 対応を開発中
- microSD / Flash / HostFS を利用したストレージ構成
- FDD イメージおよび HDD イメージからの起動
- Human68k 起動経路の実装・検証
- タッチ UI を利用したポータブル X68000 環境

---

## M5Stack Tab5 について

M5Stack Tab5 は ESP32-P4 を搭載したポータブル開発端末です。

主なハードウェアは次のとおりです。

- ESP32-P4 / 32-bit RISC-V デュアルコア
- 最大 360 MHz
- 16 MB Flash
- 32 MB Octal PSRAM
- 5 inch IPS TFT
- 1280 × 720
- capacitive touch panel
- microSD
- USB Type-A Host
- USB Type-C OTG
- ES8388 audio codec
- built-in speaker

一般的な ESP32 よりも大きなメモリと高い演算性能を持ち、さらに 720p LCD、USB Host、オーディオ、microSD が一体化されているため、ポータブル X68000 環境を作るターゲットとして非常に面白いハードウェアです。

M5Stack Tab5 official documentation:

https://docs.m5stack.com/ja/core/Tab5

---

## アーキテクチャ

X68K Tab では、X68000 エミュレーションを単一の巨大なループとして処理するのではなく、ESP32-P4 の 2 つの高性能 CPU コアを役割別に利用しています。

```text
                    X68K Tab Emulator

        +---------------------------------------+
        |                                       |
        |   CPU1                  CPU0           |
        |   Guest timeline        Host services  |
        |                                       |
        |   68000 execution       Video compose  |
        |   LSI state             YM2151 / FM    |
        |   IRQ / timers          Final mix      |
        |   ADPCM generation      USB / Touch    |
        |                         SD / Flash      |
        |          <---- Mailbox ---->           |
        +---------------------------------------+
              |          |          |          |
              v          v          v          v
             PIE      LCD DMA   SRAM/PSRAM    vgmM5
```

アーキテクチャ図をリポジトリへ配置する場合は、例えば次のように README から参照できます。

```markdown
![X68K Tab architecture](docs/x68k_tab_architecture.png)
```

### CPU1 — Guest time axis

CPU1 は、X68000 側から見た「時間」をできるだけ一貫させることを重視しています。

主な担当は次のとおりです。

- 68000 実行
- 各 LSI の状態管理
- 割り込み
- タイマ
- DMA に関係するゲスト側イベント
- ADPCM 生成
- X68000 側のサイクル進行

画面描画や USB 処理の都合で 68000 の実行が不規則に止まることを避け、ゲスト側の時間軸をできるだけ独立させることが狙いです。

### CPU0 — Host processing

CPU0 は Tab5 側のサービスと最終出力を担当します。

- グラフィック合成
- LCD 出力
- YM2151 / FM 音源生成
- FM + ADPCM の最終ミックス
- スピーカー出力
- USB Host
- キーボード / マウス / JoyPAD
- タッチ UI
- SD / Flash / HostFS
- ESP-IDF / M5Stack 周辺処理

ゲスト時間軸に直接必要のない重い処理を CPU0 側へ逃がすことで、CPU1 が X68000 のエミュレーションに集中できる構造を目指しています。

### CPU 間通信

CPU0 / CPU1 間では、可能な限り同期的な巨大ロックを避け、Mailbox 的なメッセージ交換と共有バッファを使う設計にしています。

重要なのは、

**「CPU0 の仕事が少し遅れても CPU1 のゲスト時間軸を必要以上に止めない」**

ことです。

音声についても、サンプル生成と最終ミックス／出力を分離し、共有バッファの受け渡しを軽量化する方向で調整しています。

---

## グラフィック高速化

X68000 の表示処理は、単純な framebuffer だけではありません。

- Graphic VRAM
- Text VRAM
- BG
- Sprite
- palette
- priority
- transparency
- scroll
- CRT timing

などを組み合わせる必要があります。

PC 上では CPU パワーで処理できる部分でも、組み込み SoC では画面合成が大きな負荷になるため、X68K Tab では ESP32-P4 のハードウェア機能を積極的に利用します。

### PIE エンジン

現在、描画処理の一部について P4 向け高速経路を実装・評価しています。

想定している代表的な処理は次のとおりです。

- 128-bit 並列ピクセル処理
- パレット展開のベクトル化
- ブランチを減らした透過判定
- RGB565 合成
- key-color overlay
- copy / fill / blend
- 差分描画
- メモリブロック転送
- BitBLT 的処理
- Sprite / BG 合成支援

現行実装では、すべてを無条件にハードウェア経路へ通すのではなく、**結果が C 実装と完全一致し、実際に速い場合だけ高速経路を採用する** 方針を取っています。

一部の描画経路では A/B ベンチマークを行い、画像結果の一致と速度を確認したうえで P4 向け経路を選択し、条件を満たさない場合には安全な C 実装へフォールバックします。

これは速度だけでなく、X68000 の表示互換性を壊さないための重要な設計方針です。

> PIE は 68000 命令そのものを置き換える JIT として使うのではなく、描画、メモリ、変換、比較など、エミュレーション周辺の大量データ処理を高速化するアクセラレータとして活用します。

### LCD DMA

画面の完成後は LCD DMA による転送を利用します。

目標は、

- non-blocking transfer
- double buffering
- frame buffer flip
- CPU と LCD 転送の並列化
- メモリ帯域の削減

です。

CPU が LCD へのピクセル転送を待ち続ける構造を避け、次フレームのエミュレーション／合成と LCD 転送を重ねられる構成を目指しています。

---

## SRAM / PSRAM の使い分け

Tab5 には 32 MB の PSRAM があり、X68000 エミュレーションでは非常に大きな助けになります。

一方、すべてを PSRAM に置けば速くなるわけではありません。

アクセス頻度に応じて、

- internal SRAM：頻繁に触る小さな working set
- PSRAM：VRAM、frame buffer、大きなリングバッファ、ディスク／画像用領域
- SPM / Mailbox：CPU 間通知

のように役割を分ける方向で最適化しています。

特に音声では、1 sample ごとに PSRAM へ細かく書き込むようなパターンを避け、DRAM 側の scratch buffer を経由してまとめて扱うことで、メモリ待ちによる CPU 負荷を抑えています。

今後は cache line alignment、burst access、shared buffer の粒度などもさらに詰めていく予定です。

---

## サウンド

X68K Tab では FM と ADPCM を別々に生成し、最終段でミックスして Tab5 のオーディオへ送ります。

### YM2151 / vgmM5 engine

YM2151 (OPM) は vgmM5 系のバックエンドを利用し、ESP32-P4 向けに軽量な FM 合成経路を構築しています。

現在の開発版では 44.1 kHz 系の生成経路を動作させながら、さらに次の方向へ移行・最適化を進めています。

- branchless matrix operation
- 32-bit fixed-point DDS
- native-rate generation
- YM2151 の時間軸を崩さない生成
- FM 演算の CPU 負荷削減
- PSRAM 書き込み量削減
- FM / ADPCM の効率的な空間ミックス

将来的には YM2151 のネイティブ生成レートを意識した約 53.2 kHz 系の処理も含め、**高精度でありながら軽量な FM エンジン** を目指します。

速度だけを優先して音を近似するのではなく、X68000 の音楽で重要なタイミング、エンベロープ、FM の響きを維持することを重視しています。

### ADPCM

ADPCM はゲスト側時間軸との結び付きが強いため、CPU1 側で生成し、CPU0 側の最終オーディオ処理へ受け渡す構成を基本としています。

FM と ADPCM の生成タイミングを無理に同じタスクへ押し込まず、それぞれを適した場所で処理することで、音切れや watchdog 発生を抑えながら CPU 負荷を分散します。

---

## ストレージと起動

X68K Tab では複数の起動経路を扱えるようにしています。

### microSD

主なディスクイメージやユーザーデータを保存します。

現在の開発では次のような経路を検証しています。

- FDD image
- XDF
- DIM
- HDD / HDF
- HostFS
- FAT32 上のファイルアクセス

### Flash quick boot

Human68k を含む起動環境について、毎回 SD 上の OS を最初から読むだけでなく、Flash 側から素早く起動し、SD の FAT32 領域をデータドライブとして利用する構成も開発しています。

これにより、ポータブル端末として電源投入から短時間で X68000 環境へ入れることを目標にしています。

### HDD boot

HDD ブートについては IPL から HD0 を選択し、HDD IPL を RAM へロードして Human68k の読み込みへ進む経路まで動作確認を進めています。

SCSI については、ソフトウェアから必要となる高レベルの経路を段階的に実装していますが、実機の SCSI バス信号／全フェーズを電気的・低レベルに完全再現することを現時点の目標にはしていません。

---

## 入力

Tab5 を単なる表示装置ではなく、単体で遊べる X68000 端末にすることも X68K Tab の目標です。

現在、以下を対象にしています。

- capacitive touch
- on-screen UI
- USB keyboard
- USB mouse
- USB gamepad / joystick

USB JoyPAD の disconnect / reconnect など、実機器依存の挙動については引き続き回帰試験と改善を行っています。

---

## 現在の開発状況

2026 年 8 月時点では、プロジェクトは RC 相当の開発段階です。

主な回帰確認項目は次のとおりです。

| Area | Status |
|---|---|
| IPL boot | Working / testing |
| Human68k boot | Working / testing |
| HDD / HDF boot | Working / testing |
| FDD XDF / DIM | Working / testing |
| HostFS / SD FAT32 | Working / testing |
| Flash quick boot | Working / testing |
| Graphic VRAM | Working, optimization ongoing |
| Text / BG / Sprite composition | Working, optimization ongoing |
| 256 / 512 / wide graphic modes | Regression testing |
| LCD output | Working |
| YM2151 FM | Working, optimization ongoing |
| ADPCM | Working |
| FM + ADPCM mix | Working, tuning ongoing |
| USB keyboard | Working / testing |
| USB mouse | Working / testing |
| USB JoyPAD | Working / reconnect testing |
| Touch UI | Working / evolving |
| PIE acceleration | Partial / expanding |
| Stability | Continuous regression testing |

ゲームやデモによって X68000 のハードウェアの使い方が大きく異なるため、「Human68k が起動する」ことと「X68000 全ソフトが完全互換で動作する」ことは別だと考えています。

動作報告、再現しないタイトル、表示化け、音の違い、入力デバイスの相性などの情報は歓迎です。

---

## 今後の高速化予定

X68K Tab では、ESP32-P4 のクロックを上げるだけではなく、**処理そのものを P4 の構造に合わせる** ことを重視しています。

### Graphics

- PIE 対象処理の拡大
- palette conversion の vector 化
- branchless transparency
- Sprite / BG overlay の高速化
- dirty rectangle / diff rendering
- page / scroll access の最適化
- VRAM fetch の burst 化
- RGB565 合成の高速化
- C fallback と高速経路の自動選択

### CPU / bus

- 68000 instruction dispatch の hot-path 最適化
- memory map fast-path
- ROM / RAM / VRAM access の分岐削減
- read/write callback overhead の削減
- interrupt / event queue の軽量化
- CPU1 の guest timeline jitter 削減

### Multi-core

- SPM / Mailbox の活用範囲拡大
- lock-free / low-lock buffer
- CPU0 / CPU1 間のコピー削減
- 非同期 producer / consumer 化
- audio / video task の blocking 削減

### Memory

- frequently-used tables の internal SRAM 化
- PSRAM burst access
- cache-line alignment
- framebuffer / VRAM layout 改善
- audio scratch buffer 最適化

### Audio

- vgmM5 FM engine のさらなる軽量化
- complete 32-bit fixed-point DDS path
- native-rate YM2151 generation
- FM / ADPCM mixer 最適化
- resampling コスト削減
- audio buffer latency の短縮
- underflow recovery 改善

### LCD

- DMA と frame generation の並列化
- double buffer / flip 最適化
- unnecessary copy の削減
- partial update の評価

---

## ビルド

現在の開発環境は PlatformIO + ESP-IDF / M5Unified 系です。

例：

```bash
pio run -e m5stack-tab5
```

書き込み：

```bash
pio run -e m5stack-tab5 -t upload
```

シリアルモニタ：

```bash
pio device monitor -b 115200
```

開発中はメモリ配置や ESP-IDF / M5Unified の更新によってビルド条件が変わる可能性があります。

安定版を公開する段階で、推奨 PlatformIO / ESP-IDF バージョンを固定する予定です。

---

## ROM / OS / ディスクイメージについて

X68K Tab は X68000 のソフトウェア資産を利用するためのエミュレータです。

ゲーム、アプリケーション、ディスクイメージなどの著作権は、それぞれの権利者に帰属します。

本プロジェクトを利用する場合は、使用する ROM、OS、ゲーム、データ等について、それぞれの配布条件・使用許諾に従ってください。

市販ゲーム等の無断配布を目的とするプロジェクトではありません。

---

# 著作権・ライセンス・謝辞

X68K Tab は、長年にわたって作られてきた X68000 エミュレーション技術、資料、ソフトウェア、そして現在の ESP32 / M5Stack エコシステムの上に成り立っています。

各作者・開発者・資料を残してくださった皆様に深く感謝します。

## SHARP X68000

X68000 はシャープ株式会社が開発したコンピュータです。

X68K Tab では、シャープ・プロダクツ・ユーザーズ・フォーラム（FSHARP）を通して無償公開された X68000 用システムソフトウェアを利用する場合があります。

これらのソフトウェアを利用・収録・再配布する場合は、当時公開された使用許諾条件に従います。

本リポジトリでは、適用されるオリジナルの許諾条件を次のファイルへ原文のまま収録する予定です。

```text
LICENSE_SHARP_X68000.txt
```

SHARP 由来ソフトウェアの使用・複製・改変・再配布条件については、README の要約ではなく **必ずオリジナルの許諾文を優先して参照してください**。

特に、当時の許諾文には、X シリーズおよびそのエミュレータ上での使用、複製・解析・改変、無償配布、改変成果物のソース公開、再頒布時の許諾文添付などについて条件が定められています。

X68K Tab が SHARP 由来ソフトウェアを含む形で配布される場合も、この条件を尊重し、対象物を有償販売することを前提としません。

参考：X68000 LIBRARY / SHARP ソフトウェア

https://retropc.net/x68000/software/sharp/

SHARP、X68000、および関連するソフトウェア、名称、商標等の権利は、シャープ株式会社および各権利者に帰属します。

**X68K Tab は個人による非公式プロジェクトであり、シャープ株式会社とは関係なく、同社による承認・協賛を受けたものではありません。**

## PX68K

X68K Tab は PX68K をベースとして M5Stack Tab5 / ESP32-P4 へ移植・再構成しているプロジェクトです。

PX68K は、WinX68k、xkeropi など X68000 エミュレータの長い開発系譜を受け継いだポータブル X68000 エミュレータです。

hissorii さんをはじめ、WinX68k / xkeropi / PX68K の開発、移植、解析、資料整備に関わった皆様に感謝します。

PX68K:

https://github.com/hissorii/px68k

X68K Tab で変更していない第三者コードについては、元の著作権表示および配布条件を維持してください。

また、PX68K 自体が複数の先行実装を取り込んできたプロジェクトであるため、公開時には各ソースファイルのヘッダ、同梱ドキュメント、upstream の記載を確認し、それぞれの条件を尊重します。

## WinX68k / xkeropi / fmgen and contributors

PX68K の upstream には、WinX68k、xkeropi、68000 CPU エミュレーション実装、fmgen など、多くの開発者による成果が含まれています。

これらを作り、公開し、長年維持してきた皆様に感謝します。

X68K Tab ではオーディオや画面処理の一部を ESP32-P4 向けに置き換えていますが、X68000 のハードウェア挙動を理解し移植するうえで、これらの実装と資料は非常に重要な基盤となっています。

## vgmM5 / YM2151 work

X68K Tab の FM 音源では、vgmM5 系 YM2151 バックエンドを ESP32-P4 向けに統合・最適化しています。

FM 音源を組み込み CPU 上で軽量かつ高精度に再生するための実装、検証、知見を公開してきた開発者の皆様に感謝します。

vgmM5 に由来するコードをリポジトリへ含める場合は、そのコードに適用される著作権表示およびライセンスを `THIRD_PARTY_NOTICES.md` 等へ明記してください。

## M5Stack

M5Stack、M5Stack Tab5、M5Unified、M5GFX、M5Burner および関連名称は M5Stack Technology Co., Ltd. の製品・プロジェクトです。

X68K Tab は M5Stack とは独立したプロジェクトです。

M5Stack Tab5:

https://docs.m5stack.com/ja/core/Tab5

M5Unified:

https://github.com/m5stack/M5Unified

## Espressif

ESP32-P4、ESP-IDF および関連技術を開発・公開している Espressif Systems に感謝します。

X68K Tab では ESP32-P4 のデュアルコア、DMA、メモリシステム、LCD 周辺機能、各種ハードウェアアクセラレーションを積極的に利用しています。

---

## このリポジトリ自身のライセンスについて

X68K Tab は複数の upstream ソフトウェアを含む／参照するため、**リポジトリ内のすべてのファイルに単一のライセンスが一律適用されるとは限りません**。

公開時には、少なくとも次のように整理することを想定しています。

```text
LICENSE
LICENSE_SHARP_X68000.txt
THIRD_PARTY_NOTICES.md
```

- X68K Tab 固有の新規コード：`LICENSE` に記載するプロジェクトライセンス
- PX68K および upstream 由来コード：各ファイル／upstream の条件
- SHARP 公開ソフトウェア：`LICENSE_SHARP_X68000.txt`
- その他ライブラリ：各ライブラリのライセンス

第三者コードの copyright / license header は削除しないでください。

> [!NOTE]
> 公開前に、実際にリポジトリへ含めるソースとバイナリを基準として `THIRD_PARTY_NOTICES.md` を最終確認する予定です。
> 特に SHARP 公開ソフトウェアを release firmware に同梱する場合は、オリジナルの許諾条件を必ず添付します。

---

## プロジェクトの目標

X68K Tab の目標は、ベンチマークの数字だけを大きくすることではありません。

X68000 のソフトウェアが期待している時間、画面、音、割り込み、入力の関係を保ちながら、1980～90 年代のコンピュータを 2020 年代の組み込み SoC の構造に再配置することです。

MC68000 を CPU1 で走らせ、CPU0 が画面と音と周辺処理を引き受け、PIE がピクセル処理を助け、DMA が表示を運び、vgmM5 が FM 音源を生成する。

そうすることで、小さな Tab5 の中に **「ちゃんと X68000 らしく動く環境」** を作れないか試しています。

まだ完成ではありませんが、少しずつ実機らしさと速度の両方を詰めていきます。

---

## 動作報告・Issue・Pull Request

動作報告は歓迎です。

特に次の情報があると解析しやすくなります。

- 使用した X68000 software / game / demo
- boot media type
- XDF / DIM / HDF 等の種類
- 画面モード
- 発生した症状
- 再現手順
- serial log
- 可能であれば画面写真／動画
- USB デバイスの場合は製品名

X68000 はソフトごとにハードウェアの使い方がかなり違うため、「このタイトルだけおかしい」という報告も非常に重要です。

Issues / Pull Requests も歓迎します。

---

## 最後に

PX68K を作った方々、さらにその前の WinX68k / xkeropi を作った方々、X68000 の解析情報を残してくださった方々、SHARP / FSHARP を通じて当時のソフトウェア資産を公開してくださった皆様、そして現在の ESP32-P4 / M5Stack エコシステムを作っている皆様に感謝します。

30 年以上前の X68000 を、手のひらサイズの RISC-V マシン上で、当時とはまったく違うアーキテクチャを使ってもう一度動かす。

**X68K Tab は、その過程そのものを楽しむためのプロジェクトでもあります。**
