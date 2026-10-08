#ifdef __GNUC__
#include <unistd.h>
#endif
#include <stdint.h>
#include <string.h>
#include <stdarg.h>

#include "osdepend.h"

#include "emu.h"
#include "emuopts.h"
#include "ioport.h"
#include "render.h"
#include "ui/uimain.h"
#include "uiinput.h"
#include "drivenum.h"
#include "../frontend/mame/mame.h"

#include "libretro.h"
#include "libretro_shared.h"
#include "libretro_core_options.h"
#include "libretro_vfs.h"

/* forward decls / externs / prototypes */

extern void retro_finish();
extern void retro_main_loop();

int POSTNOTIFY     = 5; /* See retro_loop() */
int RLOOP          = 1;
int ENDEXEC        = 0;
int retro_pause    = 0;
int SHIFTON        = -1;
char RPATH[RETRO_PATH_MAX];
bool first_run     = true;
bool audio_ready   = false;
bool retro_load_ok = false;
bool libretro_supports_bitmasks = false;
static bool libretro_supports_option_categories = false;
bool libretro_supports_ff_override = false;
bool libretro_ff_enabled = false;

int fb_width       = 640;
int fb_height      = 480;
int max_width      = 720;
int max_height     = 720;
float retro_aspect = (float)4.0f / (float)3.0f;
float view_aspect  = 1.0f;
float sample_rate  = 48000.0f;
float retro_fps    = 60.0f;
int video_changed  = VIDEO_CHANGED_NONE;
int screen_configured = 0;

static bool draw_this_frame = true;
static char cdi_model[16] = "cdimono1";
static bool cdi_test_plug_enabled = false;

const char *retro_save_directory;
const char *retro_system_directory;
const char *retro_content_directory;

//FIXME: re-add way to handle 16/32 bit
#ifdef M16B
uint16_t videoBuffer[4096*3072];
#define LOG_PIXEL_BYTES 1
#else
unsigned int videoBuffer[4096*3072];
#define LOG_PIXEL_BYTES 2*1
#endif

/* FIXME: re-add way to handle OGL  */
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#include "retroogl.c"
#endif

void extract_basename(char *buf, const char *path, size_t size)
{
   char *ext = NULL;
   const char *base = strrchr(path, '/');

   if (!base)
      base = strrchr(path, '\\');
   if (!base)
      base = path;

   if (*base == '\\' || *base == '/')
      base++;

   strncpy(buf, base, size - 1);
   buf[size - 1] = '\0';

   ext = strrchr(buf, '.');
   if (ext)
      *ext = '\0';
}

void extract_directory(char *buf, const char *path, size_t size)
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

   base = strrchr(buf, '\"');
   if (base)
      strncpy(buf, base + 1, size - 1);
}

retro_log_printf_t log_cb = NULL;
retro_environment_t environ_cb = NULL;
retro_input_state_t input_state_cb = NULL;
retro_input_poll_t input_poll_cb = NULL;
retro_video_refresh_t video_cb = NULL;
retro_audio_sample_batch_t audio_batch_cb = NULL;

void retro_set_input_state(retro_input_state_t cb) { input_state_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { input_poll_cb = cb; }
void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t cb) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }

/* Audio output buffer */
static struct {
   int16_t *data;
   int32_t size;
   int32_t capacity;
} output_audio_buffer = {NULL, 0, 0};

static void ensure_output_audio_buffer_capacity(int32_t capacity)
{
   if (capacity <= output_audio_buffer.capacity)
      return;

   output_audio_buffer.data = (int16_t*)realloc(output_audio_buffer.data, capacity * sizeof(*output_audio_buffer.data));
   output_audio_buffer.capacity = capacity;
   log_cb(RETRO_LOG_DEBUG, "Output audio buffer capacity set to %d\n", capacity);
}

static void init_output_audio_buffer(int32_t capacity)
{
   output_audio_buffer.data = NULL;
   output_audio_buffer.size = 0;
   output_audio_buffer.capacity = 0;
   ensure_output_audio_buffer_capacity(capacity);
}

static void free_output_audio_buffer()
{
   free(output_audio_buffer.data);
   output_audio_buffer.data = NULL;
   output_audio_buffer.size = 0;
   output_audio_buffer.capacity = 0;
}

