"""Let the audio's signal processing compile like native code.

wasm2c turns every ordinary load and store on the engine's shared memory into a relaxed atomic
on a volatile pointer, plus an empty asm that forces the read. Correct for any code, but the
compiler can't vectorise, combine or reorder anything, and a profile of a benchmark on an
iPhone 13 Pro Max put the audio (Opus/Silk/CELT decoding, mixing, reverb, the synthesiser) at
about 37% of the game's CPU time; that heat throttles the phone within minutes.

These functions only transform buffers: they never wait on another thread through an ordinary
load (their synchronisation uses wasm atomics, which stay atomic). Their ordinary loads and
stores become plain memcpy accesses, as the browser's compiler treats them.
"""
from pathlib import Path
from engine_names import fn
import re
root = Path(__file__).resolve().parents[1]
# The engine functions, by their name in game.wasm's name section (Scripts/engine_names.py).
NAMES = [
    'rage::audBiquadFilterEffect::Process(rage::audDspEffectBuffer&)',
    'rage::audBiquadFilterEffect::ProcessInterleaved_4Pole(float vector[4], float vector[4], float vector[4]&, float vector[4]&, float vector[4]&, float vector[4]&, float const*, float*, unsigned int)',
    'rage::audBiquadFilterEffect::ProcessInterleaved_2Pole(float vector[4], float vector[4], float vector[4]&, float vector[4]&, float const*, float*, unsigned int)',
    'rage::audDelayEffect::Process(rage::audDspEffectBuffer&)',
    'rage::audDelayEffect4::Process(rage::audDspEffectBuffer&)',
    'rage::audEarlyReflectionEffect::Process(rage::audDspEffectBuffer&)',
    'rage::audEarlyReflectionEffect::ProcessInterleaved(float const*, float*, unsigned int)',
    'rage::audReverbEffect4::Process(rage::audDspEffectBuffer&)',
    'rage::audReverbEffect4::ProcessInterleaved(float const*, float*, unsigned int)',
    'rage::audUnderwaterEffect::Process(rage::audDspEffectBuffer&)',
    'rage::audUnderwaterEffect::ProcessInterleaved(float const*, float*, unsigned int)',
    'rage::audVariableDelayEffect::Process(rage::audDspEffectBuffer&)',
    'rage::synthCore::GenerateFrame(float const**, rage::atRangeArray<unsigned short, 4> const&)',
    'rage::synthCoreDspEffect::Process(rage::audDspEffectBuffer&)',
    'rage::audMixerDevice::MixBuffers()',
    'void rage::InterleaveAndMix_1to4<256u>(float vector[4], float vector[4], float const*, float*, float*, float*, float*)',
    'rage::audRateConvertorLinear::Process(float, float*, short const*, unsigned int, unsigned int, float)',
    'rage::audMixerSubmix::ConvertBufferToFormat(rage::audDspEffectBuffer&, rage::audMixBufferFormat)',
    'silk_bwexpander_32',
    'silk_LPC_inverse_pred_gain_c',
    'silk_NLSF2A',
    'silk_bwexpander',
    'silk_LPC_analysis_filter',
    'silk_sum_sqr_shift',
    'silk_PLC',
    'silk_reset_decoder',
    'silk_resampler_private_up2_HQ',
    'silk_resampler_private_IIR_FIR',
    'silk_resampler_private_down_FIR',
    'silk_resampler',
    'ec_dec_init',
    'ec_decode',
    'ec_dec_update',
    'ec_dec_bit_logp',
    'ec_dec_icdf',
    'ec_dec_uint',
    'ec_dec_bits',
    'silk_stereo_decode_pred',
    'silk_stereo_decode_mid_only',
    'silk_NLSF_unpack',
    'silk_decode_indices',
    'silk_decode_pulses',
    'silk_decode_frame',
    'silk_Get_Decoder_Size',
    'silk_ResetDecoder',
    'comb_filter',
    'unquant_energy_finalise',
    'exp_rotation',
    'renormalise_vector',
    'denormalise_bands',
    'quant_band',
    'quant_partition',
    '_celt_lpc',
    '_celt_autocorr',
    'celt_pitch_xcorr_c',
    'clt_mdct_backward_c',
    'celt_decoder_get_size',
    'celt_decode_with_ec_dred',
    'deemphasis',
    'celt_synthesis',
    'celt_decode_with_ec',
    'opus_decode_frame',
    'lrint',
    'lrintf',
]
FUNCTIONS = [fn(name) for name in NAMES]
LOAD = re.compile(r'\b((?:i32|i64|f32|f64)_load(?:8_s|8_u|16_s|16_u|32_s|32_u)?|v128_load)_shared\(')
STORE = re.compile(r'\b((?:i32|i64|f32|f64)_store(?:8|16|32)?|v128_store)_shared\(')
HEADER = '''// Included right after the file's game-impl header (which has no include guard).
#include <string.h>
// Plain (non-atomic, non-volatile) ordinary accesses for the audio DSP (Scripts/prepare_plain_dsp.py).
// PG_ATOMIC_DSP=1 builds keep wasm2c's atomic accesses there, for comparisons.
#if PG_ATOMIC_DSP
#define PG_PLAIN_LOAD(name, t1, t2, t3) static inline t3 name##_plain(uint8_t* const base, uint64_t size, wasm_rt_shared_memory_t* mem, u64 addr) { return name##_shared(base, size, mem, addr); }
#define PG_PLAIN_STORE(name, t1, t2) static inline void name##_plain(uint8_t* const base, uint64_t size, wasm_rt_shared_memory_t* mem, u64 addr, t2 value) { name##_shared(base, size, mem, addr, value); }
#define v128_load_plain v128_load_shared
#define v128_store_plain v128_store_shared
#else
#define PG_PLAIN_LOAD(name, t1, t2, t3) static inline t3 name##_plain(uint8_t* const base, uint64_t size, wasm_rt_shared_memory_t* mem, u64 addr) { \\
  (void)base; (void)size; t1 value; memcpy(&value, (const void*)(uintptr_t)MEM_ADDR(mem, addr, sizeof(t1)), sizeof(t1)); return (t3)(t2)value; }
#define PG_PLAIN_STORE(name, t1, t2) static inline void name##_plain(uint8_t* const base, uint64_t size, wasm_rt_shared_memory_t* mem, u64 addr, t2 value) { \\
  (void)base; (void)size; t1 wrapped = (t1)value; memcpy((void*)(uintptr_t)MEM_ADDR(mem, addr, sizeof(t1)), &wrapped, sizeof(t1)); }
static inline v128 v128_load_plain(uint8_t* const base, uint64_t size, wasm_rt_shared_memory_t* mem, u64 addr) {
  (void)base; (void)size; return simde_wasm_v128_load((const void*)(uintptr_t)MEM_ADDR(mem, addr, 16)); }
static inline void v128_store_plain(uint8_t* const base, uint64_t size, wasm_rt_shared_memory_t* mem, u64 addr, v128 value) {
  (void)base; (void)size; simde_wasm_v128_store((void*)(uintptr_t)MEM_ADDR(mem, addr, 16), value); }
#endif
PG_PLAIN_LOAD(i32_load, u32, u32, u32) PG_PLAIN_LOAD(i64_load, u64, u64, u64) PG_PLAIN_LOAD(f32_load, f32, f32, f32) PG_PLAIN_LOAD(f64_load, f64, f64, f64)
PG_PLAIN_LOAD(i32_load8_s, s8, s32, u32) PG_PLAIN_LOAD(i64_load8_s, s8, s64, u64) PG_PLAIN_LOAD(i32_load8_u, u8, u32, u32) PG_PLAIN_LOAD(i64_load8_u, u8, u64, u64)
PG_PLAIN_LOAD(i32_load16_s, s16, s32, u32) PG_PLAIN_LOAD(i64_load16_s, s16, s64, u64) PG_PLAIN_LOAD(i32_load16_u, u16, u32, u32) PG_PLAIN_LOAD(i64_load16_u, u16, u64, u64)
PG_PLAIN_LOAD(i64_load32_s, s32, s64, u64) PG_PLAIN_LOAD(i64_load32_u, u32, u64, u64)
PG_PLAIN_STORE(i32_store, u32, u32) PG_PLAIN_STORE(i64_store, u64, u64) PG_PLAIN_STORE(f32_store, f32, f32) PG_PLAIN_STORE(f64_store, f64, f64)
PG_PLAIN_STORE(i32_store8, u8, u32) PG_PLAIN_STORE(i32_store16, u16, u32) PG_PLAIN_STORE(i64_store8, u8, u64) PG_PLAIN_STORE(i64_store16, u16, u64) PG_PLAIN_STORE(i64_store32, u32, u64)
'''
wanted = set(FUNCTIONS)
patched = 0
for path in sorted((root / 'Generated').glob('game_*.c')):
    s = path.read_text()
    changed = False
    for found in list(re.finditer(r'^[a-z0-9]+ w2c_game_f(\d+)\(w2c_game\* instance[^\n]*\{$', s, re.M)):
        if int(found.group(1)) not in wanted: continue
        start = s.index(found.group(0))
        end = s.index('\n}\n', start)
        body = s[start:end]
        new = STORE.sub(r'\1_plain(', LOAD.sub(r'\1_plain(', body))
        if new != body:
            s = s[:start] + new + s[end:]; changed = True; patched += 1
        wanted.discard(int(found.group(1)))
    if changed:
        include = '#include "game-plain-impl.h"'
        if include not in s:
            first = re.search(r'^#include "game-[a-z-]*impl\.h"$', s, re.M)
            s = s[:first.end()] + '\n' + include + s[first.end():]
        path.write_text(s)
(root / 'Generated/game-plain-impl.h').write_text(HEADER)
print(f'plain DSP: {patched} functions patched, {len(FUNCTIONS)} pinned')
