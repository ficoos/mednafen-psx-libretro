#include "mednafen/mednafen-types.h"
#include "mednafen/mednafen.h"
#include "mednafen/git.h"
#include "mednafen/general.h"
#include <iostream>
#include "libretro.h"

static MDFNGI *game;
static retro_video_refresh_t video_cb;
static retro_audio_sample_t audio_cb;
static retro_audio_sample_batch_t audio_batch_cb;
static retro_environment_t environ_cb;
static retro_input_poll_t input_poll_cb;
static retro_input_state_t input_state_cb;

static MDFN_Surface *surf;
static bool rgb32;

static uint16_t conv_buf[680 * 480] __attribute__((aligned(16)));
static uint32_t mednafen_buf[680 * 480] __attribute__((aligned(16)));
static bool failed_init;

void retro_init()
{
   MDFN_PixelFormat pix_fmt(MDFN_COLORSPACE_RGB, 16, 8, 0, 24);
   surf = new MDFN_Surface(mednafen_buf, 680, 480, 680, pix_fmt);

   std::vector<MDFNGI*> ext;
   MDFNI_InitializeModules(ext);

   const char *dir = NULL;

   std::vector<MDFNSetting> settings;

   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      std::string eu_path = dir;
      eu_path += "/scph5502.bin";

      std::string jp_path = dir;
      jp_path += "/scph5500.bin";

      std::string na_path = dir;
      na_path += "/scph5501.bin";

      MDFNSetting jp_setting = { "psx.bios_jp", MDFNSF_EMU_STATE, "SCPH-5500 BIOS", NULL, MDFNST_STRING, jp_path.c_str() };
      MDFNSetting eu_setting = { "psx.bios_eu", MDFNSF_EMU_STATE, "SCPH-5502 BIOS", NULL, MDFNST_STRING, eu_path.c_str() };
      MDFNSetting na_setting = { "psx.bios_na", MDFNSF_EMU_STATE, "SCPH-5501 BIOS", NULL, MDFNST_STRING, na_path.c_str() };
      MDFNSetting filesys = { "filesys.path_sav", MDFNSF_NOFLAGS, "Memcards", NULL, MDFNST_STRING, "." };
      settings.push_back(jp_setting);
      settings.push_back(eu_setting);
      settings.push_back(na_setting);
      settings.push_back(filesys);
      MDFNI_Initialize(dir, settings);
   }
   else
   {
      fprintf(stderr, "System directory is not defined. Cannot continue ...\n");
      failed_init = true;
   }

   // Hints that we need a fairly powerful system to run this.
   unsigned level = 3;
   environ_cb(RETRO_ENVIRONMENT_SET_PERFORMANCE_LEVEL, &level);
}

void retro_deinit()
{
   delete surf;
   surf = NULL;
}

void retro_reset()
{
   MDFNI_Reset();
}

bool retro_load_game_special(unsigned, const struct retro_game_info *, size_t)
{
   return false;
}

