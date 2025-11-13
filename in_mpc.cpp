/*
	Copyright (C) 2006-2007 Nicolas BOTTI <rududu at laposte.net>
	This file is part of the Musepack Winamp plugin.

	This library is free software; you can redistribute it and/or
	modify it under the terms of the GNU Lesser General Public
	License as published by the Free Software Foundation; either
	version 2.1 of the License, or (at your option) any later version.

	This library is distributed in the hope that it will be useful,
	but WITHOUT ANY WARRANTY; without even the implied warranty of
	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
	Lesser General Public License for more details.

	You should have received a copy of the GNU Lesser General Public
	License along with this library; if not, write to the Free Software
	Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#define PLUGIN_VER L"2.3.7"

#include <windows.h>
#include <stdlib.h>
#include <shlwapi.h>
#include <strsafe.h>
#include <winamp/in2.h>
#include <nu/servicebuilder.h>
#include "mpc_player.h"
#include "api.h"
#include "resource.h"
#include <loader/loader/paths.h>
#include <loader/loader/utils.h>

// TODO add to lang.h
// {4A4C2530-521A-4d59-8568-A8303362C8A8}
static const GUID InMPCLangGUID =
{ 0x4a4c2530, 0x521a, 0x4d59, { 0x85, 0x68, 0xa8, 0x30, 0x33, 0x62, 0xc8, 0xa8 } };

void about(HWND hwndParent);
/*int infoDlg(const in_char *fn, HWND hwnd);*/

int init(void);
void quit(void);
int play(const in_char *fn);
void stop(void);

void pause(void);
void unpause(void);
int ispaused(void);

//int isourfile(const in_char *fn);
int getlength(void);
int getoutputtime(void);
void getfileinfo(const in_char *filename, in_char *title, int *length_in_ms);
void setoutputtime(const int time_in_ms);

void setvolume(int volume);
void setpan(int pan);

void GetFileExtensions(void);

// wasabi based services for localisation support
SETUP_API_LNG_VARS;

api_metadata2 *WASABI_API_METADATA = 0;
mpc_player *player = NULL, *info_player = NULL;
CRITICAL_SECTION g_info_cs = { 0 };
FILETIME ftLastWriteTime = { 0 };

// module definition.
In_Module plugin = 
{
	IN_VER_WACUP, // defined in IN2.H
	"Musepack winamp plugin",
	0, // hMainWindow (filled in by winamp)
	0, // hDllInstance (filled in by winamp)
	NULL,//"mpc\0Musepack Audio File (*.mpc)\0mp+\0Musepack Audio File (*.mp+)\0"
	1, // is_seekable
	1, // uses output plug-in system
	NULL,
	about,
	init,
	quit,
	getfileinfo,
	0/*infoDlg*/,
	0/*isourfile*/,
	play,
	pause,
	unpause,
	ispaused,
	stop,
	
	getlength,
	getoutputtime,
	setoutputtime,

	setvolume,
	setpan,
#if 0
	0,0,0,0,0,0,0,0,0, // visualization calls filled in by winamp

	0,0, // dsp calls filled in by winamp

	eq_set,

	0, // setinfo call filled in by winamp

	0 // out_mod filled in by winamp
#endif
	IN_INIT_VIS_RELATED_CALLS,
	0,
	0,
	IN_INIT_WACUP_EQSET_EMPTY
	0,
	0,		// out_mod,
	NULL,	// api_service
	INPUT_HAS_READ_META | INPUT_HAS_WRITE_META |
	INPUT_USES_UNIFIED_ALT3 |
	INPUT_HAS_FORMAT_CONVERSION_UNICODE |
	INPUT_HAS_FLOATING_POINT_FORMAT_CONVERSION |
	INPUT_HAS_FORMAT_CONVERSION_SET_TIME_MODE,
	GetFileExtensions,	// loading optimisation
	IN_INIT_WACUP_END_STRUCT
};

void GetFileExtensions(void)
{
	if (!plugin.FileExtensions)
	{
		wchar_t buffer[48];
		size_t description_len = 0;
		LPCWSTR description = LngStringCopyGetLen(IDS_MPC_FILES, buffer,
								   ARRAYSIZE(buffer), &description_len);
		plugin.FileExtensions = BuildInputFileListString(L"MPC;MP+", 7,
										 description, description_len);
	}
}

void about(HWND hwndParent)
{
	wchar_t message[1024]/* = { 0 }*/;
	PrintfCch(message, ARRAYSIZE(message), LangString(IDS_ABOUT_TEXT),
			  plugin.description, WACUP_Copyright(), __DATE__, L"r495");
	AboutMessageBox(hwndParent, message, (LPCWSTR)LangString(IDS_ABOUT_TITLE));
}

int init(void)
{
	StartPluginLangWithDesc(plugin.hDllInstance, InMPCLangGUID, IDS_PLUGIN_NAME,
											   PLUGIN_VER, &plugin.description);

	InitializeCriticalSectionEx(&g_info_cs, 400, CRITICAL_SECTION_NO_DEBUG_INFO);

	return IN_INIT_SUCCESS;
}

