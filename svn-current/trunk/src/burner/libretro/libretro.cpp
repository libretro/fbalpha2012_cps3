#include "libretro.h"
#include "burner.h"

#include <vector>
#include <string>

#define FBA_VERSION "v0.2.97.29" // Sept 16, 2013 (SVN)

#define CORE_OPTION_NAME "fbalpha2012_cps3"

#ifdef WII_VM
#include <unistd.h> // sleep
#include <dirent.h>
#include "wii_vm.h"
#include "wii_progressbar.h"

bool CreateCache = false;
FILE *BurnCacheFile;
char CacheDir[1024];
char ParentName[1024];
unsigned int CacheSize;
#endif

#if defined(_XBOX) || defined(_WIN32)
char slash = '\\';
#else
char slash = '/';
#endif
bool analog_controls_enabled = false;

static void log_dummy(enum retro_log_level level, const char *fmt, ...) { }

static void set_environment(void);
static bool apply_dipswitch_from_variables();

static void init_audio_buffer(INT32 sample_rate, INT32 fps);

static retro_environment_t environ_cb;
static retro_log_printf_t log_cb = log_dummy;
static retro_video_refresh_t video_cb;
static retro_input_poll_t poll_cb;
static retro_input_state_t input_cb;
static retro_audio_sample_batch_t audio_batch_cb;

// FBARL ---

static uint8_t keybinds[0x5000][2];

#define RETRO_DEVICE_ID_JOYPAD_EMPTY 255
static UINT8 diag_input_hold_frame_delay  = 0;
static int   diag_input_combo_start_frame = 0;
static bool  diag_combo_activated         = false;
static bool  one_diag_input_pressed       = false;
static bool  all_diag_input_pressed       = true;