static void upload_output_audio_buffer()
{
   if (!audio_ready)
   {
      unsigned samples = (sample_rate / retro_fps) * sizeof(*output_audio_buffer.data);

      if (output_audio_buffer.capacity - output_audio_buffer.size < samples)
         ensure_output_audio_buffer_capacity((output_audio_buffer.capacity + samples) * 1.5);

      memset(output_audio_buffer.data + output_audio_buffer.size, 0, samples * sizeof(*output_audio_buffer.data));
      output_audio_buffer.size += samples;
   }
   audio_batch_cb(output_audio_buffer.data, output_audio_buffer.size / 2);
   output_audio_buffer.size = 0;

   audio_ready = false;
}

void retro_audio_queue(const int16_t *data, int32_t samples)
{
   if ((samples < 1) || retro_pause)
      return;

   if (output_audio_buffer.capacity - output_audio_buffer.size < samples)
      ensure_output_audio_buffer_capacity((output_audio_buffer.capacity + samples) * 1.5);

   memcpy(output_audio_buffer.data + output_audio_buffer.size, data, samples * sizeof(*output_audio_buffer.data));
   output_audio_buffer.size += samples;

   audio_ready = true;
}

/* LED interface */
static retro_set_led_state_t led_state_cb = NULL;
static unsigned int retro_led_state[2] = {0};

#define CDD_READ        0x0100
#define CDD_READY       0x0400
#define CDD_DATA        0x1000
#define CDD_SCSI        0x2000

int CDD_status = CDD_READY;

static void retro_led_interface(void)
{
   /* 0: Power
    * 1: CD */

   unsigned int led_state[2] = {0};
   unsigned int l            = 0;

   led_state[0] = (!retro_pause) ? 1 : 0;
   led_state[1] = (CDD_status & CDD_READ) ? 1 : 0;

   for (l = 0; l < sizeof(led_state)/sizeof(led_state[0]); l++)
   {
      if (retro_led_state[l] != led_state[l])
      {
         retro_led_state[l] = led_state[l];
         led_state_cb(l, led_state[l]);
      }
   }

   if (CDD_status & CDD_SCSI)
      CDD_status &= ~(CDD_READ | CDD_DATA);
}

void retro_fastforwarding(bool enabled)
{
   struct retro_fastforwarding_override ff_override;
   bool frontend_ff_enabled = false;

   if (!libretro_supports_ff_override)
      return;

   environ_cb(RETRO_ENVIRONMENT_GET_FASTFORWARDING, &frontend_ff_enabled);
   if (enabled && frontend_ff_enabled)
      return;

   ff_override.ratio          = -1;
   ff_override.fastforward    = enabled;
   ff_override.inhibit_toggle = enabled;
   libretro_ff_enabled        = enabled;

   environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, &ff_override);
}

static const struct retro_controller_description default_controllers[] =
{
   { "RetroPad", RETRO_DEVICE_JOYPAD },
   { "Keyboard", RETRO_DEVICE_KEYBOARD },
   { "None", RETRO_DEVICE_NONE },
   { NULL, 0 }
};

static void retro_set_inputs(void)
{
   const struct retro_controller_info ports[] =
   {
      { default_controllers, sizeof(default_controllers) / sizeof(default_controllers[0]) },
      { NULL, 0 }
   };

   environ_cb(RETRO_ENVIRONMENT_SET_CONTROLLER_INFO, (void*)ports);
}

void retro_set_environment(retro_environment_t cb)
{
   struct retro_led_interface led_interface;
   bool option_categories = false;

   environ_cb = cb;

   libretro_set_core_options(environ_cb, &option_categories);
   libretro_supports_option_categories |= option_categories;

   retro_set_inputs();

   bool support_no_game = true;
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_NO_GAME, &support_no_game);

   if (environ_cb(RETRO_ENVIRONMENT_GET_LED_INTERFACE, &led_interface))
      if (led_interface.set_led_state && !led_state_cb)
         led_state_cb = led_interface.set_led_state;
}

static void update_runtime_variables()
{
   mame_machine_manager *manager = mame_machine_manager::instance();
   if (manager && manager->machine())
   {
      auto const &ports = manager->machine()->ioport().ports();
      auto const service_port = ports.find(":SERVICE");
      if (service_port != ports.end())
      {
         if (ioport_field *test_plug = service_port->second->field(0x01))
            test_plug->set_value(cdi_test_plug_enabled ? 1 : 0);
      }
   }
}