void quit(void)
{ 
	if (player != NULL)
	{
		delete player;
		player = NULL;
	}

	ServiceRelease(plugin.service, WASABI_API_METADATA, api_metadata2GUID);

	DeleteCriticalSection(&g_info_cs);
}

/*int isourfile(const in_char *fn)
{ 
	return 0; 
}*/

// called when winamp wants to play a file
int play(const in_char *fn)
{
	if (player == NULL)
	{
		player = new mpc_player();
	}

	if (player != NULL)
	{
		return player->play(fn);
	}
	return 1;
}

void stop(void)
{
	if (player != NULL)
	{
		player->stop();
	}
}

void pause(void)
{
	if (player != NULL)
	{
		player->paused = 1;

		if (plugin.outMod)
		{
			plugin.outMod->Pause(1);
		}
	}
}

void unpause(void)
{
	if (player != NULL)
	{
		player->paused = 0;

		if (plugin.outMod)
		{
			plugin.outMod->Pause(0);
		}
	}
}

int ispaused(void)
{ 
	return ((player != NULL) ? player->paused : 0);
}

void setvolume(int volume)
{
	if (plugin.outMod && plugin.outMod->SetVolume)
	{
		plugin.outMod->SetVolume(volume);
	}
}

void setpan(int pan)
{
	if (plugin.outMod && plugin.outMod->SetPan)
	{
		plugin.outMod->SetPan(pan);
	}
}

int getlength(void)
{
	if (player == NULL)
	{
		player = new mpc_player();
	}
	return ((player != NULL) ? player->getLength() : 0);
}

// returns current output position, in ms.
int getoutputtime(void)
{
	return (plugin.outMod ? plugin.outMod->GetOutputTime() : 0);
}

// called when the user releases the seek scroll bar.
// usually we use it to set seek_needed to the seek
// point (seek_needed is -1 when no seek is needed)
// and the decode thread checks seek_needed.
void setoutputtime(const int time_in_ms)
{
	if (player != NULL)
	{
		player->setOutputTime(time_in_ms);
	}
}

// this gets called when the use hits Alt+3 to get the file info.
// if you need more info, ask me :)
#if 0
int infoDlg(const in_char *fn, HWND hwnd)
{
	// TODO
	/*mpc_player info_play(fn, &mod);
	return info_play.infoDlg(hwnd);
	//#define INFOBOX_EDITED 0
	//#define INFOBOX_UNCHANGED 1*/
	return INFOBOX_UNCHANGED;
}
#endif

// this is an odd function. it is used to get the title and/or
// length of a track.
// if filename is either NULL or of length 0, it means you should
// return the info of lastfn. Otherwise, return the information
// for the file in filename.
// if title is NULL, no title is copied into it.
// if length_in_ms is NULL, no length is copied into it.
void getfileinfo(const in_char *filename, in_char *title, int *length_in_ms)
{
	if (title)
	{
		char _title[2048]/* = { 0 }*/;
		_title[0] = 0;

		EnterCriticalSection(&g_info_cs);

		const bool playing = (!filename || !*filename || ((player != NULL) &&
								   SameStr(player->getFilename(), filename)));

		LeaveCriticalSection(&g_info_cs);

		if (playing)
		{
			if (player == NULL)
			{
				player = new mpc_player();

				if (player != NULL)
				{
					player->openFile(filename);
				}
			}

			if (player != NULL)
			{
				player->getFileInfo(_title, length_in_ms);
			}
		}
		else
		{
			mpc_player* info_play = new mpc_player(filename);
			if (info_play != NULL)
			{
				info_play->getFileInfo(_title, length_in_ms);
				delete info_play;
			}
		}

		if (_title[0] != 0)
		{
		/*CopyCchStr(title, 2048, AutoWide(_title));/*/
		PrintfCch(title, 2048, L"%S", _title);/**/
		}
	}
}

const bool getMetadataSvc(void)
{
	if (WASABI_API_METADATA == NULL) {
		ServiceBuild(plugin.service, WASABI_API_METADATA, api_metadata2GUID);
	}
	return (WASABI_API_METADATA != NULL);
}

// exported symbol. Returns output module.
extern "C" __declspec(dllexport) In_Module * winampGetInModule2(void)
{
	return &plugin;
}

extern "C" __declspec(dllexport) int winampUninstallPlugin(HINSTANCE hDllInst, HWND hwndDlg, int param)
{
	// prompt to remove our settings with default as no (just incase)
	/*if (UninstallSettingsPrompt(reinterpret_cast<const wchar_t*>(plugin.description)))
	{
		SaveNativeIniString(WINAMP_INI, L"in_flac", 0, 0);
	}*/

	// we should be good to go now
	return IN_PLUGIN_UNINSTALL_NOW;
}

