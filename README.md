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

`firmware/MyVersioFirmware.cpp` は最小構成のサイン波オシレーター example です。

- KNOB_0: 周波数 (50Hz 〜 2050Hz)
- KNOB_1: 音量

ここから自分の音源処理・エフェクトに書き換えていく想定の雛形です。

## 参考リンク

- [libDaisy (GitHub)](https://github.com/electro-smith/libDaisy)
- [DaisySP (GitHub)](https://github.com/electro-smith/DaisySP)
- [Daisy Wiki (Getting Started)](https://github.com/electro-smith/DaisyWiki/wiki)
- [DaisyExamples - versio](https://github.com/electro-smith/DaisyExamples/tree/master/versio)
- [Noise Engineering: Create your own firmware on a Versio module](https://noiseengineering.us/blogs/loquelic-literitas-the-blog/create-your-own-firmware-on-a-versio-module/)
- [Daisy Forum](https://forum.electro-smith.com/)
