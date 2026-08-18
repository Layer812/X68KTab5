/*
 * PX68K source modified for the Tab5 port.
 * Intent: Adapt PX68K startup and memory ownership for standalone ESP32-P4/Tab5 operation, including embedded ROMs and large PSRAM-backed guest memory.
 * Layer8 Aug/17/2026
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>
#include <math.h>
#include <strings.h>

#include <libretro.h>
#include <libretro_core_options.h>
#include <string/stdstring.h>
#ifdef USE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#endif

#include "libretro/winx68k.h"
#include "libretro/dosio.h"
#include "libretro/dswin.h"
#include "libretro/windraw.h"
#include "libretro/joystick.h"
#include "libretro/keyboard.h"
#include "libretro/prop.h"
#include "libretro/status.h"
#include "libretro/timer.h"
#include "libretro/mouse.h"
#include "libretro/winui.h"
#include "fmgen/fmg_wrap.h"
#include "m68000/m68000.h"
#include "x68k/adpcm.h"
#include "x68k/fdd.h"
#include "x68k/sram.h"
#include "x68k/sysport.h"
#include "x68k/x68kmemory.h"
#ifndef NO_MERCURY
#include "x68k/mercury.h"
#endif
#include "x68k/mfp.h"
#include "x68k/gvram.h"
#include "x68k/tvram.h"
#include "x68k/crtc.h"
#include "x68k/dmac.h"
#include "x68k/irqh.h"
#include "x68k/palette.h"
#include "x68k/bg.h"
#include "x68k/pia.h"
#include "x68k/ioc.h"
#include "x68k/scsi.h"
#include "x68k/sasi.h"
#include "x68k/fdc.h"
#include "x68k/rtc.h"
#include "x68k/scc.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_cpu.h"
#include "sdkconfig.h"
/*
 * Build 5.12a: libretro.c is compiled as the standalone PX68K component.
 * PlatformIO does not expose esp_timer's public include directory to this
 * component unless it declares an ESP-IDF component dependency.  The app
 * already links esp_timer, and the profiler only needs this one stable IDF
 * function, so keep the PX68K component dependency-free and declare the
 * ABI here instead of including esp_timer.h.
 */
extern int64_t esp_timer_get_time(void);

/* Build 5.43: scheduler/device profiler uses the local CPU cycle counter.
 * P4 runs at the configured 360 MHz here, and this is orders of magnitude
 * cheaper than thousands of esp_timer_get_time() calls in one sampled frame. */
static inline uint32_t tab5_perf_ccount(void)
{
    return (uint32_t)esp_cpu_get_cycle_count();
}

static inline uint32_t tab5_perf_cycles_to_us(uint64_t cycles)
{
    const uint32_t mhz = (uint32_t)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ;
    return mhz ? (uint32_t)(cycles / mhz) : 0u;
}
#endif

#ifdef ESP_PLATFORM
#ifdef TCM_DRAM_ATTR
#define PX68K_SCHEDHOT TCM_DRAM_ATTR
#else
#define PX68K_SCHEDHOT DRAM_ATTR
#endif
#else
#define PX68K_SCHEDHOT
#endif

#ifdef _WIN32
#define SLASH '\\'
#else
#define SLASH '/'
#endif

#define MODE_HIGH_ACTUAL 55.46 /* floor((10*100*1000^2 / VSYNC_HIGH)) / 100 */
#define MODE_NORM_ACTUAL 61.46 /* floor((10*100*1000^2 / VSYNC_NORM)) / 100 */
#define MODE_HIGH_COMPAT 55.5  /* 31.50 kHz - commonly used  */
#define MODE_NORM_COMPAT 59.94 /* 15.98 kHz - actual value should be ~61.46 fps. this is lowered to
                     * reduced the chances of audio stutters due to mismatch
                     * fps when vsync is used since most monitors are only capable
                     * of upto 60Hz refresh rate. */
enum
{
   MODES_ACTUAL,
   MODES_COMPAT,
   MODE_NORM = 0,
   MODE_HIGH,
   MODES
};

const float framerates[2][2] = {
   { MODE_NORM_ACTUAL, MODE_HIGH_ACTUAL },
   { MODE_NORM_COMPAT, MODE_HIGH_COMPAT }
};

char	winx68k_dir[2048];
char	winx68k_ini[2048];

uint16_t	VLINE_TOTAL = 567;
PX68K_SCHEDHOT uint32_t VLINE = 0;
PX68K_SCHEDHOT uint32_t vline = 0;

#define SOUNDRATE 44100.0
#define SNDSZ round(SOUNDRATE / FRAMERATE)

static int firstcall          = 1;

static uint32_t old_ram_size     = 0;
static int old_clkdiv         = 0;

static int oldrw=0,oldrh      = 0;
static char RPATH[512];
static char RETRO_DIR[512];
static const char *retro_save_directory;
static const char *retro_system_directory;
static const char *retro_browse_directory;
const char *retro_content_directory;
char retro_system_conf[512]; /* system directory path */
char retro_browse_conf[512]; /* file browser default path */
char base_dir[MAX_PATH];

static uint8_t Core_Key_State[512];
static uint8_t Core_old_Key_State[512];

static bool joypad1, joypad2;

static bool opt_analog;

static char CMDFILE[512];

/* Args for experimental_cmdline */
static char ARGUV[64][1024];
static unsigned char ARGUC = 0;

/* Args for Core */
static char XARGV[64][1024];
static const char* xargv_cmd[64];
static int PARAMCOUNT     = 0;

static uint8_t DispFrame  = 0;
static int FrameSkipCount = 0;
static int FrameSkipQueue = 0;
static PX68K_SCHEDHOT int ClkUsed = 0;

uint32_t retrow           = 800;
uint32_t retroh           = 600;
int CHANGEAV              = 0;
int CHANGEAV_TIMING       = 0; /* Separate change of geometry from change of refresh rate */
int VID_MODE              = MODE_NORM; /* what framerate we start in */
static float FRAMERATE;
uint32_t libretro_supports_input_bitmasks = 0;
unsigned int total_usec   = (unsigned int) -1;

static int16_t soundbuf[1024 * 2];
static int soundbuf_size;

uint16_t *videoBuffer;

enum {
   menu_out,
   menu_enter,
   menu_in
};

static int menu_mode = menu_out;

static retro_video_refresh_t video_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_set_rumble_state_t rumble_cb;
retro_input_state_t input_state_cb;
retro_audio_sample_t audio_cb;
retro_audio_sample_batch_t audio_batch_cb;
#ifdef ESP_PLATFORM
/*
 * Standalone ESP32-P4 build has no libretro frontend to install
 * RETRO_ENVIRONMENT_GET_LOG_INTERFACE.
 *
 * PX68K device code calls log_cb directly, so leaving it NULL causes
 * an instruction-access fault as soon as the IPL touches such a path
 * (first observed in SRAM_WriteEnable()).
 */
static void px68k_esp_log_nop(enum retro_log_level level,
                              const char *fmt, ...)
{
    (void)level;
    (void)fmt;
}

retro_log_printf_t log_cb = px68k_esp_log_nop;
#else
retro_log_printf_t log_cb;
#endif
#ifdef USE_LIBRETRO_VFS
struct retro_vfs_interface_info vfs_iface_info;
#endif

static unsigned no_content;

static bool opt_rumble_enabled = false;

#define MAX_DISKS 10

typedef enum
{
   FDD0 = 0,
   FDD1 = 1
} disk_drive;

/* .dsk swap support */
struct disk_control_interface_t
{
   unsigned dci_version;                        /* disk control interface version, 0 = use old interface */
   unsigned total_images;                       /* total number if disk images */
   unsigned index;                              /* currect disk index */
   disk_drive cur_drive;                        /* current active drive */
   bool inserted[2];                            /* tray state for FDD0/FDD1, 0 = disk ejected, 1 = disk inserted */

   char path[MAX_DISKS][MAX_PATH];              /* disk image paths */
   char label[MAX_DISKS][MAX_PATH];             /* disk image base name w/o extension */

   unsigned g_initial_disc;                     /* initial disk index */
   char g_initial_disc_path[MAX_PATH];          /* initial disk path */
};

static struct disk_control_interface_t disk;
static struct retro_disk_control_callback dskcb;
static struct retro_disk_control_ext_callback dskcb_ext;

static struct retro_input_descriptor input_descs[64];

static struct retro_input_descriptor input_descs_p1[] = {
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2 - Touroku" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2 - Menu" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3" },
   { 0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3" },
};
static struct retro_input_descriptor input_descs_p2[] = {
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A, "A" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B, "B" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X, "X" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y, "Y" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START, "Start" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "Right" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT, "Left" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP, "Up" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN, "Down" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R, "R" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L, "L" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2, "R2" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2, "L2" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3, "R3" },
   { 1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3, "L3" },

};

static struct retro_input_descriptor input_descs_null[] = {
   { 0, 0, 0, 0, NULL }
};

static bool is_path_absolute(const char* path)
{
   if (path[0] == SLASH)
      return true;

#ifdef _WIN32
   if ((path[0] >= 'a' && path[0] <= 'z') ||
      (path[0]  >= 'A' && path[0] <= 'Z'))
   {
      if (path[1] == ':')
         return true;
   }
#endif
   return false;
}

static void extract_basename(char *buf, const char *path, size_t size)
{
   const char *base = strrchr(path, '/');
   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   char *ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

static void extract_directory(char *buf, const char *path, size_t size)
{
   char *base = NULL;

   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
      buf[0] = '\0';
}

/* BEGIN MIDI INTERFACE */
#include "x68k/midi.h"
#include "libretro/mmsystem.h"
static int libretro_supports_midi_output = 0;
static struct retro_midi_interface midi_cb = { 0 };
static bool libretro_supports_option_categories = 0;

void midi_out_short_msg(size_t msg)
{
   if (libretro_supports_midi_output && midi_cb.output_enabled())
   {
      midi_cb.write(msg         & 0xFF, 0); /* status byte */
      midi_cb.write((msg >> 8)  & 0xFF, 0); /* note no. */
      midi_cb.write((msg >> 16) & 0xFF, 0); /* velocity */
   }
}

void midi_out_long_msg(uint8_t *s, size_t len)
{
   if (libretro_supports_midi_output && midi_cb.output_enabled())
   {
      int i;
      for (i = 0; i < len; i++)
         midi_cb.write(s[i], 0);
   }
}

int midi_out_open(void **phmo)
{
   if (libretro_supports_midi_output && midi_cb.output_enabled())
   {
      *phmo = &midi_cb;
      return 0;
   }
   return 1;
}

static void update_variable_midi_interface(int running)
{
   struct retro_variable var;

   var.key = "px68k_midi_output";
   var.value = NULL;

   if (!running && environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Config.MIDI_SW = 0;
      else if (!strcmp(var.value, "enabled"))
         Config.MIDI_SW = 1;
   }

   var.key = "px68k_midi_output_type";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "LA"))
         Config.MIDI_Type = 0;
      else if (!strcmp(var.value, "GM"))
         Config.MIDI_Type = 1;
      else if (!strcmp(var.value, "GS"))
         Config.MIDI_Type = 2;
      else if (!strcmp(var.value, "XG"))
         Config.MIDI_Type = 3;
   }
}

static void midi_interface_init(void)
{
   libretro_supports_midi_output = 0;
   if (environ_cb(RETRO_ENVIRONMENT_GET_MIDI_INTERFACE, &midi_cb))
      libretro_supports_midi_output = 1;
}

/* END OF MIDI INTERFACE */

static void update_variable_disk_drive_swap(void)
{
   struct retro_variable var =
   {
      "px68k_disk_drive",
      NULL
   };

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "FDD0") == 0)
         disk.cur_drive = FDD0;
      else
         disk.cur_drive = FDD1;
   }
}

static bool set_eject_state(bool ejected)
{
   if (disk.index == disk.total_images)
      return true; /* Frontend is trying to set "no disk in tray" */

   if (ejected)
   {
      FDD_EjectFD(disk.cur_drive);
      Config.FDDImage[disk.cur_drive][0] = '\0';
   }
   else
   {
      strcpy(Config.FDDImage[disk.cur_drive], disk.path[disk.index]);
      FDD_SetFD(disk.cur_drive, Config.FDDImage[disk.cur_drive], 0);
   }
   disk.inserted[disk.cur_drive] = !ejected;
   return true;
}

static bool get_eject_state(void)
{
   update_variable_disk_drive_swap();
   return !disk.inserted[disk.cur_drive];
}

static unsigned get_image_index(void)
{
   return disk.index;
}

static bool set_image_index(unsigned index)
{
   disk.index = index;
   return true;
}

static unsigned get_num_images(void)
{
   return disk.total_images;
}

static bool add_image_index(void)
{
   if (disk.total_images >= MAX_DISKS)
      return false;

   disk.total_images++;
   return true;
}

static bool replace_image_index(unsigned index, const struct retro_game_info *info)
{
   char image[MAX_PATH];
   strcpy(disk.path[index], info->path);
   extract_basename(image, info->path, sizeof(image));
   snprintf(disk.label[index], sizeof(disk.label), "%s", image);
   return true;
}

static bool disk_set_initial_image(unsigned index, const char *path)
{
   if (string_is_empty(path))
      return false;

   disk.g_initial_disc = index;
   strncpy(disk.g_initial_disc_path, path, sizeof(disk.g_initial_disc_path));

   return true;
}

static bool disk_get_image_path(unsigned index, char *path, size_t len)
{
   if (len < 1)
      return false;

   if (index < disk.total_images)
   {
      if (!string_is_empty(disk.path[index]))
      {
         strncpy(path, disk.path[index], len);
         return true;
      }
   }

   return false;
}

static bool disk_get_image_label(unsigned index, char *label, size_t len)
{
   if (len < 1)
      return false;

   if (index < disk.total_images)
   {
      if (!string_is_empty(disk.label[index]))
      {
         strncpy(label, disk.label[index], len);
         return true;
      }
   }

   return false;
}

static void attach_disk_swap_interface(void)
{
   dskcb.set_eject_state = set_eject_state;
   dskcb.get_eject_state = get_eject_state;
   dskcb.set_image_index = set_image_index;
   dskcb.get_image_index = get_image_index;
   dskcb.get_num_images  = get_num_images;
   dskcb.add_image_index = add_image_index;
   dskcb.replace_image_index = replace_image_index;

   environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_INTERFACE, &dskcb);
}

