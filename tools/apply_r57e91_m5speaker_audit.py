from pathlib import Path
import shutil, sys

root = Path(__file__).resolve().parents[1]
target = root / 'components' / 'M5Unified' / 'src' / 'utility' / 'Speaker_Class.cpp'
if not target.exists():
    print(f'ERROR: M5Unified Speaker_Class.cpp not found: {target}')
    sys.exit(2)

s = target.read_text(encoding='utf-8')
marker = 'PX68K_M5SPK_R57E91'
if marker in s:
    print('R57E91 M5 speaker audit already applied.')
    sys.exit(0)

anchors = [
    '#include <esp_log.h>',
    'namespace m5\n{',
    '    bool flg_nodata = false;',
    '              self->_play_channel_bits.fetch_and(~(1 << ch));',
    '      flg_nodata = (data_length == 0);',
    '          while (!ulTaskNotifyTake( pdTRUE, 0 ) && --retry)\n          {\n            size_t write_bytes;\n            _i2s_write(i2s_port, sound_buf32, dma_buf_len * sizeof(int32_t), &write_bytes, portMAX_DELAY);\n          }',
    '        _i2s_write(i2s_port, sound_buf32, data_bytes, &write_bytes, 0);',
]
missing = [a for a in anchors if a not in s]
if missing:
    print('ERROR: local M5Unified source does not match the safe R57E91 patch anchors.')
    for a in missing:
        print('  missing:', repr(a))
    print('No M5Unified file was changed.')
    sys.exit(3)

bak = target.with_suffix(target.suffix + '.r57e91.bak')
if not bak.exists():
    shutil.copy2(target, bak)
    print('Backup:', bak)

s = s.replace('#include <esp_log.h>', '#include <esp_log.h>\n#include <esp_timer.h>', 1)

block = r'''
/* PX68K_M5SPK_R57E91
 * Counter-only tap inside M5Unified 0.2.19 Speaker_Class.  It does not alter
 * queueing, interpolation, I2S parameters, DMA sizes, or wait policy.  The
 * purpose is to distinguish an upstream PCM discontinuity from a real
 * virtual-wav/I2S starvation that the Tab5 host-ring counters can miss. */
static std::atomic<uint32_t> s_px68k_r91_arm{0};
static std::atomic<uint32_t> s_px68k_r91_qexhaust{0};
static std::atomic<uint32_t> s_px68k_r91_qgap_min{0xFFFFFFFFu};
static std::atomic<uint32_t> s_px68k_r91_qgap_max{0};
static std::atomic<uint32_t> s_px68k_r91_q_last_us{0};
static std::atomic<uint32_t> s_px68k_r91_nodata_enter{0};
static std::atomic<uint32_t> s_px68k_r91_zero_dma{0};
static std::atomic<uint32_t> s_px68k_r91_zero_dma_cur{0};
static std::atomic<uint32_t> s_px68k_r91_zero_dma_max{0};

static inline void px68k_r91_atomic_max(std::atomic<uint32_t>& dst, uint32_t v)
{
  uint32_t old = dst.load(std::memory_order_relaxed);
  while (v > old && !dst.compare_exchange_weak(old, v, std::memory_order_relaxed)) {}
}

static inline void px68k_r91_atomic_min(std::atomic<uint32_t>& dst, uint32_t v)
{
  uint32_t old = dst.load(std::memory_order_relaxed);
  while (v < old && !dst.compare_exchange_weak(old, v, std::memory_order_relaxed)) {}
}

static inline void px68k_r91_note_qexhaust(void)
{
  if (!s_px68k_r91_arm.load(std::memory_order_relaxed)) return;
  const uint32_t now = (uint32_t)esp_timer_get_time();
  const uint32_t last = s_px68k_r91_q_last_us.exchange(now, std::memory_order_relaxed);
  s_px68k_r91_qexhaust.fetch_add(1, std::memory_order_relaxed);
  if (last) {
    const uint32_t gap = now - last;
    px68k_r91_atomic_min(s_px68k_r91_qgap_min, gap);
    px68k_r91_atomic_max(s_px68k_r91_qgap_max, gap);
  }
}

extern "C" void px68k_m5spk_r57e91_reset(void)
{
  s_px68k_r91_arm.store(0, std::memory_order_release);
  s_px68k_r91_qexhaust.store(0, std::memory_order_relaxed);
  s_px68k_r91_qgap_min.store(0xFFFFFFFFu, std::memory_order_relaxed);
  s_px68k_r91_qgap_max.store(0, std::memory_order_relaxed);
  s_px68k_r91_q_last_us.store(0, std::memory_order_relaxed);
  s_px68k_r91_nodata_enter.store(0, std::memory_order_relaxed);
  s_px68k_r91_zero_dma.store(0, std::memory_order_relaxed);
  s_px68k_r91_zero_dma_cur.store(0, std::memory_order_relaxed);
  s_px68k_r91_zero_dma_max.store(0, std::memory_order_relaxed);
  s_px68k_r91_arm.store(1, std::memory_order_release);
}

extern "C" void px68k_m5spk_r57e91_get(uint32_t* qexhaust,
                                         uint32_t* qgap_min_us,
                                         uint32_t* qgap_max_us,
                                         uint32_t* nodata_enter,
                                         uint32_t* zero_dma_writes,
                                         uint32_t* zero_dma_burst_max)
{
  s_px68k_r91_arm.store(0, std::memory_order_release);
  if (qexhaust) *qexhaust = s_px68k_r91_qexhaust.load(std::memory_order_relaxed);
  if (qgap_min_us) {
    const uint32_t v = s_px68k_r91_qgap_min.load(std::memory_order_relaxed);
    *qgap_min_us = (v == 0xFFFFFFFFu) ? 0u : v;
  }
  if (qgap_max_us) *qgap_max_us = s_px68k_r91_qgap_max.load(std::memory_order_relaxed);
  if (nodata_enter) *nodata_enter = s_px68k_r91_nodata_enter.load(std::memory_order_relaxed);
  if (zero_dma_writes) *zero_dma_writes = s_px68k_r91_zero_dma.load(std::memory_order_relaxed);
  if (zero_dma_burst_max) *zero_dma_burst_max = s_px68k_r91_zero_dma_max.load(std::memory_order_relaxed);
}

'''
s = s.replace('namespace m5\n{', block + 'namespace m5\n{', 1)
s = s.replace('    bool flg_nodata = false;', '    bool flg_nodata = false;\n    bool px68k_r91_prev_nodata = false;', 1)

