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

#include <windows.h>
#include <math.h>
#include <strsafe.h>

#include <sstream>
#include <iostream>

#include "in2.h"
#include "mpc_player.h"
#include "resource.h"
#include <mpc/minimax.h>

#include <tag.h>
#include <tfile.h>

const char* mpc_player::GenreList [NO_GENRES] = {
    "", "Blues", "Classic Rock", "Country", "Dance", "Disco", "Funk",
    "Grunge", "Hip-Hop", "Jazz", "Metal", "New Age", "Oldies",
    "Other", "Pop", "R&B", "Rap", "Reggae", "Rock",
    "Techno", "Industrial", "Alternative", "Ska", "Death Metal", "Pranks",
    "Soundtrack", "Euro-Techno", "Ambient", "Trip-Hop", "Vocal", "Jazz+Funk",
    "Fusion", "Trance", "Classical", "Instrumental", "Acid", "House",
    "Game", "Sound Clip", "Gospel", "Noise", "AlternRock", "Bass",
    "Soul", "Punk", "Space", "Meditative", "Instrumental Pop", "Instrumental Rock",
    "Ethnic", "Gothic", "Darkwave", "Techno-Industrial", "Electronic", "Pop-Folk",
    "Eurodance", "Dream", "Southern Rock", "Comedy", "Cult", "Gangsta",
    "Top 40", "Christian Rap", "Pop/Funk", "Jungle", "Native American", "Cabaret",
    "New Wave", "Psychadelic", "Rave", "Showtunes", "Trailer", "Lo-Fi",
    "Tribal", "Acid Punk", "Acid Jazz", "Polka", "Retro", "Musical",
    "Rock & Roll", "Hard Rock", "Folk", "Folk/Rock", "National Folk", "Swing",
    "Fast-Fusion", "Bebob", "Latin", "Revival", "Celtic", "Bluegrass", "Avantgarde",
    "Gothic Rock", "Progressive Rock", "Psychedelic Rock", "Symphonic Rock", "Slow Rock", "Big Band",
    "Chorus", "Easy Listening", "Acoustic", "Humour", "Speech", "Chanson",
    "Opera", "Chamber Music", "Sonata", "Symphony", "Booty Bass", "Primus",
    "Po""rn Groove", "Satire", "Slow Jam", "Club", "Tango", "Samba",
    "Folklore", "Ballad", "Power Ballad", "Rhythmic Soul", "Freestyle", "Duet",
    "Punk Rock", "Drum Solo", "A capella", "Euro-House", "Dance Hall",
    "Goa", "Drum & Bass", "Club House", "Ha""rd""co""re", "Terror",
    "Indie", "BritPop", "NegerPunk", "Polsk Punk", "Beat",
    "Christian Gangsta", "Heavy Metal", "Black Metal", "Crossover", "Contemporary C",
    "Christian Rock", "Merengue", "Salsa", "Thrash Metal", "Anime", "JPop",
    "SynthPop",
};

mpc_player::mpc_player(In_Module * in_mod)
{
	init(in_mod);
}

mpc_player::mpc_player(const char * fn, In_Module * in_mod)
{
	init(in_mod);
	openFile(fn);
}

mpc_player::~mpc_player(void)
{
	closeFile();
}

void mpc_player::init(In_Module * in_mod)
{
	thread_handle=INVALID_HANDLE_VALUE;
	killDecodeThread=0;

	demux = 0;
	mod = in_mod;
	wait_event = 0;
	tag_file = 0;
	lastfn[0] = 0;
	
	TagLib::FileRef fileRef;
	fileRef.file()->useWinAnsiCP(true);
}

int mpc_player::openFile(const char * fn)
{
	if (strcmp(fn, lastfn) == 0)
		return 0;

	closeFile();

    mpc_status err = mpc_reader_init_stdio(&reader, fn);
    if(err < 0) return 1;

    demux = mpc_demux_init(&reader);
	if(!demux) {
		mpc_reader_exit_stdio(&reader);
		return 1;
	}

    mpc_demux_get_info(demux,  &si);
	strcpy(lastfn, fn);
	return 0;
}