void attach_disk_swap_interface_ext(void)
{
   dskcb_ext.set_eject_state = set_eject_state;
   dskcb_ext.get_eject_state = get_eject_state;
   dskcb_ext.set_image_index = set_image_index;
   dskcb_ext.get_image_index = get_image_index;
   dskcb_ext.get_num_images  = get_num_images;
   dskcb_ext.add_image_index = add_image_index;
   dskcb_ext.replace_image_index = replace_image_index;
   dskcb_ext.set_initial_image = NULL;
   dskcb_ext.get_image_path = disk_get_image_path;
   dskcb_ext.get_image_label = disk_get_image_label;

   environ_cb(RETRO_ENVIRONMENT_SET_DISK_CONTROL_EXT_INTERFACE, &dskcb_ext);
}

static void disk_swap_interface_init(void)
{
   unsigned i;
   disk.dci_version  = 0;
   disk.total_images = 0;
   disk.index        = 0;
   disk.cur_drive    = FDD1;
   disk.inserted[0]  = false;
   disk.inserted[1]  = false;

   disk.g_initial_disc         = 0;
   disk.g_initial_disc_path[0] = '\0';

   for (i = 0; i < MAX_DISKS; i++)
   {
      disk.path[i][0]  = '\0';
      disk.label[i][0] = '\0';
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_DISK_CONTROL_INTERFACE_VERSION, &disk.dci_version) && (disk.dci_version >= 1))
      attach_disk_swap_interface_ext();
   else
      attach_disk_swap_interface();
}
/* end .dsk swap support */

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { audio_cb = cb; }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }

static int loadcmdfile(char *argv)
{
   int res  = 0;
   FILE *fp = fopen(argv, "r");

   if (fp)
   {
      if (fgets(CMDFILE, 512, fp) != NULL)
         res = 1;
      fclose(fp);
   }

   return res;
}

static size_t handle_extension(char *path, char *ext)
{
   size_t len = strlen(path);
   if (len >= 4 &&
         path[len - 4] == '.' &&
         path[len - 3] == ext[0] &&
         path[len - 2] == ext[1] &&
         path[len - 1] == ext[2])
      return 1;
   return 0;
}

static void parse_cmdline(const char *argv)
{
   char *p, *p2, *start_of_word;
   int c, c2;
   static char buffer[512 * 4];
   enum states { DULL, IN_WORD, IN_STRING } state = DULL;

   strcpy(buffer, argv);
   strcat(buffer, " \0");

   for (p = buffer; *p != '\0'; p++)
   {
      c = (unsigned char) *p; /* convert to unsigned char for is* functions */
      switch (state)
      {
         case DULL: /* not in a word, not in a double quoted string */
            if (isspace(c)) /* still not in a word, so ignore this char */
               continue;
            /* not a space -- if it's a double quote we go to IN_STRING, else to IN_WORD */
            if (c == '"')
            {
               state = IN_STRING;
               start_of_word = p + 1; /* word starts at *next* char, not this one */
               continue;
            }
            state = IN_WORD;
            start_of_word = p; /* word starts here */
            continue;
         case IN_STRING:
            /* we're in a double quoted string, so keep going until we hit a close " */
            if (c == '"')
            {
               /* word goes from start_of_word to p-1
                *... do something with the word ... */
               for (c2 = 0, p2 = start_of_word; p2 < p; p2++, c2++)
                  ARGUV[ARGUC][c2] = (unsigned char) *p2;

               ARGUC++;

               state = DULL; /* back to "not in word, not in string" state */
            }
            continue; /* either still IN_STRING or we handled the end above */
         case IN_WORD:
            /* we're in a word, so keep going until we get to a space */
            if (isspace(c))
            {
               /* word goes from start_of_word to p-1
                *... do something with the word ... */
               for (c2 = 0, p2 = start_of_word; p2 <p; p2++, c2++)
                  ARGUV[ARGUC][c2] = (unsigned char) *p2;

               ARGUC++;

               state = DULL; /* back to "not in word, not in string" state */
            }
            continue; /* either still IN_WORD or we handled the end above */
      }
   }
}


static bool read_m3u(const char *file)
{
   unsigned index = 0;
   char line[MAX_PATH];
   char name[MAX_PATH];
   FILE *f = fopen(file, "r");

   if (!f)
      return false;

   while (fgets(line, sizeof(line), f) && index < sizeof(disk.path) / sizeof(disk.path[0]))
   {
      if (line[0] == '#')
         continue;

      char *carriage_return = strchr(line, '\r');
      if (carriage_return)
         *carriage_return = '\0';

      char *newline = strchr(line, '\n');
      if (newline)
         *newline = '\0';

      /* Remove any beginning and ending quotes as these can cause issues when feeding the paths into command line later */
      if (line[0] == '"')
          memmove(line, line + 1, strlen(line));

      if (line[strlen(line) - 1] == '"')
          line[strlen(line) - 1]  = '\0';

      if (line[0] != '\0')
      {
         char image_label[4096];
         char *custom_label;
         size_t len = 0;

         if (is_path_absolute(line))
            strncpy(name, line, sizeof(name));
         else
            snprintf(name, sizeof(name), "%s%c%s", base_dir, SLASH, line);

         custom_label = strchr(name, '|');
         if (custom_label)
         {
            /* get disk path */
            len = custom_label + 1 - name;
            strncpy(disk.path[index], name, len - 1);

            /* get custom label */
            custom_label++;
            strncpy(disk.label[index], custom_label, sizeof(disk.label[index]));
         }
         else
         {
            /* copy path */
            strncpy(disk.path[index], name, sizeof(disk.path[index]));

            /* extract base name from path for labels */
            extract_basename(image_label, name, sizeof(image_label));
            strncpy(disk.label[index], image_label, sizeof(disk.label[index]));
         }

         index++;
      }
   }

   disk.total_images = index;
   fclose(f);

   return (disk.total_images != 0);
}

static void Add_Option(const char* option)
{
   static int first = 0;

   if(first == 0)
   {
      PARAMCOUNT = 0;
      first++;
   }

   strcpy(XARGV[PARAMCOUNT++], option);
}

static int retro_load_game_internal(const char *argv)
{
   if (strlen(argv) > strlen("cmd"))
   {
      int res = 0;
      if (handle_extension((char*)argv, "cmd") || handle_extension((char*)argv, "CMD"))
      {
         int i;

         if (!(res = loadcmdfile((char*)argv)))
         {
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "%s\n", "[libretro]: failed to read cmd file ...");
            return 0;
         }

         parse_cmdline(CMDFILE);

         /* handle relative paths, append content dir if needed */
         for (i = 1; i < ARGUC; i++)
         {
            if (!is_path_absolute(ARGUV[i]))
            {
               char tmp[2048] = { 0 };
               strcpy(tmp, ARGUV[i]);
               ARGUV[i][0] = '\0';
               sprintf(ARGUV[i], "%s%c%s", base_dir, SLASH, tmp);
            }
         }
      }
      else if (handle_extension((char*)argv, "m3u") || handle_extension((char*)argv, "M3U"))
      {
         if (!read_m3u((char*)argv))
         {
            if (log_cb)
               log_cb(RETRO_LOG_ERROR, "%s\n", "[libretro]: failed to read m3u file ...");
            return 0;
         }

         if(disk.total_images > 1)
         {
            sprintf((char*)argv, "%s \"%s\" \"%s\"", "px68k", disk.path[0], disk.path[1]);
            disk.inserted[1] = true;
         }
         else
            sprintf((char*)argv, "%s \"%s\"", "px68k", disk.path[0]);

         disk.inserted[0] = true;
         parse_cmdline(argv);
      }
   }

   return 1;
}

#define MEM_SIZE 0xc00000

