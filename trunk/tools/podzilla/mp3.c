/*
 * Copyright (C) 2004 Bernard Leach
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef IPOD
#define USE_LIBINTEL
#else
#define USE_LIBMAD
#endif

#include <sys/types.h>
#include <sys/wait.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <linux/soundcard.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#ifdef USE_LIBINTEL
#include <ippdefs.h>
#include <ippAC.h>
#include "mp3decoder.h"
#endif

#include "pz.h"


static GR_WINDOW_ID mp3_wid;
static GR_GC_ID mp3_gc;
static GR_SCREEN_INFO screen_info;
static long remaining_time;
static char current_album[128];
static char current_artist[128];
static char current_title[128];
static int dsp_fd, mixer_fd;
static int dsp_vol;
static int decoding_finished;
static int mp3_pause;

int is_mp3_type(char *extension)
{
	return strcmp(extension, ".mp3") == 0;
}

static void draw_time()
{
	struct tm *tm;
	time_t rem_time;
	char onscreen_timer[128];

	/* draw the time using currenttime */
	rem_time = remaining_time / 1000;
	tm = gmtime(&rem_time);
	sprintf(onscreen_timer, "Time: %02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);

	GrText(mp3_wid, mp3_gc, 40, 80, onscreen_timer, -1, GR_TFASCII);
}

static void mp3_do_draw(GR_EVENT * event)
{
	GrSetGCForeground(mp3_gc, BLACK);
	GrFillRect(mp3_wid, mp3_gc, 0, 0, screen_info.cols, screen_info.rows);
	GrSetGCForeground(mp3_gc, WHITE);

	GrText(mp3_wid, mp3_gc, 8, 20, current_album, -1, GR_TFASCII);
	GrText(mp3_wid, mp3_gc, 8, 34, current_artist, -1, GR_TFASCII);
	GrText(mp3_wid, mp3_gc, 8, 48, current_title, -1, GR_TFASCII);

	draw_time();
}

static int mp3_do_keystroke(GR_EVENT * event)
{
        switch (event->keystroke.ch) {
        case '\r':
        case '\n': 
		break;
	case 'm':
		decoding_finished = 1;
		break;
	case 'd':
		mp3_pause = !mp3_pause;
		if (mp3_pause) {
			pz_draw_header("MP3 Playback - ||");
		}
		else {
			pz_draw_header("MP3 Playback");
		}
		break;
	case 'l':
		if (mixer_fd >= 0) {
			int vol = dsp_vol & 0xff;
			if (vol > 0) {
				vol--;
				vol = dsp_vol = vol << 8 | vol;
				ioctl(mixer_fd, SOUND_MIXER_WRITE_PCM, &vol);
			}
		}
		break;
	case 'r':
		if (mixer_fd >= 0) {
			int vol = dsp_vol & 0xff;
			if (vol < 100) {
				vol++;
				vol = dsp_vol = vol << 8 | vol;
				ioctl(mixer_fd, SOUND_MIXER_WRITE_PCM, &vol);
			}
		}
		break;
	}

	return 1;
}

static int audiobufpos, audiobuf_len;
static char *audiobuf;

static void setup_dsp(int sample_rate, int n_channels)
{
	int val;

	val = AFMT_S16_LE;
	ioctl(dsp_fd, SNDCTL_DSP_SETFMT, &val);
	ioctl(dsp_fd, SNDCTL_DSP_SPEED, &sample_rate);
	ioctl(dsp_fd, SNDCTL_DSP_CHANNELS, &n_channels);
}

static void mp3_event_handler()
{
	GR_EVENT event;

	if (GrPeekEvent(&event))
	{
		do {
			GrGetNextEventTimeout(&event, 1000);
			if (event.type != GR_EVENT_TYPE_TIMEOUT) {
				pz_event_handler(&event);
			}
		} while (mp3_pause && !decoding_finished);
	}
}

#ifdef USE_LIBMAD
static void decode_mp3()
{
	while (!decoding_finished) {
		mp3_event_handler();
	}

	pz_close_window(mp3_wid);
}
#endif

#ifdef USE_LIBINTEL
static void init_dsp(int channels, MP3DecoderState *ds)
{
	int fs[3] = { 44100, 48000, 32000 };
	IppMP3FrameHeader h = ds->FrameHdr;

	setup_dsp(fs[h.samplingFreq], channels);
}

static int abread(void * ptr, size_t size, size_t nmemb)
{
	int copylen = size * nmemb;
	if (copylen + audiobufpos > audiobuf_len) {
		copylen = audiobuf_len - audiobufpos;
	}
	
	memcpy(ptr, audiobuf + audiobufpos, copylen);
	audiobufpos += copylen;

	return copylen;
}

static int FillMP3BitStream(MP3BitStream *bs)
{
	int Mask;	/* Modulo mask to force end wrap-around for ring buffer */
	int ByteCount;	/* Byte counter for block reads from file */
	int n;		/* Secondary byte counter for block reads */
	Ipp8u *StreamBuf;	/* Ring buffer base pointer */

	/* Set modulo counting mask given ring buffer length */
	Mask = bs->Len-1;

	/* Point to ring buffer */
	StreamBuf = bs->Stream;

	/* Block fill ring buffer from file device using 1 of 3 cases:  
	   a) head>tail, b) head<tail, c) head==tail */
	if (bs->Head < bs->Tail)
	{
		ByteCount = abread(&(StreamBuf[((bs->Tail)+1)&Mask]), sizeof(Ipp8u), STREAM_BUF_SIZE-1-(bs->Tail));

		if ((ByteCount == (STREAM_BUF_SIZE-1-(bs->Tail)))&&(bs->Head > 0))
		{
			n = abread(&(StreamBuf[0]), sizeof(Ipp8u), bs->Head);
			bs->Tail = (n-1);
			ByteCount += n;
		}
		else
		{
			bs->Tail = bs->Tail + ByteCount;
		}
	}
	else if (bs->Head>bs->Tail)
	{
		ByteCount = abread(&(StreamBuf[((bs->Tail)+1)&Mask]), sizeof(Ipp8u), (bs->Head)-1-(bs->Tail));
		bs->Tail = bs->Tail + ByteCount;
	}
	else  /* Head==Tail, i.e., ring buffer empty */
	{
		ByteCount = abread(&(StreamBuf[0]), sizeof(Ipp8u), STREAM_BUF_SIZE);
		bs->Tail = ByteCount - 1;
		bs->Head = 0;
	}

	return ByteCount;
}