static UINT8 *diag_input                  = NULL;
static UINT8 diag_input_start[]      =  {RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_start_a_b[]  =  {RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_start_l_r[]  =  {RETRO_DEVICE_ID_JOYPAD_START,  RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_select[]     =  {RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_select_a_b[] =  {RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_A, RETRO_DEVICE_ID_JOYPAD_B, RETRO_DEVICE_ID_JOYPAD_EMPTY };
static UINT8 diag_input_select_l_r[] =  {RETRO_DEVICE_ID_JOYPAD_SELECT, RETRO_DEVICE_ID_JOYPAD_L, RETRO_DEVICE_ID_JOYPAD_R, RETRO_DEVICE_ID_JOYPAD_EMPTY };

static bool gamepad_controls_p1 = true;
static bool gamepad_controls_p2 = true;
static bool newgen_controls_p1  = false;
static bool newgen_controls_p2  = false;
static bool remap_lr_p1         = false;
static bool remap_lr_p2         = false;
static bool core_aspect_par     = false;

extern INT32 EnableHiscores;

#define STAT_NOFIND  0
#define STAT_OK      1
#define STAT_CRC     2
#define STAT_SMALL   3
#define STAT_LARGE   4

struct ROMFIND
{
	unsigned int nState;
	int nArchive;
	int nPos;
	BurnRomInfo ri;
};

static std::vector<std::string> g_find_list_path;
static ROMFIND g_find_list[1024];
static unsigned g_rom_count;

static uint32_t *g_fba_frame;
static int16_t *g_audio_buf;
static INT32 nAudSegLen = 0;
static INT32 g_audio_samplerate = 48000;
UINT32 nFrameskip = 1;

// libretro globals

void retro_set_video_refresh(retro_video_refresh_t cb) { video_cb = cb; }
void retro_set_audio_sample(retro_audio_sample_t) { }
void retro_set_audio_sample_batch(retro_audio_sample_batch_t cb) { audio_batch_cb = cb; }
void retro_set_input_poll(retro_input_poll_t cb) { poll_cb = cb; }
void retro_set_input_state(retro_input_state_t cb) { input_cb = cb; }

static const struct retro_variable var_empty = { NULL, NULL };

static const struct retro_variable var_fba_aspect    = { CORE_OPTION_NAME "_aspect", "Core-provided aspect ratio; DAR|PAR" };
#ifdef WII_VM
static const struct retro_variable var_fba_frameskip = { CORE_OPTION_NAME "_frameskip", "Frameskip; 1|2|3|4|5|0" };
#else
static const struct retro_variable var_fba_frameskip = { CORE_OPTION_NAME "_frameskip", "Frameskip; 0|1|2|3|4|5" };
#endif
static const struct retro_variable var_fba_cpu_speed_adjust = { CORE_OPTION_NAME "_cpu_speed_adjust", "CPU overclock; 100|110|120|130|140|150|160|170|180|190|200" };
static const struct retro_variable var_fba_diagnostic_input = { CORE_OPTION_NAME "_diagnostic_input", "Diagnostic Input; None|Hold Start|Start + A + B|Hold Start + A + B|Start + L + R|Hold Start + L + R|Hold Select|Select + A + B|Hold Select + A + B|Select + L + R|Hold Select + L + R" };
static const struct retro_variable var_fba_hiscores         = { CORE_OPTION_NAME "_hiscores", "Hiscores; enabled|disabled" };
static const struct retro_variable var_fba_samplerate       = { CORE_OPTION_NAME "_samplerate", "Samplerate (need to quit retroarch); 48000|44100|32000|22050|11025" };

// Mapping core options
static const struct retro_variable var_fba_controls_p1    = { CORE_OPTION_NAME "_controls_p1", "P1 control scheme; gamepad|arcade" };
static const struct retro_variable var_fba_controls_p2    = { CORE_OPTION_NAME "_controls_p2", "P2 control scheme; gamepad|arcade" };
static const struct retro_variable var_fba_lr_controls_p1 = { CORE_OPTION_NAME "_lr_controls_p1", "L/R P1 gamepad scheme; normal|remap to R1/R2" };
static const struct retro_variable var_fba_lr_controls_p2 = { CORE_OPTION_NAME "_lr_controls_p2", "L/R P2 gamepad scheme; normal|remap to R1/R2" };

void retro_set_environment(retro_environment_t cb)
{
   environ_cb = cb;
   set_environment();
}

char g_rom_dir[1024];
char g_save_dir[1024];
char g_system_dir[1024];
static bool driver_inited = false;

void retro_get_system_info(struct retro_system_info *info)
{
   info->library_name     = "FB Alpha 2012 CPS-3";
#ifndef GIT_VERSION
#define GIT_VERSION ""
#endif
   info->library_version  = FBA_VERSION GIT_VERSION;
   info->need_fullpath    = true;
   info->block_extract    = true;
   info->valid_extensions = "iso|zip";
}

static void InputMake(void);
static bool init_input(void);
static void check_variables(void);

TCHAR szAppHiscorePath[MAX_PATH];

// Replace the char c_find by the char c_replace in the destination c string
char* str_char_replace(char* destination, char c_find, char c_replace)
{
   for (unsigned str_idx = 0; str_idx < strlen(destination); str_idx++)
   {
      if (destination[str_idx] == c_find)
         destination[str_idx] = c_replace;
   }

   return destination;
}

std::vector<retro_input_descriptor> normal_input_descriptors;

static struct GameInp *pgi_reset;
static struct GameInp *pgi_diag;

struct dipswitch_core_option_value
{
   struct GameInp *pgi;
   BurnDIPInfo bdi;
   char friendly_name[100];
};

struct dipswitch_core_option
{
   char option_name[100];
   char friendly_name[100];

   std::string values_str;
   std::vector<dipswitch_core_option_value> values;
};

static int nDIPOffset;
static std::vector<dipswitch_core_option> dipswitch_core_options;

static void InpDIPSWGetOffset (void)
{
	int i;
	BurnDIPInfo bdi;
	nDIPOffset = 0;

	for(i = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
	{
		if (bdi.nFlags == 0xF0)
		{
			nDIPOffset = bdi.nInput;
			break;
		}
	}
}

void InpDIPSWResetDIPs (void)
{
	int i = 0;
	BurnDIPInfo bdi;
	struct GameInp * pgi = NULL;

	InpDIPSWGetOffset();

	while (BurnDrvGetDIPInfo(&bdi, i) == 0)
	{
		if (bdi.nFlags == 0xFF)
		{
			pgi = GameInp + bdi.nInput + nDIPOffset;
			if (pgi)
				pgi->Input.Constant.nConst = (pgi->Input.Constant.nConst & ~bdi.nMask) | (bdi.nSetting & bdi.nMask);	
		}
		i++;
	}
}

static int InpDIPSWInit(void)
{
   const char * drvname;
   dipswitch_core_options.clear();

   BurnDIPInfo bdi;

   if (!(drvname = BurnDrvGetTextA(DRV_NAME)))
      return 0;

   for (int i = 0, j = 0; BurnDrvGetDIPInfo(&bdi, i) == 0; i++)
   {
      /* 0xFE is the beginning label for a DIP switch entry */
      /* 0xFD are region DIP switches */
      if ((bdi.nFlags == 0xFE || bdi.nFlags == 0xFD) && bdi.nSetting > 0)
      {
         dipswitch_core_options.push_back(dipswitch_core_option());
         dipswitch_core_option *dip_option = &dipswitch_core_options.back();

         // Clean the dipswitch name to creation the core option name (removing space and equal characters)
         char option_name[100];

         // Some dipswitch has no name...
         if (bdi.szText)
            strcpy(option_name, bdi.szText);
         else // ... so, to not hang, we will generate a name based on the position of the dip (DIPSWITCH 1, DIPSWITCH 2...)
            sprintf(option_name, "DIPSWITCH %d", (char) dipswitch_core_options.size());

         strncpy(dip_option->friendly_name, option_name, sizeof(dip_option->friendly_name));

         str_char_replace(option_name, ' ', '_');
         str_char_replace(option_name, '=', '_');

         snprintf(dip_option->option_name, sizeof(dip_option->option_name), CORE_OPTION_NAME "_dipswitch_%s_%s", drvname, option_name);

         // Search for duplicate name, and add number to make them unique in the core-options file
         for (int dup_idx = 0, dup_nbr = 1; dup_idx < dipswitch_core_options.size() - 1; dup_idx++) // - 1 to exclude the current one
         {
            if (strcmp(dip_option->option_name, dipswitch_core_options[dup_idx].option_name) == 0)
            {
               dup_nbr++;
               snprintf(dip_option->option_name, sizeof(dip_option->option_name), CORE_OPTION_NAME "_dipswitch_%s_%s_%d", drvname, option_name, dup_nbr);
            }
         }

         // Reserve space for the default value
         dip_option->values.reserve(bdi.nSetting + 1); // + 1 for default value
         dip_option->values.assign(bdi.nSetting + 1, dipswitch_core_option_value());

         int values_count = 0;
         bool skip_unusable_option = false;
         for (int k = 0; values_count < bdi.nSetting; k++)
         {
            BurnDIPInfo bdi_value;
            if (BurnDrvGetDIPInfo(&bdi_value, k + i + 1) != 0)
               break;

            if (bdi_value.nFlags == 0xFE || bdi_value.nFlags == 0xFD)
               break;

            struct GameInp *pgi_value = GameInp + bdi_value.nInput + nDIPOffset;

            // When the pVal of one value is NULL => the DIP switch is unusable. So it will be skipped by removing it from the list
            if (pgi_value->Input.pVal == 0)
            {
               skip_unusable_option = true;
               break;
            }

            // Filter away NULL entries
            if (bdi_value.nFlags == 0)
               continue;

            dipswitch_core_option_value *dip_value = &dip_option->values[values_count + 1]; // + 1 to skip the default value

            BurnDrvGetDIPInfo(&(dip_value->bdi), k + i + 1);
            dip_value->pgi = pgi_value;
            strncpy(dip_value->friendly_name, dip_value->bdi.szText, sizeof(dip_value->friendly_name));

            bool is_default_value = (dip_value->pgi->Input.Constant.nConst & dip_value->bdi.nMask) == (dip_value->bdi.nSetting);

            if (is_default_value)
            {
               dipswitch_core_option_value *default_dip_value = &dip_option->values[0];

               default_dip_value->bdi = dip_value->bdi;
               default_dip_value->pgi = dip_value->pgi;

               snprintf(default_dip_value->friendly_name, sizeof(default_dip_value->friendly_name), "%s %s", "(Default)", default_dip_value->bdi.szText);
            }

            values_count++;
         }
         
	 // Truncate the list at the values_count found 
	 // to not have empty values
         if (bdi.nSetting > values_count)
            dip_option->values.resize(values_count + 1); // +1 for default value

         // Skip the unusable option by removing it from the list
         if (skip_unusable_option)
         {
            dipswitch_core_options.pop_back();
            continue;
         }

         // Create the string values for the core option
         dip_option->values_str.assign(dip_option->friendly_name);
         dip_option->values_str.append("; ");

         for (int dip_value_idx = 0; dip_value_idx < dip_option->values.size(); dip_value_idx++)
         {
            dipswitch_core_option_value *dip_value = &(dip_option->values[dip_value_idx]);

            dip_option->values_str.append(dip_option->values[dip_value_idx].friendly_name);
            if (dip_value_idx != dip_option->values.size() - 1)
               dip_option->values_str.append("|");
         }
         j++;
      }
   }

   set_environment();
   apply_dipswitch_from_variables();

   return 0;
}

static void set_environment(void)
{
   std::vector<const retro_variable*> vars_systems;

   // Add the Global core options
   vars_systems.push_back(&var_fba_aspect);
	vars_systems.push_back(&var_fba_frameskip);
   vars_systems.push_back(&var_fba_cpu_speed_adjust);
   vars_systems.push_back(&var_fba_controls_p1);
   vars_systems.push_back(&var_fba_controls_p2);
   vars_systems.push_back(&var_fba_hiscores);
    vars_systems.push_back(&var_fba_samplerate);

   // Add the remap L/R to R1/R2 options
   vars_systems.push_back(&var_fba_lr_controls_p1);
   vars_systems.push_back(&var_fba_lr_controls_p2);

   if (pgi_diag)
      vars_systems.push_back(&var_fba_diagnostic_input);

   int nbr_vars = vars_systems.size();
   int nbr_dips = dipswitch_core_options.size();

   struct retro_variable vars[nbr_vars + nbr_dips + 1]; // + 1 for the empty ending retro_variable
   
   int idx_var = 0;

   // Add the System core options
   for (int i = 0; i < nbr_vars; i++, idx_var++)
      vars[idx_var] = *vars_systems[i];

   // Add the DIP switches core options
   for (int dip_idx = 0; dip_idx < nbr_dips; dip_idx++, idx_var++)
   {
      vars[idx_var].key = dipswitch_core_options[dip_idx].option_name;
      vars[idx_var].value = dipswitch_core_options[dip_idx].values_str.c_str();
   }

   vars[idx_var] = var_empty;
   
   environ_cb(RETRO_ENVIRONMENT_SET_VARIABLES, (void*)vars);
}

// Update DIP switches value  depending of the choice 
// the user made in core options
static bool apply_dipswitch_from_variables(void)
{
   bool dip_changed = false;
   struct retro_variable var = {0};
   
   for (int dip_idx = 0; dip_idx < dipswitch_core_options.size(); dip_idx++)
   {
      dipswitch_core_option *dip_option = &dipswitch_core_options[dip_idx];

      var.key = dip_option->option_name;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var) == false)
         continue;

      for (int dip_value_idx = 0; dip_value_idx < dip_option->values.size(); dip_value_idx++)
      {
         dipswitch_core_option_value *dip_value = &(dip_option->values[dip_value_idx]);

         if (strcasecmp(var.value, dip_value->friendly_name) != 0)
            continue;

         int old_nConst = dip_value->pgi->Input.Constant.nConst;

         dip_value->pgi->Input.Constant.nConst = (dip_value->pgi->Input.Constant.nConst & ~dip_value->bdi.nMask) | (dip_value->bdi.nSetting & dip_value->bdi.nMask);
         dip_value->pgi->Input.nVal = dip_value->pgi->Input.Constant.nConst;
         if (dip_value->pgi->Input.pVal)
            *(dip_value->pgi->Input.pVal) = dip_value->pgi->Input.nVal;

         if (dip_value->pgi->Input.Constant.nConst != old_nConst)
            dip_changed = true;
      }
   }

   return dip_changed;
}

void Reinitialise(void)
{
    // Update the geometry, sfiii2 needs it (when toggling
    // between widescreen and non-widescreen
    struct retro_system_av_info av_info;
    retro_get_system_av_info(&av_info);
    environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
}

static void ForceFrameStep(int bDraw)
{
   nBurnLayer = 0xff;
   nCurrentFrame++;
   if (!bDraw)
	   pBurnDraw = NULL;
   BurnDrvFrame();
}

const int nConfigMinVersion = 0x020921;

// addition to support loading of roms without crc check
static int find_rom_by_name(char *name, const ZipEntry *list, unsigned elems)
{
   unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if(!strcmp(list[i].szName, name)) 
         return i; 
   }

   return -1;
}

static int find_rom_by_crc(uint32_t crc, const ZipEntry *list, unsigned elems)
{
	unsigned i = 0;
   for (i = 0; i < elems; i++)
   {
      if (list[i].nCrc == crc)
         return i;
   }

   return -1;
}

static void free_archive_list(ZipEntry *list, unsigned count)
{
   if (list)
   {
      unsigned i;
      for (i = 0; i < count; i++)
         free(list[i].szName);
      free(list);
   }
}

static int archive_load_rom(uint8_t *dest, int *wrote, int i)
{
   if (i < 0 || i >= g_rom_count)
      return 1;

   int archive = g_find_list[i].nArchive;

   if (ZipOpen((char*)g_find_list_path[archive].c_str()) != 0)
      return 1;

   BurnRomInfo ri = {0};
   BurnDrvGetRomInfo(&ri, i);

   if (ZipLoadFile(dest, ri.nLen, wrote, g_find_list[i].nPos) != 0)
   {
      ZipClose();
      return 1;
   }

   ZipClose();
   return 0;
}

// This code is very confusing. The original code is even more confusing :(
static bool open_archive(void)
{
	memset(g_find_list, 0, sizeof(g_find_list));

	// FBA wants some roms ... Figure out how many.
	g_rom_count = 0;
	while (!BurnDrvGetRomInfo(&g_find_list[g_rom_count].ri, g_rom_count))
		g_rom_count++;

	g_find_list_path.clear();
	
	// Check if we have said archives.
	// Check if archives are found. These are relative to g_rom_dir.
	char *rom_name;
	for (unsigned index = 0; index < 32; index++)
	{
		char path[1024];
		if (BurnDrvGetZipName(&rom_name, index))
			continue;
#if defined(_XBOX) || defined(_WIN32)
		snprintf(path, sizeof(path), "%s\\%s", g_rom_dir, rom_name);
#else
		snprintf(path, sizeof(path), "%s/%s", g_rom_dir, rom_name);
#endif

		if (ZipOpen(path) == 0)
			g_find_list_path.push_back(path);

		ZipClose();
	}

	for (unsigned z = 0; z < g_find_list_path.size(); z++)
	{
		int count;
		ZipEntry *list = NULL;
		if (ZipOpen((char*)g_find_list_path[z].c_str()) != 0)
			return false;

		ZipGetList(&list, &count);

		// Try to map the ROMs FBA wants to ROMs we find inside our pretty archives ...
		for (unsigned i = 0; i < g_rom_count; i++)
		{
			int index;
			if (g_find_list[i].nState == STAT_OK)
				continue;

			if (g_find_list[i].ri.nType == 0 || g_find_list[i].ri.nLen == 0 || g_find_list[i].ri.nCrc == 0)
			{
				g_find_list[i].nState = STAT_OK;
				continue;
			}

			index = find_rom_by_crc(g_find_list[i].ri.nCrc, list, count);
			BurnDrvGetRomName(&rom_name, i, 0);

			if (index < 0)
				index = find_rom_by_name(rom_name, list, count);

			// USE UNI-BIOS...
			if (index < 0)
				continue;              

			// Yay, we found it!
			g_find_list[i].nArchive = z;
			g_find_list[i].nPos = index;
			g_find_list[i].nState = STAT_OK;

			if (list[index].nLen < g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_SMALL;
			else if (list[index].nLen > g_find_list[i].ri.nLen)
				g_find_list[i].nState = STAT_LARGE;
		}

		free_archive_list(list, count);
		ZipClose();
	}

	// Going over every rom to see if they are properly loaded before we continue ...
	for (unsigned i = 0; i < g_rom_count; i++)
	{
		if (g_find_list[i].nState != STAT_OK)
		{
			if(!(g_find_list[i].ri.nType & BRF_OPT))
			{
				log_cb(RETRO_LOG_ERROR, "[FBA] ROM at index %d with CRC 0x%08x is required ...\n", i, g_find_list[i].ri.nCrc);
				return false;
			}
		}
	}

	BurnExtLoadRom = archive_load_rom;
	return true;
}

void retro_init(void)
{
   struct retro_log_callback log;
   if (environ_cb(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log))
      log_cb = log.log;
   else
      log_cb = log_dummy;

   BurnLibInit();
}

void retro_deinit(void)
{
   char output[128];

   if (driver_inited)
   {
      snprintf (output, sizeof(output), "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
      BurnStateSave(output, 0);
      BurnDrvExit();
   }
   driver_inited = false;
   BurnLibExit();
   if (g_fba_frame)
      free(g_fba_frame);
}

void retro_reset(void)
{
   if (pgi_reset)
   {
      pgi_reset->Input.nVal    = 1;
      *(pgi_reset->Input.pVal) = pgi_reset->Input.nVal;
   }

   ForceFrameStep(1);
}

static void check_variables(void)
{
   struct retro_variable var = {0};

   var.key = var_fba_cpu_speed_adjust.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "110") == 0)
         nBurnCPUSpeedAdjust = 0x0110;
      else if (strcmp(var.value, "120") == 0)
         nBurnCPUSpeedAdjust = 0x0120;
      else if (strcmp(var.value, "130") == 0)
         nBurnCPUSpeedAdjust = 0x0130;
      else if (strcmp(var.value, "140") == 0)
         nBurnCPUSpeedAdjust = 0x0140;
      else if (strcmp(var.value, "150") == 0)
         nBurnCPUSpeedAdjust = 0x0150;
      else if (strcmp(var.value, "160") == 0)
         nBurnCPUSpeedAdjust = 0x0160;
      else if (strcmp(var.value, "170") == 0)
         nBurnCPUSpeedAdjust = 0x0170;
      else if (strcmp(var.value, "180") == 0)
         nBurnCPUSpeedAdjust = 0x0180;
      else if (strcmp(var.value, "190") == 0)
         nBurnCPUSpeedAdjust = 0x0190;
      else if (strcmp(var.value, "200") == 0)
         nBurnCPUSpeedAdjust = 0x0200;
      else
         nBurnCPUSpeedAdjust = 0x0100;
   }

   var.key = var_fba_frameskip.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
	   if (strcmp(var.value, "0") == 0)
		   nFrameskip = 1;
	   else if (strcmp(var.value, "1") == 0)
		   nFrameskip = 2;
	   else if (strcmp(var.value, "2") == 0)
		   nFrameskip = 3;
	   else if (strcmp(var.value, "3") == 0)
		   nFrameskip = 4;
	   else if (strcmp(var.value, "4") == 0)
		   nFrameskip = 5;
	   else if (strcmp(var.value, "5") == 0)
		   nFrameskip = 6;
   }

   var.key = var_fba_controls_p1.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "gamepad") == 0)
         gamepad_controls_p1 = true;
      else
         gamepad_controls_p1 = false;
   }

   var.key = var_fba_controls_p2.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "gamepad") == 0)
         gamepad_controls_p2 = true;
      else
         gamepad_controls_p2 = false;
   }

   var.key = var_fba_aspect.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "PAR") == 0)
         core_aspect_par = true;
	  else
         core_aspect_par = false;
   }

   var.key = var_fba_lr_controls_p1.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "remap to R1/R2") == 0)
         remap_lr_p1 = true;
      else
         remap_lr_p1 = false;
   }

   var.key = var_fba_lr_controls_p2.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "remap to R1/R2") == 0)
         remap_lr_p2 = true;
      else
         remap_lr_p2 = false;
   }

   if (pgi_diag)
   {
      var.key = var_fba_diagnostic_input.key;
      if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
      {
         diag_input = NULL;
         diag_input_hold_frame_delay = 0;
         if (strcmp(var.value, "Hold Start") == 0)
         {
            diag_input = diag_input_start;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Start + A + B") == 0)
         {
            diag_input = diag_input_start_a_b;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Start + A + B") == 0)
         {
            diag_input = diag_input_start_a_b;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Start + L + R") == 0)
         {
            diag_input = diag_input_start_l_r;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Start + L + R") == 0)
         {
            diag_input = diag_input_start_l_r;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Hold Select") == 0)
         {
            diag_input = diag_input_select;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Select + A + B") == 0)
         {
            diag_input = diag_input_select_a_b;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Select + A + B") == 0)
         {
            diag_input = diag_input_select_a_b;
            diag_input_hold_frame_delay = 60;
         }
         else if(strcmp(var.value, "Select + L + R") == 0)
         {
            diag_input = diag_input_select_l_r;
            diag_input_hold_frame_delay = 0;
         }
         else if(strcmp(var.value, "Hold Select + L + R") == 0)
         {
            diag_input = diag_input_select_l_r;
            diag_input_hold_frame_delay = 60;
         }
      }
   }

   var.key = var_fba_hiscores.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "enabled") == 0)
         EnableHiscores = true;
      else
         EnableHiscores = false;
   }

   var.key = var_fba_samplerate.key;
   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE, &var))
   {
      if (strcmp(var.value, "48000") == 0)
         g_audio_samplerate = 48000;
      else if (strcmp(var.value, "44100") == 0)
         g_audio_samplerate = 44100;
      else if (strcmp(var.value, "32000") == 0)
         g_audio_samplerate = 32000;
      else if (strcmp(var.value, "22050") == 0)
         g_audio_samplerate = 22050;
      else if (strcmp(var.value, "11025") == 0)
         g_audio_samplerate = 11025;
      else
         g_audio_samplerate = 48000;
   }
}

// Set the input descriptors by combininng the 
// two lists of 'Normal' and 'Macros' inputs
static void set_input_descriptors(void)
{
   unsigned input_descriptor_idx = 0;
   struct retro_input_descriptor input_descriptors[normal_input_descriptors.size() + 1]; // + 1 for the empty ending retro_input_descriptor { 0 }

   for (unsigned i = 0; i < normal_input_descriptors.size(); i++, input_descriptor_idx++)
      input_descriptors[input_descriptor_idx] = normal_input_descriptors[i];

   input_descriptors[input_descriptor_idx] = (struct retro_input_descriptor){ 0 };
   environ_cb(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, input_descriptors);
}

void retro_run(void)
{
   bool updated = false;
   int width, height;
   BurnDrvGetVisibleSize(&width, &height);
   pBurnDraw = (uint8_t*)g_fba_frame;

   InputMake();

   ForceFrameStep(nCurrentFrame % nFrameskip == 0);

   unsigned drv_flags  = BurnDrvGetFlags();
   uint32_t height_tmp = height;
   size_t pitch_size   = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);

   switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
   {
      case BDF_ORIENTATION_VERTICAL:
      case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
         nBurnPitch = height * pitch_size;
         height = width;
         width = height_tmp;
         break;
      case BDF_ORIENTATION_FLIPPED:
      default:
         nBurnPitch = width * pitch_size;
   }

   video_cb(g_fba_frame, width, height, nBurnPitch);
   audio_batch_cb(g_audio_buf, nBurnSoundLen);

   if (environ_cb(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &updated) && updated)
   {
      bool reinit_input_performed  = false;
      bool old_gamepad_controls_p1 = gamepad_controls_p1;
      bool old_gamepad_controls_p2 = gamepad_controls_p2;
      bool old_newgen_controls_p1  = newgen_controls_p1;
      bool old_newgen_controls_p2  = newgen_controls_p2;
      bool old_remap_lr_p1         = remap_lr_p1;
      bool old_remap_lr_p2         = remap_lr_p2;
      bool old_core_aspect_par     = core_aspect_par;

      check_variables();

      apply_dipswitch_from_variables();

      // reinitialise input if user changed the control scheme
      if (old_gamepad_controls_p1 != gamepad_controls_p1 ||
          old_gamepad_controls_p2 != gamepad_controls_p2 ||
          old_newgen_controls_p1 != newgen_controls_p1 ||
          old_newgen_controls_p2 != newgen_controls_p2 ||
          old_remap_lr_p1 != remap_lr_p1 ||
          old_remap_lr_p2 != remap_lr_p2)
      {
         init_input();
         reinit_input_performed = true;
      }

      // if the reinit_input_performed is true, the 2 following methods was already called in the init_input one
      // Re-assign all the input_descriptors to retroarch
      if (!reinit_input_performed) 
         set_input_descriptors();

      // adjust aspect ratio if the needed
      if (old_core_aspect_par != core_aspect_par)
      {
         struct retro_system_av_info av_info;
         retro_get_system_av_info(&av_info);
         environ_cb(RETRO_ENVIRONMENT_SET_GEOMETRY, &av_info);
      }
   }
}

