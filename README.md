# my-versio-firmware

Noise Engineering **Versio** ユーロラックモジュール用のカスタムファームウェアです。
Versioは内部でElectrosmith社の **Daisy Seed** を使用しており、`libDaisy` / `DaisySP` を使ってC++でファームウェアを書くことができます。

## リポジトリ構成

```
my-versio-firmware/
├── firmware/               # 自作ファームウェア本体
│   ├── MyVersioFirmware.cpp
│   └── Makefile
├── libDaisy/               # ハードウェア抽象化ライブラリ (submodule)
├── DaisySP/                # DSPライブラリ (submodule)
└── README.md
```

## セットアップ

このリポジトリは `libDaisy` / `DaisySP` を git submodule として参照しています。クローン後に以下を実行してください。

```bash
git submodule update --init --recursive
```

### 必要なツールチェイン

以下がまだインストールされていません。ビルド前にセットアップしてください。

- `arm-none-eabi-gcc`（ARM用クロスコンパイラ）
- `dfu-util`（USB経由でのファームウェア書き込み用）
- `make`（インストール済み）

Electrosmithが配布している Daisy Toolchain インストーラを使うのが簡単です。

- macOS/Linux: https://github.com/electro-smith/DaisyToolchain
- あるいは Homebrew 経由: `brew install --cask gcc-arm-embedded` + `brew install dfu-util`（Homebrew自体が未インストールの場合は https://brew.sh を参照）

## ビルド

```bash
cd firmware
make
```

`build/MyVersioFirmware.bin` が生成されます。

## 書き込み（フラッシュ）

Versio背面のDaisy Seedにあるmicro-USBポートをPCに接続し、BOOTボタンを押しながらRESETボタンを押してDFUモードに入れてから：

```bash
cd firmware
make program-dfu
```

元のNoise Engineering純正ファームウェアに戻したい場合は、公式のFlashアプリを使ってください。
https://portal.noiseengineering.us/

## 現在のファームウェアの内容

`firmware/MyVersioFirmware.cpp` は、KORG ELECTRIBE R (ER-1) のOSCILLATOR / AMP / DELAYを参考にした、単発トリガーのパーカッションボイスです。信号経路は `OSC(ピッチエンベロープ付き) → AMP(エンベロープ) → DELAY(BPM同期、ピンポン)` です。

> **既知の問題**: G（8vize、OSCピッチエンベロープ量）が実機で反応しない不具合を調査中。原因切り分けのため、現在L2は一時的にGの生値をそのまま表示するデバッグ用になっている（後述）。

### ハードウェア呼称（Ruina Versioパネル準拠、実機確認済み）

このプロジェクトでは、Ruina Versioのパネル印字に合わせて以下の呼称を使う。

| 呼称 | パネル表記 | パラメータ | ファームウェア上の実体 |
|---|---|---|---|
| A | Blend | OSC 基本ピッチ (30Hz〜300Hz) | `KNOB_0` |
| B | Center | DELAY タイム切替時のグライドタイム | `KNOB_6` |
| C | Phase | DELAY フィードバック量（上限0.985） | `KNOB_4` |
| D | Fold | OSC ピッチディケイタイム | `KNOB_2` |
| E | DOOM | AMP ディケイタイム | `KNOB_3` |
| F | Drive | DELAY ミックス (Dry/Wet) | `KNOB_5` |
| G | 8vize | OSC ピッチエンベロープ量 (0〜4オクターブ) | `KNOB_1` |
| T1 | UND/X/OVR（3ポジショントグル） | DELAY分周比切替 | `SW_0` |
| T2 | OFF/ON/TRK（3ポジショントグル） | OSC波形切替 | `SW_1` |
| X-SW | Smooshのボタン部分 | タップテンポ | `hw.tap` / `SwitchPressed()` |
| X-IN | Smooshのジャック部分 | ドラムのトリガー | `hw.gate` |
| INL | Audio In L | 外部クロック入力（しきい値検出） | - |
| INR | Audio In R | (未使用) | - |
| OUTL / OUTR | Audio Out L/R | ステレオ出力（現状L/R同一信号） | - |
| L1〜L4 | 4つのRGB LED | 下記LED節を参照 | `LED_0`〜`LED_3` |