static void check_variables(void)
{
   struct retro_variable var = {0};

   var.key   = CORE_NAME "_cdi_model";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "cdimono1n"))
         snprintf(cdi_model, sizeof(cdi_model), "%s", "cdimono1n");
      else if (!strcmp(var.value, "cdimono1"))
         snprintf(cdi_model, sizeof(cdi_model), "%s", "cdimono1");
   }

   var.key   = CORE_NAME "_test_plug";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
      cdi_test_plug_enabled = !strcmp(var.value, "enabled");

   var.key   = CORE_NAME "_joystick_deadzone";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      strcpy(joystick_deadzone, var.value);
   }

   var.key   = CORE_NAME "_joystick_saturation";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      strcpy(joystick_saturation, var.value);
   }

   var.key   = CORE_NAME "_joystick_threshold";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      strcpy(joystick_threshold, var.value);
   }

   var.key   = CORE_NAME "_mouse_enable";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         mouse_enable = false;
      if (!strcmp(var.value, "enabled"))
         mouse_enable = true;
   }

   var.key   = CORE_NAME "_rotation_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "libretro"))
         rotation_mode = ROTATION_MODE_LIBRETRO;
      else if (!strcmp(var.value, "internal"))
         rotation_mode = ROTATION_MODE_INTERNAL;
      else if (!strcmp(var.value, "tate-rol"))
         rotation_mode = ROTATION_MODE_TATE_ROL;
      else if (!strcmp(var.value, "tate-ror"))
         rotation_mode = ROTATION_MODE_TATE_ROR;
      else
         rotation_mode = ROTATION_MODE_NONE;
   }

   var.key   = CORE_NAME "_alternate_renderer";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      char alternate_renderer_prev = alternate_renderer;

      if (!strcmp(var.value, "disabled"))
         alternate_renderer = 0;
      if (!strcmp(var.value, "enabled"))
         alternate_renderer = 1;
      if (!strcmp(var.value, "cropped"))
         alternate_renderer = 2;

      if (alternate_renderer != alternate_renderer_prev)
         video_changed = VIDEO_CHANGED_GEOMETRY;
   }

   var.key   = CORE_NAME "_altres";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (alternate_renderer)
      {
         char *pch;
         char str[100];
         int width          = 640;
         int height         = 480;

         snprintf(str, sizeof(str), "%s", var.value);

         pch = strtok(str, "x");
         if (pch)
            width = strtoul(pch, NULL, 0);

         pch = strtok(NULL, "x");
         if (pch)
            height = strtoul(pch, NULL, 0);

         if ((width != fb_width || height != fb_height) || video_changed)
         {
            fb_width      = width;
            fb_height     = height;
            video_changed = VIDEO_CHANGED_GEOMETRY;

            /* Respect source aspect ratio */
            if (alternate_renderer == 2)
            {
               if (width > height)
                  fb_width   = fb_height * retro_aspect;
               else
                  fb_height  = fb_width / retro_aspect;
            }

            /* Must use SET_SYSTEM_AV_INFO when max is not enough */
            if (fb_width > max_width || fb_height > max_height)
            {
               max_width     = fb_width;
               max_height    = fb_height;
               video_changed = VIDEO_CHANGED_AV_INFO;
            }
         }
      }
   }

   var.key   = CORE_NAME "_thread_mode";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "enabled"))
         thread_mode = 1;
      else
         thread_mode = 0;
   }

   var.key   = CORE_NAME "_throttle";
   var.value = NULL;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) && var.value)
   {
      if (!strcmp(var.value, "disabled"))
         throttle_enable = false;
      if (!strcmp(var.value, "enabled"))
         throttle_enable = true;
   }

}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));

   info->library_name     = "Theseus-CDi";
   info->library_version  = build_version;
   info->valid_extensions = "chd|cue|iso|bin";
   info->need_fullpath    = true;
   info->block_extract    = true;
}

void update_geometry(void)
{
   struct retro_system_av_info av_info;
   av_info.geometry.base_width   = fb_width;
   av_info.geometry.base_height  = fb_height;
   av_info.geometry.aspect_ratio = retro_aspect;
   environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
   video_changed = VIDEO_CHANGED_NONE;
}