static uint8_t *write_state_ptr;
static const uint8_t *read_state_ptr;
static unsigned state_size;

static int burn_write_state_cb(BurnArea *pba)
{
   memcpy(write_state_ptr, pba->Data, pba->nLen);
   write_state_ptr += pba->nLen;
   return 0;
}

static int burn_read_state_cb(BurnArea *pba)
{
   memcpy(pba->Data, read_state_ptr, pba->nLen);
   read_state_ptr += pba->nLen;
   return 0;
}

static int burn_dummy_state_cb(BurnArea *pba)
{
   state_size += pba->nLen;
   return 0;
}

size_t retro_serialize_size()
{
   if (state_size)
      return state_size;

   BurnAcb = burn_dummy_state_cb;
   state_size = 0;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);
   return state_size;
}

bool retro_serialize(void *data, size_t size)
{
   if (size != state_size)
      return false;

   BurnAcb         = burn_write_state_cb;
   write_state_ptr = (uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_READ, 0);

   return true;
}

bool retro_unserialize(const void *data, size_t size)
{
   if (size != state_size)
      return false;
   BurnAcb = burn_read_state_cb;
   read_state_ptr = (const uint8_t*)data;
   BurnAreaScan(ACB_FULLSCAN | ACB_WRITE, 0);

   return true;
}