static int WinX68k_Init(void)
{
    /* Do not allocate twice. */
    if (MEM || IPL || FONT)
        return (MEM && IPL && FONT) ? 1 : 0;

#ifdef ESP_PLATFORM
    /*
     * ESP32-P4 / M5Stack Tab5:
     * Put the large X68000 memory regions explicitly in PSRAM.
     *
     * IPL  : 256 KiB
     * MEM  : 12 MiB
     * FONT : 768 KiB
     */
    IPL = (uint8_t *)heap_caps_malloc(
        0x40000,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    MEM = (uint8_t *)heap_caps_malloc(
        MEM_SIZE,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    FONT = (uint8_t *)heap_caps_malloc(
        0xc0000,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    IPL  = (uint8_t*)malloc(0x40000);
    MEM  = (uint8_t*)malloc(MEM_SIZE);
    FONT = (uint8_t*)malloc(0xc0000);
#endif

    if (!IPL || !MEM || !FONT)
    {
        if (IPL) {
            free(IPL);
            IPL = NULL;
        }

        if (MEM) {
            free(MEM);
            MEM = NULL;
        }

        if (FONT) {
            free(FONT);
            FONT = NULL;
        }

        return 0;
    }

    memset(IPL,  0, 0x40000);
    memset(MEM,  0, MEM_SIZE);
    memset(FONT, 0, 0xc0000);

    return 1;
}

static void WinX68k_SCSICheck(void)
{
	static const uint8_t SCSIIMG[] = {
		0x00, 0xfc, 0x00, 0x80,				/* $fc0000 SCSI boot entry address
									 * Build 5.94c: keep the native IPL HD0 path,
									 * but place the boot routine at $FC0080 so
									 * Human68k 3.02's required boot-20..boot-8
									 * SCSI installer metadata can exist intact. */
		0x00, 0xfc, 0x00, 0x16,				/* $fc0004 IOCS vector setting entry
address (always before "Human" 8 bytes) */
		0x00, 0x00, 0x00, 0x00,				/* $fc0008 ? */
		0x48, 0x75, 0x6d, 0x61,				/* $fc000c 遶翫・*/
		0x6e, 0x36, 0x38, 0x6b,				/* $fc0010 ID "Human68k"	(always just
before start-up entry point) */
		0x4e, 0x75,							/* $fc0014 "rts"		(start-up entry point)
*/
		0x23, 0xfc, 0x00, 0xfc, 0x00, 0x2a,	/* $fc0016 遶翫・	(IOCS vector setting
entry point) */
		0x00, 0x00, 0x07, 0xd4,				/* $fc001c "move.l #$fc002a, $7d4.l" */
		0x74, 0xff,							   /* $fc0020 "moveq #-1, d2" */
		0x4e, 0x75,							   /* $fc0022 "rts" */
		0x44, 0x55, 0x4d, 0x4d, 0x59, 0x20,	/* $fc0024 ID "DUMMY " */
		0x13, 0xc1, 0x00, 0xe9, 0xf8, 0x00, /* $fc002a "move.b d1,$e9f800" host IOCS trap */
		0x4e, 0x75,							/* $fc0030 "rts" */
	};

	int i;
	uint16_t *p1, *p2;
	int scsi = 0;
	for (i = 0x30600; i < 0x30c00; i += 2)
   {
		p1 = (uint16_t *)(&IPL[i]);
		p2 = p1 + 1;
		/* xxx: works only for little endian guys */
		if (*p1 == 0xfc00 && *p2 == 0x0000)
      {
			scsi = 1;
			break;
		}
	}

	/* SCSI model time */
	if (scsi)
   {
		memset(IPL, 0, 0x2000);				      /* main is 8kb */
		memset(&IPL[0x2000], 0xff, 0x1e000);	/* remaining is 0xff */
		memcpy(IPL, SCSIIMG, sizeof(SCSIIMG));	/* fake internal SCSI BIOS */

		/*
		 * Build 5.94c: complete the Human68k 3.02 SCSI boot ABI.
		 *
		 * The real SCSI BIOS places four fields immediately before the boot
		 * routine.  Human68k 3.02 follows the boot handle and reads these exact
		 * negative offsets when it later installs the SCSI disk device driver:
		 *
		 *   boot-20  "SCSI"
		 *   boot-16  device-installer routine
		 *   boot-12  _SCSIDRV/IOCS routine parameter
		 *   boot-8   "Human68k"
		 *
		 * 5.94b successfully ran the HDS disk IPL and loaded Human68k, but its
		 * $FC0040 boot routine had no room for this metadata.  Move the routine
		 * to $FC0080, add the metadata at $FC006C-$FC007F, and route the installer
		 * through the existing host-side SCSI driver installer.
		 */
		{
			static const uint8_t installer_bridge[] = {
				0x13, 0xfc, 0x00, 0x80, 0x00, 0xe9, 0xf8, 0x02, /* move.b #$80,$E9F802 */
				0x4e, 0x75                                      /* rts */
			};
			static const uint8_t hdd_boot_bridge[] = {
				0x13, 0xfc, 0x00, 0x82, 0x00, 0xe9, 0xf8, 0x02,
				0x4a, 0x80,
				0x6b, 0x10,
				0x70, 0x00,
				0x72, 0x20,
				0x7a, 0x02,
				0x46, 0xfc, 0x20, 0x00,
				0x4e, 0xf9, 0x00, 0x00, 0x20, 0x00,
				0x4e, 0x75
			};

			/* $FC0050: Human68k SCSI device-installer callback. */
			memcpy(&IPL[0x0050], installer_bridge, sizeof(installer_bridge));

			/* Metadata is at fixed negative offsets from boot=$FC0080. */
			memcpy(&IPL[0x006c], "SCSI", 4);                  /* boot - 20 */
			IPL[0x0070] = 0x00; IPL[0x0071] = 0xfc;          /* boot - 16 */
			IPL[0x0072] = 0x00; IPL[0x0073] = 0x50;
			IPL[0x0074] = 0x00; IPL[0x0075] = 0xea;          /* boot - 12 */
			IPL[0x0076] = 0x00; IPL[0x0077] = 0xa0;
			memcpy(&IPL[0x0078], "Human68k", 8);             /* boot - 8 */

			/* $FC0080: native IPL HD0 -> HDS disk IPL bridge. */
			memcpy(&IPL[0x0080], hdd_boot_bridge, sizeof(hdd_boot_bridge));
		}
#if PX68K_TAB5_DIAG_VERBOSE
		printf("PX68K_SCSIIN: Build 5.94c HD0 entry=$FC0080 installer=$FC0050 metadata=FC006C..007F IOCS=$EA00A0\n");
#endif
	}
   else /* SASI model sees the IPL as it is */
      memcpy(IPL, &IPL[0x20000], 0x20000);
}

#define	NELEMENTS(array)	((int)(sizeof(array) / sizeof(array[0])))

int WinX68k_LoadROMs(void)
{
static const char *BIOSFILE[] = {
    "iplrom.dat",   "IPLROM.DAT",
    "iplrom30.dat", "IPLROM30.DAT",
    "iplromco.dat", "IPLROMCO.DAT",
    "iplromxv.dat", "IPLROMXV.DAT"
};
	static const char FONTFILE[] = "cgrom.dat";
	static const char FONTFILETMP[] = "cgrom.tmp";
	void *fp;
	int i;
	uint8_t tmp;

	for (fp = 0, i = 0; fp == 0 && i < NELEMENTS(BIOSFILE); ++i)
		fp = file_open_c((char *)BIOSFILE[i]);

	if (fp == 0)
	{
		if (log_cb)
			log_cb(RETRO_LOG_ERROR, "[PX68K] Error: BIOS ROM image can't be found.\n");
		return 0;
	}

	file_lread(fp, &IPL[0x20000], 0x20000);
	file_close(fp);

   /* if SCSI IPL, SCSI BIOS is established around $fc0000 */
	WinX68k_SCSICheck();

	for (i = 0; i < 0x40000; i += 2) {
		tmp = IPL[i];
		IPL[i] = IPL[i + 1];
		IPL[i + 1] = tmp;
	}

	fp = file_open_c((char *)FONTFILE);
	if (fp == 0)
    fp = file_open_c("CGROM.DAT");
	if (fp == 0)
   {
		/* cgrom.tmp present? */
		fp = file_open_c((char *)FONTFILETMP);
		/* font creation XXX - Font ROM image can't be found */
		if (fp == 0)
			return 0;
	}
	file_lread(fp, FONT, 0xc0000);
	file_close(fp);

	return 1;
}
#ifdef ESP_PLATFORM

extern const uint8_t px68k_iplromxv_bin[];
extern const size_t  px68k_iplromxv_bin_len;

extern const uint8_t px68k_cgrom_bin[];
extern const size_t  px68k_cgrom_bin_len;

int WinX68k_LoadEmbeddedROMs(void)
{
    if (!IPL || !FONT)
        return 0;

    if (px68k_iplromxv_bin_len != 0x20000)
        return 0;

    if (px68k_cgrom_bin_len != 0xC0000)
        return 0;

    /*
     * Same layout as native PX68K loader:
     *
     * IPL[00000-1FFFF] = SCSI/SASI support area
     * IPL[20000-3FFFF] = real 128 KiB IPL ROM
     */
    memset(IPL, 0, 0x40000);

    memcpy(IPL + 0x20000,
           px68k_iplromxv_bin,
           0x20000);

    /*
     * IMPORTANT:
     * Native PX68K performs this BEFORE byte swapping.
     * IPLROMXV is a SCSI-model IPL.
     */
    WinX68k_SCSICheck();

    /*
     * Same endian conversion as WinX68k_LoadROMs().
     */
    for (size_t i = 0; i < 0x40000; i += 2)
    {
        uint8_t t = IPL[i];
        IPL[i] = IPL[i + 1];
        IPL[i + 1] = t;
    }

    memcpy(FONT,
           px68k_cgrom_bin,
           0xC0000);

    return 1;
}

#endif


#ifdef ESP_PLATFORM

int WinX68k_ExecProbeFrame(void)
{
#ifdef HAVE_MUSASHI

    /*
     * Minimal version of PX68K's native frame scheduler.
     *
     * Deliberately excluded for Build 1B:
     *   WinDraw
     *   DSound
     *   ADPCM
     *   MIDI
     *   Mercury
     *   keyboard/mouse polling
     *
     * Included:
     *   68000 execution
     *   CRTC scanline progression
     *   MFP H-SYNC/raster/V-DISP events
     *   MFP Timer-A
     */

    int clk_total;

    clk_total =
        (CRTC_Regs[0x29] & 0x10)
            ? VSYNC_HIGH
            : VSYNC_NORM;

    clk_total =
        (clk_total * Config.clockmhz) / 10;

    /*
     * Defensive fallback only.
     * Normally Config.clockmhz is already valid.
     */
    if (clk_total <= 0)
        clk_total = VSYNC_NORM;

    const int total_lines =
        (VLINE_TOTAL > 0)
            ? VLINE_TOTAL
            : 567;

    int total_executed = 0;

    for (int line = 0; line < total_lines; ++line)
    {
        vline = (uint32_t)line;

        /*
         * Beginning of horizontal scanline.
         */
        MFP_Int(0);

        if ((vline >= CRTC_VSTART) &&
            (vline < CRTC_VEND))
        {
            VLINE =
                ((vline - CRTC_VSTART)
                 * CRTC_VStep) / 2;
        }
        else
        {
            VLINE = (uint32_t)-1;
        }

        /*
         * Raster interrupt edge.
         */
        if (!(MFP[MFP_AER] & 0x40) &&
            (vline == CRTC_IntLine))
        {
            MFP_Int(1);
        }

        /*
         * V-DISP edge.
         *
         * This follows PX68K's native scheduler logic.
         */
        if (MFP[MFP_AER] & 0x10)
        {
            if (vline == CRTC_VSTART)
                MFP_Int(9);
        }
        else
        {
            if (CRTC_VEND >= total_lines)
            {
                if ((long)vline ==
                    (long)(CRTC_VEND - total_lines))
                {
                    MFP_Int(9);
                }
            }
            else
            {
                if (vline == CRTC_VEND)
                    MFP_Int(9);
            }
        }

        /*
         * Distribute one frame's CPU clocks
         * accurately across all scanlines.
         */
        int begin =
            (int)(((int64_t)clk_total * line)
                  / total_lines);

        int end =
            (int)(((int64_t)clk_total * (line + 1))
                  / total_lines);

        int remaining = end - begin;

        while (remaining > 0)
        {
            int request =
                (remaining > 200)
                    ? 200
                    : remaining;

            int executed =
                m68k_execute(request);

            if (executed <= 0)
            {
                /*
                 * Avoid an infinite host loop even if
                 * CPU core reports no progress.
                 */
                executed = request;
            }

            total_executed += executed;
            remaining -= executed;
        }

        /*
         * End-of-scanline MFP processing.
         */
        MFP_TimerA();

        if ((MFP[MFP_AER] & 0x40) &&
            (vline == CRTC_IntLine))
        {
            MFP_Int(1);
        }
    }

    vline = 0;

    return total_executed;

#else

    return 0;

#endif
}

#endif

static  void WinX68k_Cleanup(void)
{
	if (IPL)
		free(IPL);
	if (MEM)
		free(MEM);
	if (FONT)
		free(FONT);
	IPL  = NULL;
	MEM  = NULL;
	FONT = NULL;
}

void WinX68k_Reset(void)
{
   OPM_Reset();

#if defined (HAVE_CYCLONE)
   m68000_reset();
   m68000_set_reg(M68K_A7, (IPL[0x30001]<<24)|(IPL[0x30000]<<16)|(IPL[0x30003]<<8)|IPL[0x30002]);
   m68000_set_reg(M68K_PC, (IPL[0x30005]<<24)|(IPL[0x30004]<<16)|(IPL[0x30007]<<8)|IPL[0x30006]);
#elif defined (HAVE_C68K)
   C68k_Reset(&C68K);
#if 0
   C68k_Set_Reg(&C68K, C68K_A7, (IPL[0x30001]<<24)|(IPL[0x30000]<<16)|(IPL[0x30003]<<8)|IPL[0x30002]);
   C68k_Set_Reg(&C68K, C68K_PC, (IPL[0x30005]<<24)|(IPL[0x30004]<<16)|(IPL[0x30007]<<8)|IPL[0x30006]);
#endif
   C68k_Set_AReg(&C68K, 7, (IPL[0x30001]<<24)|(IPL[0x30000]<<16)|(IPL[0x30003]<<8)|IPL[0x30002]);
   C68k_Set_PC(&C68K, (IPL[0x30005]<<24)|(IPL[0x30004]<<16)|(IPL[0x30007]<<8)|IPL[0x30006]);
#elif defined (HAVE_MUSASHI)
   m68k_pulse_reset();

   m68k_set_reg(M68K_REG_A7, (IPL[0x30001]<<24)|(IPL[0x30000]<<16)|(IPL[0x30003]<<8)|IPL[0x30002]);
   m68k_set_reg(M68K_REG_PC, (IPL[0x30005]<<24)|(IPL[0x30004]<<16)|(IPL[0x30007]<<8)|IPL[0x30006]);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */

   Memory_Init();
   CRTC_Init();
   DMA_Init();
   MFP_Init();
   FDC_Init();
   FDD_Reset();
   SASI_Init();
   SCSI_Init();
   IOC_Init();
   SCC_Init();
   PIA_Init();
   RTC_Init();
   TVRAM_Init();
   GVRAM_Init();
   BG_Init();
   Pal_Init();
   IRQH_Init();
   MIDI_Init();
   Keyboard_Init();

   m68000_ICountBk = 0;
   ICount = 0;

   DSound_Stop();
   SRAM_VirusCheck();
#if 0
   CDROM_Init();
#endif
   DSound_Play();
}

extern char filepath[MAX_PATH];
static int pmain(int argc, char *argv[])
{
	strcpy(winx68k_dir, retro_system_conf);
	sprintf(winx68k_ini, "%s%cconfig", retro_system_conf, SLASH);

   file_setcd(winx68k_dir);

   LoadConfig();

   /* if available, use retro_browse_conf to set StartDir config */
   if (retro_browse_conf[0] != 0)
      strcpy(filepath, retro_browse_conf);

   if (!WinDraw_MenuInit())
   {
      WinX68k_Cleanup();
      WinDraw_Cleanup();
      return 1;
   }

   StatBar_Show(Config.WindowFDDStat);
   WinUI_Init();

#if 0
   /* TODO: CLean this up */
   if (!WinX68k_Init())
   {
      WinX68k_Cleanup();
      WinDraw_Cleanup();
      return 1;
   }
#endif

   m68000_init();

   if (!WinX68k_LoadROMs())
   {
      WinX68k_Cleanup();
      WinDraw_Cleanup();
      exit (1);
   }

   /* before moving to WinDraw_Init() */
   Keyboard_Init();
   WinDraw_Init();

   ADPCM_Init();
#if 0
   /* TODO: CLean this up */
   OPM_Init(4000000/*3579545*/);
#endif
#ifndef	NO_MERCURY
   Mcry_Init(winx68k_dir);
#endif

   FDD_Init();
   SysPort_Init();
   Mouse_Init();
   Joystick_Init();
   SRAM_Init();
   /* FIXME: actually, this sets initial register values rather than suppose to set soft-reset values */
   WinX68k_Reset();
   Timer_Init();

   MIDI_Init();
   MIDI_SetMimpiMap(Config.ToneMapFile);	/* ToneMap file usage */
   MIDI_EnableMimpiDef(Config.ToneMap);

   ADPCM_SetVolume((uint8_t)Config.PCM_VOL);
   OPM_SetVolume((uint8_t)Config.OPM_VOL);
#ifndef	NO_MERCURY
   Mcry_SetVolume((uint8_t)Config.MCR_VOL);
#endif
   DSound_Play();

   /* apply defined command line settings */
   if(argc == 3 && argv[1][0] == '-' && argv[1][1] == 'h')
      strcpy(Config.HDImage[0], argv[2]);
   else
   {
      switch (argc)
      {
         case 3:
            strcpy(Config.FDDImage[1], argv[2]);
         case 2:
            strcpy(Config.FDDImage[0], argv[1]);
            break;
         case 0:
            /* start menu when running without content */
            /* menu_mode = menu_enter; */
            break;
      }
   }

   FDD_SetFD(0, Config.FDDImage[0], 0);
   FDD_SetFD(1, Config.FDDImage[1], 0);

   return 1;
}

static int pre_main(void)
{
   int i = 0;
   int Only1Arg;

   for (i = 0; i < 64; i++)
      xargv_cmd[i] = NULL;

   if (no_content)
   {
      PARAMCOUNT = 0;
      goto run_pmain;
   }

   Only1Arg = (strcmp(ARGUV[0], "px68k") == 0) ? 0 : 1;

   if (Only1Arg)
   {
      int cfgload = 0;

      Add_Option("px68k");

      if (strlen(RPATH) >= strlen("hdf"))
      {
         if (!strcasecmp(&RPATH[strlen(RPATH) - strlen("hdf")], "hdf"))
         {
            Add_Option("-h");
            cfgload = 1;
         }
      }

      Add_Option(RPATH);
   }
   else
   {
      /* Pass all cmdline args */
      for (i = 0; i < ARGUC; i++)
         Add_Option(ARGUV[i]);
   }

   for (i = 0; i < PARAMCOUNT; i++)
      xargv_cmd[i] = (char*)(XARGV[i]);

run_pmain:
   pmain(PARAMCOUNT, (char **)xargv_cmd);

   if (PARAMCOUNT)
      xargv_cmd[PARAMCOUNT - 2] = NULL;
   
   return 0;
}

static void retro_set_controller_descriptors(void)
{
   unsigned i;
   unsigned size = 16;

   for (i = 0; i < (2 * size); i++)
      input_descs[i] = input_descs_null[0];

   if (joypad1 && joypad2)
   {
      for (i = 0; i < (2 * size); i++)
      {
         if (i < size)
            input_descs[i] = input_descs_p1[i];
         else
            input_descs[i] = input_descs_p2[i - size];
      }
   }
   else if (joypad1 || joypad2)
   {
      for (i = 0; i < size; i++)
      {
         if (joypad1)
            input_descs[i] = input_descs_p1[i];
         else
            input_descs[i] = input_descs_p2[i];
      }
   }
   else
      input_descs[0] = input_descs_null[0];
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, &input_descs);
}

void retro_set_controller_port_device(unsigned port, unsigned device)
{
   if (port >= 2)
      return;

   switch (device)
   {
      case RETRO_DEVICE_JOYPAD:
         if (port == 0)
            joypad1 = true;
         if (port == 1)
            joypad2 = true;
         break;
      case RETRO_DEVICE_KEYBOARD:
         if (port == 0)
            joypad1 = false;
         if (port == 1)
            joypad2 = false;
         break;
      case RETRO_DEVICE_NONE:
         if (port == 0)
            joypad1 = false;
         if (port == 1)
            joypad2 = false;
         break;
      default:
         if (log_cb)
            log_cb(RETRO_LOG_ERROR, "[libretro]: Invalid device, setting type to RETRO_DEVICE_JOYPAD ...\n");
   }
   log_cb(RETRO_LOG_INFO, "Set Controller Device: %d, Port: %d %d %d\n", device, port, joypad1, joypad2);
   retro_set_controller_descriptors();
}

void retro_set_environment(retro_environment_t cb)
{
   int nocontent = 1;

   static const struct retro_controller_description port[] = {
      { "RetroPad",              RETRO_DEVICE_JOYPAD },
      { "RetroKeyboard",         RETRO_DEVICE_KEYBOARD },
      { 0 },
   };

   static const struct retro_controller_info ports[] = {
      { port, 2 },
      { port, 2 },
      { NULL, 0 },
   };

   environ_cb = cb;
   cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
   cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &nocontent);

   libretro_supports_option_categories = 0;
   libretro_set_core_options(cb, &libretro_supports_option_categories);

#ifdef USE_LIBRETRO_VFS
   {
      vfs_iface_info.required_interface_version = 1;
      vfs_iface_info.iface                      = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_iface_info))
      {
         filestream_vfs_init(&vfs_iface_info);
	      path_vfs_init(&vfs_iface_info);
         dirent_vfs_init(&vfs_iface_info);
      }
   }