void update_av_info(void)
{
   struct retro_system_av_info av_info;
   retro_get_system_av_info(&av_info);
   environ_cb(RETRO_ENVIRONMENT_SET_SYSTEM_AV_INFO, &av_info);
   video_changed = VIDEO_CHANGED_NONE;
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   info->geometry.base_width   = fb_width;
   info->geometry.base_height  = fb_height;
   info->geometry.aspect_ratio = retro_aspect;

   info->geometry.max_width    = max_width;
   info->geometry.max_height   = max_height;

   info->timing.fps            = retro_fps;
   info->timing.sample_rate    = sample_rate;
}

static void fallback_log(enum retro_log_level level, const char *fmt, ...)
{
   (void)level;
   va_list va;
   va_start(va, fmt);
   vfprintf(stderr, fmt, va);
   va_end(va);
}

void retro_init(void)
{
   const char *system_dir  = NULL;
   const char *content_dir = NULL;
   const char *save_dir    = NULL;

//FIXME: re-add way to handle 16/32 bit
#ifdef M16B
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;
#else
   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
#endif

   struct retro_log_callback log;
   log_cb = fallback_log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir) && system_dir)
   {
      /* if defined, use the system directory */
      retro_system_directory = system_dir;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_CONTENT_DIRECTORY, &content_dir) && content_dir)
   {
      /* if defined, use the content directory */
      retro_content_directory = content_dir;
   }

   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) && save_dir)
   {
      /* If save directory is defined use it,
       * otherwise use system directory. */
      retro_save_directory = *save_dir ? save_dir : retro_system_directory;
   }
   else
   {
      /* make retro_save_directory the same,
       * in case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY
       * is not implemented by the frontend. */
      retro_save_directory = retro_system_directory;
   }

   if (!environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
   {
      log_cb(RETRO_LOG_ERROR, "pixel format not supported\n");
      exit(0);
   }

   #define input_descriptor_macro(c) \
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,   "Joy Left" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT,  "Joy Right" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,     "Joy Up" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,   "Joy Down" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,      "A" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,      "B" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,      "X" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,      "Y" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,      "L" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,      "R" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT, "Select" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,  "Start" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L2,     "L2" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L3,     "L3" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R2,     "R2" },\
      { c, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R3,     "R3" },\
      { c, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_X, "Left Stick X" },\
      { c, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT, RETRO_DEVICE_ID_ANALOG_Y, "Left Stick Y" },\
      { c, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_X, "Right Stick X" },\
      { c, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT, RETRO_DEVICE_ID_ANALOG_Y, "Right Stick Y" },

   struct retro_input_descriptor input_descriptors[] =
   {
      input_descriptor_macro(0)
      input_descriptor_macro(1)
      input_descriptor_macro(2)
      input_descriptor_macro(3)
      input_descriptor_macro(4)
      input_descriptor_macro(5)
      input_descriptor_macro(6)
      input_descriptor_macro(7)
      { 0 },
   };
   #undef input_descriptor_macro
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);

   if (environ_cb(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;

   if (environ_cb(RETRO_ENVIRONMENT_SET_FASTFORWARDING_OVERRIDE, NULL))
      libretro_supports_ff_override = true;

   bool achievements = true;
   environ_cb(RETRO_ENVIRONMENT_SET_SUPPORT_ACHIEVEMENTS, &achievements);

   static struct retro_keyboard_callback keyboard_callback = {retro_keyboard_event};
   environ_cb(RETRO_ENVIRONMENT_SET_KEYBOARD_CALLBACK, &keyboard_callback);

   memset(videoBuffer, 0, sizeof(videoBuffer));
   init_output_audio_buffer(2048);

   retro_pause = 0;

   libretro_vfs_init();

   log_cb(RETRO_LOG_INFO, "---------------------------\n");
   log_cb(RETRO_LOG_INFO, "Theseus-CDi %s\n", build_version);
   log_cb(RETRO_LOG_INFO, "Forked from lr-mame. CD-i\n");
   log_cb(RETRO_LOG_INFO, "driver backport by OM3GAZX.\n");
   log_cb(RETRO_LOG_INFO, "---------------------------\n");
}

void retro_deinit(void)
{
   free_output_audio_buffer();
   if (retro_load_ok)
      retro_finish();
}

void retro_reset(void)
{
   mame_reset = 1;
}

void retro_run(void)
{
   bool updated = false;

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      check_variables();
      update_runtime_variables();
   }

   if (!retro_pause)
      retro_main_loop();
   RLOOP = 1;

   /* Automatic loading fast-forward */
   /* LED interface */
   if (led_state_cb)
      retro_led_interface();

   if (first_run)
   {
      /* Skip drawing the first frame due to a gray border */
      first_run       = false;
      draw_this_frame = false;
   }

//FIXME: re-add way to handle OGL
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
   do_glflush();
#else
   if (draw_this_frame)
      video_cb(videoBuffer, fb_width, fb_height, fb_width << LOG_PIXEL_BYTES);
   else
      video_cb(NULL, fb_width, fb_height, fb_width << LOG_PIXEL_BYTES);
#endif
   upload_output_audio_buffer();

   if (video_changed == VIDEO_CHANGED_AV_INFO)
      update_av_info();
   else if (video_changed == VIDEO_CHANGED_GEOMETRY)
      update_geometry();
}

