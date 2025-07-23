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

#include <winamp/in2.h>
#include <winamp/wa_ipc.h>
#include <nu/autowide.h>
#include "mpc_player.h"
#include "api.h"
#include "resource.h"
#include <mpc/minimax.h>
#include <loader/loader/utils.h>

extern const bool getMetadataSvc(void);

/*#include <tag.h>
#include <tfile.h>*/

mpc_player::mpc_player(void)
{
	init();
}

mpc_player::mpc_player(const wchar_t *fn)
{
	init();
	openFile(fn);
}

mpc_player::~mpc_player(void)
{
	closeFile();

	if (sample_buffer != NULL)
	{
		SafeFree(sample_buffer);
		sample_buffer = NULL;
	}
}

void mpc_player::init(void)
{
	thread_handle = INVALID_HANDLE_VALUE;
	killDecodeThread = 0;

	paused = 0;
	decode_pos_sample = 0;
	seek_offset = -1;

	demux = 0;
	wait_event = 0;
	token = NULL;
	lastfn = NULL;

	wanted_channels = 2;
	output_channels = 2;

	output_bits = 16;

	reentrant = already_tried = false;

	sample_buffer = (MPC_SAMPLE_FORMAT*)SafeMalloc(sizeof(MPC_SAMPLE_FORMAT) * MPC_DECODER_BUFFER_LENGTH);
}

int mpc_player::open(const wchar_t * fn, int *size, int *bps, int *nch, int *srate, bool useFloat)
{
	const int ret = openFile(fn);
	if (!ret)
	{
		*bps = (const int)plugin.config->GetUnsigned(
			   playbackConfigGroupGUID, L"bits", 16);
		output_bits = (mpc_uint16_t)*bps;
		*nch = si.channels;
		*srate = si.sample_freq;
		*size = (int)(si.samples * ((*bps) / 8) * si.channels);
	}
	return ret;
}

int mpc_player::openFile(const wchar_t * fn)
{
	if (SameStr(fn, lastfn)) {
		return 0;
	}

	closeFile();

    if(mpc_reader_init_stdio_w(&reader, fn) < 0) return 1;

    demux = mpc_demux_init(&reader);
	if(!demux) {
		mpc_reader_exit_stdio(&reader);
		return 1;
	}

    mpc_demux_get_info(demux, &si);

	lastfn = SafeWideDupFreeOld(fn, lastfn);

	wanted_channels = ((!plugin.config->GetUnsigned(playbackConfigGroupGUID,
												  L"mono", false) ? 2 : 1));
	return 0;
}

void mpc_player::closeFile(void)
{
	if (demux != 0) {
		mpc_demux_exit(demux);
		demux = 0;
		mpc_reader_exit_stdio(&reader);
	}

	if (lastfn != NULL)
	{
		SafeFree(lastfn);
		lastfn = NULL;
	}

	if (getMetadataSvc())
	{
		WASABI_API_METADATA->FreeExtendedFileInfoToken(&token);
	}
}

void mpc_player::setOutputTime(const int time_in_ms)
{
	seek_offset = time_in_ms;
	if (wait_event) SetEvent(wait_event);
}

const size_t mpc_player::scaleSamples(short * buffer, int num_samples)
{
	if (((wanted_channels == 1) && (wanted_channels != si.channels)))
	{
		// try to deal with the playback option to force mono playback
		// so if we're not already providing mono playback then we'll
		// need convert what's been generated into a valid merged mono
		num_samples /= 2;

		for (int i = 0, index = 0; i < num_samples; i++, index++)
		{
			const int pos = (i * 2);
			sample_buffer[index] = ((sample_buffer[pos] + sample_buffer[(pos + 1)]) / 2.f);
		}
	}

	FloatToIntInterleaved(buffer, sample_buffer, output_bits, num_samples);

	return (size_t)(num_samples * (output_bits / 8) * output_channels);
}

DWORD WINAPI mpc_player::runThread(void * pThis)
{
	return ((mpc_player*)(pThis))->decodeFile();
}