void retro_cheat_reset() { }
void retro_cheat_set(unsigned, bool, const char*) { }

void retro_get_system_av_info(struct retro_system_av_info *info)
{
   int width, height;
   BurnDrvGetVisibleSize(&width, &height);
   int maximum                     = width > height ? width : height;
   struct retro_game_geometry geom = { (unsigned)width, (unsigned)height, (unsigned)maximum, (unsigned)maximum };
   
   int game_aspect_x, game_aspect_y;
   BurnDrvGetAspect(&game_aspect_x, &game_aspect_y);

   if (game_aspect_x != 0 && game_aspect_y != 0 && !core_aspect_par)
      geom.aspect_ratio = (float)game_aspect_x / (float)game_aspect_y;

   struct retro_system_timing timing = { (nBurnFPS / 100.0), (nBurnFPS / 100.0) * nAudSegLen };

   info->geometry = geom;
   info->timing   = timing;
}

static bool fba_init(unsigned driver, const char *game_zip_name)
{
   nBurnDrvActive = driver;

   if (!open_archive())
   {
      log_cb(RETRO_LOG_ERROR, "[FBA] Cannot find driver.\n");
      return false;
   }

   // Announcing to fba which samplerate we want
   nBurnSoundRate = g_audio_samplerate;

   // Some game drivers won't initialize with an undefined nBurnSoundLen
   init_audio_buffer(nBurnSoundRate, 6000);

   nBurnBpp = 2;
   nFMInterpolation = 3;
   nInterpolation = 1;

   analog_controls_enabled = init_input();

   InpDIPSWInit();

   BurnDrvInit();

   // Now we know real game fps, let's initialize sound buffer again
   init_audio_buffer(nBurnSoundRate, nBurnFPS);

   char input[128];
   snprintf (input, sizeof(input), "%s%c%s.fs", g_save_dir, slash, BurnDrvGetTextA(DRV_NAME));
   BurnStateLoad(input, 0, NULL);

   int width, height;
   BurnDrvGetVisibleSize(&width, &height);
   unsigned drv_flags = BurnDrvGetFlags();
   if (!(drv_flags & BDF_GAME_WORKING))
   {
      log_cb(RETRO_LOG_ERROR, "[FBA] Game %s is not marked as working\n", game_zip_name);
      return false;
   }
   size_t pitch_size = nBurnBpp == 2 ? sizeof(uint16_t) : sizeof(uint32_t);
   if (drv_flags & BDF_ORIENTATION_VERTICAL)
      nBurnPitch = height * pitch_size;
   else
      nBurnPitch = width * pitch_size;

   unsigned rotation;
   switch (drv_flags & (BDF_ORIENTATION_FLIPPED | BDF_ORIENTATION_VERTICAL))
   {
      case BDF_ORIENTATION_VERTICAL:
         rotation = 1;
         break;

      case BDF_ORIENTATION_FLIPPED:
         rotation = 2;
         break;

      case BDF_ORIENTATION_VERTICAL | BDF_ORIENTATION_FLIPPED:
         rotation = 3;
         break;

      default:
         rotation = 0;
   }

   environ_cb(RETRO_ENVIRONMENT_SET_ROTATION, &rotation);

   BurnRecalcPal();

#ifdef FRONTEND_SUPPORTS_RGB565
   if(nBurnBpp == 4)
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_XRGB8888;

      if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt))
         log_cb(RETRO_LOG_INFO, "Frontend supports XRGB888 - will use that instead of XRGB1555.\n");
   }
   else
   {
      enum retro_pixel_format fmt = RETRO_PIXEL_FORMAT_RGB565;

      if(environ_cb(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &fmt)) 
         log_cb(RETRO_LOG_INFO, "Frontend supports RGB565 - will use that instead of XRGB1555.\n");
   }