bool retro_load_game(const struct retro_game_info *info)
{
   retro_load_ok = false;

   check_variables();

//FIXME: re-add way to handle OGL
#if defined(HAVE_OPENGL) || defined(HAVE_OPENGLES)
#if defined(HAVE_OPENGLES)
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGLES2;
#else
   hw_render.context_type = RETRO_HW_CONTEXT_OPENGL;
#endif
   hw_render.context_reset = context_reset;
   hw_render.context_destroy = context_destroy;
   /*
   hw_render.depth = true;
   hw_render.stencil = true;
   hw_render.bottom_left_origin = true;
   */
   if (!environ_cb(RETRO_ENVIRONMENT_SET_HW_RENDER, &hw_render))
      return false;
#endif

   g_rom_dir[0] = '\0';
   RPATH[0]     = '\0';

   if (info)
   {
      if (!info->path || !info->path[0])
      {
         log_cb(RETRO_LOG_ERROR, "%s: content path is empty.\n", __func__);
         return false;
      }

      if (strchr(info->path, '"'))
      {
         log_cb(RETRO_LOG_ERROR, "%s: content path contains an unsupported quote character.\n", __func__);
         return false;
      }

      extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));
      int const command_length = snprintf(RPATH, sizeof(RPATH), "%s -cdrom \"%s\"", cdi_model, info->path);
      if ((command_length < 0) || (size_t(command_length) >= sizeof(RPATH)))
      {
         RPATH[0] = '\0';
         log_cb(RETRO_LOG_ERROR, "%s: content path is too long for the CD-i launch command.\n", __func__);
         return false;
      }
   }

   int res = mmain2(1, RPATH);

   /* Force success with empty content */
   if (!RPATH[0])
      res = 0;

   if (res != 0)
   {
      /* Must wait a bit for failure to finish properly */
      log_cb(RETRO_LOG_DEBUG, "%s osd_sleep\n", __func__);
      osd_sleep(osd_ticks_per_second());
      log_cb(RETRO_LOG_DEBUG, "%s osd_sleep done\n", __func__);
      return false;
   }

   retro_load_ok = true;
   update_runtime_variables();

   return true;
}

void retro_unload_game(void)
{
   if (     mame_machine_manager::instance() != NULL
         && mame_machine_manager::instance()->machine() != NULL
         && mame_machine_manager::instance()->machine()->options().autosave()
         && (mame_machine_manager::instance()->machine()->system().flags & MACHINE_SUPPORTS_SAVE) != 0)
	  mame_machine_manager::instance()->machine()->immediate_save("auto");

   if (retro_pause == 0)
      retro_pause = -1;
}

size_t retro_serialize_size(void)
{
   if (     mame_machine_manager::instance() != NULL
	      && mame_machine_manager::instance()->machine() != NULL
	      && ram_state::get_size(mame_machine_manager::instance()->machine()->save()) > 0)
      return ram_state::get_size(mame_machine_manager::instance()->machine()->save());

   return 0;
}
bool retro_serialize(void *data, size_t size)
{
   save_error error = STATERR_NOT_FOUND;
   if (     mame_machine_manager::instance() != NULL
	      && mame_machine_manager::instance()->machine() != NULL
	      && ram_state::get_size(mame_machine_manager::instance()->machine()->save()) > 0)
      error = mame_machine_manager::instance()->machine()->save().write_buffer((u8*)data, size);

   if (error != STATERR_NONE)
      log_cb(RETRO_LOG_ERROR, "State save error %d.\n", error);
   return (error == STATERR_NONE);
}
bool retro_unserialize(const void *data, size_t size)
{
   save_error error = STATERR_NOT_FOUND;
   if (     mame_machine_manager::instance() != NULL
         && mame_machine_manager::instance()->machine() != NULL
         &&	ram_state::get_size(mame_machine_manager::instance()->machine()->save()) > 0)
      error = mame_machine_manager::instance()->machine()->save().read_buffer((u8*)data, size);

   if (error != STATERR_NONE)
      log_cb(RETRO_LOG_ERROR, "State load error %d.\n", error);
   return (error == STATERR_NONE);
}

