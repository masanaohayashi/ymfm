# ymfmplus 拡張エンジン設計

状態: 全体の設計・ロードマップ。数値演算層は `ymfm::precision::fm_engine<Real, N>`
として実装済み。現在のAPI・検証方法は [high-precision.md](high-precision.md) を参照。
以下の `extended::synth` は後続の発音管理層の案であり、現在の公開APIではない。

## 要件

- 優先対象は YM2414 (OPZ)、YM2151 (OPM)。
- 内部音声演算と波形テーブルを浮動小数点化する。最終出力だけの変換ではない。
- 利用側が float32 / float64 を選択できる。double経路を途中でfloatへ落とさない。
- 位相系は64 bit整数とする。
- 将来のSIMD化を考慮し、ボイス方向に連続するSoA配置と演算カーネルを採用する。
- 7 bit 未満の連続量パラメーターを最低 7 bit に拡張する。
- ピッチ LFO とピッチ EG を追加する。
- 音量などの実時間操作に補間を用意する。
- 最大 128 ボイスを独立に処理する。
- 拡張レジスタではなく、関数を通じてパラメーターを操作する。
- オリジナルモードと既存ソース/API/出力動作を維持する。

## 互換性の境界

既存の ymfm::ym2151 / ymfm::ym2414、レジスタアクセス、generate、
output_data、タイマー、保存状態を変更しない。従来クラスをそのまま
オリジナルモードとして使えるようにする。

拡張用に ymfm::extended::synth を追加する。モデルを opm / opz で選択し、
共通の4オペレーターエンジンと8種類のアルゴリズムを使う。
チップを16個並べて整数出力を混ぜる構成にはしない。

拡張モードは音源としての高精度化を目的とし、実機の量子化・DAC丸め・
演算途中の整数クリップまでの再現はオリジナルモードに任せる。
拡張状態の保存形式は既存の保存状態から分離し、バージョンを持たせる。
発音中の両モード間の状態変換は初期実装の対象に含めない。

## 数値表現

- オペレーター出力、変調、フィードバック、EG、ゲイン、ミックス: Real。
  `Real = float` または `double` を利用側が選ぶ。波形テーブルと補間係数も同型。
- 位相と位相増分: uint64_t、全範囲を1周期とするQ0.64形式。
  unsigned加算で自然に周回させる。負の位相変調も定義済みのunsigned演算で扱う。
- 周波数から整数位相増分への変換は最低double精度で行い、float32経路でも
  24 bitの周波数除算精度が64 bit位相アキュムレーターを制限しないようにする。
- 時間の基準: ホストの sample_rate。サンプルレートを変えてもHz・秒の意味は変えない。
- インデックス、列挙値、サンプル数、乱数/LFSR: 整数。
- 波形評価とEGに従来の固定小数点丸めを持ち込まない。
- 出力は選択したReal型の非クリップ・ステレオ加算。リミッターや自動正規化は内蔵しない。

float 化だけでは音質向上を保証できない。エイリアシングは別の課題として扱い、
旧版との音色差、スペクトル、CPU負荷を測定する。

## パラメーター

7 bit 化は連続量に適用する。Real の0〜127を標準コントロール値とし、
整数に丸めず小数値も受け付ける。既に8 bit以上ある連続量は精度を落とさない。
Hz・秒・cent・dBを直接指定するAPIも同じ内部パラメーターへ接続する。

アルゴリズム、波形番号、各種モード、ON/OFFは列挙値・boolとする。
アルゴリズム間や波形番号間を数値補間する意味は定義しない。

| 対象 | 拡張での扱い |
| --- | --- |
| AR / D1R / D2R / RR | 各0〜127、小数可。旧レートの応答を基準に間を補間 |
| D1L / sustain level | 0〜127、小数可。旧最大値の特殊な減衰量を変換時に保存 |
| TL | 元の0〜127を維持し、小数・実時間補間を追加 |
| feedback | 0〜127、小数可。旧8段階の強度を基準に連続化 |
| multiplier / fine | 正の周波数比を直接指定可能。旧0の倍率0.5を変換時に保存 |
| detune / DT2 | cent等の連続量へ変換。元の値は変換関数で再現 |
| rate scaling | 0〜127、小数可。キーに対する速度変化を連続化 |
| AM/PM sensitivity | 連続した深さへ変換 |
| OPZ fixed frequency | Hzで指定し、低周波でも位相を量子化しない |
| OPZ EG shift / reverb | 連続量へ拡張。原値との対応を文書化 |
| pan / voice volume | 連続値。補間を適用 |

単に7 bit値を旧ビット幅へ右シフトして渡す実装は禁止する。
旧値→拡張値の明示的な変換関数を用意し、旧レート位置でのEG速度を検証する。
旧整数コア特有のステップや丸めまで一致することは要求しない。

## 発音と変調