void mpc_player::closeFile(void)
{
	if (demux != 0) {
		mpc_demux_exit(demux);
		demux = 0;
		mpc_reader_exit_stdio(&reader);
	}
	if (tag_file != 0) {
		delete tag_file;
		tag_file = 0;
	}
	lastfn[0] = 0;
}

void mpc_player::setOutputTime(int time_in_ms)
{
	seek_offset = time_in_ms;
	if (wait_event) SetEvent(wait_event);
}

void mpc_player::scaleSamples(short * buffer, int len)
{
	for (int i = 0; i < len; i++){
		int tmp = (int) (sample_buffer[i] * (1 << 15));
		tmp = clip(tmp, -(1 << 15), ((1 << 15) - 1));
		buffer[i] = (short) tmp;
	}
}

// FIXME : use 576 samples if advice still valid

// note that if you adjust the size of sample_buffer, for say, 1024
// sample blocks, it will still work, but some of the visualization 
// might not look as good as it could. Stick with 576 sample blocks
// if you can, and have an additional auxiliary (overflow) buffer if 
// necessary.. 


DWORD WINAPI mpc_player::runThread(void * pThis)
{
	return ((mpc_player*)(pThis))->decodeFile();
}

int mpc_player::decodeFile(void)
{
	int done = 0;

	wait_event = CreateEvent(0, FALSE, FALSE, 0);

	while (!killDecodeThread) 
	{
		if (seek_offset != -1) {
			mpc_demux_seek_second(demux, seek_offset / 1000.);
			mod->outMod->Flush(seek_offset);
			decode_pos_sample = (__int64)seek_offset * (__int64)si.sample_freq / 1000;
			seek_offset = -1;
			done = 0;
		}

		if (done) {
			mod->outMod->CanWrite();	// some output drivers need CanWrite
									    // to be called on a regular basis.

			if (!mod->outMod->IsPlaying())  {
				PostMessage(mod->hMainWindow,WM_WA_EOF, 0, 0);
				break;
			}
			WaitForSingleObject(wait_event, 100);		// give a little CPU time back to the system.
		} else if (mod->outMod->CanWrite() >= (int)((MPC_FRAME_LENGTH * sizeof(short) * si.channels)*(mod->dsp_isactive()?2:1))) {
			// CanWrite() returns the number of bytes you can write, so we check that
			// to the block size. the reason we multiply the block size by two if 
			// mod->dsp_isactive() is that DSP plug-ins can change it by up to a 
			// factor of two (for tempo adjustment).

			mpc_frame_info frame;

			frame.buffer = sample_buffer;
			mpc_demux_decode(demux, &frame);

			if(frame.bits == -1) {
				done = 1;
			} else {
				short output_buffer[MPC_FRAME_LENGTH * 2]; // default 2 channels
				int decode_pos_ms = getOutputTime();
				decode_pos_sample += frame.samples;

				scaleSamples(output_buffer, frame.samples * si.channels);
				
				// give the samples to the vis subsystems
				mod->SAAddPCMData((char *)output_buffer, si.channels, sizeof(short) * 8, decode_pos_ms);	
				mod->VSAAddPCMData((char *)output_buffer, si.channels, sizeof(short) * 8, decode_pos_ms);

				// if we have a DSP plug-in, then call it on our samples
				if (mod->dsp_isactive()) 
					frame.samples = mod->dsp_dosamples(output_buffer, frame.samples , sizeof(short) * 8, si.channels, si.sample_freq);

				// write the pcm data to the output system
				mod->outMod->Write((char*)output_buffer, frame.samples * sizeof(short) * si.channels);
			}
		} else WaitForSingleObject(wait_event, 1000);
	}

	CloseHandle(wait_event);
	wait_event = 0;

	return 0;
}