#endif
}

static void update_variables(int running)
{
   int i = 0, snd_opt = 0;
   char key[256] = {0};
   struct retro_variable var = {0};

   update_variable_midi_interface(running);

   strcpy(key, "px68k_joytype");
   var.key = key;
   for (i = 0; i < 2; i++)
   {
      key[strlen("px68k_joytype")] = '1' + i;
      var.value = NULL;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      {
         int val = Config.JOY_TYPE[i];
         if (!(strcmp(var.value, "Default (2 Buttons)")))
            Config.JOY_TYPE[i] = PAD_2BUTTON;
         else if (!(strcmp(var.value, "CPSF-MD (8 Buttons)")))
            Config.JOY_TYPE[i] = PAD_CPSF_MD;
         else if (!(strcmp(var.value, "CPSF-SFC (8 Buttons)")))
            Config.JOY_TYPE[i] = PAD_CPSF_SFC;
         else if (!(strcmp(var.value, "Cyberstick (Digital)")))
            Config.JOY_TYPE[i] = PAD_CYBERSTICK_D;
         else if (!(strcmp(var.value, "Cyberstick (Analog)")))
            Config.JOY_TYPE[i] = PAD_CYBERSTICK_A;
         if (Config.JOY_TYPE[i] != val)
         {
            log_cb(RETRO_LOG_DEBUG, "player: %d type: %d\n", i + 1, Config.JOY_TYPE[i]);
            Joystick_Init();
         }
      }
   }

   var.key = "px68k_cpuspeed";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "10Mhz") == 0)
         Config.clockmhz = 10;
      else if (strcmp(var.value, "16Mhz") == 0)
         Config.clockmhz = 16;
      else if (strcmp(var.value, "25Mhz") == 0)
         Config.clockmhz = 25;
      else if (strcmp(var.value, "33Mhz (OC)") == 0)
         Config.clockmhz = 33;
      else if (strcmp(var.value, "66Mhz (OC)") == 0)
         Config.clockmhz = 66;
      else if (strcmp(var.value, "100Mhz (OC)") == 0)
         Config.clockmhz = 100;
   }

   var.key = "px68k_ramsize";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int value = 0;
      if (strcmp(var.value, "1MB") == 0)
         value = 1;
      else if (strcmp(var.value, "2MB") == 0)
         value = 2;
      else if (strcmp(var.value, "3MB") == 0)
         value = 3;
      else if (strcmp(var.value, "4MB") == 0)
         value = 4;
      else if (strcmp(var.value, "5MB") == 0)
         value = 5;
      else if (strcmp(var.value, "6MB") == 0)
         value = 6;
      else if (strcmp(var.value, "7MB") == 0)
         value = 7;
      else if (strcmp(var.value, "8MB") == 0)
         value = 8;
      else if (strcmp(var.value, "9MB") == 0)
         value = 9;
      else if (strcmp(var.value, "10MB") == 0)
         value = 10;
      else if (strcmp(var.value, "11MB") == 0)
         value = 11;
      else if (strcmp(var.value, "12MB") == 0)
         value = 12;

      Config.ram_size = (value * 1024 * 1024);
   }

   var.key = "px68k_analog";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         opt_analog = false;
      if (!strcmp(var.value, "enabled"))
         opt_analog = true;
   }

   var.key    = "px68k_adpcm_vol";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      snd_opt = atoi(var.value);
      if (snd_opt != Config.PCM_VOL)
      {
         Config.PCM_VOL = snd_opt;
         ADPCM_SetVolume((uint8_t)Config.PCM_VOL);
      }
   }

   var.key    = "px68k_opm_vol";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      snd_opt = atoi(var.value);
      if (snd_opt != Config.OPM_VOL)
      {
         Config.OPM_VOL = snd_opt;
         OPM_SetVolume((uint8_t)Config.OPM_VOL);
      }
   }

#ifndef NO_MERCURY
   var.key    = "px68k_mercury_vol";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      snd_opt = atoi(var.value);
      if (snd_opt != Config.MCR_VOL)
      {
         Config.MCR_VOL = snd_opt;
         Mcry_SetVolume((uint8_t)Config.MCR_VOL);
      }
   }
#endif

   update_variable_disk_drive_swap();

   var.key    = "px68k_menufontsize";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (strcmp(var.value, "normal") == 0)
         Config.MenuFontSize = 0;
      else
         Config.MenuFontSize = 1;
   }

   var.key    = "px68k_joy1_select";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "XF1"))
         Config.joy1_select_mapping = KBD_XF1;
      else if (!strcmp(var.value, "XF2"))
         Config.joy1_select_mapping = KBD_XF2;
      else if (!strcmp(var.value, "XF3"))
         Config.joy1_select_mapping = KBD_XF3;
      else if (!strcmp(var.value, "XF4"))
         Config.joy1_select_mapping = KBD_XF4;
      else if (!strcmp(var.value, "XF5"))
         Config.joy1_select_mapping = KBD_XF5;
      else if (!strcmp(var.value, "F1"))
         Config.joy1_select_mapping = KBD_F1;
      else if (!strcmp(var.value, "F2"))
         Config.joy1_select_mapping = KBD_F2;
      else if (!strcmp(var.value, "OPT1"))
         Config.joy1_select_mapping = KBD_OPT1;
      else if (!strcmp(var.value, "OPT2"))
         Config.joy1_select_mapping = KBD_OPT2;
      else
         Config.joy1_select_mapping = 0;
   }

   var.key    = "px68k_save_fdd_path";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Config.save_fdd_path = 0;
      if (!strcmp(var.value, "enabled"))
         Config.save_fdd_path = 1;
   }

   var.key    = "px68k_save_hdd_path";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Config.save_hdd_path = 0;
      if (!strcmp(var.value, "enabled"))
         Config.save_hdd_path = 1;
   }

   var.key    = "px68k_rumble_on_disk_read";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         opt_rumble_enabled = false;
      if (!strcmp(var.value, "enabled"))
         opt_rumble_enabled = true;
   }

   /* PX68K Menu */

   var.key      = "px68k_joy_mouse";
   var.value    = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int value = 0;
      if (!strcmp(var.value, "Joystick"))
         value = 0;
      else if (!strcmp(var.value, "Mouse"))
         value = 1;

      Config.JoyOrMouse = value;
      Mouse_StartCapture(value == 1);
   }

   var.key    = "px68k_vbtn_swap";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "TRIG1 TRIG2"))
         Config.VbtnSwap = 0;
      else if (!strcmp(var.value, "TRIG2 TRIG1"))
         Config.VbtnSwap = 1;
   }

   var.key    = "px68k_no_wait_mode";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Config.NoWaitMode = 0;
      else if (!strcmp(var.value, "enabled"))
         Config.NoWaitMode = 1;
   }

   var.key    = "px68k_frameskip";
   var.value  = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "Auto Frame Skip"))
         Config.FrameRate = 7;
      else if (!strcmp(var.value, "1/2 Frame"))
         Config.FrameRate = 2;
      else if (!strcmp(var.value, "1/3 Frame"))
         Config.FrameRate = 3;
      else if (!strcmp(var.value, "1/4 Frame"))
         Config.FrameRate = 4;
      else if (!strcmp(var.value, "1/5 Frame"))
         Config.FrameRate = 5;
      else if (!strcmp(var.value, "1/6 Frame"))
         Config.FrameRate = 6;
      else if (!strcmp(var.value, "1/8 Frame"))
         Config.FrameRate = 8;
      else if (!strcmp(var.value, "1/16 Frame"))
         Config.FrameRate = 16;
      else if (!strcmp(var.value, "1/32 Frame"))
         Config.FrameRate = 32;
      else if (!strcmp(var.value, "1/60 Frame"))
         Config.FrameRate = 60;
      else if (!strcmp(var.value, "Full Frame"))
         Config.FrameRate = 1;
   }

   var.key     = "px68k_adjust_frame_rates";
   var.value   = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      int temp = Config.AdjustFrameRates;
      if (!strcmp(var.value, "disabled"))
         Config.AdjustFrameRates = 0;
      else if (!strcmp(var.value, "enabled"))
         Config.AdjustFrameRates = 1;

      if (running) /* minimize the chance of resetting av_info during startup */
         CHANGEAV_TIMING = CHANGEAV_TIMING || Config.AdjustFrameRates != temp;
   }

   var.key   = "px68k_audio_desync_hack";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Config.AudioDesyncHack = 0;
      else if (!strcmp(var.value, "enabled"))
         Config.AudioDesyncHack = 1;
   }

   var.key   = "px68k_text_off";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Debug_Text = 1;
      else if (!strcmp(var.value, "enabled"))
         Debug_Text = 0;
   }

   var.key   = "px68k_grp_off";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Debug_Grp = 1;
      else if (!strcmp(var.value, "enabled"))
         Debug_Grp = 0;
   }

   var.key   = "px68k_sp_off";
   var.value = NULL;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         Debug_Sp = 1;
      else if (!strcmp(var.value, "enabled"))
         Debug_Sp = 0;
   }
}

/************************************
 * libretro implementation
 ************************************/

void retro_get_system_info(struct retro_system_info *info)
{
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
#ifndef PX68K_VERSION
#define PX68K_VERSION "0.15+"
#endif
   info->library_name     = "PX68K";
   info->library_version  = PX68K_VERSION GIT_VERSION;
   info->need_fullpath    = true;
   info->valid_extensions = "dim|img|d88|88d|hdm|dup|2hd|xdf|hdf|cmd|m3u";
   info->block_extract    = false;
}


void retro_get_system_av_info(struct retro_system_av_info *info)
{
   /* FIXME handle PAL/NTSC */
   struct retro_game_geometry geom   = { retrow, retroh, 800, 600, 4.0 / 3.0 };
   struct retro_system_timing timing = { FRAMERATE, SOUNDRATE };
   info->geometry                    = geom;
   info->timing                      = timing;
}

static void frame_time_cb(retro_usec_t usec)
{
   total_usec += usec;
   /* -1 is reserved as an error code for unavailable a la stdlib clock() */
   if (total_usec == (unsigned int) -1)
      total_usec = 0;
}

static void setup_frame_time_cb(void)
{
   struct retro_frame_time_callback cb;

   cb.callback   = frame_time_cb;
   cb.reference  = ceil(1000000 / FRAMERATE);
   if (!environ_cb(RETRO_ENVIRONMENT_SET_FRAME_TIME_CALLBACK, &cb))
      total_usec = (unsigned int) -1;
   else if (total_usec == (unsigned int) -1)
      total_usec = 0;
}

/* TODO/FIXME - implement savestates */
int StateAction(StateMem *sm, int load, int data_only)
{
   SFORMAT StateRegs[] =
   {
      SFARRAYN(MEM, MEM_SIZE, "RAM"),
      SFARRAYN(SRAM, 16384, "SRAM"),
      SFVAR(ICount),
      SFVAR(ClkUsed),
      SFVAR(VLINE),
      SFVAR(VLINE_TOTAL),

      SFVAR(tick),
      SFVAR(timercnt),

      SFEND
   };

   int ret = 0, count = 0;

   ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "MAIN", false);
   ret &= m68000_StateAction(sm, load, data_only);
   ret &= GVRAM_StateAction(sm, load, data_only);
   ret &= TVRAM_StateAction(sm, load, data_only);
   ret &= CRTC_StateAction(sm, load, data_only);
   ret &= Pal_StateAction(sm, load, data_only);
   ret &= BG_StateAction(sm, load, data_only);
   ret &= DMAC_StateAction(sm, load, data_only);
   ret &= MFP_StateAction(sm, load, data_only);
   ret &= IRQH_StateAction(sm, load, data_only);
   ret &= SCC_StateAction(sm, load, data_only);
   ret &= FDC_StateAction(sm, load, data_only);
   ret &= FDD_StateAction(sm, load, data_only);
   ret &= SASI_StateAction(sm, load, data_only);

   ret &= RTC_StateAction(sm, load, data_only);
   ret &= PIA_StateAction(sm, load, data_only);
   ret &= SysPort_StateAction(sm, load, data_only);
   ret &= IOC_StateAction(sm, load, data_only);
   ret &= SRAM_StateAction(sm, load, data_only);

   /* sound-related states */
   ret &= dswin_StateAction(sm, load, data_only);
   ret &= ADPCM_StateAction(sm, load, data_only);
   ret &= MIDI_StateAction(sm, load, data_only);
   ret &= OPM_StateAction(sm, load, data_only);

   return ret;
}

static bool UsingFastSavestates(void)
{
   int flags;
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVESTATE_CONTEXT, &flags))
      return ((flags == RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_INSTANCE) ||
            ((flags == RETRO_SAVESTATE_CONTEXT_RUNAHEAD_SAME_BINARY)));
   return false;
}

size_t retro_serialize_size(void)
{
   StateMem st;

   st.data           = NULL;
   st.loc            = 0;
   st.len            = 0;
   st.malloced       = 0;
   st.initial_malloc = 0;
   st.fastsavestates = 0;

   if (!PX68KSS_SaveSM(&st, 0, 0, NULL, NULL, NULL))
      return 0;

   free(st.data);

   return st.len;
}

bool retro_serialize(void *data, size_t size)
{
   StateMem st;
   bool ret          = false;
   uint8_t *_dat     = (uint8_t*)malloc(size);

   if (!_dat)
      return false;

   st.data           = _dat;
   st.loc            = 0;
   st.len            = 0;
   st.malloced       = size;
   st.initial_malloc = 0;
   st.fastsavestates = UsingFastSavestates();

   ret = PX68KSS_SaveSM(&st, 0, 0, NULL, NULL, NULL);

   memcpy(data, st.data, size);
   free(st.data);

   return ret;
}

bool retro_unserialize(const void *data, size_t size)
{
   StateMem st;
   bool ret = false;

   st.data           = (uint8_t*)data;
   st.loc            = 0;
   st.len            = size;
   st.malloced       = 0;
   st.initial_malloc = 0;
   st.fastsavestates = UsingFastSavestates();

   ret = PX68KSS_LoadSM(&st, 0, 0);

   return ret;
}

/* TODO/FIXME - implement cheats */
void retro_cheat_reset(void) { }
void retro_cheat_set(unsigned index, bool enabled, const char *code) { }