#endif

   return true;
}

static void init_audio_buffer(INT32 sample_rate, INT32 fps)
{
	// [issue #206]
	// For games where sample_rate/1000 > fps/100
	// we don't change nBurnSoundRate, but we adjust some length
	if ((sample_rate / 1000) > (fps / 100))
		sample_rate = fps * 10;
	nAudSegLen = (sample_rate * 100 + (fps >> 1)) / fps;
	if (g_audio_buf)
		free(g_audio_buf);
	g_audio_buf = (int16_t*)malloc(nAudSegLen<<2 * sizeof(int16_t));
	memset(g_audio_buf, 0, nAudSegLen<<2 * sizeof(int16_t));
	nBurnSoundLen = nAudSegLen;
	pBurnSoundOut = g_audio_buf;
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
   strncpy(buf, path, size - 1);
   buf[size - 1] = '\0';

   char *base = strrchr(buf, '/');
   if (!base)
      base = strrchr(buf, '\\');

   if (base)
      *base = '\0';
   else
   {
      buf[0] = '.';
      buf[1] = '\0';
   }
}

static unsigned int BurnDrvGetIndexByName(const char* name)
{
   unsigned int i;
   unsigned int ret = ~0U;

   for (i = 0; i < nBurnDrvCount; i++)
   {
      nBurnDrvActive = i;
      if (!strcmp(BurnDrvGetText(DRV_NAME), name))
      {
         ret = i;
         break;
      }
   }
   return ret;
}