old = '              self->_play_channel_bits.fetch_and(~(1 << ch));'
new = '''              const auto px68k_r91_old_bits = self->_play_channel_bits.fetch_and(~(1 << ch));
              if (ch == 0 && (px68k_r91_old_bits & 1u)) { px68k_r91_note_qexhaust(); }'''
s = s.replace(old, new, 1)

old = '      flg_nodata = (data_length == 0);'
new = '''      flg_nodata = (data_length == 0);
      if (s_px68k_r91_arm.load(std::memory_order_relaxed) && flg_nodata && !px68k_r91_prev_nodata) {
        s_px68k_r91_nodata_enter.fetch_add(1, std::memory_order_relaxed);
      }
      px68k_r91_prev_nodata = flg_nodata;'''
s = s.replace(old, new, 1)

# Count only the zero-filled DMA writes used after M5Unified has no wav data.
old = '''          while (!ulTaskNotifyTake( pdTRUE, 0 ) && --retry)
          {
            size_t write_bytes;
            _i2s_write(i2s_port, sound_buf32, dma_buf_len * sizeof(int32_t), &write_bytes, portMAX_DELAY);
          }'''
new = '''          while (!ulTaskNotifyTake( pdTRUE, 0 ) && --retry)
          {
            size_t write_bytes;
            if (s_px68k_r91_arm.load(std::memory_order_relaxed)) {
              s_px68k_r91_zero_dma.fetch_add(1, std::memory_order_relaxed);
              const uint32_t cur = s_px68k_r91_zero_dma_cur.fetch_add(1, std::memory_order_relaxed) + 1u;
              px68k_r91_atomic_max(s_px68k_r91_zero_dma_max, cur);
            }
            _i2s_write(i2s_port, sound_buf32, dma_buf_len * sizeof(int32_t), &write_bytes, portMAX_DELAY);
          }'''
s = s.replace(old, new, 1)

# Any real data write terminates the zero-DMA burst.
old = '        _i2s_write(i2s_port, sound_buf32, data_bytes, &write_bytes, 0);'
new = '''        if (s_px68k_r91_arm.load(std::memory_order_relaxed)) {
          s_px68k_r91_zero_dma_cur.store(0, std::memory_order_relaxed);
        }
        _i2s_write(i2s_port, sound_buf32, data_bytes, &write_bytes, 0);'''
s = s.replace(old, new, 1)

target.write_text(s, encoding='utf-8')
print('Applied R57E91 counter-only M5 speaker audit:', target)