bool retro_load_game(const struct retro_game_info *info)
{
   no_content = 1;
   RPATH[0] = '\0';

   if (info && info->path)
   {
      const char *full_path = info->path;
      no_content            = 0;
      strcpy(RPATH, full_path);
      extract_directory(base_dir, info->path, sizeof(base_dir));

      if (!retro_load_game_internal(RPATH))
         return false;
   }

   /* alloc memory pointers */
   if (!WinX68k_Init())
      return false;
   
   /* alloc OPM-related pointers */
   if (!OPM_Init(4000000/*3579545*/))
      return false;

   return true;
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info
*info, size_t num_info) { return false; }

void retro_unload_game(void)
{
   RPATH[0]    = '\0';
   firstcall   = 0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void *retro_get_memory_data(unsigned id)
{
   if ( id == RETRO_MEMORY_SYSTEM_RAM )
      return MEM;
   return NULL;
}

size_t retro_get_memory_size(unsigned id)
{
   if ( id == RETRO_MEMORY_SYSTEM_RAM )
      return 0xc00000;
    return 0;
}

void retro_init(void)
{
   struct retro_log_callback log;
   struct retro_rumble_interface rumble;
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
   const char *system_dir      = NULL;
   const char *content_dir     = NULL;
   const char *save_dir        = NULL;
   const char *browse_dir      = NULL;

   retro_system_conf[0] = 0;
   retro_browse_conf[0] = 0;

   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = NULL;

   /* if defined, use the system directory */
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
      retro_system_directory = system_dir;

   /* if defined, use the system directory */
   if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir)
      retro_content_directory = content_dir;

   /* If save directory is defined use it, otherwise use system directory */
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
      retro_save_directory = *save_dir ? save_dir : retro_system_directory;
   else
      /* make retro_save_directory the same in case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY is not implemented by the frontend */
      retro_save_directory = retro_system_directory;
   
   /* If browse directory is defined use it for StartDir config */
   if (environ_cb(RETRO_ENVIRONMENT_GET_FILE_BROWSER_START_DIRECTORY, &browse_dir) && browse_dir)
      retro_browse_directory = browse_dir;

   if (!retro_system_directory)
      strcpy(RETRO_DIR, ".");
   else
      strcpy(RETRO_DIR, retro_system_directory);

   sprintf(retro_system_conf, "%s%ckeropi", RETRO_DIR, SLASH);

   if (retro_browse_directory)
      strcpy(retro_browse_conf, retro_browse_directory);

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      exit(0);

   if (environ_cb(RETRO_ENVIRONMENT_GET_RUMBLE_INTERFACE, &rumble) && rumble.set_rumble_state)
      rumble_cb = rumble.set_rumble_state;

   libretro_supports_input_bitmasks = 0;
   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_input_bitmasks = 1;

   disk_swap_interface_init();
#if 0
    struct retro_keyboard_callback cbk = { keyboard_cb };
    environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &cbk);
#endif

   midi_interface_init();

   /* set sane defaults */
   Config.save_fdd_path = 1;
   Config.clockmhz      = 10;
   Config.ram_size      = 2 * 1024 *1024;
   Config.JOY_TYPE[0]   = 0;
   Config.JOY_TYPE[1]   = 0;

   update_variables(0);

   memset(Core_Key_State, 0, 512);
   memset(Core_old_Key_State, 0, sizeof(Core_old_Key_State));

   FRAMERATE = framerates[Config.AdjustFrameRates][VID_MODE];
   setup_frame_time_cb();
}

void retro_deinit(void)
{
   SRAM_UpdateBoot();

   OPM_Cleanup();
#ifndef	NO_MERCURY
   Mcry_Cleanup();
#endif

   Joystick_Cleanup();
   SRAM_Cleanup();
   FDD_Cleanup();
#if 0
   CDROM_Cleanup();
#endif
   MIDI_Cleanup();
   WinX68k_Cleanup();
   WinDraw_Cleanup();

   SaveConfig();
   libretro_supports_input_bitmasks    = 0;
   libretro_supports_midi_output       = 0;
   libretro_supports_option_categories = 0;
}

void retro_reset(void)
{
   WinX68k_Reset();
   if (Config.MIDI_SW && Config.MIDI_Reset)
	   MIDI_Reset();
}

static void rumble_frames(void)
{
   static int last_read_state;

   if (!rumble_cb)
      return;

   if (last_read_state != FDD_IsReading)
   {
      if (opt_rumble_enabled && FDD_IsReading)
      {
         rumble_cb(0, RETRO_RUMBLE_STRONG, 0x8000);
         rumble_cb(0, RETRO_RUMBLE_WEAK, 0x800);
      }
      else
      {
         rumble_cb(0, RETRO_RUMBLE_STRONG, 0);
         rumble_cb(0, RETRO_RUMBLE_WEAK, 0);
      }
   }

   last_read_state = FDD_IsReading;
}

typedef struct {
   uint16_t lrkey;
   uint8_t keycode;
} LRKCNV;

static const LRKCNV KeyTable[] = {
   { RETROK_ESCAPE,        0x01 }, /* ESC */
   { RETROK_1,             0x02 }, /* 1 ! */
   { RETROK_2,             0x03 }, /* 2 " */
   { RETROK_3,             0x04 }, /* 3 # */
   { RETROK_4,             0x05 }, /* 4 $ */
   { RETROK_5,             0x06 }, /* 5 % */
   { RETROK_6,             0x07 }, /* 6 & */
   { RETROK_7,             0x08 }, /* 7 ' */
   { RETROK_8,             0x09 }, /* 8 ( */
   { RETROK_9,             0x0a }, /* 9 ) */
   { RETROK_0,             0x0b }, /* 0 _ */
   { RETROK_MINUS,         0x0c }, /* - = */
   { RETROK_EQUALS,        0x0d }, /* ^ ~ */
   { RETROK_BACKSLASH,     0x0e }, /* ・ゑｽ･ | */ /* yen symbol */
   { RETROK_BACKSPACE,     0x0f }, /* BS */

   { RETROK_TAB,           0x10 }, /* TAB */
   { RETROK_q,             0x11 }, /* q Q */
   { RETROK_w,             0x12 }, /* e Q */
   { RETROK_e,             0x13 }, /* e E */
   { RETROK_r,             0x14 }, /* r R */
   { RETROK_t,             0x15 }, /* t T */
   { RETROK_y,             0x16 }, /* y Y */
   { RETROK_u,             0x17 }, /* u U */
   { RETROK_i,             0x18 }, /* i I */
   { RETROK_o,             0x19 }, /* o O */
   { RETROK_p,             0x1a }, /* p P */
   { RETROK_BACKQUOTE,     0x1b }, /* @ ` */
   { RETROK_LEFTBRACKET,   0x1c }, /*[] ] } */
   { RETROK_RETURN,        0x1d }, /* RETURN */

   { RETROK_a,             0x1e }, /* a A */
   { RETROK_s,             0x1f }, /* s S */
   { RETROK_d,             0x20 }, /* d D */
   { RETROK_f,             0x21 }, /* f F */
   { RETROK_g,             0x22 }, /* g G */
   { RETROK_h,             0x23 }, /* h H */
   { RETROK_j,             0x24 }, /* j J */
   { RETROK_k,             0x25 }, /* k K */
   { RETROK_l,             0x26 }, /* l L */
   { RETROK_SEMICOLON,     0x27 }, /* ; + */
   { RETROK_QUOTE,         0x28 }, /* : * */
   { RETROK_RIGHTBRACKET,  0x29 }, /* [ { */

   { RETROK_z,             0x2a }, /* z Z */
   { RETROK_x,             0x2b }, /* x X */
   { RETROK_c,             0x2c }, /* c C */
   { RETROK_v,             0x2d }, /* v V */
   { RETROK_b,             0x2e }, /* b B */
   { RETROK_n,             0x2f }, /* n N */
   { RETROK_m,             0x30 }, /* m M */
   { RETROK_COMMA,         0x31 }, /* , < */
   { RETROK_PERIOD,        0x32 }, /* . > */
   { RETROK_SLASH,         0x33 }, /* / ? */
   { RETROK_0,             0x34 }, /* underquote _ as shift+0 which was empty, Japanese
                                    chars can't overlap as we're not using them */

   { RETROK_SPACE,         0x35 }, /* SPACE */
   { RETROK_HOME,          0x36 }, /* HOME */
   { RETROK_DELETE,        0x37 }, /* DEL */
   { RETROK_PAGEDOWN,      0x38 }, /* ROLL UP */
   { RETROK_PAGEUP,        0x39 }, /* ROLL DOWN */
   { RETROK_END,           0x3a }, /* UNDO */
   { RETROK_LEFT,          0x3b }, /* 遶翫・*/
   { RETROK_UP,            0x3c }, /* 遶翫・*/
   { RETROK_RIGHT,         0x3d }, /* 遶翫・*/
   { RETROK_DOWN,          0x3e }, /* 遶翫・*/

   { RETROK_CLEAR,         0x3f }, /* CLR */
   { RETROK_KP_DIVIDE,     0x40 }, /* / */
   { RETROK_KP_MULTIPLY,   0x41 }, /* * */
   { RETROK_KP_MINUS,      0x42 }, /* - */
   { RETROK_KP7,           0x43 }, /* 7 */
   { RETROK_KP8,           0x44 }, /* 8 */
   { RETROK_KP9,           0x45 }, /* 9 */
   { RETROK_KP_PLUS,       0x46 }, /* + */
   { RETROK_KP4,           0x47 }, /* 4 */
   { RETROK_KP5,           0x48 }, /* 5 */
   { RETROK_KP6,           0x49 }, /* 6 */
   { RETROK_KP_EQUALS,     0x4a }, /* = */
   { RETROK_KP1,           0x4b }, /* 1 */
   { RETROK_KP2,           0x4c }, /* 2 */
   { RETROK_KP3,           0x4d }, /* 3 */
   { RETROK_KP_ENTER,      0x4e }, /* ENTER */
   { RETROK_KP0,           0x4f }, /*  */
#if 0
   { RETROK_COMMA,0x50 },  /* . > */
#endif
   { RETROK_KP_PERIOD,     0x51 }, /* . */

   { RETROK_PRINT,         0x52 }, /* symbol input (kigou) */
   { RETROK_SCROLLOCK,     0x53 }, /* registration (touroku) */
   { RETROK_F11,           0x54 }, /* HELP */
#if 0
   { NC,                   0x55 }, /* XF1 */
   { NC,                   0x56 }, /* XF2 */
   { NC,                   0x57 }, /* XF3 */
   { NC,                   0x58 }, /* XF4 */
   { NC,                   0x59 }, /* XF5 */

   { NC,                   0x5a }, /* KANA */
   { NC,                   0x5b }, /* ROMAN Alphabet */
   { NC,                   0x5c }, /* Enter code */
#endif
   { RETROK_CAPSLOCK,      0x5d }, /* CAPSLOCK */

   { RETROK_INSERT,        0x5e }, /* INSERT */
#if 0
   { NC,                   0x5f }, /* Hiragana */
   { NC,                   0x60 }, /* Full-width */
#endif
   { RETROK_BREAK,         0x61 },  /* BREAK */
   { RETROK_PAUSE,         0x61 },  /* BREAK (allow shift+break combo) */
#if 0
   { NC,                   0x62 }, /* COPY */
#endif
   { RETROK_F1,            0x63},  /* F1 */
   { RETROK_F2,            0x64},  /* F2 */
   { RETROK_F3,            0x65},  /* F3 */
   { RETROK_F4,            0x66},  /* F4 */
   { RETROK_F5,            0x67},  /* F5 */
   { RETROK_F6,            0x68},  /* F6 */
   { RETROK_F7,            0x69},  /* F7 */
   { RETROK_F8,            0x6a},  /* F8 */
   { RETROK_F9,            0x6b},  /* F9 */
   { RETROK_F10,           0x6c},  /* F10 */

#if 0
   { NC,                   0x6d }, /* unused */
   { NC,                   0x6e }, /* unused */
   { NC,                   0x6f }, /* unused */
#endif

   { RETROK_LSHIFT,        0x70 }, /* SHIFT */
   { RETROK_RSHIFT,        0x70 }, /* SHIFT */
   { RETROK_LCTRL,         0x71 }, /* CTRL */
   { RETROK_RCTRL,         0x71 }, /* CTRL */
   { RETROK_LSUPER,        0x72 }, /* OPT.1 */
   { RETROK_RSUPER,        0x73 }, /* OPT.2 */
   { RETROK_LALT,          0x72 }, /* OPT.1 */
   { RETROK_RALT,          0x73 }, /* OPT.2 */
};

static void handle_retrok(void)
{
   int i;

#define KEYP(a, b)                                                            \
   {                                                                          \
      if (Core_Key_State[a] && Core_Key_State[a] != Core_old_Key_State[a])    \
      {                                                                       \
         log_cb(RETRO_LOG_DEBUG, "KeyDown: lrkey = %3d code = %02x\n", a, b); \
         send_keycode(b, 2);                                                  \
      }                                                                       \
      else if (!Core_Key_State[a] && Core_Key_State[a] != Core_old_Key_State[a]) \
      {                                                                       \
         log_cb(RETRO_LOG_DEBUG, "KeyUp: lrkey = %3d code = %02x\n", a, b);   \
         send_keycode(b, 1);                                                  \
      }                                                                       \
   }

   if(Core_Key_State[RETROK_F12] && Core_Key_State[RETROK_F12]!=Core_old_Key_State[RETROK_F12]  )
   {
      if (menu_mode == menu_out)
      {
         oldrw     = retrow;
         oldrh     = retroh;
         retrow    = 800;
         retroh    = 600;
         CHANGEAV  = 1;
         menu_mode = menu_enter;
         DSound_Stop();
      }
      else
      {
         CHANGEAV  = 1;
         retrow    = oldrw;
         retroh    = oldrh;
         DSound_Play();
         menu_mode = menu_out;
      }
   }

   if (Core_Key_State[RETROK_COMPOSE])
   {
      static const LRKCNV KeyTable1[] = {
         { RETROK_F1,      0x55 }, /* XF1 */
         { RETROK_F2,      0x56 }, /* XF2 */
         { RETROK_F3,      0x57 }, /* XF3 */
         { RETROK_F4,      0x58 }, /* XF4 */
         { RETROK_F5,      0x59 }, /* XF5 */
         { RETROK_F6,      0x5a }, /* KANA */
         { RETROK_F7,      0x5b }, /* ROMAN Alphabet */
         { RETROK_F8,      0x5c }, /* Enter code */
         { RETROK_F9,      0x5f }, /* Hiragana */
         { RETROK_F10,     0x60 }, /* Full-width */
      };
      int i;
      for (i = 0; i < NELEMENTS(KeyTable1); i++)
      {
         KEYP(KeyTable1[i].lrkey, KeyTable1[i].keycode);
      }
   }
   else
   {
      for (i = 0; i < NELEMENTS(KeyTable); i++)
         KEYP(KeyTable[i].lrkey, KeyTable[i].keycode);
   }
   
   /* only process kb_to_joypad map when its not zero, else button is used as
    * joypad select mode */
   if (Config.joy1_select_mapping)
      KEYP(RETROK_XFX, Config.joy1_select_mapping);
#undef KEYP
}