void mpc_player::getFileInfo(char *title, int *length_in_ms)
{
	if (length_in_ms) *length_in_ms = getLength();
	if (title) {
		if (tag_file == 0)
			tag_file = new TagLib::FileRef(lastfn, false);

		if (tag_file->isNull() || !tag_file->tag()) {
			char *p = lastfn + strlen(lastfn);
			while (*p != '\\' && p >= lastfn) p--;
			strcpy(title,++p);
		} else {
			TagLib::Tag *tag = tag_file->tag();
			sprintf(title, "%s - %s", tag->artist().toCString(), tag->title().toCString());
		}
	}
}

// stop playing.
void mpc_player::stop(void)
{ 
	if (thread_handle != INVALID_HANDLE_VALUE) {
		killDecodeThread = 1;
		if (wait_event) SetEvent(wait_event);
		if (WaitForSingleObject(thread_handle,10000) == WAIT_TIMEOUT) {
			MessageBoxA(mod->hMainWindow,"error asking thread to die!\n",
				"error killing decode thread", 0);
			TerminateThread(thread_handle, 0);
		}
		CloseHandle(thread_handle);
		thread_handle = INVALID_HANDLE_VALUE;
	}

	// close output system
	mod->outMod->Close();

	// deinitialize visualization
	mod->SAVSADeInit();
	
	closeFile();
}

int mpc_player::play(char *fn) 
{ 
	int maxlatency;

	paused=0;
	decode_pos_sample = 0;
	seek_offset=-1;

	if (openFile(fn) != 0) return 1;

	// -1 and -1 are to specify buffer and prebuffer lengths.
	// -1 means to use the default, which all input plug-ins should
	// really do.
	maxlatency = mod->outMod->Open(si.sample_freq, si.channels, sizeof(short) * 8, -1,-1);

	// maxlatency is the maxium latency between a outMod->Write() call and
	// when you hear those samples. In ms. Used primarily by the visualization
	// system.
	if (maxlatency < 0) // error opening device
		return 1;

	// dividing by 1000 for the first parameter of setinfo makes it
	// display 'H'... for hundred.. i.e. 14H Kbps.
	mod->SetInfo((int)(si.average_bitrate / 1000), si.sample_freq / 1000, si.channels, 1);

	// initialize visualization stuff
	mod->SAVSAInit(maxlatency, si.sample_freq);
	mod->VSASetInfo(si.sample_freq, si.channels);

	// set the output plug-ins default volume.
	// volume is 0-255, -666 is a token for
	// current volume.
	mod->outMod->SetVolume(-666); 

	// launch decode thread
	killDecodeThread=0;
	thread_handle = CreateThread(0, 0, (LPTHREAD_START_ROUTINE) runThread, this, 0, 0);
	
	return 0; 
}

void mpc_player::writeTags(HWND hDlg)
{
	if (!tag_file->isNull() && tag_file->tag()) {
		TagLib::Tag *tag = tag_file->tag();

		WCHAR buf[2048];

		GetDlgItemTextW(hDlg, IDC_TITLE, buf, 2048);
		tag->setTitle(buf);
		GetDlgItemTextW(hDlg, IDC_ARTIST, buf, 2048);
		tag->setArtist(buf);
		GetDlgItemTextW(hDlg, IDC_ALBUM, buf, 2048);
		tag->setAlbum(buf);
		GetDlgItemTextW(hDlg, IDC_YEAR, buf, 2048);
		TagLib::String year(buf);
		tag->setYear(year.toInt());
		GetDlgItemTextW(hDlg, IDC_TRACK, buf, 2048);
		TagLib::String track(buf);
		tag->setTrack(track.toInt());
		GetDlgItemTextW(hDlg, IDC_GENRE, buf, 2048);
		tag->setGenre(buf);
		GetDlgItemTextW(hDlg, IDC_COMMENT, buf, 2048);
		tag->setComment(buf);

		tag_file->save();
	}
}

