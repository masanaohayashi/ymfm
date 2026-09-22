# OPM / OPZ 高精度演算コア

`src/ymfm_precision.h` は、YM2151 / YM2414 の4オペレーター構成を基にした
オプトインの高精度演算層です。既存の `ymfm::ym2151` / `ymfm::ym2414` は
変更していません。従来のレジスタAPI、整数出力、保存状態はそのまま使えます。

## 型の選択と最小例

C++14、ヘッダーのみで使用できます。演算型はコンパイル時に選びます。
実行時に選択するアプリケーションでは、両方をインスタンス化して切り替えてください。

```cpp
#include "ymfm_precision.h"

using namespace ymfm::precision;
fm_engine<double, 8> synth(model::opz); // floatならfloat32、doubleならfloat64
synth.prepare(48000.0);
voice_parameters<double> patch;
patch.algorithm = 4;
patch.frequency = 440.0;
patch.feedback = 50.25;
patch.operators[0].waveform = 1;
patch.operators[0].total_level = 24.5;
patch.operators[0].attack = 110.25;
patch.operators[0].decay = 40.5;
patch.operators[0].sustain_level = 64.0;
synth.set_voice(0, patch); // boolの戻り値で入力エラーを確認できる
synth.key_on(0);
double left[256], right[256];
synth.render(left, right, 256);
synth.key_off(0);         // releaseへ移行。続けてrenderする
```

`fm_engine_f32<N>` / `fm_engine_f64<N>` の別名もあります。
`N` は1〜128。現在はボイス番号を直接操作する演算層で、自動ボイス割当や
MIDI note-onの管理は行いません。`key_on/off(voice, mask)` のmaskは4個の
オペレーターを選ぶ下位4 bitです。0は何も変更しません。

`prepare(sample_rate, reference_clock)` は全発音を停止して時間基準を設定します。
標準のreference_clockは3,579,545 Hzで、EG速度とノイズの基準に使います。
波形の周波数はHz指定なので、出力サンプルレートによって変化しません。
`reset()` は音色を保持して発音状態を初期化します。

出力は選択した型のステレオ平面バッファへ上書きします。全ボイスの和なので
±1を超えることがあります。クリップ、DAC量子化、自動正規化はしません。
左右バッファは互いに重ならず、指定したフレーム数の容量が必要です。

## 高精度化した箇所

| 箇所 | 実装 |
| --- | --- |
| 位相アキュムレーター / 増分 | uint64_t、Q0.64周期、unsigned周回 |
| 位相変調 | Realで求めた周期数を符号付きの意味を保って整数オフセットへ変換 |
| 周波数→位相増分 | 最低doubleで除算後に整数化。float32時の周波数量子化を回避 |
| 波形テーブル | 解析式から直接生成したReal値とReal補間係数、8波形×4096区間 |
| 波形補間 | 3次Hermite。波形の折れ目では片側の微分を使い、隣の区間へ漏らさない |
| オペレーター / 変調 / フィードバック | 選択したReal型。整数への振幅丸めなし |
| 音量EG | Realの減衰量。旧EGの平均速度を用いた連続更新 |
| EG微小増分 | 補償加算で残差を保持し、float32の低速EGが停止することを回避 |
| 振幅変換 | `exp2(-attenuation/64)`、旧整数の指数テーブルを使わない |
| LFO | 64 bit位相、RealのAM/PM、独立した2系統 |
| ノイズ | OPM/OPZの17 bit多項式。連続クロックと出力ラッチを分離、振幅はReal |
| 加算 / 出力 | Real、途中の整数クリップなし |

波形テーブル生成時はlong doubleの解析式を使い、その結果をRealで保存します。
float64版がfloat32テーブルを拡大したものになることはありません。
ARM環境などlong doubleとdoubleが同じ精度の環境でも利用できます。

演算精度と実機の量子化再現は別です。拡張側は整数ROMの誤差、DAC丸め、
EGの階段状更新を再現しません。高精度側と元の音源のbit一致は意図していません。
オーバーサンプリング・アンチエイリアス処理はまだ含まれていません。

## パラメーターと編集

`set_voice` はボイスのパラメーター全体を検査してから反映します。
不正な範囲・NaN・Inf・アルゴリズム番号は拒否し、現在の設定を保持します。
発音中の編集で位相やEGをリセットしません。現段階では変更は即時反映で、
音量操作の時間補間は後続の機能です。波形テーブルの補間とは異なります。

- AR/D1R/D2R/RR/TL/feedback: Realで0〜127。小数も内部まで保持する。
- AR/D1R/D2Rは旧5 bitレートを連続化。0は進行を停止する。
- RRは旧4 bitレートを連続化。0でも最も遅いreleaseとして進行する。
- sustain_level: 0〜127を0〜992減衰単位に対応させる。旧最大値の特殊な値は
  `legacy<Real>::sustain4()` が変換する。
