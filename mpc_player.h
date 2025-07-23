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

#pragma once

#include <mpc/mpcdec.h>
#include <fileref.h>

// post this to the main window at end of file (after playback as stopped)
#define WM_WA_EOF WM_USER+2

#define NO_GENRES 149

class mpc_player
{
public:
	mpc_player(In_Module * in_mod);
	mpc_player(const char * fn, In_Module * in_mod);
	~mpc_player(void);

	int openFile(const char * fn);
	int play(char *fn);
	void stop(void);

	void getFileInfo(char *title, int *length_in_ms);
	int getExtendedFileInfo(const char *data, char *dest, int destlen);
	int getLength(void) {return (int)(mpc_streaminfo_get_length(&si) * 1000);}
	int getOutputTime(void) {return (int)(decode_pos_sample * 1000 / si.sample_freq);}

	void setOutputTime(int time_in_ms);

	int infoDlg(HWND hwnd);
	void initDlg(HWND hDlg);
	void writeTags(HWND hDlg);

	int paused;				// are we paused?

private:
	char lastfn[MAX_PATH];	// currently playing file (used for getting info on the current file)
	static const char* GenreList[NO_GENRES];
	mpc_streaminfo si;
	mpc_reader reader;
	mpc_demux* demux;
	TagLib::FileRef * tag_file;

    MPC_SAMPLE_FORMAT sample_buffer[MPC_DECODER_BUFFER_LENGTH];

	__int64 decode_pos_sample; // decoding position in samples;
	volatile int seek_offset; // if != -1, it is the point that the decode 
							  // thread should seek to, in ms.
	volatile int killDecodeThread;	// the kill switch for the decode thread

	HANDLE thread_handle;	// the handle to the decode thread
	HANDLE wait_event;

	In_Module * mod;

	static DWORD WINAPI runThread(void * pThis);
	int decodeFile(void);
	void closeFile(void);

	void init(In_Module * in_mod);

	void scaleSamples(short * buffer, int len);
};