#define CLOCK_SLICE 200

/*  Core Main Loop */
static void WinX68k_Exec(void)
{
   int clk_total, clkdiv, usedclk, hsync, clk_next, clk_count, clk_line=0;
   int KeyIntCnt = 0, MouseIntCnt = 0;
   uint32_t t_start = timeGetTime(), t_end;

   if(!(cpu_readmem24_dword(0xed0008) == Config.ram_size))
   {
      cpu_writemem24(0xe8e00d, 0x31); /* SRAM write permission */
      cpu_writemem24_dword(0xed0008, Config.ram_size); /* Define RAM amount */
   }

   if (Config.FrameRate != 7)
      DispFrame = (DispFrame + 1) % Config.FrameRate;
   else
   {				/* Auto Frame Skip */
      if (FrameSkipQueue)
      {
         if (FrameSkipCount > 15)
         {
            FrameSkipCount = 0;
            FrameSkipQueue++;
            DispFrame      = 0;
         }
         else
         {
            FrameSkipCount++;
            FrameSkipQueue--;
            DispFrame      = 1;
         }
      }
      else
      {
         FrameSkipCount    = 0;
         DispFrame         = 0;
      }
   }

   vline     = 0;
   clk_count = -ICount;
   clk_total = (CRTC_Regs[0x29] & 0x10) ? VSYNC_HIGH : VSYNC_NORM;

   clk_total = (clk_total*Config.clockmhz)/10;
   clkdiv    = Config.clockmhz;

#if 0
   if (Config.XVIMode == 1)
   {
      clk_total = (clk_total * 16) / 10;
      clkdiv    = 16;
   }
   else if (Config.XVIMode == 2)
   {
      clk_total = (clk_total * 24) / 10;
      clkdiv    = 24;
   }
   else
      clkdiv    = 10;
#endif

   if(clkdiv != old_clkdiv || Config.ram_size != old_ram_size)
   {
      old_clkdiv = clkdiv;
      old_ram_size = Config.ram_size;
   }

   ICount  += clk_total;
   clk_next = (clk_total/VLINE_TOTAL);
   hsync    = 1;

   do
   {
      int m, n = (ICount > CLOCK_SLICE) ? CLOCK_SLICE : ICount;

      if ( hsync )
      {
         hsync    = 0;
         clk_line = 0;
         MFP_Int(0);
         if ((vline >= CRTC_VSTART) && (vline < CRTC_VEND))
            VLINE = ((vline - CRTC_VSTART) * CRTC_VStep) / 2;
         else
            VLINE = (uint32_t)-1;
         if ((!(MFP[MFP_AER] & 0x40)) && (vline == CRTC_IntLine))
            MFP_Int(1);
         if (MFP[MFP_AER] & 0x10)
         {
            if (vline == CRTC_VSTART)
               MFP_Int(9);
         }
         else
         {
            if (CRTC_VEND >= VLINE_TOTAL)
            {
               if ((long)vline == (CRTC_VEND - VLINE_TOTAL))
                  MFP_Int(9);		/* Is it Exciting Hour? 繝ｻ繝ｻOTAL<VEND繝ｻ繝ｻ*/
            }
            else
            {
               if ((long)vline == (VLINE_TOTAL-1))
                  MFP_Int(9);		/* Is it Crazy Climber? */
            }
         }
      }

      {
#if defined (HAVE_CYCLONE)
         m68000_execute(n);
#elif defined (HAVE_C68K)
         C68k_Exec(&C68K, n);
#elif defined (HAVE_MUSASHI)
         m68k_execute(n);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
         m          = (n-m68000_ICountBk);
         ClkUsed   += m*10;
         usedclk    = ClkUsed/clkdiv;
         clk_line  += usedclk;
         ClkUsed   -= usedclk*clkdiv;
         ICount    -= m;
         clk_count += m;
      }

      MFP_Timer(usedclk);
      RTC_Timer(usedclk);
      DMA_Exec(0);
      DMA_Exec(1);
      DMA_Exec(2);

      if (clk_count >= clk_next)
      {
         MIDI_DelayOut((Config.MIDIAutoDelay)?(Config.BufferSize*5):Config.MIDIDelay);
         MFP_TimerA();
         if ((MFP[MFP_AER] & 0x40) && (vline == CRTC_IntLine))
            MFP_Int(1);
         if ( (!DispFrame) && (vline >= CRTC_VSTART) && (vline < CRTC_VEND))
         {
            if ( CRTC_VStep==1 )
            {
               /* HighReso 256dot (read twice) */
               if ( vline%2 )
                  WinDraw_DrawLine();
            }
            else if (CRTC_VStep == 4)
            {
               /* LowReso 512dot
                * draw twice per scanline (interlace) */
               WinDraw_DrawLine();
               VLINE++;
               WinDraw_DrawLine();
            }
            else /* High 512dot / Low 256dot */
               WinDraw_DrawLine();
         }

         ADPCM_PreUpdate(clk_line);
         OPM_Timer(clk_line);
         MIDI_Timer(clk_line);
#ifndef	NO_MERCURY
         Mcry_PreUpdate(clk_line);
#endif

         KeyIntCnt++;
         if (KeyIntCnt > (VLINE_TOTAL/4))
         {
            KeyIntCnt = 0;
            Keyboard_Int();
         }
         MouseIntCnt++;
         if (MouseIntCnt > (VLINE_TOTAL / 8))
         {
            MouseIntCnt = 0;
            SCC_IntCheck();
         }
         DSound_Send0(clk_line);

         vline++;
         clk_next  = (clk_total*(vline+1))/VLINE_TOTAL;
         hsync     = 1;
      }
   } while (vline < VLINE_TOTAL);

   if (CRTC_Mode & 2)
   {
      /* FastClr byte adjustment (PITAPAT) */
      if (CRTC_FastClr)
      {
         /* if FastClr=1 and CRTC_Mode&2 then end */
         CRTC_FastClr--;
         if (!CRTC_FastClr)
            CRTC_Mode &= 0xfd;
      }
      else
      {
         /* FastClr start */
         if (CRTC_Regs[0x29] & 0x10)
            CRTC_FastClr = 1;
         else
            CRTC_FastClr = 2;
         TVRAM_SetAllDirty();
         GVRAM_FastClear();
      }
   }

   /* One final audio batch per emulated frame. Register writes flush earlier as needed. */
   DSound_FlushPending();

   FDD_SetFDInt();
   if (!DispFrame)
      WinDraw_Draw();

   t_end = timeGetTime();
   if ((int)(t_end - t_start) > ((CRTC_Regs[0x29] & 0x10) ? 14 : 16))
   {
      FrameSkipQueue += ((t_end - t_start) / ((CRTC_Regs[0x29] & 0x10) ? 14 : 16)) + 1;
      if (FrameSkipQueue > 100)
         FrameSkipQueue = 100;
   }
}

void retro_run(void)
{
   int i;
   int mouse_x, mouse_y, mouse_l, mouse_r;
   bool updated    = false;
   static bool mbL = false, mbR = false;

   if (firstcall)
   {
      pre_main();
      firstcall     = 0;
      /* Initialization done */
      update_variables(0);
      soundbuf_size = SNDSZ;
      return;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
      update_variables(1);

   if (CHANGEAV || CHANGEAV_TIMING)
   {
      if (CHANGEAV_TIMING)
      {
         struct retro_system_av_info system_av_info;
         retro_get_system_av_info(&system_av_info);
         FRAMERATE                            = framerates[Config.AdjustFrameRates][VID_MODE];
         system_av_info.timing.fps            = FRAMERATE;
         environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &system_av_info);
         setup_frame_time_cb();
         CHANGEAV_TIMING                      = 0;
         CHANGEAV                             = 0;
      }
      if (CHANGEAV)
      {
         struct retro_system_av_info system_av_info;
         system_av_info.geometry.base_width   = retrow;
         system_av_info.geometry.base_height  = retroh;
         system_av_info.geometry.aspect_ratio = (float)4.0/3.0;
         environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &system_av_info);
         CHANGEAV                             = 0;
      }
      soundbuf_size                           = SNDSZ;
   }

   input_poll_cb();
   rumble_frames();

   FDD_IsReading = 0;

   if (     (menu_mode == menu_out)
         && (  Config.AudioDesyncHack
            || Config.NoWaitMode
            || Timer_GetCount()))
   {
      Joystick_Update(0, -1, 0);
      Joystick_Update(0, -1, 1);

      WinX68k_Exec();
   }

   mouse_x       = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_X);
   mouse_y       = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_Y);

   Mouse_Event(0, mouse_x, mouse_y);

   mouse_l       = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_LEFT);
   mouse_r       = input_state_cb(0, RETRO_DEVICE_MOUSE, 0, RETRO_DEVICE_ID_MOUSE_RIGHT);

   if(!mbL && mouse_l)
   {
      mbL         = true;
      Mouse_Event(1,1.0,0);
   }
   else if(mbL && !mouse_l)
   {
      mbL         = false;
      Mouse_Event(1,0,0);
   }
   if(!mbR && mouse_r)
   {
      mbR         = true;
      Mouse_Event(2,1.0,0);
   }
   else if(mbR && !mouse_r)
   {
      mbR         = false;
      Mouse_Event(2,0,0);
   }

   for(i = 0; i < 320; i++)
      Core_Key_State[i] = input_state_cb(0, RETRO_DEVICE_KEYBOARD, 0, i) ? 0x80: 0;

   Core_Key_State[RETROK_XFX] = 0;

   /* Joypad Key for Menu */
   if (input_state_cb(0, RETRO_DEVICE_JOYPAD,0, RETRO_DEVICE_ID_JOYPAD_L2))
      Core_Key_State[RETROK_F12] = 0x80;

   if (input_state_cb(0, RETRO_DEVICE_JOYPAD,0, RETRO_DEVICE_ID_JOYPAD_R2))  /*Joypad key for touroku key in order to enable MIDI when preseed on start up in Wolfteam games*/
      Core_Key_State[RETROK_SCROLLOCK] = 0x80;

   if (Config.joy1_select_mapping)
   {
      /* Joypad Key for Mapping */
      if (input_state_cb(0, RETRO_DEVICE_JOYPAD,0,
               RETRO_DEVICE_ID_JOYPAD_SELECT))
         Core_Key_State[RETROK_XFX] = 0x80;
   }

   if(memcmp( Core_Key_State,Core_old_Key_State , sizeof(Core_Key_State) ) )
      handle_retrok();

   memcpy(Core_old_Key_State,Core_Key_State , sizeof(Core_Key_State) );

   if (menu_mode != menu_out)
   {
      int ret = 0;
      int key = 0;
      if (Core_Key_State[RETROK_RIGHT] || Core_Key_State[RETROK_PAGEDOWN])
         key |= JOY_RIGHT;
      if (Core_Key_State[RETROK_LEFT] || Core_Key_State[RETROK_PAGEUP])
         key |= JOY_LEFT;
      if (Core_Key_State[RETROK_UP])
         key |= JOY_UP;
      if (Core_Key_State[RETROK_DOWN])
         key |= JOY_DOWN;
      if (Core_Key_State[RETROK_z] || Core_Key_State[RETROK_RETURN])
         key |= JOY_TRG1;
      if (Core_Key_State[RETROK_x] || Core_Key_State[RETROK_BACKSPACE])
         key |= JOY_TRG2;

      Joystick_Update(1, key, 0);

      ret       = WinUI_Menu(menu_mode == menu_enter);
      menu_mode = menu_in;
      if (ret == WUM_MENU_END)
      {
         DSound_Play();
         menu_mode = menu_out;
      }
   }

   if (Config.AudioDesyncHack)
   {
      int nsamples = audio_samples_avail();
      if (nsamples > soundbuf_size)
         audio_samples_discard(nsamples - soundbuf_size);
   }
   raudio_callback(soundbuf, NULL, soundbuf_size << 2);

   if (libretro_supports_midi_output && midi_cb.output_enabled())
      midi_cb.flush();

   audio_batch_cb((const int16_t*)soundbuf, soundbuf_size);
   /* TODO/FIXME - hardcoded pitch here */
   video_cb(videoBuffer, retrow, retroh, /*retrow*/ 800 << 1);
}

/* BUILD2_TAB5_VIDEO_BEGIN
 *
 * Standalone ESP32-P4 video path.
 *
 * Keep the known-good Build 1B CPU/CRTC/MFP scheduler, but restore
 * PX68K's native WinDraw scanline rendering.  No audio/input/frontend
 * callbacks are involved here.
 */
#ifdef ESP_PLATFORM

int WinX68k_VideoProbeInit(void)
{
    /*
     * In the normal libretro environment the frontend-side startup
     * path provides the RGB565 output buffer.
     *
     * Standalone ESP32-P4 has no frontend, so allocate PX68K's
     * maximum 800 x 600 x RGB565 surface explicitly in PSRAM.
     */
    if (!videoBuffer)
    {
        const size_t pixels = 800u * 600u;
        const size_t bytes  = pixels * sizeof(uint16_t);

        videoBuffer = (uint16_t *)heap_caps_calloc(
            pixels,
            sizeof(uint16_t),
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT
        );

        if (!videoBuffer)
            return 0;
    }

    /*
     * Initial libretro output geometry.
     * WinDraw may later reduce retrow/retroh according to CRTC mode,
     * but the backing pitch remains 800 pixels.
     */
    retrow = 800;
    retroh = 600;

    WinDraw_Init();

    return 1;
}

const uint16_t *WinX68k_GetVideoBuffer(void)
{
    return videoBuffer;
}

uint32_t WinX68k_GetVideoWidth(void)
{
    return retrow;
}

uint32_t WinX68k_GetVideoHeight(void)
{
    return retroh;
}

uint32_t WinX68k_GetVideoPitchPixels(void)
{
    /*
     * PX68K libretro submits video with pitch = 800 * sizeof(uint16_t).
     */
    return 800;
}

/*
 * ESP32-P4 standalone startup path.
 *
 * The libretro frontend normally reaches these calls through pmain().
 * app_main() does not run pmain(), so leaving them out means Config is
 * mostly zero and FDD/SRAM/SystemPort/ADPCM never receive their one-time
 * initialization.  In particular Config.ram_size==0 gets written to SRAM
 * by the frame scheduler, and FDD_Init()/XDF_Init() are skipped.
 */