bool retro_load_game(const struct retro_game_info *info)
{
   if (failed_init)
      return false;

   enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;
   if (environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
      rgb32 = true;

   game = MDFNI_LoadGame("psx", info->path);
   return game;
}

void retro_unload_game()
{
   MDFNI_CloseGame();
}

#ifndef __SSE2__
#error "SSE2 required."
#endif

#include <emmintrin.h>

// PSX core should be able to output ARGB1555 directly,
// so we can avoid this conversion step.
// Done in SSE2 here because any system that can run this
// core to begin with will be at least that powerful (as of writing).
static inline void convert_surface()
{
   const uint32_t *pix = surf->pixels;
   for (unsigned i = 0; i < 680 * 480; i += 8)
   {
      __m128i pix0 = _mm_load_si128((const __m128i*)(pix + i + 0));
      __m128i pix1 = _mm_load_si128((const __m128i*)(pix + i + 4));

      __m128i red0   = _mm_and_si128(pix0, _mm_set1_epi32(0xf80000));
      __m128i green0 = _mm_and_si128(pix0, _mm_set1_epi32(0x00f800));
      __m128i blue0  = _mm_and_si128(pix0, _mm_set1_epi32(0x0000f8));
      __m128i red1   = _mm_and_si128(pix1, _mm_set1_epi32(0xf80000));
      __m128i green1 = _mm_and_si128(pix1, _mm_set1_epi32(0x00f800));
      __m128i blue1  = _mm_and_si128(pix1, _mm_set1_epi32(0x0000f8));

      red0   = _mm_srli_epi32(red0,   19 - 10);
      green0 = _mm_srli_epi32(green0, 11 -  5);
      blue0  = _mm_srli_epi32(blue0,   3 -  0);

      red1   = _mm_srli_epi32(red1,   19 - 10);
      green1 = _mm_srli_epi32(green1, 11 -  5);
      blue1  = _mm_srli_epi32(blue1,   3 -  0);

      __m128i res0 = _mm_or_si128(_mm_or_si128(red0, green0), blue0);
      __m128i res1 = _mm_or_si128(_mm_or_si128(red1, green1), blue1);

      _mm_store_si128((__m128i*)(conv_buf + i), _mm_packs_epi32(res0, res1));
   }
}

static unsigned retro_devices[2];

// Hardcoded for PSX. No reason to parse lots of structures ...
// See mednafen/psx/input/gamepad.cpp
static void update_input()
{
   union
   {
      uint32_t u32[2][1 + 8];
      uint8_t u8[2][2 * sizeof(uint16_t) + 8 * sizeof(uint32_t)];
   } static buf;

   uint16_t input_buf[2] = {0};
   static unsigned map[] = {
      RETRO_DEVICE_ID_JOYPAD_SELECT,
      RETRO_DEVICE_ID_JOYPAD_L3,
      RETRO_DEVICE_ID_JOYPAD_R3,
      RETRO_DEVICE_ID_JOYPAD_START,
      RETRO_DEVICE_ID_JOYPAD_UP,
      RETRO_DEVICE_ID_JOYPAD_RIGHT,
      RETRO_DEVICE_ID_JOYPAD_DOWN,
      RETRO_DEVICE_ID_JOYPAD_LEFT,
      RETRO_DEVICE_ID_JOYPAD_L2,
      RETRO_DEVICE_ID_JOYPAD_R2,
      RETRO_DEVICE_ID_JOYPAD_L,
      RETRO_DEVICE_ID_JOYPAD_R,
      RETRO_DEVICE_ID_JOYPAD_X,
      RETRO_DEVICE_ID_JOYPAD_A,
      RETRO_DEVICE_ID_JOYPAD_B,
      RETRO_DEVICE_ID_JOYPAD_Y,
   };

   for (unsigned j = 0; j < 2; j++)
   {
      for (unsigned i = 0; i < 16; i++)
         input_buf[j] |= input_state_cb(j, RETRO_DEVICE_JOYPAD, 0, map[i]) ? (1 << i) : 0;
   }

   // Buttons.
   buf.u8[0][0] = (input_buf[0] >> 0) & 0xff;
   buf.u8[0][1] = (input_buf[0] >> 8) & 0xff;
   buf.u8[1][0] = (input_buf[1] >> 0) & 0xff;
   buf.u8[1][1] = (input_buf[1] >> 8) & 0xff;

   // Analogs
   for (unsigned j = 0; j < 2; j++)
   {
      int analog_left_x = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
            RETRO_DEVICE_ID_ANALOG_X);

      int analog_left_y = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
            RETRO_DEVICE_ID_ANALOG_Y);

      int analog_right_x = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
            RETRO_DEVICE_ID_ANALOG_X);

      int analog_right_y = input_state_cb(j, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
            RETRO_DEVICE_ID_ANALOG_Y);

      uint32_t r_right = analog_right_x > 0 ?  analog_right_x : 0;
      uint32_t r_left  = analog_right_x < 0 ? -analog_right_x : 0;
      uint32_t r_down  = analog_right_y > 0 ?  analog_right_y : 0;
      uint32_t r_up    = analog_right_y < 0 ? -analog_right_y : 0;

      uint32_t l_right = analog_left_x > 0 ?  analog_left_x : 0;
      uint32_t l_left  = analog_left_x < 0 ? -analog_left_x : 0;
      uint32_t l_down  = analog_left_y > 0 ?  analog_left_y : 0;
      uint32_t l_up    = analog_left_y < 0 ? -analog_left_y : 0;

      buf.u32[j][1] = r_right;
      buf.u32[j][2] = r_left;
      buf.u32[j][3] = r_down;
      buf.u32[j][4] = r_up;

      buf.u32[j][5] = l_right;
      buf.u32[j][6] = l_left;
      buf.u32[j][7] = l_down;
      buf.u32[j][8] = l_up;
   }

   for (int j = 0; j < 2; j++) {
        switch(retro_devices[j]) {
            case RETRO_DEVICE_DUAL_ANALOG:
               game->SetInput(j, "dualanalog", &buf.u8[j]);
               break;
            default:
               game->SetInput(j, "gamepad", &buf.u8[j]);
               break;
        }
   }
}