int mpc_player::decodeFile(void)
{
	int done = 0;

	wait_event = CreateEvent(0, FALSE, FALSE, 0);

	while (wait_event && !killDecodeThread)
	{
		if (seek_offset != -1) {
			mpc_demux_seek_second(demux, seek_offset / 1000.);
			plugin.outMod->Flush(seek_offset);
			decode_pos_sample = (__int64)seek_offset * (__int64)si.sample_freq / 1000;
			seek_offset = -1;
			done = 0;
		}

		if (done) {
			plugin.outMod->CanWrite();	// some output drivers need CanWrite
									    // to be called on a regular basis.

			if (!plugin.outMod->IsPlaying())  {
				PostEOF();
				break;
			}
			WaitForSingleObject(wait_event, 100);		// give a little CPU time back to the system.
		} else if (plugin.outMod->CanWrite() >= (int)((MPC_FRAME_LENGTH * (output_bits / 8) *
												output_channels)*(plugin.dsp_isactive()?2:1))) {
			// CanWrite() returns the number of bytes you can write, so we check that
			// to the block size. the reason we multiply the block size by two if 
			// mod->dsp_isactive() is that DSP plug-ins can change it by up to a 
			// factor of two (for tempo adjustment).

			mpc_frame_info frame = { 0 };
			frame.buffer = sample_buffer;
			mpc_demux_decode(demux, &frame);

			if(frame.bits == -1) {
				done = 1;
			} else {
				short output_buffer[MPC_FRAME_LENGTH * 4] = { 0 }; // default 2 channels
				int decode_pos_ms = getOutputTime();
				decode_pos_sample += frame.samples;

				scaleSamples(output_buffer, frame.samples * si.channels);
				
				// give the samples to the vis subsystems
				plugin.SAAddPCMData((char *)output_buffer, output_channels, output_bits, decode_pos_ms);
				/*plugin.VSAAddPCMData((char *)output_buffer, output_channels, output_bits, decode_pos_ms);*/

				// if we have a DSP plug-in, then call it on our samples
				if (plugin.dsp_isactive())
					frame.samples = plugin.dsp_dosamples(output_buffer, frame.samples, output_bits, output_channels, si.sample_freq);

				// write the pcm data to the output system
				plugin.outMod->Write((char*)output_buffer, frame.samples * (output_bits / 8) * output_channels);
			}
		} else WaitForSingleObject(wait_event, 10);
	}

	if (wait_event)
	{
		CloseHandle(wait_event);
		wait_event = 0;
	}
	return 0;
}

int mpc_player::decode(char *dest, const size_t len)
{
	mpc_frame_info frame = { 0 };
	frame.buffer = sample_buffer;
	mpc_demux_decode(demux, &frame);

	if (frame.bits != -1) {
		short output_buffer[MPC_FRAME_LENGTH * 4] = { 0 }; // default 2 channels
		decode_pos_sample += frame.samples;
		const size_t size = scaleSamples(output_buffer, frame.samples * si.channels);
		memcpy(dest, output_buffer, min(len, size));
		return (int)min(len, size);
	}

	return 0;
}

void mpc_player::getFileInfo(char *title, int *length_in_ms)
{
	if (length_in_ms) *length_in_ms = getLength();
	/*if (title) {
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
	}*/
}

// stop playing.
void mpc_player::stop(void)
{ 
	if (CheckThreadHandleIsValid(&thread_handle)) {/*/
	if (thread_handle != INVALID_HANDLE_VALUE) {/**/
		killDecodeThread = 1;
		if (wait_event) SetEvent(wait_event);
#if 1
		WaitForThreadToClose(&thread_handle, 10000);
#else
		if (WaitForSingleObject(thread_handle,10000) == WAIT_TIMEOUT) {
			/*MessageBoxW(mod->hMainWindow,L"error asking thread to die!\n",
						  L"error killing decode thread", 0);*/
			TerminateThread(thread_handle, 0);
		}
#endif
		if (thread_handle != INVALID_HANDLE_VALUE) {
			CloseHandle(thread_handle);
			thread_handle = INVALID_HANDLE_VALUE;
		}
	}

	// close output system
	if (plugin.outMod && plugin.outMod->Close)
	{
		plugin.outMod->Close();
	}

	// deinitialize visualization
	if (plugin.outMod)
	{
		plugin.SAVSADeInit();
	}
	
	closeFile();
}