Smoosh（X-SW / X-IN）はRuina Versioパネル上では1つの表記だが、ボタンとジャックは電気的に別回路になっており、本ファームウェアでは別々の用途に割り当てている。

### トリガー

**X-IN**（Smooshのジャック、ゲート/トリガー入力）の立ち上がりで発音します。

### テンポ

DELAYタイムは`TEMPO_BPM`（`MyVersioFirmware.cpp`冒頭、現在136）を初期値とし、以下のどちらかで上書きできる。**後から入力があった方が優先される**（明示的な切替スイッチは無い、シンプルな「最後に触った方が勝つ」方式）。

- **X-SW**（Smooshのボタン）: タップテンポ。2回連続で叩くと、その間隔をテンポとして採用する
- **INL**（Audio In L）: 外部クロック入力。オーディオ入力へパルス列（ゲート/トリガー信号）を送ると、しきい値を超えた立ち上がりの間隔をテンポとして採用する。INLはAC結合のオーディオ入力なので厳密なCV入力ではないが、クロックパルスのような短い信号であれば問題なく検出できる

どちらも極端に短い/長い間隔（20〜1200BPM相当の範囲外）は誤操作・ノイズとみなして無視する。

### トグルスイッチ

- **T1**（3ポジション）: DELAYタイムの分周比（4分 / 8分 / 16分音符）
- **T2**（3ポジション）: OSC波形（Sine / Square / **Clap**）

分周比を切り替えた瞬間、ディレイタイムは即座に切り替わらず指定のグライドタイムでなめらかに遷移する。これによりフィードバックしている音のピッチが一瞬揺れる、テープ/BBDディレイのような効果が得られる。

### DELAY（ピンポン）

L/Rそれぞれに独立したディレイラインを持ち、`kDelayCrossFeed`（`MyVersioFirmware.cpp`、現在0.75）の比率で左右をクロスフィードする「ピンポンディレイ」構成。ドライ音（原音）は常にL/R均等（センター）。1.0未満にしているため、繰り返すたびに左右の広がりが少しずつセンターへ収束しながら減衰していく（1.0にすると収束せず完全なピンポンになる）。

### Clap（旧Noise）

T2をClapにすると、OSCがホワイトノイズ→バンドパスフィルター（中心1000Hz、低レゾナンス）を通った柔らかめの音になる。トリガー直後に短いノイズバーストを3回連打（808/909系ハンドクラップの模倣、角は少し丸めてクリック感を軽減）し、その後は通常通りE（AMPディケイ）でテールが減衰する。Clap時はA/D/G（ピッチ関連）は使われない。

### LED

- L1: AMPエンベロープの発音インジケーター
- L2: **[デバッグ中]** Gの生値をそのまま白色の明るさで表示（本来はOSC波形インジケーター。Gの不具合切り分けが済み次第、元に戻す）
- L3: 現在のDELAY分周比（4分=赤 / 8分=緑 / 16分=青、T1で切替）
- L4: DELAYの状態表示。色（緑→赤）でフィードバック量、明滅速度でディレイタイム（分周比のテンポ）を表す

## 参考リンク

- [libDaisy (GitHub)](https://github.com/electro-smith/libDaisy)
- [DaisySP (GitHub)](https://github.com/electro-smith/DaisySP)
- [Daisy Wiki (Getting Started)](https://github.com/electro-smith/DaisyWiki/wiki)
- [DaisyExamples - versio](https://github.com/electro-smith/DaisyExamples/tree/master/versio)
- [Noise Engineering: Create your own firmware on a Versio module](https://noiseengineering.us/blogs/loquelic-literitas-the-blog/create-your-own-firmware-on-a-versio-module/)
- [Daisy Forum](https://forum.electro-smith.com/)
