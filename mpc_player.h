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

extern In_Module plugin;

class mpc_player
{
public:
	explicit mpc_player(void);
	mpc_player(const wchar_t * fn);
	~mpc_player(void);

	int openFile(const wchar_t * fn);
	int play(const wchar_t *fn);
	void stop(void);

	void getFileInfo(char *title, int *length_in_ms);
	int getExtendedFileInfo(const char *data, wchar_t *dest, const int destlen);
	int getLength(void) {return (int)(mpc_streaminfo_get_length(&si) * 1000);}
	//int getOutputTime(void) {return (int)(decode_pos_sample * 1000 / si.sample_freq);}

	void setOutputTime(const int time_in_ms);

	void writeTags(HWND hDlg);

	int open(const wchar_t * fn, int *size, int *bps,
			 int *nch, int *srate, bool useFloat);
	int decode(char *dest, const size_t len);

	const wchar_t* getFilename(void) { return lastfn; }

	int paused;				// are we paused?

private:
	bool reentrant, already_tried;
	mpc_uint16_t output_bits;

	void *token;
	wchar_t *lastfn;	// currently playing file (used for getting info on the current file)
	mpc_streaminfo si;
	mpc_reader reader;
	mpc_demux* demux;

	MPC_SAMPLE_FORMAT* sample_buffer;

	volatile int seek_offset; // if != -1, it is the point that the decode 
							  // thread should seek to, in ms.
	volatile int killDecodeThread;	// the kill switch for the decode thread

	mpc_uint32_t wanted_channels;
	mpc_uint32_t output_channels;

	HANDLE thread_handle;	// the handle to the decode thread

	static DWORD WINAPI runThread(void * pThis);
	int decodeFile(void);
	void closeFile(void);

	void init(void);

	const size_t scaleSamples(short * buffer, int len);
};