unsigned retro_get_region (void) { return RETRO_REGION_NTSC; }

void *find_mame_bank_base(offs_t start, address_space &space)
{
   for (auto &bank : mame_machine_manager::instance()->machine()->memory().banks())
      // if ( bank.second->addrstart() == start)
         return bank.second->base();
   return NULL;
}

void *retro_get_memory_data(unsigned type)
{
   void *best_match1 = NULL;
   void *best_match2 = NULL;
   void *best_match3 = NULL;
   int space_index   = 0;

   /* Eventually the RA cheat system can be updated to accommodate multiple memory
    * locations, but for now this does a pretty good job for MAME since most of the machines
    * have a single primary RAM segment that is marked read/write as AMH_RAM.
    *
    * This will find a best match based on certain qualities of the address_map_entry objects.
    */
   if (     type == RETRO_MEMORY_SYSTEM_RAM
         && mame_machine_manager::instance() != NULL
         && mame_machine_manager::instance()->machine() != NULL)
   {
      memory_interface_enumerator iter(mame_machine_manager::instance()->machine()->root_device());
      for (device_memory_interface &memory : iter)
      {
         for (space_index = 0; space_index < memory.num_spaces(); space_index++)
         {
            if (memory.has_space(space_index))
            {
               auto &space = memory.space(space_index);
               for (address_map_entry &entry : space.map()->m_entrylist)
               {
                  if (entry.m_read.m_type == AMH_RAM)
                  {
                     if (entry.m_write.m_type == AMH_RAM)
                     {
                        if (entry.m_share == NULL)
                           best_match1 = find_mame_bank_base(entry.m_addrstart, space);
                        else
                           best_match2 = find_mame_bank_base(entry.m_addrstart, space);
                     }
                     else
                        best_match3 = find_mame_bank_base(entry.m_addrstart, space);
                  }
               }
            }
         }
      }
   }
   return (best_match1 != NULL ? best_match1 : (best_match2 != NULL) ? best_match2 : best_match3);
}

size_t retro_get_memory_size(unsigned type)
{
   size_t best_match1 = 0;
   size_t best_match2 = 0;
   size_t best_match3 = 0;
   int space_index    = 0;

   if (     type == RETRO_MEMORY_SYSTEM_RAM
         && mame_machine_manager::instance() != NULL
         && mame_machine_manager::instance()->machine() != NULL)
   {
      memory_interface_enumerator iter(mame_machine_manager::instance()->machine()->root_device());
      for (device_memory_interface &memory : iter)
      {
         for (space_index = 0; space_index < memory.num_spaces(); space_index++)
         {
            if (memory.has_space(space_index))
            {
               auto &space = memory.space(space_index);
               for (address_map_entry &entry : space.map()->m_entrylist)
               {
                  if (entry.m_read.m_type == AMH_RAM)
                  {
                     if (entry.m_write.m_type == AMH_RAM)
                     {
                        if (entry.m_share == NULL)
                           best_match1 = entry.m_addrend - entry.m_addrstart + 1;
                        else
                           best_match2 = entry.m_addrend - entry.m_addrstart + 1;
                     }
                     else
                        best_match3 = entry.m_addrend - entry.m_addrstart + 1;
                  }
               }
            }
         }
      }
   }

   return (best_match1 != 0 ? best_match1 : (best_match2 != 0) ? best_match2 : best_match3);
}

bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info) { return false; }
void retro_cheat_reset(void) {}
void retro_cheat_set(unsigned unused, bool unused1, const char* unused2) {}
void retro_set_controller_port_device(unsigned in_port, unsigned device) {}

void *retro_get_fb_ptr(void)
{
   return videoBuffer;
}

void retro_frame_draw_enable(bool enable)
{
   draw_this_frame = enable;
}