bool retro_load_game(const struct retro_game_info *info)
{
   char basename[128];

   if (!info)
      return false;

   extract_basename(basename, info->path, sizeof(basename));
   extract_directory(g_rom_dir, info->path, sizeof(g_rom_dir));

   const char *dir = NULL;
   // If save directory is defined use it, ...
   if (environ_cb(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &dir) && dir)
   {
      strncpy(g_save_dir, dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_INFO, "Setting save dir to %s\n", g_save_dir);
   }
   else
   {
      // ... otherwise use rom directory
      strncpy(g_save_dir, g_rom_dir, sizeof(g_save_dir));
      log_cb(RETRO_LOG_ERROR, "Save dir not defined => use roms dir %s\n", g_save_dir);
   }

   // If system directory is defined use it, ...
   if (environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &dir) && dir)
   {
      strncpy(g_system_dir, dir, sizeof(g_system_dir));
      log_cb(RETRO_LOG_INFO, "Setting system dir to %s\n", g_system_dir);
   }
   else
   {
      // ... otherwise use rom directory
      strncpy(g_system_dir, g_rom_dir, sizeof(g_system_dir));
      log_cb(RETRO_LOG_ERROR, "System dir not defined => use roms dir %s\n", g_system_dir);
   }

   unsigned i = BurnDrvGetIndexByName(basename);
   if (i < nBurnDrvCount)
   {
      INT32 width, height;
      set_environment();
      check_variables();

      if (!fba_init(i, basename))
         goto error;

      driver_inited = true;

      BurnDrvGetFullSize(&width, &height);

      g_fba_frame = (uint32_t*)malloc(width * height * sizeof(uint32_t));

      return true;
   }

error:
   log_cb(RETRO_LOG_ERROR, "[FBA] Cannot load this game.\n");
   return false;
}


void retro_unload_game(void)
{
   if (driver_inited)
   {
      BurnDrvExit();
      driver_inited = false;  
   }
}

unsigned retro_get_region(void) { return RETRO_REGION_NTSC; }
void *retro_get_memory_data(unsigned) { return 0; }
size_t retro_get_memory_size(unsigned) { return 0; }
bool retro_load_game_special(unsigned, const struct retro_game_info*, size_t) { return false; }
unsigned retro_api_version(void) { return RETRO_API_VERSION; }
void retro_set_controller_port_device(unsigned, unsigned) { }

static bool init_input(void)
{
   // Define nMaxPlayers early; GameInpInit() needs it (normally defined in DoLibInit()).
   nMaxPlayers = BurnDrvGetMaxPlayers();
   GameInpInit();
   GameInpDefault();

   // Handle twinstick games (issue libretro/fbalpha#102)
   bool twinstick_game[5] = { false, false, false, false, false };
   bool up_is_mapped[5] = { false, false, false, false, false };
   bool down_is_mapped[5] = { false, false, false, false, false };
   bool left_is_mapped[5] = { false, false, false, false, false };
   bool right_is_mapped[5] = { false, false, false, false, false };

   bool has_analog = false;
   struct GameInp* pgi = GameInp;
   for (unsigned i = 0; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->nType == BIT_ANALOG_REL)
      {
         has_analog = true;
         break;
      }
   }

   /* initialization */
   struct BurnInputInfo bii;
   memset(&bii, 0, sizeof(bii));

   // Bind to nothing.
   for (unsigned i = 0; i < 0x5000; i++)
      keybinds[i][0] = 0xff;

   pgi = GameInp;
   pgi_reset = NULL;
   pgi_diag = NULL;

   normal_input_descriptors.clear();

   for(unsigned int i = 0; i < nGameInpCount; i++, pgi++)
   {
      BurnDrvGetInputInfo(&bii, i);

      bool value_found = false;

      bool bPlayerInInfo = (toupper(bii.szInfo[0]) == 'P' && bii.szInfo[1] >= '1' && bii.szInfo[1] <= '4'); // Because some of the older drivers don't use the standard input naming.
      bool bPlayerInName = (bii.szName[0] == 'P' && bii.szName[1] >= '1' && bii.szName[1] <= '4');

      if (bPlayerInInfo || bPlayerInName)
      {
         INT32 nPlayer = -1;

         if (bPlayerInName)
            nPlayer = bii.szName[1] - '1';
         if (bPlayerInInfo && nPlayer == -1)
            nPlayer = bii.szInfo[1] - '1';

         char* szi = bii.szInfo + 3;

         if (strncmp("select", szi, 6) == 0)
	 {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            value_found = true;
         }
         if (strncmp("coin", szi, 4) == 0)
	 {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_SELECT;
            value_found = true;
         }
         if (strncmp("start", szi, 5) == 0)
	 {
            keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_START;
            value_found = true;
         }
         if (strncmp("up", szi, 2) == 0)
	 {
            if( up_is_mapped[nPlayer] )
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_X;
               twinstick_game[nPlayer] = true;
            }
	    else
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_UP;
               up_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }
         if (strncmp("down", szi, 4) == 0)
	 {
            if( down_is_mapped[nPlayer] )
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_B;
               twinstick_game[nPlayer] = true;
            }
	    else
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_DOWN;
               down_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }
         if (strncmp("left", szi, 4) == 0)
	 {
            if( left_is_mapped[nPlayer] )
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_Y;
               twinstick_game[nPlayer] = true;
            }
	    else
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_LEFT;
               left_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }
         if (strncmp("right", szi, 5) == 0)
	 {
            if( right_is_mapped[nPlayer] )
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_A;
               twinstick_game[nPlayer] = true;
            }
	    else
	    {
               keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_RIGHT;
               right_is_mapped[nPlayer] = true;
            }
            value_found = true;
         }

         if (strncmp("fire ", szi, 5) == 0)
	 {
            char *szb = szi + 5;
            INT32 nButton = strtol(szb, NULL, 0);
            if (twinstick_game[nPlayer])
	    {
               switch (nButton)
	       {
                  case 1:
                     keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_R;
                     value_found = true;
                     break;
                  case 2:
                     keybinds[pgi->Input.Switch.nCode][0] = RETRO_DEVICE_ID_JOYPAD_L;
                     value_found = true;
                     break;
               }
			}
            else if (nFireButtons <= 4)
	    {
                  switch (nButton)
		  {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
               }
            }
	    else
	    {
               if (bStreetFighterLayout)
	       {
                  switch (nButton)
		  {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R : RETRO_DEVICE_ID_JOYPAD_L) : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
                     case 5:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_L);
                        value_found = true;
                        break;
                     case 6:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_R) : RETRO_DEVICE_ID_JOYPAD_R);
                        value_found = true;
                        break;
                  }
               }
	       else
	       {
                  switch (nButton)
		  {
                     case 1:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_B : RETRO_DEVICE_ID_JOYPAD_A);
                        value_found = true;
                        break;
                     case 2:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_A : RETRO_DEVICE_ID_JOYPAD_B);
                        value_found = true;
                        break;
                     case 3:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R2 : RETRO_DEVICE_ID_JOYPAD_R) : RETRO_DEVICE_ID_JOYPAD_X);
                        value_found = true;
                        break;
                     case 4:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_Y : RETRO_DEVICE_ID_JOYPAD_Y);
                        value_found = true;
                        break;
                     case 5:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_X : RETRO_DEVICE_ID_JOYPAD_L);
                        value_found = true;
                        break;
                     case 6:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_R : RETRO_DEVICE_ID_JOYPAD_L) : RETRO_DEVICE_ID_JOYPAD_R);
                        value_found = true;
                        break;
                     case 7:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_L2 : RETRO_DEVICE_ID_JOYPAD_R2) : RETRO_DEVICE_ID_JOYPAD_R2);
                        value_found = true;
                        break;
                     case 8:
                        keybinds[pgi->Input.Switch.nCode][0] = ((gamepad_controls_p1 && nPlayer == 0) || (gamepad_controls_p2 && nPlayer == 1) ? ((remap_lr_p1 && nPlayer == 0) || (remap_lr_p2 && nPlayer == 1) ? RETRO_DEVICE_ID_JOYPAD_L : RETRO_DEVICE_ID_JOYPAD_L2) : RETRO_DEVICE_ID_JOYPAD_L2);
                        value_found = true;
                        break;
                  }
               }
            }
         }

         if (!value_found)
            continue;

         if(nPlayer >= 0)
            keybinds[pgi->Input.Switch.nCode][1] = nPlayer;

         UINT32 port = (UINT32)nPlayer;
         UINT32 device = RETRO_DEVICE_JOYPAD;
         UINT32 index = 0;
         UINT32 id = keybinds[pgi->Input.Switch.nCode][0];

         // "P1 XXX" - try to exclude the "P1 " from the szName
         INT32 offset_player_x = 0;
         if (strlen(bii.szName) > 3 && bii.szName[0] == 'P' && bii.szName[2] == ' ')
            offset_player_x = 3;

         char* description = bii.szName + offset_player_x;
         
         normal_input_descriptors.push_back((retro_input_descriptor){ port, device, index, id, description });
      }

      // Store the pgi that controls the reset input
      if (strcmp(bii.szInfo, "reset") == 0)
      {
         value_found = true;
         pgi_reset   = pgi;
      }

      // Store the pgi that controls the diagnostic input
      if (strcmp(bii.szInfo, "diag") == 0)
      {
         value_found = true;
         pgi_diag    = pgi;
      }
   }

   // Update core option for diagnostic input
   set_environment();

   // Read the user core option values
   check_variables();

   // The list of normal and macro input_descriptors are filled, we can assign all the input_descriptors to retroarch
   set_input_descriptors();

   return has_analog;
}

