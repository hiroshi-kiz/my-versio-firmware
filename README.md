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

`firmware/MyVersioFirmware.cpp` は、KORG ELECTRIBE R (ER-1) のOSCILLATOR / AMP / DELAYを参考にした、単発トリガーのパーカッションボイスです。信号経路は `OSC(ピッチエンベロープ付き) → AMP(エンベロープ) → DELAY(BPM同期)` です。

### トリガー

ゲート入力（またはパネルの momentary ボタン）の立ち上がりで発音します。

### ノブ

| ノブ | パラメータ |
|---|---|
| KNOB_0 | OSC 基本ピッチ (30Hz〜300Hz) |
| KNOB_1 | OSC ピッチエンベロープ量 (0〜4オクターブ) |
| KNOB_2 | OSC ピッチディケイタイム |
| KNOB_3 | AMP ディケイタイム |
| KNOB_4 | DELAY フィードバック量 |
| KNOB_5 | DELAY ミックス (Dry/Wet) |
| KNOB_6 | DELAY タイム切替時のグライドタイム |

### トグルスイッチ

- **SW_0**（3ポジション）: DELAYタイムの分周比（4分 / 8分 / 16分音符）。BPMは`MyVersioFirmware.cpp`冒頭の`TEMPO_BPM`定数で固定（現在136）。外部クロックには依存しないので、変更したい場合はこの定数を書き換えて再ビルド・再書き込みする。
- **SW_1**（3ポジション）: OSC波形（Sine / Square / Noise）

分周比を切り替えた瞬間、ディレイタイムは即座に切り替わらず指定のグライドタイムでなめらかに遷移する。これによりフィードバックしている音のピッチが一瞬揺れる、テープ/BBDディレイのような効果が得られる。

### LED

- LED_0: AMPエンベロープの発音インジケーター
- LED_1: 現在のOSC波形（Sine=赤 / Square=緑 / Noise=青）
- LED_2: 現在のDELAY分周比（4分=赤 / 8分=緑 / 16分=青）
- LED_3: DELAYの状態表示。色（緑→赤）でフィードバック量、明滅速度でディレイタイム（分周比のテンポ）を表す

## 参考リンク

- [libDaisy (GitHub)](https://github.com/electro-smith/libDaisy)
- [DaisySP (GitHub)](https://github.com/electro-smith/DaisySP)
- [Daisy Wiki (Getting Started)](https://github.com/electro-smith/DaisyWiki/wiki)
- [DaisyExamples - versio](https://github.com/electro-smith/DaisyExamples/tree/master/versio)
- [Noise Engineering: Create your own firmware on a Versio module](https://noiseengineering.us/blogs/loquelic-literitas-the-blog/create-your-own-firmware-on-a-versio-module/)
- [Daisy Forum](https://forum.electro-smith.com/)