void mpc_player::initDlg(HWND hDlg)
{
	if (lastfn[0] == 0) {
		SetDlgItemText(hDlg, IDC_FILE, "Can't open file");
		return;
	}

	std::ostringstream tmp;

	tmp << "Streamversion " << si.stream_version;
	tmp << "\nEncoder : " << si.encoder;
	tmp << "\nProfile : " << si.profile_name;
	tmp << "\nPNS : ";
	if (si.pns) tmp << "on";
	else tmp << "off";
	tmp << "\nGapless : ";
	if (si.is_true_gapless) tmp << "on";
	else tmp << "off";
	tmp << "\nAverage bitrate : " << floor(si.average_bitrate * 1.e-3 + .5) << " Kbps";
	tmp << "\nSamplerate : " << si.sample_freq;
	tmp << "\nChannels : " << si.channels;
	tmp << "\nFile size : " << si.total_file_length << " Bytes";
	tmp << "\nDuration : " << si.samples / (60 * si.sample_freq) << ":" << (si.samples / si.sample_freq) % 60;
	tmp << "\nSamples : " << si.samples;
	tmp << "\nTrack Peak : " << si.peak_title / 256. << " dB";
	tmp << "\nTrack Level : " << si.gain_title / 256. << " dB";
	tmp << "\nAlbum Peak : " << si.peak_album / 256. << " dB";
	tmp << "\nAlbum Level : " << si.gain_album / 256. << " dB";

	SetDlgItemText(hDlg, IDC_STREAM_INFO, tmp.str().c_str());

	SetDlgItemText(hDlg, IDC_FILE, lastfn);

	if (tag_file == 0)
		tag_file = new TagLib::FileRef(lastfn, false);

	if (!tag_file->isNull() && tag_file->tag()) {
		TagLib::Tag *tag = tag_file->tag();
		WCHAR buf[2048];

		MultiByteToWideChar(CP_UTF8, 0, tag->title().toCString(true), -1, buf, 2048);
		SetDlgItemTextW(hDlg, IDC_TITLE, buf);
		MultiByteToWideChar(CP_UTF8, 0, tag->artist().toCString(true), -1, buf, 2048);
		SetDlgItemTextW(hDlg, IDC_ARTIST, buf);
		MultiByteToWideChar(CP_UTF8, 0, tag->album().toCString(true), -1, buf, 2048);
		SetDlgItemTextW(hDlg, IDC_ALBUM, buf);
		tmp.str("");
		tmp << tag->year();
		SetDlgItemText(hDlg, IDC_YEAR, tmp.str().c_str());
		tmp.str("");
		tmp << tag->track();
		SetDlgItemText(hDlg, IDC_TRACK, tmp.str().c_str());
		MultiByteToWideChar(CP_UTF8, 0, tag->genre().toCString(true), -1, buf, 2048);
		SetDlgItemTextW(hDlg, IDC_GENRE, buf);
		MultiByteToWideChar(CP_UTF8, 0, tag->comment().toCString(true), -1, buf, 2048);
		SetDlgItemTextW(hDlg, IDC_COMMENT, buf);
	}

	HWND hGenre = GetDlgItem ( hDlg, IDC_GENRE );
	for ( int n = 0; n < NO_GENRES; n++ )
        SendMessage ( hGenre, CB_ADDSTRING, 0, (LONG)(LPSTR)GenreList[n] );
}

