// BSD 3-Clause License
//
// Copyright (c) 2021, Aaron Giles
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// 1. Redistributions of source code must retain the above copyright notice, this
//    list of conditions and the following disclaimer.
//
// 2. Redistributions in binary form must reproduce the above copyright notice,
//    this list of conditions and the following disclaimer in the documentation
//    and/or other materials provided with the distribution.
//
// 3. Neither the name of the copyright holder nor the names of its
//    contributors may be used to endorse or promote products derived from
//    this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
// DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
// FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
// DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
// SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
// CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
// OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.

namespace ymfm
{

//*********************************************************
//  GLOBAL TABLE LOOKUPS
//*********************************************************

//-------------------------------------------------
//  abs_sin_attenuation - given a sin (phase) input
//  where the range 0-2*PI is mapped onto 10 bits,
//  return the absolute value of sin(input),
//  logarithmically-adjusted and treated as an
//  attenuation value, in 4.8 fixed point format
//-------------------------------------------------

inline uint32_t abs_sin_attenuation(uint32_t input)
{
	// the values here are stored as 4.8 logarithmic values for 1/4 phase
	// this matches the internal format of the OPN chip, extracted from the die
	static uint16_t const s_sin_table[256] =
	{
		0x859,0x6c3,0x607,0x58b,0x52e,0x4e4,0x4a6,0x471,0x443,0x41a,0x3f5,0x3d3,0x3b5,0x398,0x37e,0x365,
		0x34e,0x339,0x324,0x311,0x2ff,0x2ed,0x2dc,0x2cd,0x2bd,0x2af,0x2a0,0x293,0x286,0x279,0x26d,0x261,
		0x256,0x24b,0x240,0x236,0x22c,0x222,0x218,0x20f,0x206,0x1fd,0x1f5,0x1ec,0x1e4,0x1dc,0x1d4,0x1cd,
		0x1c5,0x1be,0x1b7,0x1b0,0x1a9,0x1a2,0x19b,0x195,0x18f,0x188,0x182,0x17c,0x177,0x171,0x16b,0x166,
		0x160,0x15b,0x155,0x150,0x14b,0x146,0x141,0x13c,0x137,0x133,0x12e,0x129,0x125,0x121,0x11c,0x118,
		0x114,0x10f,0x10b,0x107,0x103,0x0ff,0x0fb,0x0f8,0x0f4,0x0f0,0x0ec,0x0e9,0x0e5,0x0e2,0x0de,0x0db,
		0x0d7,0x0d4,0x0d1,0x0cd,0x0ca,0x0c7,0x0c4,0x0c1,0x0be,0x0bb,0x0b8,0x0b5,0x0b2,0x0af,0x0ac,0x0a9,
		0x0a7,0x0a4,0x0a1,0x09f,0x09c,0x099,0x097,0x094,0x092,0x08f,0x08d,0x08a,0x088,0x086,0x083,0x081,
		0x07f,0x07d,0x07a,0x078,0x076,0x074,0x072,0x070,0x06e,0x06c,0x06a,0x068,0x066,0x064,0x062,0x060,
		0x05e,0x05c,0x05b,0x059,0x057,0x055,0x053,0x052,0x050,0x04e,0x04d,0x04b,0x04a,0x048,0x046,0x045,
		0x043,0x042,0x040,0x03f,0x03e,0x03c,0x03b,0x039,0x038,0x037,0x035,0x034,0x033,0x031,0x030,0x02f,
		0x02e,0x02d,0x02b,0x02a,0x029,0x028,0x027,0x026,0x025,0x024,0x023,0x022,0x021,0x020,0x01f,0x01e,
		0x01d,0x01c,0x01b,0x01a,0x019,0x018,0x017,0x017,0x016,0x015,0x014,0x014,0x013,0x012,0x011,0x011,
		0x010,0x00f,0x00f,0x00e,0x00d,0x00d,0x00c,0x00c,0x00b,0x00a,0x00a,0x009,0x009,0x008,0x008,0x007,
		0x007,0x007,0x006,0x006,0x005,0x005,0x005,0x004,0x004,0x004,0x003,0x003,0x003,0x002,0x002,0x002,
		0x002,0x001,0x001,0x001,0x001,0x001,0x001,0x001,0x000,0x000,0x000,0x000,0x000,0x000,0x000,0x000
	};

	// if the top bit is set, we're in the second half of the curve
	// which is a mirror image, so invert the index
	if (bitfield(input, 8))
		input = ~input;

	// return the value from the table
	return s_sin_table[input & 0xff];
}


//-------------------------------------------------
//  attenuation_to_volume - given a 5.8 fixed point
//  logarithmic attenuation value, return a 13-bit
//  linear volume
//-------------------------------------------------

inline uint32_t attenuation_to_volume(uint32_t input)
{
	// look up the fractional part, then shift by the whole; the table is
	// shared with the vector output stage, which needs it from another file
	return g_power_table[input & 0xff] >> (input >> 8);
}


//-------------------------------------------------
//  algorithm_ops_for - return the packed wiring
//  description for one algorithm
//
//      ---------x use opout[x] as operator 2 input
//      ------xxx- use opout[x] as operator 3 input
//      ---xxx---- use opout[x] as operator 4 input
//      --x------- include opout[1] in final sum
//      -x-------- include opout[2] in final sum
//      x--------- include opout[3] in final sum
//-------------------------------------------------

inline uint32_t algorithm_ops_for(uint32_t algorithm)
{
#define ALGORITHM(op2in, op3in, op4in, op1out, op2out, op3out) \
	((op2in) | ((op3in) << 1) | ((op4in) << 4) | ((op1out) << 7) | ((op2out) << 8) | ((op3out) << 9))
	static uint16_t const s_algorithm_ops[8+4] =
	{
		ALGORITHM(1,2,3, 0,0,0),    //  0: O1 -> O2 -> O3 -> O4 -> out (O4)
		ALGORITHM(0,5,3, 0,0,0),    //  1: (O1 + O2) -> O3 -> O4 -> out (O4)
		ALGORITHM(0,2,6, 0,0,0),    //  2: (O1 + (O2 -> O3)) -> O4 -> out (O4)
		ALGORITHM(1,0,7, 0,0,0),    //  3: ((O1 -> O2) + O3) -> O4 -> out (O4)
		ALGORITHM(1,0,3, 0,1,0),    //  4: ((O1 -> O2) + (O3 -> O4)) -> out (O2+O4)
		ALGORITHM(1,1,1, 0,1,1),    //  5: ((O1 -> O2) + (O1 -> O3) + (O1 -> O4)) -> out (O2+O3+O4)
		ALGORITHM(1,0,0, 0,1,1),    //  6: ((O1 -> O2) + O3 + O4) -> out (O2+O3+O4)
		ALGORITHM(0,0,0, 1,1,1),    //  7: (O1 + O2 + O3 + O4) -> out (O1+O2+O3+O4)
		ALGORITHM(1,2,3, 0,0,0),    //  8: O1 -> O2 -> O3 -> O4 -> out (O4)         [same as 0]
		ALGORITHM(0,2,3, 1,0,0),    //  9: (O1 + (O2 -> O3 -> O4)) -> out (O1+O4)   [unique]
		ALGORITHM(1,0,3, 0,1,0),    // 10: ((O1 -> O2) + (O3 -> O4)) -> out (O2+O4) [same as 4]
		ALGORITHM(0,2,0, 1,0,1)     // 11: (O1 + (O2 -> O3) + O4) -> out (O1+O3+O4) [unique]
	};
#undef ALGORITHM
	return s_algorithm_ops[algorithm];
}


//-------------------------------------------------
//  attenuation_increment - given a 6-bit ADSR
//  rate value and a 3-bit stepping index,
//  return a 4-bit increment to the attenutaion
//  for this step (or for the attack case, the
//  fractional scale factor to decrease by)
//-------------------------------------------------

inline uint32_t attenuation_increment_packed(uint32_t rate)
{
	static uint32_t const s_increment_table[64] =
	{
		0x00000000, 0x00000000, 0x10101010, 0x10101010,  // 0-3    (0x00-0x03)
		0x10101010, 0x10101010, 0x11101110, 0x11101110,  // 4-7    (0x04-0x07)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 8-11   (0x08-0x0B)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 12-15  (0x0C-0x0F)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 16-19  (0x10-0x13)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 20-23  (0x14-0x17)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 24-27  (0x18-0x1B)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 28-31  (0x1C-0x1F)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 32-35  (0x20-0x23)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 36-39  (0x24-0x27)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 40-43  (0x28-0x2B)
		0x10101010, 0x10111010, 0x11101110, 0x11111110,  // 44-47  (0x2C-0x2F)
		0x11111111, 0x21112111, 0x21212121, 0x22212221,  // 48-51  (0x30-0x33)
		0x22222222, 0x42224222, 0x42424242, 0x44424442,  // 52-55  (0x34-0x37)
		0x44444444, 0x84448444, 0x84848484, 0x88848884,  // 56-59  (0x38-0x3B)
		0x88888888, 0x88888888, 0x88888888, 0x88888888   // 60-63  (0x3C-0x3F)
	};
	return s_increment_table[rate];
}

inline uint32_t attenuation_increment(uint32_t rate, uint32_t index)
{
	return bitfield(attenuation_increment_packed(rate), 4*index, 4);
}


//-------------------------------------------------
//  detune_adjustment - given a 5-bit key code
//  value and a 3-bit detune parameter, return a
//  6-bit signed phase displacement; this table
//  has been verified against Nuked's equations,
//  but the equations are rather complicated, so
//  we'll keep the simplicity of the table
//-------------------------------------------------

inline int32_t detune_adjustment(uint32_t detune, uint32_t keycode)
{
	static uint8_t const s_detune_adjustment[32][4] =
	{
		{ 0,  0,  1,  2 },  { 0,  0,  1,  2 },  { 0,  0,  1,  2 },  { 0,  0,  1,  2 },
		{ 0,  1,  2,  2 },  { 0,  1,  2,  3 },  { 0,  1,  2,  3 },  { 0,  1,  2,  3 },
		{ 0,  1,  2,  4 },  { 0,  1,  3,  4 },  { 0,  1,  3,  4 },  { 0,  1,  3,  5 },
		{ 0,  2,  4,  5 },  { 0,  2,  4,  6 },  { 0,  2,  4,  6 },  { 0,  2,  5,  7 },
		{ 0,  2,  5,  8 },  { 0,  3,  6,  8 },  { 0,  3,  6,  9 },  { 0,  3,  7, 10 },
		{ 0,  4,  8, 11 },  { 0,  4,  8, 12 },  { 0,  4,  9, 13 },  { 0,  5, 10, 14 },
		{ 0,  5, 11, 16 },  { 0,  6, 12, 17 },  { 0,  6, 13, 19 },  { 0,  7, 14, 20 },
		{ 0,  8, 16, 22 },  { 0,  8, 16, 22 },  { 0,  8, 16, 22 },  { 0,  8, 16, 22 }
	};
	int32_t result = s_detune_adjustment[keycode][detune & 3];
	return bitfield(detune, 2) ? -result : result;
}


//-------------------------------------------------
//  opm_key_code_to_phase_step - converts an
//  OPM concatenated block (3 bits), keycode
//  (4 bits) and key fraction (6 bits) to a 0.10
//  phase step, after applying the given delta;
//  this applies to OPM and OPZ, so it lives here
//  in a central location
//-------------------------------------------------

inline uint32_t opm_key_code_to_phase_step(uint32_t block_freq, int32_t delta)
{
	// The phase step is essentially the fnum in OPN-speak. To compute this table,
	// we used the standard formula for computing the frequency of a note, and
	// then converted that frequency to fnum using the formula documented in the
	// YM2608 manual.
	//
	// However, the YM2608 manual describes everything in terms of a nominal 8MHz
	// clock, which produces an FM clock of:
	//
	//    8000000 / 24(operators) / 6(prescale) = 55555Hz FM clock
	//
	// Whereas the descriptions for the YM2151 use a nominal 3.579545MHz clock:
	//
	//    3579545 / 32(operators) / 2(prescale) = 55930Hz FM clock
	//
	// To correct for this, the YM2608 formula was adjusted to use a clock of
	// 8053920Hz, giving this equation for the fnum:
	//
	//    fnum = (double(144) * freq * (1 << 20)) / double(8053920) / 4;
	//
	// Unfortunately, the computed table differs in a few spots from the data
	// verified from an actual chip. The table below comes from David Viens'
	// analysis, used with his permission.

	// extract the block (octave) first
	uint32_t block = bitfield(block_freq, 10, 3);

	// the keycode (bits 6-9) is "gappy", mapping 12 values over 16 in each
	// octave; to correct for this, we multiply the 4-bit value by 3/4 (or
	// rather subtract 1/4); note that a (invalid) value of 15 will bleed into
	// the next octave -- this is confirmed
	uint32_t adjusted_code = bitfield(block_freq, 6, 4) - bitfield(block_freq, 8, 2);

	// now re-insert the 6-bit fraction
	int32_t eff_freq = (adjusted_code << 6) | bitfield(block_freq, 0, 6);

	// now that the gaps are removed, add the delta
	eff_freq += delta;

	// handle over/underflow by adjusting the block:
	if (uint32_t(eff_freq) >= 768)
	{
		// minimum delta is -512 (PM), so we can only underflow by 1 octave
		if (eff_freq < 0)
		{
			eff_freq += 768;
			if (block-- == 0)
				return g_phase_step_table[0] >> 7;
		}

		// maximum delta is +512+608 (PM+detune), so we can overflow by up to 2 octaves
		else
		{
			eff_freq -= 768;
			if (eff_freq >= 768)
				block++, eff_freq -= 768;
			if (block++ >= 7)
				return g_phase_step_table[767];
		}
	}

	// look up the phase shift for the key code, then shift by octave
	return g_phase_step_table[eff_freq] >> (block ^ 7);
}


//-------------------------------------------------
//  opn_lfo_pm_phase_adjustment - given the 7 most
//  significant frequency number bits, plus a 3-bit
//  PM depth value and a signed 5-bit raw PM value,
//  return a signed PM adjustment to the frequency;
//  algorithm written to match Nuked behavior
//-------------------------------------------------

inline int32_t opn_lfo_pm_phase_adjustment(uint32_t fnum_bits, uint32_t pm_sensitivity, int32_t lfo_raw_pm)
{
	// this table encodes 2 shift values to apply to the top 7 bits
	// of fnum; it is effectively a cheap multiply by a constant
	// value containing 0-2 bits
	static uint8_t const s_lfo_pm_shifts[8][8] =
	{
		{ 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77, 0x77 },
		{ 0x77, 0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x72 },
		{ 0x77, 0x77, 0x77, 0x72, 0x72, 0x72, 0x17, 0x17 },
		{ 0x77, 0x77, 0x72, 0x72, 0x17, 0x17, 0x12, 0x12 },
		{ 0x77, 0x77, 0x72, 0x17, 0x17, 0x17, 0x12, 0x07 },
		{ 0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01 },
		{ 0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01 },
		{ 0x77, 0x77, 0x17, 0x12, 0x07, 0x07, 0x02, 0x01 }
	};

	// look up the relevant shifts
	int32_t abs_pm = (lfo_raw_pm < 0) ? -lfo_raw_pm : lfo_raw_pm;
	uint32_t const shifts = s_lfo_pm_shifts[pm_sensitivity][bitfield(abs_pm, 0, 3)];

	// compute the adjustment
	int32_t adjust = (fnum_bits >> bitfield(shifts, 0, 4)) + (fnum_bits >> bitfield(shifts, 4, 4));
	if (pm_sensitivity > 5)
		adjust <<= pm_sensitivity - 5;
	adjust >>= 2;

	// every 16 cycles it inverts sign
	return (lfo_raw_pm < 0) ? -adjust : adjust;
}



//*********************************************************
//  FM OPERATOR
//*********************************************************

//-------------------------------------------------
//  fm_operator - constructor
//-------------------------------------------------

template<class RegisterType>
fm_operator<RegisterType>::fm_operator(fm_engine_base<RegisterType> &owner, uint32_t opnum, uint32_t opoffs) :
	m_choffs(0),
	m_opoffs(opoffs),
	m_opnum(opnum),
	m_phase(owner.op_phase(opnum)),
	m_env_attenuation(owner.eg_atten(opnum)),
	m_env_state(owner.eg_state(opnum)),
	m_ssg_inverted(false),
	m_key_state(0),
	m_keyon_live(0),
	m_regs(owner.regs()),
	m_owner(owner)
{
	m_phase = 0;
	m_env_attenuation = 0x3ff;
	m_env_state = EG_RELEASE;
}


//-------------------------------------------------
//  reset - reset the channel state
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::reset()
{
	// reset our data
	m_phase = 0;
	m_env_attenuation = 0x3ff;
	m_env_state = EG_RELEASE;
	m_ssg_inverted = 0;
	m_key_state = 0;
	m_keyon_live = 0;
}


//-------------------------------------------------
//  save_restore - save or restore the data
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::save_restore(ymfm_saved_state &state)
{
	state.save_restore(m_phase);

	// round-trip through the original types: the storage changed, the snapshot
	// format must not
	uint16_t attenuation = uint16_t(m_env_attenuation);
	state.save_restore(attenuation);
	m_env_attenuation = attenuation;
	envelope_state eg = envelope_state(m_env_state);
	state.save_restore(eg);
	m_env_state = eg;
	state.save_restore(m_ssg_inverted);
	state.save_restore(m_key_state);
	state.save_restore(m_keyon_live);
}


//-------------------------------------------------
//  prepare - prepare for clocking
//-------------------------------------------------

template<class RegisterType>
bool fm_operator<RegisterType>::prepare()
{
	// cache the data
	m_regs.cache_operator_data(m_choffs, m_opoffs, m_cache);

	// clock the key state
	clock_keystate(uint32_t(m_keyon_live != 0));
	m_keyon_live &= ~(1 << KEYON_CSM);

	// mirror the cache into the engine's parallel arrays, after any key-on has
	// settled the state; the envelope update carries the current rate rather
	// than looking it up per sample, so it is refreshed here too
	m_owner.publish_op_cache(m_opnum, m_opoffs, m_cache);

	// we're active until we're quiet after the release
	return (m_env_state != (RegisterType::EG_HAS_REVERB ? EG_REVERB : EG_RELEASE) || m_env_attenuation < EG_QUIET);
}


//-------------------------------------------------
//  clock - master clocking function
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::clock(uint32_t env_counter, int32_t lfo_raw_pm)
{
	// Families with SSG-EG keep the per-operator envelope here, because
	// clock_ssg_eg_state can rewrite the phase and attenuation before it and
	// so cannot be hoisted. Everyone else has their envelopes clocked for the
	// whole chip at once by the engine, eight operators at a time.
	if (RegisterType::EG_HAS_SSG)
	{
		// clock the SSG-EG state (OPN/OPNA)
		if (m_regs.op_ssg_eg_enable(m_opoffs))
			clock_ssg_eg_state();
		else
			m_ssg_inverted = false;

		// clock the envelope if on an envelope cycle; env_counter is a x.2 value
		if (bitfield(env_counter, 0, 2) == 0)
			clock_envelope(env_counter >> 2);
	}
	else
		m_ssg_inverted = false;

	// clock the phase
	clock_phase(lfo_raw_pm);
}


//-------------------------------------------------
//  compute_volume - compute the 14-bit signed
//  volume of this operator, given a phase
//  modulation and an AM LFO offset
//-------------------------------------------------

template<class RegisterType>
int32_t fm_operator<RegisterType>::compute_volume(uint32_t phase, uint32_t am_offset) const
{
	// the low 10 bits of phase represents a full 2*PI period over
	// the full sin wave

	// early out if the envelope is effectively off
	if (m_env_attenuation > EG_QUIET)
		return 0;

	// get the absolute value of the sin, as attenuation, as a 4.8 fixed point value
	uint32_t sin_attenuation = m_cache.waveform[phase & (RegisterType::WAVEFORM_LENGTH - 1)];

	// get the attenuation from the evelope generator as a 4.6 value, shifted up to 4.8
	uint32_t env_attenuation = envelope_attenuation(am_offset) << 2;

	// combine into a 5.8 value, then convert from attenuation to 13-bit linear volume
	int32_t result = attenuation_to_volume((sin_attenuation & 0x7fff) + env_attenuation);

	// negate if in the negative part of the sin wave (sign bit gives 14 bits)
	return bitfield(sin_attenuation, 15) ? -result : result;
}


//-------------------------------------------------
//  compute_noise_volume - compute the 14-bit
//  signed noise volume of this operator, given a
//  noise input value and an AM offset
//-------------------------------------------------

template<class RegisterType>
int32_t fm_operator<RegisterType>::compute_noise_volume(uint32_t am_offset) const
{
	// application manual says the logarithmic transform is not applied here, so we
	// just use the raw envelope attenuation, inverted (since 0 attenuation should be
	// maximum), and shift it up from a 10-bit value to an 11-bit value
	int32_t result = (envelope_attenuation(am_offset) ^ 0x3ff) << 1;

	// QUESTION: is AM applied still?

	// negate based on the noise state
	return bitfield(m_regs.noise_state(), 0) ? -result : result;
}


//-------------------------------------------------
//  keyonoff - signal a key on/off event
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::keyonoff(uint32_t on, keyon_type type)
{
	m_keyon_live = (m_keyon_live & ~(1 << int(type))) | (bitfield(on, 0) << int(type));
}


//-------------------------------------------------
//  start_attack - start the attack phase; called
//  when a keyon happens or when an SSG-EG cycle
//  is complete and restarts
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::start_attack(bool is_restart)
{
	// don't change anything if already in attack state
	if (m_env_state == EG_ATTACK)
		return;
	m_env_state = EG_ATTACK;

	// generally not inverted at start, except if SSG-EG is enabled and
	// one of the inverted modes is specified; leave this alone on a
	// restart, as it is managed by the clock_ssg_eg_state() code
	if (RegisterType::EG_HAS_SSG && !is_restart)
		m_ssg_inverted = m_regs.op_ssg_eg_enable(m_opoffs) & bitfield(m_regs.op_ssg_eg_mode(m_opoffs), 2);

	// reset the phase when we start an attack due to a key on
	// (but not when due to an SSG-EG restart except in certain cases
	// managed directly by the SSG-EG code)
	if (!is_restart)
		m_phase = 0;

	// if the attack rate >= 62 then immediately go to max attenuation
	if (m_cache.eg_rate[EG_ATTACK] >= 62)
		m_env_attenuation = 0;
}


//-------------------------------------------------
//  start_release - start the release phase;
//  called when a keyoff happens
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::start_release()
{
	// don't change anything if already in release state
	if (m_env_state >= EG_RELEASE)
		return;
	m_env_state = EG_RELEASE;

	// if attenuation if inverted due to SSG-EG, snap the inverted attenuation
	// as the starting point
	if (RegisterType::EG_HAS_SSG && m_ssg_inverted)
	{
		m_env_attenuation = (0x200 - m_env_attenuation) & 0x3ff;
		m_ssg_inverted = false;
	}
}


//-------------------------------------------------
//  clock_keystate - clock the keystate to match
//  the incoming keystate
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::clock_keystate(uint32_t keystate)
{
	assert(keystate == 0 || keystate == 1);

	// has the key changed?
	if ((keystate ^ m_key_state) != 0)
	{
		m_key_state = keystate;

		// if the key has turned on, start the attack
		if (keystate != 0)
		{
			// OPLL has a DP ("depress"?) state to bring the volume
			// down before starting the attack
			if (RegisterType::EG_HAS_DEPRESS && m_env_attenuation < 0x200)
				m_env_state = EG_DEPRESS;
			else
				start_attack();
		}

		// otherwise, start the release
		else
			start_release();
	}
}


//-------------------------------------------------
//  clock_ssg_eg_state - clock the SSG-EG state;
//  should only be called if SSG-EG is enabled
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::clock_ssg_eg_state()
{
	// work only happens once the attenuation crosses above 0x200
	if (!bitfield(m_env_attenuation, 9))
		return;

	// 8 SSG-EG modes:
	//    000: repeat normally
	//    001: run once, hold low
	//    010: repeat, alternating between inverted/non-inverted
	//    011: run once, hold high
	//    100: inverted repeat normally
	//    101: inverted run once, hold low
	//    110: inverted repeat, alternating between inverted/non-inverted
	//    111: inverted run once, hold high
	uint32_t mode = m_regs.op_ssg_eg_mode(m_opoffs);

	// hold modes (1/3/5/7)
	if (bitfield(mode, 0))
	{
		// set the inverted flag to the end state (0 for modes 1/7, 1 for modes 3/5)
		m_ssg_inverted = bitfield(mode, 2) ^ bitfield(mode, 1);

		// if holding, force the attenuation to the expected value once we're
		// past the attack phase
		if (m_env_state != EG_ATTACK)
			m_env_attenuation = m_ssg_inverted ? 0x200 : 0x3ff;
	}

	// continuous modes (0/2/4/6)
	else
	{
		// toggle invert in alternating mode (even in attack state)
		m_ssg_inverted ^= bitfield(mode, 1);

		// restart attack if in decay/sustain states
		if (m_env_state == EG_DECAY || m_env_state == EG_SUSTAIN)
			start_attack(true);

		// phase is reset to 0 in modes 0/4
		if (bitfield(mode, 1) == 0)
			m_phase = 0;
	}

	// in all modes, once we hit release state, attenuation is forced to maximum
	if (m_env_state == EG_RELEASE)
		m_env_attenuation = 0x3ff;
}


//-------------------------------------------------
//  clock_envelope - clock the envelope state
//  according to the given count
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::clock_envelope(uint32_t env_counter)
{
	// Written as conditional assignment rather than control flow: this is the
	// hottest function in the core and the 8-channel SIMD form has to evaluate
	// both arms of every one of these anyway. Keeping the scalar version in the
	// same shape means the two can be read side by side, and the scalar one
	// stays the bit-exact reference for the vector one.

	// handle attack->decay transitions, then decay->sustain; the second must
	// see the result of the first, because a sustain level of 0 skips decay
	// entirely (audible on the cymbals in channel 0 of shinobi's test sound #5)
	uint32_t state = m_env_state;
	state = (state == EG_ATTACK && m_env_attenuation == 0) ? uint32_t(EG_DECAY) : state;
	state = (state == EG_DECAY && m_env_attenuation >= m_cache.eg_sustain) ? uint32_t(EG_SUSTAIN) : state;

	// fetch the appropriate 6-bit rate value from the cache; the vector form
	// selects among one array per state instead of indexing, since a per-lane
	// indexed load would be a gather
	uint32_t rate = m_cache.eg_rate[state];

	// compute the rate shift value; this is the shift needed to
	// apply to the env_counter such that it becomes a 5.11 fixed
	// point number
	uint32_t rate_shift = rate >> 2;
	uint32_t shifted_counter = env_counter << rate_shift;

	// the fractional part being non-zero means it is not time to clock; this
	// was an early return, and becomes the mask the whole update is under
	const bool clocking = bitfield(shifted_counter, 0, 11) == 0;

	// determine the increment based on the non-fractional part of env_counter;
	// the original ternary is just a max, which has no branch
	uint32_t relevant_bits = bitfield(shifted_counter, std::max<uint32_t>(rate_shift, 11), 3);
	uint32_t increment = attenuation_increment(rate, relevant_bits);

	// attack is the only one that increases. The glitch means attack rates of
	// 62/63 do not increment if changed after the initial key on (where they
	// are handled specially); nukeykt confirms this happens on OPM, OPN and
	// OPL/OPLL at least, so assume it is true for everyone.
	const uint32_t attack_delta = (rate < 62)
		? ((~uint32_t(m_env_attenuation) * increment) >> 4) : 0u;
	const uint32_t attacked = uint16_t(uint32_t(m_env_attenuation) + attack_delta);

	// all other cases are similar: non-SSG-EG applies the increment, SSG-EG
	// only below the mid-point and then at 4x, and the result is clamped
	uint32_t decayed = m_env_attenuation;
	if (RegisterType::EG_HAS_SSG && m_regs.op_ssg_eg_enable(m_opoffs))
		decayed += (m_env_attenuation < 0x200) ? 4 * increment : 0u;
	else
		decayed += increment;
	decayed = (decayed >= 0x400) ? 0x3ffu : decayed;

	const bool attacking = (state == EG_ATTACK);
	const uint32_t updated = attacking ? attacked : decayed;
	m_env_attenuation = uint16_t(clocking ? updated : uint32_t(m_env_attenuation));

	// transition from release to reverb, should switch at -18dB; only
	// reachable from the non-attack arm, so testing the state is enough
	if (RegisterType::EG_HAS_REVERB && clocking && state == EG_RELEASE
		&& m_env_attenuation >= 0xc0)
		state = EG_REVERB;

	m_env_state = envelope_state(state);

	// transition from depress to attack; start_attack() has side effects
	// beyond the envelope, so it stays real control flow. It folds away
	// entirely for families without a depress state.
	if (RegisterType::EG_HAS_DEPRESS && clocking && !attacking
		&& m_env_state == EG_DEPRESS && m_env_attenuation >= 0x200)
		start_attack();
}


//-------------------------------------------------
//  clock_phase - clock the 10.10 phase value; the
//  OPN version of the logic has been verified
//  against the Nuked phase generator
//-------------------------------------------------

template<class RegisterType>
void fm_operator<RegisterType>::clock_phase(int32_t lfo_raw_pm)
{
	// read from the cache, or recalculate if PM active
	uint32_t phase_step = m_cache.phase_step;
	if (phase_step == opdata_cache::PHASE_STEP_DYNAMIC)
		phase_step = m_regs.compute_phase_step(m_choffs, m_opoffs, m_cache, lfo_raw_pm);

	// finally apply the step to the current phase value
	m_phase += phase_step;
}


//-------------------------------------------------
//  envelope_attenuation - return the effective
//  attenuation of the envelope
//-------------------------------------------------

template<class RegisterType>
uint32_t fm_operator<RegisterType>::envelope_attenuation(uint32_t am_offset) const
{
	uint32_t result = m_env_attenuation >> m_cache.eg_shift;

	// invert if necessary due to SSG-EG
	if (RegisterType::EG_HAS_SSG && m_ssg_inverted)
		result = (0x200 - result) & 0x3ff;

	// add in LFO AM modulation
	if (m_regs.op_lfo_am_enable(m_opoffs))
		result += am_offset;

	// add in total level and KSL from the cache
	result += m_cache.total_level;

	// clamp to max, apply shift, and return
	return std::min<uint32_t>(result, 0x3ff);
}



//*********************************************************
//  FM CHANNEL
//*********************************************************

//-------------------------------------------------
//  fm_channel - constructor
//-------------------------------------------------

template<class RegisterType>
fm_channel<RegisterType>::fm_channel(fm_engine_base<RegisterType> &owner, uint32_t chnum, uint32_t choffs) :
	m_choffs(choffs),
	m_chnum(chnum),
	m_feedback_0(owner.ch_feedback_0(chnum)),
	m_feedback_1(owner.ch_feedback_1(chnum)),
	m_feedback_in(owner.ch_feedback_next(chnum)),
	m_op{ nullptr, nullptr, nullptr, nullptr },
	m_regs(owner.regs()),
	m_owner(owner)
{
}


//-------------------------------------------------
//  reset - reset the channel state
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::reset()
{
	// reset our data
	m_feedback_0 = m_feedback_1 = 0;
	m_feedback_in = 0;
}


//-------------------------------------------------
//  save_restore - save or restore the data
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::save_restore(ymfm_saved_state &state)
{
	// round-trip through the original types: the storage changed, the snapshot
	// format must not
	int16_t fb0 = int16_t(m_feedback_0), fb1 = int16_t(m_feedback_1), fbin = int16_t(m_feedback_in);
	state.save_restore(fb0);
	state.save_restore(fb1);
	state.save_restore(fbin);
	m_feedback_0 = fb0;
	m_feedback_1 = fb1;
	m_feedback_in = fbin;
}


//-------------------------------------------------
//  keyonoff - signal key on/off to our operators
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::keyonoff(uint32_t states, keyon_type type, uint32_t chnum)
{
	for (uint32_t opnum = 0; opnum < m_op.size(); opnum++)
		if (m_op[opnum] != nullptr)
			m_op[opnum]->keyonoff(bitfield(states, opnum), type);

	if (debug::LOG_KEYON_EVENTS && ((debug::GLOBAL_FM_CHANNEL_MASK >> chnum) & 1) != 0)
		for (uint32_t opnum = 0; opnum < m_op.size(); opnum++)
			if (m_op[opnum] != nullptr)
				debug::log_keyon("%c%s\n", bitfield(states, opnum) ? '+' : '-', m_regs.log_keyon(m_choffs, m_op[opnum]->opoffs()).c_str());
}


//-------------------------------------------------
//  prepare - prepare for clocking
//-------------------------------------------------

template<class RegisterType>
bool fm_channel<RegisterType>::prepare()
{
	uint32_t active_mask = 0;

	m_owner.publish_channel_cache(m_chnum, m_choffs);

	// prepare all operators and determine if they are active
	for (uint32_t opnum = 0; opnum < m_op.size(); opnum++)
		if (m_op[opnum] != nullptr)
			if (m_op[opnum]->prepare())
				active_mask |= 1 << opnum;

	return (active_mask != 0);
}


//-------------------------------------------------
//  clock - master clock of all operators
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::clock(uint32_t env_counter, int32_t lfo_raw_pm)
{
	// clock the feedback through
	m_feedback_0 = m_feedback_1;
	m_feedback_1 = m_feedback_in;

	for (uint32_t opnum = 0; opnum < m_op.size(); opnum++)
		if (m_op[opnum] != nullptr)
			m_op[opnum]->clock(env_counter, lfo_raw_pm);

/*
useful temporary code for envelope debugging
if (m_choffs == 0x101)
{
	for (uint32_t opnum = 0; opnum < m_op.size(); opnum++)
	{
		auto &op = *m_op[((opnum & 1) << 1) | ((opnum >> 1) & 1)];
		printf(" %c%03X%c%c ",
			"PADSRV"[op.debug_eg_state()],
			op.debug_eg_attenuation(),
			op.debug_ssg_inverted() ? '-' : '+',
			m_regs.op_ssg_eg_enable(op.opoffs()) ? '0' + m_regs.op_ssg_eg_mode(op.opoffs()) : ' ');
	}
printf(" -- ");
}
*/
}


//-------------------------------------------------
//  output_2op - combine 4 operators according to
//  the specified algorithm, returning a sum
//  according to the rshift and clipmax parameters,
//  which vary between different implementations
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::output_2op(output_data &output, uint32_t rshift, int32_t clipmax) const
{
	// The first 2 operators should be populated
	assert(m_op[0] != nullptr);
	assert(m_op[1] != nullptr);

	// AM amount is the same across all operators; compute it once
	uint32_t am_offset = m_regs.lfo_am_offset(m_choffs);

	// operator 1 has optional self-feedback
	int32_t opmod = 0;
	uint32_t feedback = m_regs.ch_feedback(m_choffs);
	if (feedback != 0)
		opmod = (m_feedback_0 + m_feedback_1) >> (10 - feedback);

	// compute the 14-bit volume/value of operator 1 and update the feedback
	int32_t op1value = m_feedback_in = int16_t(m_op[0]->compute_volume(m_op[0]->phase() + opmod, am_offset));

	// now that the feedback has been computed, skip the rest if all volumes
	// are clear; no need to do all this work for nothing
	if (m_regs.ch_output_any(m_choffs) == 0)
		return;

	// Algorithms for two-operator case:
	//    0: O1 -> O2 -> out
	//    1: (O1 + O2) -> out
	int32_t result;
	if (bitfield(m_regs.ch_algorithm(m_choffs), 0) == 0)
	{
		// some OPL chips use the previous sample for modulation instead of
		// the current sample
		opmod = (RegisterType::MODULATOR_DELAY ? m_feedback_1 : op1value) >> 1;
		result = m_op[1]->compute_volume(m_op[1]->phase() + opmod, am_offset) >> rshift;
	}
	else
	{
		result = (RegisterType::MODULATOR_DELAY ? m_feedback_1 : op1value) >> rshift;
		result += m_op[1]->compute_volume(m_op[1]->phase(), am_offset) >> rshift;
		int32_t clipmin = -clipmax - 1;
		result = clamp(result, clipmin, clipmax);
	}

	// add to the output
	add_to_output(m_choffs, output, result);
}


//-------------------------------------------------
//  output_4op - combine 4 operators according to
//  the specified algorithm, returning a sum
//  according to the rshift and clipmax parameters,
//  which vary between different implementations
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::output_4op(output_data &output, uint32_t rshift, int32_t clipmax) const
{
	// all 4 operators should be populated
	assert(m_op[0] != nullptr);
	assert(m_op[1] != nullptr);
	assert(m_op[2] != nullptr);
	assert(m_op[3] != nullptr);

	// AM amount is the same across all operators; compute it once
	uint32_t am_offset = m_regs.lfo_am_offset(m_choffs);

	// operator 1 has optional self-feedback
	int32_t opmod = 0;
	uint32_t feedback = m_regs.ch_feedback(m_choffs);
	if (feedback != 0)
		opmod = (m_feedback_0 + m_feedback_1) >> (10 - feedback);

	// compute the 14-bit volume/value of operator 1 and update the feedback
	int32_t op1value = m_feedback_in = int16_t(m_op[0]->compute_volume(m_op[0]->phase() + opmod, am_offset));

	// now that the feedback has been computed, skip the rest if all volumes
	// are clear; no need to do all this work for nothing
	if (m_regs.ch_output_any(m_choffs) == 0)
		return;

	// OPM/OPN offer 8 different connection algorithms for 4 operators,
	// and OPL3 offers 4 more, which we designate here as 8-11.
	//
	// The operators are computed in order, with the inputs pulled from
	// an array of values (opout) that is populated as we go:
	//    0 = 0
	//    1 = O1
	//    2 = O2
	//    3 = O3
	//    4 = (O4)
	//    5 = O1+O2
	//    6 = O1+O3
	//    7 = O2+O3
	//
	// The s_algorithm_ops table describes the inputs and outputs of each
	// algorithm as follows:
	//
	//      ---------x use opout[x] as operator 2 input
	//      ------xxx- use opout[x] as operator 3 input
	//      ---xxx---- use opout[x] as operator 4 input
	//      --x------- include opout[1] in final sum
	//      -x-------- include opout[2] in final sum
	//      x--------- include opout[3] in final sum
	uint32_t algorithm_ops = m_owner.channel_algorithm(m_chnum);

	// populate the opout table
	int16_t opout[8];
	opout[0] = 0;
	opout[1] = op1value;

	// compute the 14-bit volume/value of operator 2
	opmod = opout[bitfield(algorithm_ops, 0, 1)] >> 1;
	opout[2] = m_op[1]->compute_volume(m_op[1]->phase() + opmod, am_offset);
	opout[5] = opout[1] + opout[2];

	// compute the 14-bit volume/value of operator 3
	opmod = opout[bitfield(algorithm_ops, 1, 3)] >> 1;
	opout[3] = m_op[2]->compute_volume(m_op[2]->phase() + opmod, am_offset);
	opout[6] = opout[1] + opout[3];
	opout[7] = opout[2] + opout[3];

	// compute the 14-bit volume/value of operator 4; this could be a noise
	// value on the OPM; all algorithms consume OP4 output at a minimum
	int32_t result;
	if (m_regs.noise_enable() && m_choffs == 7)
		result = m_op[3]->compute_noise_volume(am_offset);
	else
	{
		opmod = opout[bitfield(algorithm_ops, 4, 3)] >> 1;
		result = m_op[3]->compute_volume(m_op[3]->phase() + opmod, am_offset);
	}
	result >>= rshift;

	// optionally add OP1, OP2, OP3
	int32_t clipmin = -clipmax - 1;
	if (bitfield(algorithm_ops, 7) != 0)
		result = clamp(result + (opout[1] >> rshift), clipmin, clipmax);
	if (bitfield(algorithm_ops, 8) != 0)
		result = clamp(result + (opout[2] >> rshift), clipmin, clipmax);
	if (bitfield(algorithm_ops, 9) != 0)
		result = clamp(result + (opout[3] >> rshift), clipmin, clipmax);

	// add to the output
	add_to_output(m_choffs, output, result);
}


//-------------------------------------------------
//  output_rhythm_ch6 - special case output
//  computation for OPL channel 6 in rhythm mode,
//  which outputs a Bass Drum instrument
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::output_rhythm_ch6(output_data &output, uint32_t rshift, int32_t clipmax) const
{
	// AM amount is the same across all operators; compute it once
	uint32_t am_offset = m_regs.lfo_am_offset(m_choffs);

	// Bass Drum: this uses operators 12 and 15 (i.e., channel 6)
	// in an almost-normal way, except that if the algorithm is 1,
	// the first operator is ignored instead of added in

	// operator 1 has optional self-feedback
	int32_t opmod = 0;
	uint32_t feedback = m_regs.ch_feedback(m_choffs);
	if (feedback != 0)
		opmod = (m_feedback_0 + m_feedback_1) >> (10 - feedback);

	// compute the 14-bit volume/value of operator 1 and update the feedback
	int32_t opout1 = m_feedback_in = int16_t(m_op[0]->compute_volume(m_op[0]->phase() + opmod, am_offset));

	// compute the 14-bit volume/value of operator 2, which is the result
	opmod = bitfield(m_regs.ch_algorithm(m_choffs), 0) ? 0 : (opout1 >> 1);
	int32_t result = m_op[1]->compute_volume(m_op[1]->phase() + opmod, am_offset) >> rshift;

	// add to the output
	add_to_output(m_choffs, output, result * 2);
}


//-------------------------------------------------
//  output_rhythm_ch7 - special case output
//  computation for OPL channel 7 in rhythm mode,
//  which outputs High Hat and Snare Drum
//  instruments
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::output_rhythm_ch7(uint32_t phase_select, output_data &output, uint32_t rshift, int32_t clipmax) const
{
	// AM amount is the same across all operators; compute it once
	uint32_t am_offset = m_regs.lfo_am_offset(m_choffs);
	uint32_t noise_state = bitfield(m_regs.noise_state(), 0);

	// High Hat: this uses the envelope from operator 13 (channel 7),
	// and a combination of noise and the operator 13/17 phase select
	// to compute the phase
	uint32_t phase = (phase_select << 9) | (0xd0 >> (2 * (noise_state ^ phase_select)));
	int32_t result = m_op[0]->compute_volume(phase, am_offset) >> rshift;

	// Snare Drum: this uses the envelope from operator 16 (channel 7),
	// and a combination of noise and operator 13 phase to pick a phase
	uint32_t op13phase = m_op[0]->phase();
	phase = (0x100 << bitfield(op13phase, 8)) ^ (noise_state << 8);
	result += m_op[1]->compute_volume(phase, am_offset) >> rshift;
	result = clamp(result, -clipmax - 1, clipmax);

	// add to the output
	add_to_output(m_choffs, output, result * 2);
}


//-------------------------------------------------
//  output_rhythm_ch8 - special case output
//  computation for OPL channel 8 in rhythm mode,
//  which outputs Tom Tom and Top Cymbal instruments
//-------------------------------------------------

template<class RegisterType>
void fm_channel<RegisterType>::output_rhythm_ch8(uint32_t phase_select, output_data &output, uint32_t rshift, int32_t clipmax) const
{
	// AM amount is the same across all operators; compute it once
	uint32_t am_offset = m_regs.lfo_am_offset(m_choffs);

	// Tom Tom: this is just a single operator processed normally
	int32_t result = m_op[0]->compute_volume(m_op[0]->phase(), am_offset) >> rshift;

	// Top Cymbal: this uses the envelope from operator 17 (channel 8),
	// and the operator 13/17 phase select to compute the phase
	uint32_t phase = 0x100 | (phase_select << 9);
	result += m_op[1]->compute_volume(phase, am_offset) >> rshift;
	result = clamp(result, -clipmax - 1, clipmax);

	// add to the output
	add_to_output(m_choffs, output, result * 2);
}



//*********************************************************
//  FM ENGINE BASE
//*********************************************************

//-------------------------------------------------
//  fm_engine_base - constructor
//-------------------------------------------------

template<class RegisterType>
fm_engine_base<RegisterType>::fm_engine_base(ymfm_interface &intf) :
	m_intf(intf),
	m_env_counter(0),
	m_status(0),
	m_clock_prescale(RegisterType::DEFAULT_PRESCALE),
	m_irq_mask(STATUS_TIMERA | STATUS_TIMERB),
	m_irq_state(0),
	m_timer_running{0,0},
	m_total_clocks(0),
	m_active_channels(ALL_CHANNELS),
	m_modified_channels(ALL_CHANNELS),
	m_prepare_count(0)
{
	// inform the interface of their engine
	m_intf.m_engine = this;

	m_shadow_generation = 1;
	std::memset(m_write_shadow, 0, sizeof(m_write_shadow));

	// the envelope arrays are read as whole eight-operator groups, so the
	// padding past the real operator count has to be defined
	std::memset(m_op_phase, 0, sizeof(m_op_phase));
	std::memset(m_op_phase_step, 0, sizeof(m_op_phase_step));
	std::memset(m_op_eg_shift, 0, sizeof(m_op_eg_shift));
	std::memset(m_op_total_level, 0, sizeof(m_op_total_level));
	std::memset(m_op_am_mask, 0, sizeof(m_op_am_mask));
	std::memset(m_ch_fb_shift, 0, sizeof(m_ch_fb_shift));
	std::memset(m_ch_fb_mask, 0, sizeof(m_ch_fb_mask));
	std::memset(m_ch_algorithm, 0, sizeof(m_ch_algorithm));
	std::memset(m_ch_out0_mask, 0, sizeof(m_ch_out0_mask));
	std::memset(m_ch_out1_mask, 0, sizeof(m_ch_out1_mask));
	std::memset(m_ch_out_any, 0, sizeof(m_ch_out_any));
	std::memset(m_ch_offs, 0, sizeof(m_ch_offs));
	std::memset(m_ch_fb0, 0, sizeof(m_ch_fb0));
	std::memset(m_ch_fb1, 0, sizeof(m_ch_fb1));
	std::memset(m_ch_fb_in, 0, sizeof(m_ch_fb_in));
	for (uint32_t opnum = 0; opnum < EG_COUNT; opnum++)
		m_op_waveform[opnum] = nullptr;
	std::memset(m_eg_atten, 0, sizeof(m_eg_atten));
	std::memset(m_eg_state, 0, sizeof(m_eg_state));
	std::memset(m_eg_sustain, 0, sizeof(m_eg_sustain));
	std::memset(m_eg_cur_rate, 0, sizeof(m_eg_cur_rate));
	std::memset(m_eg_cur_inc, 0, sizeof(m_eg_cur_inc));
	std::memset(m_eg_rate, 0, sizeof(m_eg_rate));
	std::memset(m_eg_inc, 0, sizeof(m_eg_inc));

	// create the channels
	for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
		m_channel[chnum] = std::make_unique<fm_channel<RegisterType>>(*this, chnum, RegisterType::channel_offset(chnum));

	// create the operators
	for (uint32_t opnum = 0; opnum < OPERATORS; opnum++)
		m_operator[opnum] = std::make_unique<fm_operator<RegisterType>>(*this, opnum, RegisterType::operator_offset(opnum));

#if (YMFM_DEBUG_LOG_WAVFILES)
	for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
		m_wavfile[chnum].set_index(chnum);
#endif

	// the kernels read through a block of pointers that never change
	m_eg_block.atten = m_eg_atten;
	m_eg_block.state = m_eg_state;
	m_eg_block.sustain = m_eg_sustain;
	m_eg_block.cur_rate = m_eg_cur_rate;
	m_eg_block.cur_inc = m_eg_cur_inc;
	m_eg_block.rate_of = &m_eg_rate[0][0];
	m_eg_block.inc_of = &m_eg_inc[0][0];
	m_eg_block.count = EG_COUNT;
	m_eg_block.has_reverb = RegisterType::EG_HAS_REVERB;

	m_out_block.phase = m_op_phase;
	m_out_block.env_atten = m_eg_atten;
	m_out_block.eg_shift = m_op_eg_shift;
	m_out_block.total_level = m_op_total_level;
	m_out_block.am_mask = m_op_am_mask;
	m_out_block.waveform = m_op_waveform;
	m_out_block.slot_base = m_slot_base;
	m_out_block.am_offset = m_ch_am_offset;
	m_out_block.fb_shift = m_ch_fb_shift;
	m_out_block.fb_mask = m_ch_fb_mask;
	m_out_block.fb0 = m_ch_fb0;
	m_out_block.fb1 = m_ch_fb1;
	m_out_block.fb_in = m_ch_fb_in;
	m_out_block.active = m_ch_active;
	m_out_block.algorithm = m_ch_algorithm;
	m_out_block.out0_mask = m_ch_out0_mask;
	m_out_block.out1_mask = m_ch_out1_mask;
	m_out_block.contributes = m_ch_contributes;
	m_out_block.clipmax = 0;

	// do the initial operator assignment
	assign_operators();
}


//-------------------------------------------------
//  publish_eg_cache - mirror one operator's
//  envelope cache into the parallel arrays
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::publish_op_cache(uint32_t opnum, uint32_t opoffs, opdata_cache const &cache)
{
	m_eg_sustain[opnum] = cache.eg_sustain;
	for (uint32_t state = 0; state < EG_STATES; state++)
	{
		uint32_t const rate = cache.eg_rate[state];
		m_eg_rate[state][opnum] = rate;
		// the increment table is eight 4-bit values packed into one word and
		// depends only on the rate, so resolve it here instead of indexing a
		// table per operator per sample
		m_eg_inc[state][opnum] = attenuation_increment_packed(rate);
	}
	uint32_t const current = m_eg_state[opnum];
	m_eg_cur_rate[opnum] = m_eg_rate[current][opnum];
	m_eg_cur_inc[opnum] = m_eg_inc[current][opnum];

	m_op_phase_step[opnum] = cache.phase_step;
	m_op_eg_shift[opnum] = cache.eg_shift;
	m_op_total_level[opnum] = cache.total_level;
	m_op_am_mask[opnum] = m_regs.op_lfo_am_enable(opoffs) ? 0xffffffffu : 0u;
	m_op_waveform[opnum] = cache.waveform;
}


//-------------------------------------------------
//  publish_channel_cache - decode the per-channel
//  register fields the output stage reads
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::publish_channel_cache(uint32_t chnum, uint32_t choffs)
{
	uint32_t const feedback = m_regs.ch_feedback(choffs);
	// feedback of 0 means no feedback at all, not a shift of 10
	m_ch_fb_shift[chnum] = 10 - feedback;
	m_ch_fb_mask[chnum] = (feedback != 0) ? 0xffffffffu : 0u;
	m_ch_algorithm[chnum] = algorithm_ops_for(m_regs.ch_algorithm(choffs));
	m_ch_out0_mask[chnum] = m_regs.ch_output_0(choffs) ? 0xffffffffu : 0u;
	m_ch_out1_mask[chnum] = m_regs.ch_output_1(choffs) ? 0xffffffffu : 0u;
	m_ch_out_any[chnum] = m_regs.ch_output_any(choffs) ? 0xffffffffu : 0u;
	m_ch_offs[chnum] = choffs;
}


//-------------------------------------------------
//  reset - reset the overall state
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::reset()
{
	// reset all status bits
	set_reset_status(0, 0xff);

	// register type-specific initialization
	m_regs.reset();

	// the register file just changed underneath the shadow
	forget_written_values();

	// explicitly write to the mode register since it has side-effects
	// QUESTION: old cores initialize this to 0x30 -- who is right?
	write(RegisterType::REG_MODE, 0);

	// reset the channels
	for (auto &chan : m_channel)
		chan->reset();

	// reset the operators
	for (auto &op : m_operator)
		op->reset();
}


//-------------------------------------------------
//  save_restore - save or restore the data
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::save_restore(ymfm_saved_state &state)
{
	// a restore replaces the register file wholesale
	forget_written_values();

	// save our data
	state.save_restore(m_env_counter);
	state.save_restore(m_status);
	state.save_restore(m_clock_prescale);
	state.save_restore(m_irq_mask);
	state.save_restore(m_irq_state);
	state.save_restore(m_timer_running[0]);
	state.save_restore(m_timer_running[1]);
	state.save_restore(m_total_clocks);

	// save the register/family data
	m_regs.save_restore(state);

	// save channel data
	for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
		m_channel[chnum]->save_restore(state);

	// save operator data
	for (uint32_t opnum = 0; opnum < OPERATORS; opnum++)
		m_operator[opnum]->save_restore(state);

	// invalidate any caches
	invalidate_caches();
}


//-------------------------------------------------
//  clock - iterate over all channels, clocking
//  them forward one step
//-------------------------------------------------

template<class RegisterType>
uint32_t fm_engine_base<RegisterType>::clock(uint32_t chanmask)
{
	// update the clock counter
	m_total_clocks++;

	// if something was modified, prepare
	// also prepare every 4k samples to catch ending notes
	if (m_modified_channels != 0 || m_prepare_count++ >= 4096)
	{
		// reassign operators to channels if dynamic
		if (RegisterType::DYNAMIC_OPS)
			assign_operators();

		// call each channel to prepare
		m_active_channels = 0;
		for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
			if (bitfield(chanmask, chnum))
				if (m_channel[chnum]->prepare())
					m_active_channels |= 1 << chnum;

		// Collect the operators whose step moves every sample. Everything
		// else keeps the published step, so the per-sample pass is a straight
		// add over the whole chip.
		m_dynamic_count = 0;
		for (uint32_t opnum = 0; opnum < OPERATORS; opnum++)
			if (m_operator[opnum] != nullptr && m_operator[opnum]->cached_phase_step() == opdata_cache::PHASE_STEP_DYNAMIC)
				m_dynamic_ops[m_dynamic_count++] = uint8_t(opnum);

		// Whether the vector output stage applies depends only on the
		// registers, so decide it here rather than on every sample. Noise
		// steals channel 7's fourth operator, and a two-operator channel has a
		// different shape.
		m_vector_output_now = m_vector_output_ok && m_regs.noise_enable() == 0;
		for (uint32_t chnum = 0; chnum < CHANNELS && m_vector_output_now; chnum++)
			if (!m_channel[chnum]->is4op())
				m_vector_output_now = false;

		// reset the modified channels and prepare count
		m_modified_channels = m_prepare_count = 0;
	}

	// if the envelope clock divider is 1, just increment by 4;
	// otherwise, increment by 1 and manually wrap when we reach the divide count
	if (RegisterType::EG_CLOCK_DIVIDER == 1)
		m_env_counter += 4;
	else if (bitfield(++m_env_counter, 0, 2) == RegisterType::EG_CLOCK_DIVIDER)
		m_env_counter += 4 - RegisterType::EG_CLOCK_DIVIDER;

	// clock the noise generator
	int32_t lfo_raw_pm = m_regs.clock_noise_and_lfo();

	// clock every operator's envelope in one pass; the per-operator path is
	// only used by families whose SSG-EG stage has to run in between
	if (!RegisterType::EG_HAS_SSG && bitfield(m_env_counter, 0, 2) == 0)
	{
		eg_clock(m_eg_block, m_env_counter >> 2);
	}

	// now update the state of all the channels and operators
	if (VECTOR_PHASE && chanmask == RegisterType::ALL_CHANNELS)
	{
		// The whole chip's phase in one pass. Only the operators the cache
		// marked dynamic need a step recomputed; everyone else kept theirs
		// from the last prepare, so the rest is a straight parallel add.
		// The step a static operator would compute here is the one it already
		// has, so the whole chip can go through the same pass rather than
		// gathering the dynamic ones out of it; when nothing is dynamic there
		// is nothing to do at all.
		if (m_dynamic_count != 0
			&& !dynamic_phase_steps<RegisterType>(m_regs, lfo_raw_pm, EG_COUNT, m_op_phase_step))
		{
			for (uint32_t index = 0; index < m_dynamic_count; index++)
			{
				uint32_t const opnum = m_dynamic_ops[index];
				m_op_phase_step[opnum] = m_operator[opnum]->dynamic_phase_step(lfo_raw_pm);
			}
		}
		phase_clock(m_op_phase, m_op_phase_step, EG_COUNT);

		// the operator half of fm_channel::clock is what we just did; the
		// feedback shift register is all that is left, and it lives in the
		// engine's arrays too, so it goes eight lanes at a time as well
		static_assert(CH_COUNT == 8 || !VECTOR_PHASE, "the slot helpers take eight channels");
		feedback_clock_x8(m_ch_fb0, m_ch_fb1, m_ch_fb_in);
	}
	else
	{
		for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
			if (bitfield(chanmask, chnum))
				m_channel[chnum]->clock(m_env_counter, lfo_raw_pm);
	}

	// return the envelope counter as it is used to clock ADPCM-A
	return m_env_counter;
}


//-------------------------------------------------
//  output - compute a sum over the relevant
//  channels
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::output(output_data &output, uint32_t rshift, int32_t clipmax, uint32_t chanmask) const
{
	// mask out some channels for debug purposes
	chanmask &= debug::GLOBAL_FM_CHANNEL_MASK;

	// mask out inactive channels
	if (!YMFM_DEBUG_LOG_WAVFILES)
		chanmask &= m_active_channels;

	// handle the rhythm case, where some of the operators are dedicated
	// to percussion (this is an OPL-specific feature)
	if (m_regs.rhythm_enable())
	{
		// we don't support the OPM noise channel here; ensure it is off
		assert(m_regs.noise_enable() == 0);

		// precompute the operator 13+17 phase selection value
		uint32_t op13phase = m_operator[13]->phase();
		uint32_t op17phase = m_operator[17]->phase();
		uint32_t phase_select = (bitfield(op13phase, 2) ^ bitfield(op13phase, 7)) | bitfield(op13phase, 3) | (bitfield(op17phase, 5) ^ bitfield(op17phase, 3));

		// sum over all the desired channels
		for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
			if (bitfield(chanmask, chnum))
			{
#if (YMFM_DEBUG_LOG_WAVFILES)
				auto reference = output;
#endif
				if (chnum == 6)
					m_channel[chnum]->output_rhythm_ch6(output, rshift, clipmax);
				else if (chnum == 7)
					m_channel[chnum]->output_rhythm_ch7(phase_select, output, rshift, clipmax);
				else if (chnum == 8)
					m_channel[chnum]->output_rhythm_ch8(phase_select, output, rshift, clipmax);
				else if (m_channel[chnum]->is4op())
					m_channel[chnum]->output_4op(output, rshift, clipmax);
				else
					m_channel[chnum]->output_2op(output, rshift, clipmax);
#if (YMFM_DEBUG_LOG_WAVFILES)
				m_wavfile[chnum].add(output, reference);
#endif
			}
	}
	else
	{
#if YMFM_HAVE_VECTOR_OUTPUT
		// Eight channels at once. Noise steals channel 7's fourth operator and
		// a two-operator channel has a different shape, so either sends this
		// sample down the per-channel path instead.
		if (m_vector_output_now && rshift == 0 && !YMFM_DEBUG_LOG_WAVFILES)
		{
			lfo_am_offsets<RegisterType>(m_regs, m_ch_offs, CHANNELS, m_ch_am_offset);
			channel_masks_x8(chanmask, m_ch_out_any, m_ch_active, m_ch_contributes);

			m_out_block.clipmax = clipmax;

			int32_t out0 = output.data[0];
			int32_t out1 = output.data[1 % RegisterType::OUTPUTS];
			fm_output_4op_x8(m_out_block, out0, out1);
			output.data[0] = out0;
			output.data[1 % RegisterType::OUTPUTS] = out1;
			return;
		}
#endif

		// sum over all the desired channels
		for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
			if (bitfield(chanmask, chnum))
			{
#if (YMFM_DEBUG_LOG_WAVFILES)
				auto reference = output;
#endif
				if (m_channel[chnum]->is4op())
					m_channel[chnum]->output_4op(output, rshift, clipmax);
				else
					m_channel[chnum]->output_2op(output, rshift, clipmax);
#if (YMFM_DEBUG_LOG_WAVFILES)
				m_wavfile[chnum].add(output, reference);
#endif
			}
	}
}


//-------------------------------------------------
//  write - handle writes to the OPN registers
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::write(uint16_t regnum, uint8_t data)
{
	debug::log_fm_write("%03X = %02X\n", regnum, data);

	// special case: writes to the mode register can impact IRQs;
	// schedule these writes to ensure ordering with timers
	if (regnum == RegisterType::REG_MODE)
	{
		m_intf.ymfm_sync_mode_write(data);
		return;
	}

	// Most of what a running firmware writes is the value the register
	// already holds, and that cannot change any cached value. The write still
	// goes through for its side effects; only the invalidation is skipped.
	bool modified = true;
	if (write_is_pure<RegisterType>(regnum) && regnum < RegisterType::REGISTERS)
	{
		uint32_t const stamped = (m_shadow_generation << 8) | data;
		modified = (m_write_shadow[regnum] != stamped);
		m_write_shadow[regnum] = stamped;
	}
	else
	{
		// this write can reach registers other than its own, so nothing we
		// remember about the register file can be trusted afterwards
		forget_written_values();
	}

	// most writes are passive, consumed only when needed
	uint32_t keyon_channel;
	uint32_t keyon_opmask;
	if (m_regs.write(regnum, data, keyon_channel, keyon_opmask))
	{
		// key state lives outside the register file, so this always counts
		modified = true;

		// handle writes to the keyon register(s)
		if (keyon_channel < CHANNELS)
		{
			// normal channel on/off
			m_channel[keyon_channel]->keyonoff(keyon_opmask, KEYON_NORMAL, keyon_channel);
		}
		else if (CHANNELS >= 9 && keyon_channel == RegisterType::RHYTHM_CHANNEL)
		{
			// special case for the OPL rhythm channels
			m_channel[6]->keyonoff(bitfield(keyon_opmask, 4) ? 3 : 0, KEYON_RHYTHM, 6);
			m_channel[7]->keyonoff(bitfield(keyon_opmask, 0) | (bitfield(keyon_opmask, 3) << 1), KEYON_RHYTHM, 7);
			m_channel[8]->keyonoff(bitfield(keyon_opmask, 2) | (bitfield(keyon_opmask, 1) << 1), KEYON_RHYTHM, 8);
		}
	}

	if (modified)
		m_modified_channels = ALL_CHANNELS;
}


//-------------------------------------------------
//  status - return the current state of the
//  status flags
//-------------------------------------------------

template<class RegisterType>
uint8_t fm_engine_base<RegisterType>::status() const
{
	return m_status & ~STATUS_BUSY & ~m_regs.status_mask();
}


//-------------------------------------------------
//  assign_operators - get the current mapping of
//  operators to channels and assign them all
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::assign_operators()
{
	typename RegisterType::operator_mapping map;
	m_regs.operator_map(map);

	for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
		for (uint32_t index = 0; index < 4; index++)
		{
			uint32_t opnum = bitfield(map.chan[chnum], 8 * index, 8);
			m_channel[chnum]->assign(index, (opnum == 0xff) ? nullptr : m_operator[opnum].get());
		}

	// The vector output stage indexes operators as "slot base plus channel",
	// so it only applies when the mapping really is laid out that way and the
	// chip is eight stereo four-operator channels. Checked rather than
	// assumed, because the mapping is per-family.
	m_vector_output_now = false;
	m_vector_output_ok = (CHANNELS == 8 && RegisterType::OUTPUTS == 2);
	for (uint32_t index = 0; index < 4 && m_vector_output_ok; index++)
	{
		uint32_t const first = bitfield(map.chan[0], 8 * index, 8);
		m_slot_base[index] = first;
		for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
			if (bitfield(map.chan[chnum], 8 * index, 8) != first + chnum)
				m_vector_output_ok = false;
	}
}


//-------------------------------------------------
//  update_timer - update the state of the given
//  timer
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::update_timer(uint32_t tnum, uint32_t enable, int32_t delta_clocks)
{
	// if the timer is live, but not currently enabled, set the timer
	if (enable && !m_timer_running[tnum])
	{
		// period comes from the registers, and is different for each
		uint32_t period = (tnum == 0) ? (1024 - m_regs.timer_a_value()) : 16 * (256 - m_regs.timer_b_value());

		// caller can also specify a delta to account for other effects
		period += delta_clocks;

		// reset it
		m_intf.ymfm_set_timer(tnum, period * OPERATORS * m_clock_prescale);
		m_timer_running[tnum] = 1;
	}

	// if the timer is not live, ensure it is not enabled
	else if (!enable)
	{
		m_intf.ymfm_set_timer(tnum, -1);
		m_timer_running[tnum] = 0;
	}
}


//-------------------------------------------------
//  engine_timer_expired - timer has expired - signal
//  status and possibly IRQs
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::engine_timer_expired(uint32_t tnum)
{
	assert(tnum == 0 || tnum == 1);

	// update status
	if (tnum == 0 && m_regs.enable_timer_a())
		set_reset_status(STATUS_TIMERA, 0);
	else if (tnum == 1 && m_regs.enable_timer_b())
		set_reset_status(STATUS_TIMERB, 0);

	// if timer A fired in CSM mode, trigger CSM on all relevant channels
	if (tnum == 0 && m_regs.csm())
		for (uint32_t chnum = 0; chnum < CHANNELS; chnum++)
			if (bitfield(RegisterType::CSM_TRIGGER_MASK, chnum))
			{
				m_channel[chnum]->keyonoff(0xf, KEYON_CSM, chnum);
				m_modified_channels |= 1 << chnum;
			}

	// reset
	m_timer_running[tnum] = false;
	update_timer(tnum, 1, 0);
}


//-------------------------------------------------
//  check_interrupts - check the interrupt sources
//  for interrupts
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::engine_check_interrupts()
{
	// update the state
	uint8_t old_state = m_irq_state;
	m_irq_state = ((m_status & m_irq_mask & ~m_regs.status_mask()) != 0);

	// set the IRQ status bit
	if (m_irq_state)
		m_status |= STATUS_IRQ;
	else
		m_status &= ~STATUS_IRQ;

	// if changed, signal the new state
	if (old_state != m_irq_state)
		m_intf.ymfm_update_irq(m_irq_state ? true : false);
}


//-------------------------------------------------
//  engine_mode_write - handle a mode register write
//  via timer callback
//-------------------------------------------------

template<class RegisterType>
void fm_engine_base<RegisterType>::engine_mode_write(uint8_t data)
{
	// mark all channels as modified
	m_modified_channels = ALL_CHANNELS;

	// actually write the mode register now
	uint32_t dummy1, dummy2;
	m_regs.write(RegisterType::REG_MODE, data, dummy1, dummy2);

	// reset IRQ status -- when written, all other bits are ignored
	// QUESTION: should this maybe just reset the IRQ bit and not all the bits?
	//   That is, check_interrupts would only set, this would only clear?
	if (m_regs.irq_reset())
		set_reset_status(0, 0x78);
	else
	{
		// reset timer status
		uint8_t reset_mask = 0;
		if (m_regs.reset_timer_b())
			reset_mask |= RegisterType::STATUS_TIMERB;
		if (m_regs.reset_timer_a())
			reset_mask |= RegisterType::STATUS_TIMERA;
		set_reset_status(0, reset_mask);

		// load timers; note that timer B gets a small negative adjustment because
		// the *16 multiplier is free-running, so the first tick of the clock
		// is a bit shorter
		update_timer(1, m_regs.load_timer_b(), -(m_total_clocks & 15));
		update_timer(0, m_regs.load_timer_a(), 0);
	}
}

}