int mpc_player::play(const wchar_t *fn) 
{ 
	paused=0;
	decode_pos_sample = 0;
	seek_offset=-1;

	if (openFile(fn) != 0)
		return 1;

	output_bits = (mpc_uint16_t)plugin.config->GetUnsigned(
					 playbackConfigGroupGUID, L"bits", 16);

	output_channels = si.channels;
	if ((wanted_channels == 1) && (wanted_channels != output_channels))
	{
		output_channels = 1;
	}

	// -1 and -1 are to specify buffer and prebuffer lengths.
	// -1 means to use the default, which all input plug-ins should
	// really do.
	const int maxlatency = (plugin.outMod && plugin.outMod->Open && si.sample_freq &&
							output_channels ? plugin.outMod->Open(si.sample_freq,
							output_channels, output_bits, -1,-1) : -1);

	// maxlatency is the maxium latency between a outMod->Write() call and
	// when you hear those samples. In ms. Used primarily by the visualization
	// system.
	if (maxlatency < 0) // error opening device
		return 1;

	// dividing by 1000 for the first parameter of setinfo makes it
	// display 'H'... for hundred.. i.e. 14H Kbps.
	plugin.SetInfo((int)(si.average_bitrate / 1000), si.sample_freq / 1000, output_channels, 1);

	// initialize visualization stuff
	plugin.SAVSAInit(maxlatency, si.sample_freq);
	plugin.VSASetInfo(si.sample_freq, output_channels);

	// set the output plug-ins default volume.
	// volume is 0-255, -666 is a token for
	// current volume.
	plugin.outMod->SetVolume(-666);

	// launch decode thread
	killDecodeThread=0;
	thread_handle = StartThread(runThread, this, static_cast<int>(plugin.config->
									 GetInt(playbackConfigGroupGUID, L"priority",
											THREAD_PRIORITY_HIGHEST)), 0, NULL);
	return ((thread_handle != NULL) ? 0 : 1);
}

void mpc_player::writeTags(HWND hDlg)
{
	/*if (!tag_file->isNull() && tag_file->tag()) {
		TagLib::Tag *tag = tag_file->tag();

		WCHAR buf[2048];

		GetDlgItemText(hDlg, IDC_TITLE, buf, 2048);
		tag->setTitle(buf);
		GetDlgItemText(hDlg, IDC_ARTIST, buf, 2048);
		tag->setArtist(buf);
		GetDlgItemText(hDlg, IDC_ALBUM, buf, 2048);
		tag->setAlbum(buf);
		GetDlgItemText(hDlg, IDC_YEAR, buf, 2048);
		TagLib::String year(buf);
		tag->setYear(year.toInt());
		GetDlgItemText(hDlg, IDC_TRACK, buf, 2048);
		TagLib::String track(buf);
		tag->setTrack(track.toInt());
		GetDlgItemText(hDlg, IDC_GENRE, buf, 2048);
		tag->setGenre(buf);
		GetDlgItemText(hDlg, IDC_COMMENT, buf, 2048);
		tag->setComment(buf);

		tag_file->save();
	}*/
}