int WinX68k_StandaloneInit(void)
{
    static int initialized = 0;

    if (initialized)
        return 1;

    /* Match PX68K/libretro defaults that retro_init() would normally set. */
    Config.save_fdd_path = 0;
    Config.FrameRate = 1;
    Config.clockmhz = 10;
    /* Build 5.98: WinX68k_Init() already allocates the full 12 MiB MEM
     * backing in PSRAM (MEM_SIZE=0x00c00000).  The standalone path had
     * merely advertised 2 MiB to Human68k.  Expose the full backing so
     * memory-hungry software such as SFXVI can use standard X68000 RAM
     * expansion without any additional host allocation. */
    Config.ram_size = 12 * 1024 * 1024;
    Config.OPM_VOL = 12;
    Config.PCM_VOL = 15;
    Config.MCR_VOL = 13;
    Config.BufferSize = 50;
    Config.NoWaitMode = 1;
    Config.AdjustFrameRates = 1;
    Config.AudioDesyncHack = 0;
    Config.XVIMode = 0;
    Config.JOY_TYPE[0] = PAD_CPSF_MD;
    Config.JOY_TYPE[1] = PAD_2BUTTON;
    Config.JoyOrMouse = 1;

    /* Build 5.97a: standalone previously left Config.MIDI_SW at zero.
     * That disabled PX68K's existing SHARP CZ-6BM1 / Yamaha YM3802
     * emulation and made $EAFA01-$EAFA0F look absent.  SFXVI polls the
     * YM3802 FIFO Tx-status register at $EAFA09 (bank 5), specifically
     * waiting for bit 6.  Keep the actual stock PX68K MIDI device present
     * even when the Tab5 has no external MIDI sink; midi_out_* already
     * degrades safely to a null output while FIFO/status/timers continue. */
    Config.MIDI_SW = 1;
    Config.MIDI_Type = 0; /* LA/MT-32-compatible default; hardware presence matters here. */

    /* Match the hardware one-time init section in PX68K pmain(). */
    Keyboard_Init();
    ADPCM_Init();
    FDD_Init();
    SysPort_Init();
    Mouse_Init();
    Joystick_Init();
    SRAM_Init();

    /* Build 5.13: activate the existing PX68K OPM + ADPCM mixer volumes. */
    DSound_Play();

    initialized = 1;

    printf("PX68K_STANDALONE: init OK clock=%dMHz ram=%lu FDD/XDF/SRAM/SYSPORT initialized; MIDI=CZ-6BM1/YM3802 enabled\n",
           Config.clockmhz,
           (unsigned long)Config.ram_size);

    return 1;
}

int WinX68k_FloppyReady(int drive)
{
    return FDD_IsReady(drive);
}

/* Build 5.12: low-duty standalone profiler.  The host enables this only
 * for one frame every ~600 frames, so the many esp_timer_get_time() calls
 * below do not burden normal gameplay. */
static int s_tab5_perf_sample = 0;
static uint32_t s_tab5_perf_frame_us = 0;
static uint32_t s_tab5_perf_cpu_us = 0;
static uint32_t s_tab5_perf_compose_us = 0;
static uint32_t s_tab5_perf_finalize_us = 0;
/* Build 5.17: sample-only scheduler/device breakdown. */
static uint32_t s_tab5_perf_timer_us = 0;
static uint32_t s_tab5_perf_dma_us = 0;
static uint32_t s_tab5_perf_line_us = 0;
static uint32_t s_tab5_perf_audio_timer_us = 0;
static uint32_t s_tab5_perf_input_us = 0;
static uint32_t s_tab5_perf_soundmix_us = 0;
static uint32_t s_tab5_perf_fdd_us = 0;
/* Build 5.43: complete the previously hidden dev= bucket. */
static uint32_t s_tab5_perf_mfp_us = 0;
static uint32_t s_tab5_perf_rtc_us = 0;
static uint32_t s_tab5_perf_edge_us = 0;
static uint32_t s_tab5_perf_sched_us = 0;
static uint32_t s_tab5_perf_adclk_us = 0;
static uint32_t s_tab5_perf_opmclk_us = 0;
static uint32_t s_tab5_perf_midi_us = 0;
static uint32_t s_tab5_perf_post_us = 0;

extern void m68k_tab5_opcode_profile_set(int enabled);

void WinX68k_PerfSetSample(int enabled)
{
    s_tab5_perf_sample = enabled ? 1 : 0;
    DSound_PerfSetSample(enabled);
    WinDraw_PerfSetSample(enabled);
    /* Build 5.21: the hot-op set is frozen from Build 5.20. Keep the profiler
     * code available, but disable per-instruction counting during PERF samples
     * so cpu= is clean for the IRAM experiment. */
    m68k_tab5_opcode_profile_set(0);
}

void WinX68k_PerfGetLast(uint32_t *frame_us,
                         uint32_t *cpu_us,
                         uint32_t *compose_us,
                         uint32_t *finalize_us)
{
    if (frame_us) *frame_us = s_tab5_perf_frame_us;
    if (cpu_us) *cpu_us = s_tab5_perf_cpu_us;
    if (compose_us) *compose_us = s_tab5_perf_compose_us;
    if (finalize_us) *finalize_us = s_tab5_perf_finalize_us;
}

void WinX68k_PerfGetDetail(uint32_t *timer_us,
                           uint32_t *dma_us,
                           uint32_t *line_us,
                           uint32_t *audio_timer_us,
                           uint32_t *input_us,
                           uint32_t *soundmix_us,
                           uint32_t *fdd_us)
{
    if (timer_us) *timer_us = s_tab5_perf_timer_us;
    if (dma_us) *dma_us = s_tab5_perf_dma_us;
    if (line_us) *line_us = s_tab5_perf_line_us;
    if (audio_timer_us) *audio_timer_us = s_tab5_perf_audio_timer_us;
    if (input_us) *input_us = s_tab5_perf_input_us;
    if (soundmix_us) *soundmix_us = s_tab5_perf_soundmix_us;
    if (fdd_us) *fdd_us = s_tab5_perf_fdd_us;
}

void WinX68k_PerfGetDetail543(uint32_t *mfp_us, uint32_t *rtc_us,
                              uint32_t *edge_us, uint32_t *sched_us,
                              uint32_t *adclk_us, uint32_t *opmclk_us,
                              uint32_t *midi_us, uint32_t *post_us)
{
    if (mfp_us) *mfp_us = s_tab5_perf_mfp_us;
    if (rtc_us) *rtc_us = s_tab5_perf_rtc_us;
    if (edge_us) *edge_us = s_tab5_perf_edge_us;
    if (sched_us) *sched_us = s_tab5_perf_sched_us;
    if (adclk_us) *adclk_us = s_tab5_perf_adclk_us;
    if (opmclk_us) *opmclk_us = s_tab5_perf_opmclk_us;
    if (midi_us) *midi_us = s_tab5_perf_midi_us;
    if (post_us) *post_us = s_tab5_perf_post_us;
}

void WinX68k_VideoPerfGetLast(uint32_t *grp_us, uint32_t *text_us, uint32_t *bg_us,
                              uint32_t *blend_us, uint32_t *clear_us,
                              uint32_t *dirty_lines, uint32_t *grp_calls,
                              uint32_t *text_calls, uint32_t *bg_calls,
                              uint32_t *blend_calls)
{
    WinDraw_PerfGetLast(grp_us, text_us, bg_us, blend_us, clear_us,
                        dirty_lines, grp_calls, text_calls, bg_calls, blend_calls);
}

int WinX68k_AudioReadFrames(int16_t *dst, int max_frames)
{
    return DSound_ReadFrames(dst, max_frames);
}

/* Build 5.98g12: final PCM extraction/mixing is a CPU0 host service. */
int WinX68k_AudioHostFramesAvail(void)
{
    return DSound_HostFramesAvail();
}

int WinX68k_AudioHostReadFrames(int16_t *dst, int max_frames)
{
    return DSound_HostReadFrames(dst, max_frames);
}

uint32_t WinX68k_AudioProducedFrames(void)
{
    return DSound_HostProducedFrames();
}

void WinX68k_AudioPerfGetLast(uint32_t *adpcm_us, uint32_t *opm_us, uint32_t *mix_calls, uint32_t *mix_frames)
{
    DSound_PerfGetLast(adpcm_us, opm_us, mix_calls, mix_frames);
}

void WinX68k_AudioAsyncGetStats(uint32_t *qdepth, uint32_t *event_drops, uint32_t *ring_overruns, uint32_t *fm_avail)
{
    DSound_AsyncGetStats(qdepth, event_drops, ring_overruns, fm_avail);
}

/* Build 5.62: CPU1 always advances the complete X68000 time axis, while
 * host pixel generation is an optional deadline-controlled service.  This
 * flag is set once per guest frame by the Tab5 budget manager. */
static volatile int s_tab5_host_render_enabled = 1;

void WinX68k_SetHostRenderEnabled(int enabled)
{
    s_tab5_host_render_enabled = enabled ? 1 : 0;
}