static inline INT32 CinpState(INT32 nCode)
{
   INT32 id    = keybinds[nCode][0];
   UINT32 port = keybinds[nCode][1];
   return input_cb(port, RETRO_DEVICE_JOYPAD, 0, id);
}

static inline int CinpJoyAxis(int i, int axis)
{
   switch(axis)
   {
      case 0:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 1:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_LEFT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 3:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_X);
      case 4:
         return input_cb(i, RETRO_DEVICE_ANALOG, RETRO_DEVICE_INDEX_ANALOG_RIGHT,
               RETRO_DEVICE_ID_ANALOG_Y);
      case 5:
      case 6:
      case 7:
      case 2:
	 break;
   }
   return 0;
}

static bool poll_diag_input(void)
{
   if (pgi_diag && diag_input)
   {
      one_diag_input_pressed = false;
      all_diag_input_pressed = true;

      for (int combo_idx = 0; diag_input[combo_idx] != RETRO_DEVICE_ID_JOYPAD_EMPTY; combo_idx++)
      {
         if (input_cb(0, RETRO_DEVICE_JOYPAD, 0, diag_input[combo_idx]) == false)
            all_diag_input_pressed = false;
         else
            one_diag_input_pressed = true;
      }

      if (diag_combo_activated == false && all_diag_input_pressed)
      {
         if (diag_input_combo_start_frame == 0) // => User starts holding all the combo inputs
            diag_input_combo_start_frame = nCurrentFrame;
         else if ((nCurrentFrame - diag_input_combo_start_frame) > diag_input_hold_frame_delay) // Delays of the hold reached
            diag_combo_activated = true;
      }
      else if (one_diag_input_pressed == false)
      {
         diag_combo_activated = false;
         diag_input_combo_start_frame = 0;
      }

      if (diag_combo_activated)
      {
         // Cancel each input of the combo at the emulator side to not interfere when the diagnostic menu will be opened and the combo not yet released
         struct GameInp* pgi = GameInp;
         for (int combo_idx = 0; diag_input[combo_idx] != RETRO_DEVICE_ID_JOYPAD_EMPTY; combo_idx++)
         {
            for (int i = 0; i < nGameInpCount; i++, pgi++)
            {
               if (pgi->nInput == GIT_SWITCH)
               {
                  pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
            }
         }

         // Activate the diagnostic key
         pgi_diag->Input.nVal = 1;
         *(pgi_diag->Input.pVal) = pgi_diag->Input.nVal;

         // Return true to stop polling game inputs while diagnostic combo inputs is pressed
         return true;
      }
   }

   // Return false to poll game inputs
   return false;
}

static void InputTick(void)
{
	struct GameInp *pgi;
	UINT32 i;

	for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++)
	{
		INT32 nAdd = 0;
		if ((pgi->nInput &  GIT_GROUP_SLIDER) == 0) // not a slider
			continue;

		if (pgi->nInput == GIT_JOYSLIDER)
		{
			// Get state of the axis
			nAdd = CinpJoyAxis(pgi->Input.Slider.JoyAxis.nJoy, pgi->Input.Slider.JoyAxis.nAxis);
			nAdd /= 0x100;
		}

		// nAdd is now -0x100 to +0x100

		// Change to slider speed
		nAdd *= pgi->Input.Slider.nSliderSpeed;
		nAdd /= 0x100;

		if (pgi->Input.Slider.nSliderCenter)
		{ // Attact to center
			INT32 v = pgi->Input.Slider.nSliderValue - 0x8000;
			v *= (pgi->Input.Slider.nSliderCenter - 1);
			v /= pgi->Input.Slider.nSliderCenter;
			v += 0x8000;
			pgi->Input.Slider.nSliderValue = v;
		}

		pgi->Input.Slider.nSliderValue += nAdd;
		// Limit slider
		if (pgi->Input.Slider.nSliderValue < 0x0100)
			pgi->Input.Slider.nSliderValue = 0x0100;
		if (pgi->Input.Slider.nSliderValue > 0xFF00)
			pgi->Input.Slider.nSliderValue = 0xFF00;
	}
}