// return 1 if you want winamp to show it's own file info dialogue, 0 if you want to show your own (via In_Module.InfoBox)
// if returning 1, remember to implement winampGetExtendedFileInfo("formatinformation")!
extern "C" __declspec(dllexport) int winampUseUnifiedFileInfoDlg(const wchar_t * fn)
{
	return 1;
}

// should return a child window of 513x271 pixels (341x164 in msvc dlg units), or return NULL for no tab.
// Fill in name (a buffer of namelen characters), this is the title of the tab (defaults to "Advanced").
// filename will be valid for the life of your window. n is the tab number. This function will first be 
// called with n == 0, then n == 1 and so on until you return NULL (so you can add as many tabs as you like).
// The window you return will recieve WM_COMMAND, IDOK/IDCANCEL messages when the user clicks OK or Cancel.
// when the user edits a field which is duplicated in another pane, do a SendMessage(GetParent(hwnd),WM_USER,(WPARAM)L"fieldname",(LPARAM)L"newvalue");
// this will be broadcast to all panes (including yours) as a WM_USER.
extern "C" __declspec(dllexport) HWND winampAddUnifiedFileInfoPane(int n, const wchar_t *filename, HWND parent, wchar_t *name, size_t namelen)
{
	/*if (n == 0)
	{
		// add first pane
		g_info = new FLACInfo;
		if (g_info)
		{
			SetProp(parent, L"INBUILT_NOWRITEINFO", (HANDLE)1);

			// TODO localise
			CopyCchStr(name, namelen, L"Vorbis Comment Tag");

			g_info->filename = filename;
			g_info->metadata.Open(filename, true);
			return LangCreateDialog(IDD_INFOCHILD_ADVANCED, parent,
									ChildProc_Advanced, (LPARAM)g_info);
		}
	}*/
	return NULL;
}

extern "C" __declspec(dllexport) int winampGetExtendedFileInfoW(const wchar_t *fn, const char *data,
																wchar_t *dest, size_t destlen)
{
	if (SameStrA(data, "type") ||
		SameStrA(data, "lossless") ||
		SameStrA(data, "streammetadata"))
	{
		dest[0] = L'0';
		dest[1] = L'\0';
		return 1;
	}
    else if (SameStrNA(data, "stream", 6) &&
             (SameStrA((data + 6), "type") ||
              SameStrA((data + 6), "genre") ||
              SameStrA((data + 6), "url") ||
              SameStrA((data + 6), "name") ||
			  SameStrA((data + 6), "title")))
    {
        return 0;
    }

	if (!fn || !fn[0])
	{
		return 0;
	}

	if (SameStrA(data, "family"))
	{
		LPCWSTR e = FindPathExtension(fn);
		if ((e != NULL) && (SameStr(e, L"MPC") || SameStr(e, L"MP+")))
		{
			LngStringCopy(IDS_FAMILY_STRING, dest, destlen);
			return 1;
		}
		return 0;
	}

	const bool reset = SameStrA(data, "reset");
	// this might sometimes mess up so we'll see if what's
	// being requested is a reset & if it is then we'll do
	// a check to see if something else has the lock to do
	// a quick bail to try to avoid a hang related failure
	if (reset)
	{
		if (!TryEnterCriticalSection(&g_info_cs))
		{
			return 0;
		}
	}
	else
	{
		EnterCriticalSection(&g_info_cs);
	}

	if (reset || HasFileTimeChanged(fn, &ftLastWriteTime) ||
		!info_player || !info_player->getFilename() ||
		!SameStr(fn, info_player->getFilename()))
	{
		if (info_player != NULL)
		{
			delete info_player;
			info_player = NULL;
		}

		if (!reset)
		{
			info_player = new mpc_player(fn);
		}
	}

	int ret = 0;
	if (info_player != NULL)
	{
		ret = info_player->getExtendedFileInfo(data, dest, (int)destlen);
	}
	LeaveCriticalSection(&g_info_cs);
	return ret;
}

static void *save_token;
wchar_t* setFn = 0;
extern "C" __declspec(dllexport) int winampSetExtendedFileInfoW(const wchar_t *filename, const char *data, wchar_t *val)
{
	if (!SameStr(filename, setFn))
	{
		setFn = SafeWideDupFreeOld(filename, setFn);
	}

	return (getMetadataSvc() ? WASABI_API_METADATA->SetExtendedFileInfo(filename, data, val,
																	&save_token, true) : 0);
}

extern "C" int __declspec (dllexport) winampWriteExtendedFileInfo(void)
{
	if (getMetadataSvc())
	{
		const int ret = WASABI_API_METADATA->WriteExtendedFileInfo(&save_token);

		EnterCriticalSection(&g_info_cs);

		if (info_player != NULL)
		{
			delete info_player;
			info_player = NULL;
		}

		// update last modified so we're not re-queried on our own updates
		UpdateFileTimeChanged(setFn, &ftLastWriteTime);

		SafeFree(setFn);
		setFn = 0;

		LeaveCriticalSection(&g_info_cs);

		return ret;
	}
	return 0;
}