// Message handler for info box.
LRESULT CALLBACK About(HWND hDlg, UINT message, WPARAM wParam, LPARAM lParam)
{
	switch (message)
	{
	case WM_INITDIALOG:
		SetWindowLongPtr(hDlg, DWLP_USER, lParam);
		((mpc_player *) lParam)->initDlg(hDlg);
		return TRUE;

	case WM_COMMAND:
		if (LOWORD(wParam) == IDC_CANCEL || LOWORD(wParam) == IDCANCEL) {
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		} else if (LOWORD(wParam) == IDC_RELOAD) {
			((mpc_player*)GetWindowLongPtr(hDlg, DWLP_USER))->initDlg(hDlg);
		} else if (LOWORD(wParam) == IDC_SAVE) {
			((mpc_player*)GetWindowLongPtr(hDlg, DWLP_USER))->writeTags(hDlg);
			EndDialog(hDlg, LOWORD(wParam));
			return TRUE;
		} else if (LOWORD(wParam) == IDC_LOGO) {
			ShellExecute( hDlg, "open", "http://www.musepack.net", NULL, NULL, SW_NORMAL);
		}
		break;
	}
	return FALSE;
}

int mpc_player::infoDlg(HWND hwnd)
{
	DialogBoxParam(mod->hDllInstance, (LPCTSTR)IDD_INFO_BOX, hwnd, (DLGPROC)About, (LPARAM) this);
	return 0;
}

int mpc_player::getExtendedFileInfo(const char *data, char *dest, int destlen )
{
	if (!stricmp(data, "length")) {
		StringCchPrintfA(dest, destlen, "%u", getLength());
	} else if (!stricmp(data, "bitrate")) {
		StringCchPrintfA(dest, destlen, "%u", (unsigned int)(si.average_bitrate/1000.));
	} else if (!stricmp(data, "replaygain_album_gain"))	{
		if (si.gain_album)
			StringCchPrintfA(dest, destlen, "%-+.2f dB", 64.82f - si.gain_album / 256.f);
	} else if (!stricmp(data, "replaygain_album_peak"))	{
		if (si.peak_album)
			StringCchPrintfA(dest, destlen, "%-.9f", (float)((1 << 15) / pow(10, si.peak_album / (20 * 256))));
	} else if (!stricmp(data, "replaygain_track_gain"))	{
		if (si.gain_title)
			StringCchPrintfA(dest, destlen, "%-+.2f dB", 64.82f - si.gain_title / 256.f);
	} else if (!stricmp(data, "replaygain_track_peak"))	{		
		if (si.peak_title)
			StringCchPrintfA(dest, destlen, "%-.9f", (float)((1 << 15) / pow(10, si.peak_title / (20 * 256))));
	} else {

		if (tag_file == 0)
			tag_file = new TagLib::FileRef(lastfn, false);

		if (!tag_file->isNull() && tag_file->tag()) {
			TagLib::Tag *tag = tag_file->tag();
			WCHAR buf[2048];

			if (!stricmp(data, "title"))
				MultiByteToWideChar(CP_UTF8, 0, tag->title().toCString(true), -1, buf, 2048);
			else if (!stricmp(data, "artist"))
				MultiByteToWideChar(CP_UTF8, 0, tag->artist().toCString(true), -1, buf, 2048);
			else if (!stricmp(data, "album"))
				MultiByteToWideChar(CP_UTF8, 0, tag->album().toCString(true), -1, buf, 2048);
			else if (!stricmp(data, "comment"))
				MultiByteToWideChar(CP_UTF8, 0, tag->comment().toCString(true), -1, buf, 2048);
			else if (!stricmp(data, "genre"))
				MultiByteToWideChar(CP_UTF8, 0, tag->genre().toCString(true), -1, buf, 2048);
			else if (!stricmp(data, "trackno")) {
				StringCchPrintfA(dest, destlen, "%u", tag->track());
				return 1;
			} else if (!stricmp(data, "year")) {
				StringCchPrintfA(dest, destlen, "%u", tag->year());
				return 1;
			}

			WideCharToMultiByte(CP_ACP, 0, buf, -1, dest, destlen, NULL, NULL);
		} else
			return 0;
	}
	return 1;
}