int WinX68k_ExecVideoProbeFrame(void)
{
#ifdef HAVE_MUSASHI
    static int key_int_cnt = 0;
    static int mouse_int_cnt = 0;
    int clk_total;
    int clkdiv;
    int total_executed = 0;
    const int perf_sample = s_tab5_perf_sample;
    int64_t perf_frame_start = 0;
    uint64_t perf_cpu_us = 0;
    uint64_t perf_compose_us = 0;
    uint64_t perf_finalize_us = 0;
    uint64_t perf_timer_us = 0;
    uint64_t perf_dma_us = 0;
    uint64_t perf_line_us = 0;
    uint64_t perf_audio_timer_us = 0;
    uint64_t perf_input_us = 0;
    uint64_t perf_soundmix_us = 0;
    uint64_t perf_fdd_us = 0;
    uint64_t perf_mfp_us = 0;
    uint64_t perf_rtc_us = 0;
    uint64_t perf_edge_us = 0;
    uint64_t perf_sched_us = 0;
    uint64_t perf_adclk_us = 0;
    uint64_t perf_opmclk_us = 0;
    uint64_t perf_midi_us = 0;
    uint64_t perf_post_us = 0;

    if (perf_sample)
        perf_frame_start = esp_timer_get_time();

    /* Standalone equivalent of the timing-critical parts of WinX68k_Exec(). */
    FDD_IsReading = 0;

    if (Config.clockmhz <= 0)
        Config.clockmhz = 10;
    if (Config.ram_size <= 0)
        Config.ram_size = 12 * 1024 * 1024;

    if (cpu_readmem24_dword(0xED0008) != (uint32_t)Config.ram_size)
    {
        cpu_writemem24(0xE8E00D, 0x31);
        cpu_writemem24_dword(0xED0008, Config.ram_size);
    }

    clk_total = (CRTC_Regs[0x29] & 0x10) ? VSYNC_HIGH : VSYNC_NORM;
    clk_total = (clk_total * Config.clockmhz) / 10;
    clkdiv = Config.clockmhz;

    if (clk_total <= 0)
        clk_total = VSYNC_NORM;
    if (clkdiv <= 0)
        clkdiv = 10;

    const int total_lines = (VLINE_TOTAL > 0) ? VLINE_TOTAL : 567;

    /* Build 5.64: distribute one frame's CPU clocks across scanlines without
     * the two signed 64-bit multiply/divides previously done for every line.
     * base/rem + a Bresenham-style remainder accumulator is mathematically
     * identical to floor(clk_total*(line+1)/N)-floor(clk_total*line/N). */
    const int line_clk_base = clk_total / total_lines;
    const int line_clk_rem  = clk_total % total_lines;
    int line_clk_error = 0;

    /*
     * Build 5.3: keep PX68K's global ICount in step with the standalone
     * Musashi scheduler.  MFP GetGPIP() derives the horizontal sync level
     * from ICount % HSYNC_CLK.  The earlier standalone loop never changed
     * ICount, so guest code polling MFP GPIP could see HSYNC permanently
     * stuck.  Human68k's text-console scroll path synchronizes before the
     * CRTC raster-copy sequence, which explains a full screen followed by
     * RC=0 and an apparently dead console.
     *
     * Native WinX68k_Exec() adds one frame of clocks to ICount and consumes
     * it as the CPU runs.  This standalone loop already schedules an exact
     * frame, so start at the frame budget and consume the same executed
     * cycles below.
     */
    ICount = clk_total;

#define TAB5_PERF_DRAWLINE() do { \
        if (perf_sample) { \
            uint32_t _t0 = tab5_perf_ccount(); \
            WinDraw_DrawLine(); \
            perf_compose_us += (uint32_t)(tab5_perf_ccount() - _t0); \
        } else { \
            WinDraw_DrawLine(); \
        } \
    } while (0)

    for (int line = 0; line < total_lines; ++line)
    {
        int clk_line = 0;
        vline = (uint32_t)line;

        uint32_t perf_edge_t0 = perf_sample ? tab5_perf_ccount() : 0u;
        /* Beginning of scanline: same MFP edges as native PX68K. */
        MFP_Int(0);

        if ((vline >= CRTC_VSTART) && (vline < CRTC_VEND))
            VLINE = ((vline - CRTC_VSTART) * CRTC_VStep) / 2;
        else
            VLINE = (uint32_t)-1;

        if (!(MFP[MFP_AER] & 0x40) && (vline == CRTC_IntLine))
            MFP_Int(1);

        if (MFP[MFP_AER] & 0x10)
        {
            if (vline == CRTC_VSTART)
                MFP_Int(9);
        }
        else
        {
            if (CRTC_VEND >= total_lines)
            {
                if ((long)vline == (long)(CRTC_VEND - total_lines))
                    MFP_Int(9);
            }
            else if ((long)vline == (long)(total_lines - 1))
            {
                MFP_Int(9);
            }
        }

        int remaining = line_clk_base;
        line_clk_error += line_clk_rem;
        if (line_clk_error >= total_lines)
        {
            line_clk_error -= total_lines;
            ++remaining;
        }
        if (perf_sample)
            perf_edge_us += (uint32_t)(tab5_perf_ccount() - perf_edge_t0);

        while (remaining > 0)
        {
            const uint32_t perf_iter_t0 = perf_sample ? tab5_perf_ccount() : 0u;
            uint32_t perf_cpu_cc = 0u, perf_mfp_cc = 0u, perf_rtc_cc = 0u, perf_dma_cc = 0u;
            int request = (remaining > CLOCK_SLICE) ? CLOCK_SLICE : remaining;
            int executed;

            if (perf_sample)
            {
                uint32_t t0 = tab5_perf_ccount();
                executed = m68k_execute(request);
                perf_cpu_cc = (uint32_t)(tab5_perf_ccount() - t0);
                perf_cpu_us += perf_cpu_cc;
            }
            else
            {
                executed = m68k_execute(request);
            }

            if (executed <= 0)
                executed = request;

            total_executed += executed;
            remaining -= executed;
            if (remaining < 0)
                remaining = 0;

            /* Match native PX68K timing state used by MFP GetGPIP(). */
            ICount -= executed;
            if (ICount < 0)
                ICount = 0;

            /* Build 5.64: the normal X68000 10 MHz configuration maps CPU
             * clocks 1:1 onto PX68K's 10 MHz peripheral timebase.  For any
             * legal residual ClkUsed (0..9), the old multiply/divide sequence
             * produces exactly usedclk=executed and leaves the residual intact.
             * Keep the generic converter for 16/24 MHz modes and unusual
             * restored state. */
            int usedclk;
            if (__builtin_expect(clkdiv == 10 && (unsigned)ClkUsed < 10u, 1))
            {
                usedclk = executed;
            }
            else
            {
                ClkUsed += executed * 10;
                usedclk = ClkUsed / clkdiv;
                ClkUsed -= usedclk * clkdiv;
            }
            clk_line += usedclk;

            if (perf_sample)
            {
                uint32_t t0 = tab5_perf_ccount();
                MFP_Timer(usedclk);
                perf_mfp_cc = (uint32_t)(tab5_perf_ccount() - t0);
                perf_mfp_us += perf_mfp_cc;

                t0 = tab5_perf_ccount();
                RTC_Timer(usedclk);
                perf_rtc_cc = (uint32_t)(tab5_perf_ccount() - t0);
                perf_rtc_us += perf_rtc_cc;
                perf_timer_us += (uint64_t)perf_mfp_cc + perf_rtc_cc;

                t0 = tab5_perf_ccount();
                DMA_ExecActive012();
                perf_dma_cc = (uint32_t)(tab5_perf_ccount() - t0);
                perf_dma_us += perf_dma_cc;

                const uint32_t iter_cc = (uint32_t)(tab5_perf_ccount() - perf_iter_t0);
                const uint32_t known_cc = perf_cpu_cc + perf_mfp_cc + perf_rtc_cc + perf_dma_cc;
                if (iter_cc > known_cc)
                    perf_sched_us += (uint32_t)(iter_cc - known_cc);
            }
            else
            {
                MFP_Timer(usedclk);
                RTC_Timer(usedclk);
                DMA_ExecActive012();
            }
        }

        if (perf_sample)
        {
            uint32_t t0 = tab5_perf_ccount();
            MIDI_DelayOut((Config.MIDIAutoDelay) ? (Config.BufferSize * 5) : Config.MIDIDelay);
            MFP_TimerA();
            if ((MFP[MFP_AER] & 0x40) && (vline == CRTC_IntLine))
                MFP_Int(1);
            perf_line_us += (uint32_t)(tab5_perf_ccount() - t0);
        }
        else
        {
            MIDI_DelayOut((Config.MIDIAutoDelay) ? (Config.BufferSize * 5) : Config.MIDIDelay);
            MFP_TimerA();
            if ((MFP[MFP_AER] & 0x40) && (vline == CRTC_IntLine))
                MFP_Int(1);
        }

        if (s_tab5_host_render_enabled &&
            (vline >= CRTC_VSTART) && (vline < CRTC_VEND))
        {
            if (CRTC_VStep == 1)
            {
                if (vline & 1U)
                    TAB5_PERF_DRAWLINE();
            }
            else if (CRTC_VStep == 4)
            {
                TAB5_PERF_DRAWLINE();
                VLINE++;
                TAB5_PERF_DRAWLINE();
            }
            else
            {
                TAB5_PERF_DRAWLINE();
            }
        }

        /* Native PX68K device time progression. */
        if (perf_sample)
        {
            uint32_t t0 = tab5_perf_ccount();
            ADPCM_PreUpdate(clk_line);
            uint32_t cc = (uint32_t)(tab5_perf_ccount() - t0);
            perf_adclk_us += cc;
            perf_audio_timer_us += cc;

            t0 = tab5_perf_ccount();
            OPM_Timer(clk_line);
            cc = (uint32_t)(tab5_perf_ccount() - t0);
            perf_opmclk_us += cc;
            perf_audio_timer_us += cc;

            t0 = tab5_perf_ccount();
            MIDI_Timer(clk_line);
            cc = (uint32_t)(tab5_perf_ccount() - t0);
            perf_midi_us += cc;
            perf_audio_timer_us += cc;
        }
        else
        {
            ADPCM_PreUpdate(clk_line);
            OPM_Timer(clk_line);
            MIDI_Timer(clk_line);
        }

        if (perf_sample)
        {
            uint32_t t0 = tab5_perf_ccount();
            if (++key_int_cnt > (total_lines / 4))
            {
                key_int_cnt = 0;
                Keyboard_Int();
            }
            if (++mouse_int_cnt > (total_lines / 8))
            {
                mouse_int_cnt = 0;
                SCC_IntCheck();
            }
            perf_input_us += (uint32_t)(tab5_perf_ccount() - t0);
        }
        else
        {
            if (++key_int_cnt > (total_lines / 4))
            {
                key_int_cnt = 0;
                Keyboard_Int();
            }
            if (++mouse_int_cnt > (total_lines / 8))
            {
                mouse_int_cnt = 0;
                SCC_IntCheck();
            }
        }

        /* Build 5.13: this native PX68K call was the missing standalone
         * audio-production step.  It mixes OPM + ADPCM into dswin's 44.1 kHz
         * stereo PCM ring according to emulated clock progress. */
        if (perf_sample)
        {
            uint32_t t0 = tab5_perf_ccount();
            DSound_Send0(clk_line);
            perf_soundmix_us += (uint32_t)(tab5_perf_ccount() - t0);
        }
        else
        {
            DSound_Send0(clk_line);
        }
    }

    uint32_t perf_post_t0 = perf_sample ? tab5_perf_ccount() : 0u;
    vline = 0;

    if (CRTC_Mode & 2)
    {
        if (CRTC_FastClr)
        {
            CRTC_FastClr--;
            if (!CRTC_FastClr)
                CRTC_Mode &= 0xFD;
        }
        else
        {
            CRTC_FastClr = (CRTC_Regs[0x29] & 0x10) ? 1 : 2;
            TVRAM_SetAllDirty();
            GVRAM_FastClear();
        }
    }
    if (perf_sample)
        perf_post_us += (uint32_t)(tab5_perf_ccount() - perf_post_t0);

    /* Build 5.19: render any PCM still pending at the emulated frame boundary. */
    if (perf_sample)
    {
        uint32_t t0 = tab5_perf_ccount();
        DSound_FlushPending();
        perf_soundmix_us += (uint32_t)(tab5_perf_ccount() - t0);
    }
    else
    {
        DSound_FlushPending();
    }

    if (perf_sample)
    {
        uint32_t t0 = tab5_perf_ccount();
        FDD_SetFDInt();
        perf_fdd_us = (uint32_t)(tab5_perf_ccount() - t0);
    }
    else
    {
        FDD_SetFDInt();
    }

    if (perf_sample)
    {
        uint32_t t0 = tab5_perf_ccount();
        if (s_tab5_host_render_enabled)
            WinDraw_Draw();
        perf_finalize_us = (uint32_t)(tab5_perf_ccount() - t0);

        s_tab5_perf_cpu_us = tab5_perf_cycles_to_us(perf_cpu_us);
        s_tab5_perf_compose_us = tab5_perf_cycles_to_us(perf_compose_us);
        s_tab5_perf_finalize_us = tab5_perf_cycles_to_us(perf_finalize_us);
        s_tab5_perf_timer_us = tab5_perf_cycles_to_us(perf_timer_us);
        s_tab5_perf_dma_us = tab5_perf_cycles_to_us(perf_dma_us);
        s_tab5_perf_line_us = tab5_perf_cycles_to_us(perf_line_us);
        s_tab5_perf_audio_timer_us = tab5_perf_cycles_to_us(perf_audio_timer_us);
        s_tab5_perf_input_us = tab5_perf_cycles_to_us(perf_input_us);
        s_tab5_perf_soundmix_us = tab5_perf_cycles_to_us(perf_soundmix_us);
        s_tab5_perf_fdd_us = tab5_perf_cycles_to_us(perf_fdd_us);
        s_tab5_perf_mfp_us = tab5_perf_cycles_to_us(perf_mfp_us);
        s_tab5_perf_rtc_us = tab5_perf_cycles_to_us(perf_rtc_us);
        s_tab5_perf_edge_us = tab5_perf_cycles_to_us(perf_edge_us);
        s_tab5_perf_sched_us = tab5_perf_cycles_to_us(perf_sched_us);
        s_tab5_perf_adclk_us = tab5_perf_cycles_to_us(perf_adclk_us);
        s_tab5_perf_opmclk_us = tab5_perf_cycles_to_us(perf_opmclk_us);
        s_tab5_perf_midi_us = tab5_perf_cycles_to_us(perf_midi_us);
        s_tab5_perf_post_us = tab5_perf_cycles_to_us(perf_post_us);
        s_tab5_perf_frame_us = (uint32_t)(esp_timer_get_time() - perf_frame_start);
    }
    else if (s_tab5_host_render_enabled)
    {
        WinDraw_Draw();
    }

#undef TAB5_PERF_DRAWLINE

    return total_executed;
#else
    return 0;
#endif
}

#endif /* ESP_PLATFORM */
/* BUILD2_TAB5_VIDEO_END */
#ifdef ESP_PLATFORM
int WinX68k_MountFloppy(int drive, const char *path)
{
    FILE *fp;

    if (drive < 0 || drive > 1 || !path || !path[0])
        return 0;

    const int flash_human = !strcmp(path, ":FLASH:HUMAN302.XDF");

    /* Normal removable media is verified through VFS.  Build 5.95a's
     * Human68k Quick Boot is linked into application flash, so it has no
     * host filesystem path to fopen(). */
    if (!flash_human)
    {
        fp = fopen(path, "rb");
        if (!fp)
            return 0;
        fclose(fp);
    }

    FDD_EjectFD(drive);

    snprintf(Config.FDDImage[drive],
             sizeof(Config.FDDImage[drive]),
             "%s",
             path);

    /*
     * This is the same native PX68K path used by
     * libretro disk-control insertion.
     */
    FDD_SetFD(drive, Config.FDDImage[drive], flash_human ? 1 : 0);

    /*
     * FDD_SetFD() itself returns void.
     * The previous Build 3 helper therefore reported success even
     * when the XDF backend rejected the image.
     */
    const int ready_now = FDD_IsReady(drive);

    /*
     * PX68K intentionally applies a short insertion delay:
     * FDD_SetFD() sets SetDelay=3 and FDD_SetFDInt()
     * counts it down once per emulated frame.
     *
     * Therefore ready==0 immediately after insertion is not
     * a mount failure.
     */
    printf("PX68K_FDD: mount requested drive=%d ready_now=%d path=%s%s\n",
           drive,
           ready_now,
           Config.FDDImage[drive],
           flash_human ? " [FLASH-RO]" : "");

    return 1;
}


int WinX68k_EjectFloppy(int drive)
{
    if (drive < 0 || drive > 1)
        return 0;

    FDD_EjectFD(drive);
    Config.FDDImage[drive][0] = '\0';

    printf("PX68K_FDD: eject requested drive=%d ready_now=%d\n",
           drive,
           FDD_IsReady(drive));
    return 1;
}

int WinX68k_SetFloppyReadOnly(int drive, int readonly)
{
    char path[MAX_PATH];

    if (drive < 0 || drive > 1 || !Config.FDDImage[drive][0])
        return 0;

    snprintf(path, sizeof(path), "%s", Config.FDDImage[drive]);

    /*
     * ROnly is also used by XDF_Eject() to decide whether the in-memory
     * image must be flushed.  Therefore do not flip it in-place:
     * enabling WP first ejects while still writable (safe flush), then
     * remounts the same image read-only.  Disabling WP remounts writable.
     */
    FDD_EjectFD(drive);
    snprintf(Config.FDDImage[drive], sizeof(Config.FDDImage[drive]), "%s", path);
    FDD_SetFD(drive, Config.FDDImage[drive], readonly ? 1 : 0);

    printf("PX68K_FDD: write protect remount drive=%d state=%d ready_now=%d path=%s\n",
           drive,
           FDD_IsReadOnly(drive),
           FDD_IsReady(drive),
           Config.FDDImage[drive]);

    return FDD_IsReadOnly(drive) == (readonly ? 1 : 0);
}

int WinX68k_MountSCSIHD(int target, const char *path, int readonly)
{
    if (target < 0 || target > 7 || !path || !path[0])
        return 0;

    const int ok = SCSI_MountImage(target, path, readonly);
    if (ok && PX68K_TAB5_DIAG_VERBOSE)
    {
        printf("PX68K_HDS: attached SCSI%d ro=%d blocks=%lu path=%s\n",
               target, readonly ? 1 : 0,
               (unsigned long)SCSI_ImageBlocks(target), path);
    }
    return ok;
}

int WinX68k_SCSIHDReady(int target)
{
    return SCSI_ImageReady(target);
}

int WinX68k_SCSIHDProbe(int target, uint32_t *hash_out)
{
    return SCSI_ProbeFirstBlock(target, hash_out);
}

int WinX68k_SCSIHDProbeLayout(int target, uint32_t *partition_count)
{
    return SCSI_ProbeLayout(target, partition_count);
}

int WinX68k_SCSIArmDirectBoot(int target)
{
    return SCSI_ArmDirectBoot(target);
}

uint32_t WinX68k_SCSIDebugIOCSCalls(void)
{
    return SCSI_DebugIOCSCalls();
}

uint32_t WinX68k_SCSIDebugReads(void)
{
    return SCSI_DebugReads();
}

uint32_t WinX68k_SCSIDebugInstallerCalls(void)
{
    return SCSI_DebugInstallerCalls();
}

uint32_t WinX68k_SCSIDebugInitCalls(void)
{
    return SCSI_DebugInitCalls();
}

uint32_t WinX68k_SCSIDebugDriverInstalls(void)
{
    return SCSI_DebugDriverInstalls();
}

uint32_t WinX68k_SCSIDebugPartitionCount(void)
{
    return SCSI_DebugPartitionCount();
}

uint32_t WinX68k_SCSIDebugIOCSVector(void)
{
    return SCSI_DebugIOCSVector();
}

void WinX68k_SCSIInstallIOCSVector(void)
{
    SCSI_InstallIOCSVector();
}

void WinX68k_EjectSCSIHD(int target)
{
    SCSI_UnmountImage(target);
}

#endif