void retro_run()
{
   input_poll_cb();

   update_input();

   static int16_t sound_buf[0x10000];
   static MDFN_Rect rects[480];
   rects[0].w = ~0;

   EmulateSpecStruct spec = {0};
   spec.surface = surf;
   spec.SoundRate = 44100;
   spec.SoundBuf = sound_buf;
   spec.LineWidths = rects;
   spec.SoundBufMaxSize = sizeof(sound_buf) / 2;
   spec.SoundVolume = 1.0;
   spec.soundmultiplier = 1.0;

   MDFNI_Emulate(&spec);

   unsigned width = rects[0].w;
   unsigned height = spec.DisplayRect.h;

   if (rgb32)
   {
      // FIXME: Avoid black borders. Cannot see how DisplayRect exposes this.
      const uint32_t *ptr = mednafen_buf;
      if (width == 340)
      {
         ptr += 10;
         width = 320;
      }
      else if (width == 680)
      {
         ptr += 20;
         width = 640;
      }

      video_cb(ptr, width, height, 680 << 2);
   }
   else
   {
      convert_surface();

      const uint16_t *ptr = conv_buf;
      if (width == 340)
      {
         ptr += 10;
         width = 320;
      }
      else if (width == 680)
      {
         ptr += 20;
         width = 640;
      }

      video_cb(ptr, width, height, 680 << 1);
   }

   audio_batch_cb(spec.SoundBuf, spec.SoundBufSize);
}

void retro_get_system_info(struct retro_system_info *info)
{
   memset(info, 0, sizeof(*info));
   info->library_name     = "Mednafen PSX";
   info->library_version  = "0.9.22";
   info->need_fullpath    = true;
   info->valid_extensions = "cue|CUE";
}

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   memset(info, 0, sizeof(*info));
   // Just assume NTSC for now. TODO: Verify FPS.
   info->timing.fps            = 59.94;
   info->timing.sample_rate    = 44100;
   info->geometry.base_width   = 320;
   info->geometry.base_height  = 240;
   info->geometry.max_width    = 640;
   info->geometry.max_height   = 480;
   info->geometry.aspect_ratio = 4.0 / 3.0;
}

unsigned retro_get_region(void)
{
   return RETRO_REGION_NTSC;
}

unsigned retro_api_version(void)
{
   return RETRO_API_VERSION;
}

void retro_set_controller_port_device(unsigned in_port, unsigned device)
{
    if (in_port > 1) {
            fprintf(stderr,
                "Only the 2 main ports are supported at the moment");
            return;
    }

    switch (device) {
        // TODO: Add support for other input types
        case RETRO_DEVICE_JOYPAD:
        case RETRO_DEVICE_DUAL_ANALOG:
            fprintf(stderr, "Selected controller type %u", device);
            retro_devices[in_port] = device;
            break;
        default:
            retro_devices[in_port] = RETRO_DEVICE_JOYPAD;
            fprintf(stderr,
                "Unsupported controller device, falling back to gamepad");
    }
}

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
}

void retro_set_audio_sample(retro_audio_sample_t cb)
{
   audio_cb = cb;
}

void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb)
{
   audio_batch_cb = cb;
}

void retro_set_input_poll(retro_input_poll_t cb)
{
   input_poll_cb = cb;
}

void retro_set_input_state(retro_input_state_t cb)
{
   input_state_cb = cb;
}

void retro_set_video_refresh(retro_video_refresh_t cb)
{
   video_cb = cb;
}

size_t retro_serialize_size(void)
{
   return 0;
}

bool retro_serialize(void *, size_t)
{
   return false;
}

bool retro_unserialize(const void *, size_t)
{
   return false;
}

void *retro_get_memory_data(unsigned)
{
   return NULL;
}

size_t retro_get_memory_size(unsigned)
{
   return 0;
}

void retro_cheat_reset(void)
{}

void retro_cheat_set(unsigned, bool, const char *)
{}