static void InputMake(void)
{
   poll_cb();

   if (poll_diag_input())
      return;

   struct GameInp* pgi;
   UINT32 i;

   InputTick();

   for (i = 0, pgi = GameInp; i < nGameInpCount; i++, pgi++)
   {
      if (pgi->Input.pVal == NULL)
         continue;

      switch (pgi->nInput)
      {
         case 0:                       // Undefined
            pgi->Input.nVal = 0;
            break;
         case GIT_CONSTANT:            // Constant value
            {
               pgi->Input.nVal = pgi->Input.Constant.nConst;
               *(pgi->Input.pVal) = pgi->Input.nVal;
            }
            break;
         case GIT_SWITCH:
            {
               // Digital input
               INT32 s = CinpState(pgi->Input.Switch.nCode);
               if (pgi->nType & BIT_GROUP_ANALOG)
               {
                  // Set analog controls to full
                  if (s)
                     pgi->Input.nVal = 0xFFFF;
                  else
                     pgi->Input.nVal = 0x0001;
#ifdef MSB_FIRST
                  *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
                  *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               }
               else
               {
                  // Binary controls
                  if (s)
                     pgi->Input.nVal = 1;
                  else
                     pgi->Input.nVal = 0;
                  *(pgi->Input.pVal) = pgi->Input.nVal;
               }
               break;
            }
         case GIT_KEYSLIDER: // Keyboard slider
         case GIT_JOYSLIDER: // Joystick slider
            {
               int nSlider = pgi->Input.Slider.nSliderValue;
               if (pgi->nType == BIT_ANALOG_REL)
	       {
                  nSlider -= 0x8000;
                  nSlider >>= 4;
               }

               pgi->Input.nVal = (UINT16)nSlider;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_MOUSEAXIS: // Mouse axis
            {
               pgi->Input.nVal = 0;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
            }
            break;
         case GIT_JOYAXIS_FULL:
            { // Joystick axis
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);

               if (pgi->nType == BIT_ANALOG_REL)
	       {
                  nJoy *= nAnalogSpeed;
                  nJoy >>= 13;

                  // Clip axis to 8 bits
                  if (nJoy < -32768)
                     nJoy = -32768;
                  if (nJoy >  32767)
                     nJoy =  32767;
               }
	       else
	       {
                  nJoy >>= 1;
                  nJoy += 0x8000;

                  // Clip axis to 16 bits
                  if (nJoy < 0x0001)
                     nJoy = 0x0001;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;
               }

               pgi->Input.nVal = (UINT16)nJoy;
#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_NEG:
            {				// Joystick axis Lo
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy < 32767)
               {
                  nJoy = -nJoy;

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
         case GIT_JOYAXIS_POS:
            {				// Joystick axis Hi
               INT32 nJoy = CinpJoyAxis(pgi->Input.JoyAxis.nJoy, pgi->Input.JoyAxis.nAxis);
               if (nJoy > 32767)
               {

                  if (nJoy < 0x0000)
                     nJoy = 0x0000;
                  if (nJoy > 0xFFFF)
                     nJoy = 0xFFFF;

                  pgi->Input.nVal = (UINT16)nJoy;
               }
               else
                  pgi->Input.nVal = 0;

#ifdef MSB_FIRST
               *((int *)pgi->Input.pShortVal) = pgi->Input.nVal;
#else
               *(pgi->Input.pShortVal) = pgi->Input.nVal;
#endif
               break;
            }
      }
   }
}

#ifdef ANDROID
#include <wchar.h>

size_t mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
   if (pwcs == NULL)
      return strlen(s);
   return mbsrtowcs(pwcs, &s, n, NULL);
}

size_t wcstombs(char *s, const wchar_t *pwcs, size_t n)
{
   return wcsrtombs(s, &pwcs, n, NULL);
}

#endif

#ifdef WII_VM
// Gets the cache directory containing all RomUser_[parent name], RomGame_[parent name] and RomGame_D_[parent name] files.
static void get_cache_path(char *path)
{
   const char *system_directory_c = NULL;
   environ_cb(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_directory_c);
   sprintf(path, "%s/cache/", system_directory_c);

   DIR *dir = opendir(path);
   if (dir)
      closedir(dir);
   else
   {
      printf("\nNo cache directory found!\nPlease create a 'cache' folder in %s", system_directory_c);
      sleep(8);
      exit(0);
   }
}

bool CacheInit(unsigned char* &RomUser, unsigned int RomUser_size)
{
   UINT32 RomCache = 1*MB;
   UINT32 RomGame_size = (16*MB) + (16*MB);
   UINT32 PRG_size = 0;
   char CacheName[1024];
   const char *parentrom = BurnDrvGetTextA(DRV_PARENT);
   const char *drvname   = BurnDrvGetTextA(DRV_NAME);

   // Allocate virtual memory
   RomUser = (UINT8 *)VM_Init(RomUser_size, RomCache);

   // Retrieve the cache directory path
   get_cache_path(CacheDir);

   // Always use parent name for the cache file suffix
   if (!parentrom)
   {
      if (strcmp(drvname, "jojo") == 0 || strcmp(drvname, "jojoba") == 0 || strcmp(drvname, "redearth") == 0 || strcmp(drvname, "sfiii") == 0 ||  strcmp(drvname, "sfiii2") == 0 || strcmp(drvname, "sfiii3") == 0)
         sprintf(ParentName ,"%s", drvname);
   }
   else
   {
      sprintf(ParentName ,"%s", parentrom);
   }

   sprintf(CacheName ,"%sRomUser_%s", CacheDir, ParentName);
   BurnCacheFile = fopen(CacheName, "rb");

   // Check if we need to create the cache files
   if(!BurnCacheFile)
   {
      CreateCache = true;
      size_t RomGame_size = (16*MB) + (16*MB); // sh-2 program roms RomGame + RomGame_D

      if (!strcmp(ParentName, "redearth") || !strcmp(ParentName, "sfiii"))
      {
         PRG_size = 8*MB;
      }
      else
      {
         PRG_size = 16*MB;
      }

      CacheSize = ( (RomUser_size*2 + PRG_size + RomGame_size) / (1*MB) );
      ProgressBar(-1.0, "Please wait, cache files will be written...");
   }
   else
   {
      CacheSize = ( (RomUser_size + (16*MB) + (16*MB)) / (1*MB) );
      CreateCache = false;
   }
   fclose(BurnCacheFile);

   return CreateCache;
}

int CacheHandle(struct CacheInfo* Cache, unsigned int CacheRead, const char* msg, int mode)
{
   UINT8 step = 0;
   char CacheName[1024];
   char txt[1024];
   float Progress = 0.0;
   float BarOffset = 1.0;

   if(mode == SHOW)
   {
      if(msg)
      {
         snprintf(txt, sizeof(txt), msg);
      }
      Progress = ((float)CacheRead) / CacheSize  * 5.0;
      ProgressBar(Progress - BarOffset, txt);
      return 0;
   }

   int fileidx = 0;
   UINT8* Rom;

   // Read/Write the 3 cache files and show the progress bar
   while(fileidx != 3)
   {
      if(fileidx)
         CacheRead += step;

      sprintf(CacheName ,"%s%s_%s", CacheDir, Cache[fileidx].filename, ParentName);

      if(mode == WRITE)
      {
         BurnCacheFile = fopen(CacheName, "wb");
      }
      else if(mode == READ)
      {
         BurnCacheFile = fopen(CacheName, "rb");
      }

      step = Cache[fileidx].filesize;
      Rom = Cache[fileidx].buffer;

      // Read Rom... file in 1MB chunks in order to show progress.
      for(int i = 0; i < step; i++)
      {
         if(mode == WRITE)
         {
            fwrite(Rom + (i*MB), 1*MB, 1, BurnCacheFile);
            snprintf(txt, sizeof(txt), "Writing %s : %d/%d MB", CacheName, i, step);
         }
         else if(mode == READ)
         {
            fread(Rom + (i*MB), 1*MB, 1, BurnCacheFile);
            snprintf(txt, sizeof(txt), "Reading %s : %d/%d MB", CacheName, i, step);
         }
         Progress = ((float)i + CacheRead) / CacheSize  * 5.0;
         ProgressBar(Progress - BarOffset, txt);
      }

      snprintf(txt, sizeof(txt), "%s : %d/%d MB. Done.", CacheName, step, step);
      ProgressBar(Progress - BarOffset, txt);
      fclose(BurnCacheFile);

      fileidx++;
   }

   if(Rom)
      Rom = NULL;

   return 0;
}
#endif