- total_level: 1単位は8減衰単位、約0.75 dB。振幅は厳密には2^(-TL/8)。
- ratio、detune_cents、detune_hz、fixed_hz: 周波数比・cent・Hz。
- rate_scaling: 0〜31の連続した有効EGレート加算値。自動キー追従はホスト側で更新する。
- envelope_shift / reverb: OPZの連続EG拡張。OPMでは適用しない。
- gain_left/right: 0〜16の線形ゲイン。
- LFO: frequency 0〜2000 Hz、pitch_cents ±9600 cent、amplitude 0〜1023減衰単位。
- ノイズ: 0〜1,000,000 Hz。0でラッチを保持。基準クロックを超える指定では
  LFSRクロックも引き上げる拡張動作。

固定周波数オペレーターは、通常はキー周波数とピッチLFOの影響を受けません。
`fixed_pitch_modulation` でピッチ変調のみ有効にできます。

8アルゴリズムのオペレーター番号は信号経路順0〜3です。
旧レジスタのスロット順ではありません。OPZの波形0〜7は上流の波形定義に従い、
signed sine、signed sine²、片側波形、その倍周波波形・整流波形を表現します。

音声コールバック前にコンストラクター / prepareを呼んでテーブルを初期化します。
制御関数とrenderは同じスレッドで順序付けて呼びます。別スレッドから直接
同時編集する設計ではありません。サンプル位置を指定した編集はブロックを
分割して行えます。renderはメモリ確保・ロック・I/Oを行いません。

## 元の音色から移行する場合

`ymfm_precision_import.h` の `import_opm` / `import_opz` が、既存の
`opm_registers` / `opz_registers` のチャンネルから音色を変換します。

```cpp
voice_parameters<double> patch;
if (import_opz(registers, channel, 3579545.0, patch))
    synth.set_voice(voice, patch);
```

これは音色のスナップショット変換です。レジスタを高精度側でエミュレートする
APIではなく、タイマー・CSM・発音途中の位相・EG状態を引き継ぎません。
以後はパラメーター関数で変更します。

ピッチROMは連続した平均律に置き換えます。標準クロックでA4=440 Hz、
block=0/code=0はC#0、クロック変更時は比例して周波数が変わります。
OPZの意味が未確定なchannel-volume byteは、上流と同様に変換しません。
明示的な左右ゲインは両モデルで使用できます。

## SIMDを追加する際の構造

ホットな状態と係数は `operator_bank` / `lfo_bank` のSoAです。
`phase[voice]`、`attenuation[voice]`、`output[voice]` などがそれぞれ連続します。
処理はサンプル→オペレーター→ボイスの順。オペレーター間・フィードバックの
時間依存を保ちつつ、内側のボイス群をSIMDレーンに置き換えられます。

現時点で明示的なSIMD命令は使っていません。将来はEG状態をマスク処理にし、
波形係数をgatherし、exp2等を選択した精度のベクトル関数へ置き換えられます。
C++14の過剰アラインメントのヒープ確保問題を避けるため、現在は通常の配列です。
SIMD側ではunaligned load、または専用allocatorを選んでください。

元音色を保持するAoSは制御時のみ参照し、サンプル処理の内側では参照しません。
SIMD化の際は、スカラー版を数値参照として残してください。float64版の内部を
float32演算へ置き換える最適化や、ブロック境界で状態を丸める実装は避けます。

## ビルドと検証

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=14
cmake --build build -j 4
ctest --test-dir build --output-on-failure
./build/ymfm_precision_example
```

CMakeの `ymfm_precision` はヘッダーのみのターゲットです。
音色インポートで元のregisterクラスを生成・操作する場合は `ymfm` もリンクします。

テストは以下を確認します。

- float32 / float64それぞれの8波形を、解析式と120万点以上で比較。
- 8アルゴリズムを独立したsinベースの信号経路と比較。
- 2サンプルのフィードバック、負の位相変調、整数位相の周回・微小増分。
- EGの速度を解析式と比較し、44.1/48/96 kHz間の時間基準を確認。
- 7 bit間の小数レート、float32の微小EG増分、ゼロattackとreleaseの終了。
- OPZ固定周波数、ピッチ変調の適用選択、reverb、音色インポート。
- 128番目のボイスまでの発音、非クリップ加算、ブロック分割の完全一致。
- render中のC++動的メモリ確保ゼロとノイズ多項式の独立参照。
- float32では区別できない周波数差がfloat64では位相・音声の差として残る。
- 既存全コアのビルドと実行、OPM/OPZの元出力ハッシュと保存復元。

旧出力の基準はコミット81aec25のソースを別の一時ディレクトリへ取り出して
ビルドし取得しました。テストのハッシュはOPM `9652c829404dbc75`、
OPZ `8e86f0e4e6264d91` です。

今回の高精度化と、全体ロードマップは分けて管理します。
ピッチEG、操作時の時間補間、自動ボイス割当・stealing、拡張状態の保存形式、
明示的なSIMD最適化はまだ実装していません。

実測値と完了範囲は [precision-validation.md](precision-validation.md) に記録しています。