static void RenderSound(Ipp16s *pcm, MP3DecoderState *ds)
{
	int channels = ds->Channels;
	int len = ds->pcmLen;

	if (channels == 1) {
		int i;

		pz_draw_header("MP3 Playback - Mono!");
		for (i = 0; i < len; i += MAX_CHAN)
			pcm[i+1] = pcm[i]; 
	}

	write(dsp_fd, pcm, sizeof(Ipp16s) * len);
}

static MP3BitStream bs;
static MP3DecoderState DecoderState;
static Ipp16s pcm[MAX_CHAN*MAX_GRAN*IPP_MP3_GRANULE_LEN];

static void decode_mp3()
{
	int dsp_initialised = 0;
	int nframes = 0;

	InitMP3Decoder(&DecoderState, &bs);

	decoding_finished = 0;
	while (!decoding_finished) {
		int refill = 0;

		mp3_event_handler();

		switch (DecodeMP3Frame(&bs, pcm, &DecoderState)) {
		case MP3_FRAME_COMPLETE:
			if (!dsp_initialised) {
				init_dsp(MAX_CHAN, &DecoderState);
				dsp_initialised = 1;
			}
			RenderSound(pcm, &DecoderState);
			remaining_time -= 26;
			nframes++;
			if (nframes % 40 == 0) {
				draw_time();
			}
			refill = ((bs.Head>=bs.Tail) && (STREAM_BUF_SIZE-bs.Head+bs.Tail < FIFO_THRESH)) || ((bs.Tail >= bs.Head) && (bs.Tail-bs.Head < FIFO_THRESH));
			if (refill) {
				decoding_finished = FillMP3BitStream(&bs) == 0;
			}
			break;
		case MP3_BUFFER_UNDERRUN:
		case MP3_SYNC_NOT_FOUND:
			decoding_finished = FillMP3BitStream(&bs) == 0;
			break;
		case MP3_FRAME_UNDERRUN:
			break;
		case MP3_FRAME_HEADER_INVALID:
			break;
		}
	}

	pz_close_window(mp3_wid);
}
#endif

static void start_mp3_playback(char *filename)
{
	FILE *file;

	dsp_fd = open("/dev/dsp", O_WRONLY);
	if (dsp_fd < 0) {
		pz_close_window(mp3_wid);

		new_message_window("Cannot open /dev/dsp");
		return;
	}
	mixer_fd = open("/dev/mixer", O_RDWR);
	if (mixer_fd >= 0) {
		ioctl(mixer_fd, SOUND_MIXER_READ_PCM, &dsp_vol);
	}

	pz_draw_header("Buffering...");
	file = fopen(filename, "r");
	if (file == 0) {
		pz_close_window(mp3_wid);
		close(mixer_fd);
		close(dsp_fd);

		new_message_window("Cannot open mp3");
		return;
	}

	fseek(file, 0, SEEK_END);
	audiobuf_len = ftell(file);
	audiobufpos = 0;
	audiobuf = malloc(audiobuf_len);
	if (audiobuf == 0) {
		pz_close_window(mp3_wid);
		close(mixer_fd);
		close(dsp_fd);
		fclose(file);

		new_message_window("malloc failed");
		return;
	}

	fseek(file, 0, SEEK_SET);
	fread(audiobuf, audiobuf_len, 1, file);
	fclose(file);

	pz_draw_header("MP3 Playback");

	decoding_finished = 0;
	mp3_pause = 0;

	decode_mp3();

	free(audiobuf);

	close(mixer_fd);
	close(dsp_fd);
}

void new_mp3_window(char *filename, char *album, char *artist, char *title, int len)
{
	strncpy(current_album, album, sizeof(current_album)-1);
	current_album[sizeof(current_album)-1] = 0;

	strncpy(current_artist, artist, sizeof(current_artist)-1);
	current_artist[sizeof(current_artist)-1] = 0;

	strncpy(current_title, title, sizeof(current_title)-1);
	current_title[sizeof(current_title)-1] = 0;

	remaining_time = len;

	mp3_gc = GrNewGC();
	GrSetGCUseBackground(mp3_gc, GR_TRUE);
	GrSetGCForeground(mp3_gc, WHITE);
	GrGetScreenInfo(&screen_info);

	mp3_wid = pz_new_window(0, HEADER_TOPLINE + 1, screen_info.cols, screen_info.rows - (HEADER_TOPLINE + 1), mp3_do_draw, mp3_do_keystroke);

	GrSelectEvents(mp3_wid, GR_EVENT_MASK_EXPOSURE|GR_EVENT_MASK_KEY_DOWN|GR_EVENT_MASK_TIMER);

	GrMapWindow(mp3_wid);
	mp3_do_draw(0);

	start_mp3_playback(filename);
}