各ボイスに4オペレーター、独立した音量EG、周波数、フィードバック履歴、
ゲイン、パン、ピッチEG、LFO位相を持たせる。

ピッチEGは初期レベル、attack/decay/releaseの時間とレベル、sustainレベルを
指定する。レベルの単位はcent。note-off時点の現在値からreleaseへ遷移する。
ピッチLFOはHz・centで速度と深さを指定し、波形、delay、fade-in、
key syncを用意する。OPZの2系統LFOを表現できる構成にする。

基準周波数 × 2^((pitch bend + pitch EG + pitch LFO + detune) / 1200)
を周波数計算の基本とする。固定周波数オペレーターへのピッチ変調適用は
明示的なboolとし、通常のキー追従とは分離する。

OPMのノイズ、OPZの8波形・固定周波数・EG shift・reverbをモデル固有機能
として維持する。上流OPZには挙動が推定の箇所があるため、既存実装に準じる
部分と拡張として定義する部分を区別して文書化する。

## 補間

ゲイン、パン、TL、フィードバック、周波数、LFO深さなどにランプを適用する。
時間は秒で指定し、0は即時変更。補間中の再変更は現在値から開始する。
最終サンプルで目標値へ到達させ、ブロック境界で補間をリセットしない。
ゲインはlinear / dB、周波数はHz / log-frequencyを明示的に選べるようにする。
ゼロゲインのdB補間には有限の下限と終了時の厳密なゼロを定義する。

## API案（C++14）

```cpp
namespace ymfm { namespace extended {

enum class model { opm, opz };
enum class ramp_curve { linear, logarithmic };
template<class Real> struct ramp { Real seconds; ramp_curve curve; };
struct voice_id { uint16_t slot; uint64_t generation; };

// patch、operator_parameters、pitch_envelope、lfo_parametersは値型。
// 詳細フィールドと許容範囲は実装前にヘッダーへ明記する。
template<class Real> class synth {
public:
    // 発音を停止して設定。voice_countは1〜128。
    bool prepare(double sample_rate, unsigned voice_count);
    void reset();
    bool set_patch(unsigned part, const patch<Real>& value);

    // 同音再打鍵も異なるID。割当不可は無効IDを返す。
    voice_id note_on(unsigned part, Real midi_note, Real velocity);
    bool note_off(voice_id voice);
    void all_notes_off(); // releaseを開始
    void all_sound_off(); // 即時停止

    bool set_voice_gain(voice_id, Real linear_gain, ramp<Real>);
    bool set_voice_pan(voice_id, Real pan, ramp<Real>);
    bool set_voice_pitch(voice_id, Real cents, ramp<Real>);
    bool set_operator_parameters(voice_id, unsigned op,
                                 const operator_parameters<Real>&, ramp<Real>);
    bool set_pitch_lfo(voice_id, unsigned lfo,
                       const lfo_parameters<Real>&, ramp<Real>);
    bool set_pitch_envelope(voice_id, const pitch_envelope<Real>&);

    // 事前確保済み状態のみを使用。指定フレームを上書き。
    void render(Real* left, Real* right, unsigned frames);
};
} }
```

IDに世代を含め、盗まれたボイスへの古いnote-offが別の音を止めないようにする。
上限到達時の方針はreject / released-first-then-oldestを選択可能にする。
voice stealingでは短い退避フェードを使い、単純な状態上書きによる段差を抑える。
退避用状態は事前確保し、公開ボイス上限と一時的なフェード処理数を区別する。

パッチ変更は新規発音に適用し、発音中の編集はvoice用関数で明示する。
操作とrenderは同一音声スレッド上で順序付ける。別スレッドからの操作には
ホスト側のイベントキューを使用する。サンプル精度の変更はブロックを分割して
適用できるようにし、render中の確保・ロック・I/Oを行わない。
不正なindex、NaN、Inf、範囲外値は拒否し、部分的に状態を変更しない。

## 実装順と受け入れ検証

1. 既存全コアのビルド、OPM/OPZのレジスタ列に対する出力・保存復元の
   回帰フィクスチャを追加。改修前の出力を固定する。
2. 共通float32/float64コア、OPMの4オペレーター/8アルゴリズム、7 bit EGと
   パラメーター関数を実装。中間値が旧値へ量子化されないことを検証する。
3. OPZ固有の8波形、固定周波数、EG拡張、2系統LFOを実装する。
4. ピッチEG/LFOと補間を追加。サンプルレート、ブロック分割、途中再変更で
   結果が整合し、全出力が有限になることを検証する。
5. 128ボイス管理、重複ノート、解放、stealing、古いID拒否を検証する。
6. API使用例と比較レンダーを追加。128ボイスのCPU負荷を実測して記録する。

整数モードの回帰は同一出力を要求する。拡張モードは周波数・EG応答・
波形スペクトルと聴感で検証し、整数版とのbit一致を合格条件にはしない。