int mpc_player::getExtendedFileInfo(const char *data, wchar_t *dest, const int destlen )
{
	if (SameStrA(data, "length")) {
		PrintfCch(dest, destlen, L"%u", getLength());
	} else if (SameStrA(data, "bitrate")) {
		PrintfCch(dest, destlen, L"%u", (unsigned int)(si.average_bitrate/1000.));
	} else if (SameStrA(data, "samplerate")) {
		PrintfCch(dest, destlen, L"%u", si.sample_freq);
	} else if (SameStrA(data, "bitdepth")) {
		// TODO
		dest[0] = L'-';
		dest[1] = L'1';
		dest[2] = 0;
	} else if (SameStrA(data, "formatinformation")) {
		const int time = (int)mpc_streaminfo_get_length(&si),
				  minutes = (time > 0 ? (time / 60) : 0),
				  seconds = (time > 0 ? (time % 60) : 0);

		wchar_t on_str[16]/* = { 0 }*/, off_str[16]/* = { 0 }*/, unknown_str[16]/* = { 0 }*/;
		LngStringCopy(IDS_ON, on_str, ARRAYSIZE(on_str));
		LngStringCopy(IDS_OFF, off_str, ARRAYSIZE(off_str));
		LngStringCopy(IDS_UNKNOWN, unknown_str, ARRAYSIZE(unknown_str));
		PrintfCch(dest, destlen, LangString(IDS_FORMAT_INFO),
				  si.stream_version, minutes, seconds, si.channels,
				  (si.average_bitrate > 0.0 ? (si.average_bitrate / 1000.) : 0.0),
				  si.sample_freq, (mpc_uint32_t)mpc_streaminfo_get_length_samples(&si),
				  si.encoder, si.profile_name, (si.profile - 5), ((si.pns == 0xFF) ? unknown_str :
				  (si.pns ? on_str : off_str)), (si.ms ? on_str : off_str),
				  (si.is_true_gapless ? on_str : off_str));

	} else if (SameStrA(data, "replaygain_album_gain"))	{
		if (si.gain_album) {
			PrintfCch(dest, destlen, L"%-+.2f dB",
					  64.82f - si.gain_album / 256.f);
		}
	} else if (SameStrA(data, "replaygain_album_peak"))	{
		if (si.peak_album) {
			PrintfCch(dest, destlen, L"%-.9f", (float)((1 << 15) /
								pow(10., si.peak_album / 5120.)));
		}
	} else if (SameStrA(data, "replaygain_track_gain"))	{
		if (si.gain_title) {
			PrintfCch(dest, destlen, L"%-+.2f dB",
					  64.82f - si.gain_title / 256.f);
		}
	} else if (SameStrA(data, "replaygain_track_peak"))	{
		if (si.peak_title) {
			PrintfCch(dest, destlen, L"%-.9f", (float)((1 << 15) /
								pow(10., si.peak_title / 5120.)));
		}
	} else if (lastfn) {
		const AutoWide metadata(data);
		return (getMetadataSvc() ? !!WASABI_API_METADATA->GetExtendedFileInfo(lastfn,
				L"MPC"/*this can just be hard-coded as we don't really need to know
				if it's MP+ as the metadata core doesn't need to know about that...*/,
				metadata, dest, destlen, &token, true, &reentrant, &already_tried) : 0);
	}
	return 1;
}

intptr_t create_mpc_decoder(const wchar_t* fn, int* size, int* bps,
							int* nch, int* srate, const bool _float)
{
	mpc_player* mpc = new mpc_player();
	if (mpc != NULL)
	{
		if (!mpc->open(fn, size, bps, nch, srate, _float))
		{
			return (intptr_t)mpc;
		}
		delete mpc;
	}
	return 0;
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW(const wchar_t *fn, int *size,
																	  int *bps, int *nch, int *srate)
{
	return create_mpc_decoder(fn, size, bps, nch, srate, false);
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_openW_float(const wchar_t *fn, int *size,
																			int *bps, int *nch, int *srate)
{
	return create_mpc_decoder(fn, size, bps, nch, srate, true);
}

extern "C" __declspec(dllexport) intptr_t winampGetExtendedRead_getData(intptr_t handle, char *dest,
																		size_t len, int *killswitch)
{
	mpc_player *mpc = (mpc_player *)handle;
	return ((mpc != NULL) ? mpc->decode(dest, len) : 0);
}

extern "C" __declspec (dllexport) int winampGetExtendedRead_setTime(intptr_t handle, int millisecs)
{
	// TODO
	return 0;
}

extern "C" __declspec(dllexport) void winampGetExtendedRead_close(intptr_t handle)
{
	mpc_player *mpc = (mpc_player *)handle;
	if (mpc)
	{
		delete mpc;
	}
}