/* hacktv - Analogue video transmitter for the HackRF                    */
/*=======================================================================*/
/* Copyright 2018 Philip Heron <phil@sanslogic.co.uk>                    */
/*                                                                       */
/* This program is free software: you can redistribute it and/or modify  */
/* it under the terms of the GNU General Public License as published by  */
/* the Free Software Foundation, either version 3 of the License, or     */
/* (at your option) any later version.                                   */
/*                                                                       */
/* This program is distributed in the hope that it will be useful,       */
/* but WITHOUT ANY WARRANTY; without even the implied warranty of        */
/* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         */
/* GNU General Public License for more details.                          */
/*                                                                       */
/* You should have received a copy of the GNU General Public License     */
/* along with this program.  If not, see <http://www.gnu.org/licenses/>. */

/* Thread summary:
 * 
 * Input           - Reads the data from disk/network and feeds the
 *                   audio and/or video packet queues. Sets an EOF
 *                   flag on all queues when the input reaches the
 *                   end. Ends at EOF or abort.
 * 
 * Video decoder   - Reads from the video packet queue and produces
 *                   the decoded video frames.
 * 
 * Video scaler    - Rescales decoded video frames to the correct
 *                   size and format required by hacktv.
 * 
 * Audio thread    - Reads from the audio packet queue and produces
 *                   the decoded.
 *
 * Audio resampler - Resamples the decoded audio frames to the format
 *                   required by hacktv (32000Hz, Stereo, 16-bit)
*/
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1

#define MAX(a,b) (((a) > (b)) ? (a) : (b))

#endif
#include <pthread.h>
#include <ctype.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>
#include <libavutil/imgutils.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include "hacktv.h"

/* Maximum length of the packet queue */
/* Taken from ffplay.c */
#define MAX_QUEUE_SIZE (15 * 1024 * 1024)

/* Temp hack */
char current_text[256];

typedef struct __packet_queue_item_t {
	
	AVPacket pkt;
	struct __packet_queue_item_t *next;
	
} _packet_queue_item_t;

typedef struct {
	
	int length;	/* Number of packets */
	int size;       /* Number of bytes used */
	int eof;        /* End of stream / file flag */
	int abort;      /* Abort flag */
	
	/* Pointers to the first and last packets in the queue */
	_packet_queue_item_t *first;
	_packet_queue_item_t *last;
	
	/* Thread locking and signaling */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	
} _packet_queue_t;

typedef struct {
	
	int ready;	/* Frame ready flag */
	int repeat;	/* Repeat the previous frame */
	int abort;	/* Abort flag */
	
	/* The AVFrame buffers */
	AVFrame *frame[2];
	
	/* Thread locking and signaling */
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	
} _frame_dbuffer_t;

typedef struct {
	
	/* Seek stuff */
	int width;
	int height;
	int sample_rate;
	uint32_t *video;
	vid_t *s;
	int seekflag;
	uint8_t background;
	int bstat;
	
	av_font_t *font[10];
	
	AVFormatContext *format_ctx;
	
	/* Video decoder */
	AVRational video_time_base;
	int64_t video_start_time;
	_packet_queue_t video_queue;
	AVStream *video_stream;
	AVCodecContext *video_codec_ctx;
	_frame_dbuffer_t in_video_buffer;
	int video_eof;
	
	/* Video scaling */
	struct SwsContext *sws_ctx;
	_frame_dbuffer_t out_video_buffer;
	
	/* Audio decoder */
	AVRational audio_time_base;
	int64_t audio_start_time;
	_packet_queue_t audio_queue;
	AVStream *audio_stream;
	AVCodecContext *audio_codec_ctx;
	_frame_dbuffer_t in_audio_buffer;
	int audio_eof;
	
	/* Audio resampler */
	struct SwrContext *swr_ctx;
	_frame_dbuffer_t out_audio_buffer;
	int out_frame_size;
	int allowed_error;
	
	/* Subtitle decoder */
	AVStream *subtitle_stream;
	AVCodecContext *subtitle_codec_ctx;
	int subtitle_eof;
	
	/* Threads */
	pthread_t input_thread;
	pthread_t video_decode_thread;
	pthread_t video_scaler_thread;
	pthread_t audio_decode_thread;
	pthread_t audio_scaler_thread;
	volatile int thread_abort;
	
	/* Video filter buffers */
	AVFilterContext *vbuffersink_ctx;
	AVFilterContext *vbuffersrc_ctx;
	
	/* Audio filter buffers */
	AVFilterContext *abuffersink_ctx;
	AVFilterContext *abuffersrc_ctx;
	AVRational sar, dar;
	
} av_ffmpeg_t;

static void _print_ffmpeg_error(int r)
{
	char sb[128];
	const char *sp = sb;
	
	if(av_strerror(r, sb, sizeof(sb)) < 0)
	{
		sp = strerror(AVUNERROR(r));
	}
	
	fprintf(stderr, "%s\n", sp);
}

void _audio_offset(uint8_t const **dst, uint8_t const * const *src, int offset, int nb_channels, enum AVSampleFormat sample_fmt)
{
	int planar      = av_sample_fmt_is_planar(sample_fmt);
	int planes      = planar ? nb_channels : 1;
	int block_align = av_get_bytes_per_sample(sample_fmt) * (planar ? 1 : nb_channels);
	int i;
	
	offset *= block_align;
	
	for(i = 0; i < planes; i++)
	{
		dst[i] = src[i] + offset;
	}
}

static int _packet_queue_init(_packet_queue_t *q)
{
	q->length = 0;
	q->size = 0;
	q->eof = 0;
	q->abort = 0;
	
	pthread_mutex_init(&q->mutex, NULL);
	pthread_cond_init(&q->cond, NULL);
	
	return(0);
}

static int _packet_queue_flush(_packet_queue_t *q)
{
	_packet_queue_item_t *p;
	
	pthread_mutex_lock(&q->mutex);
	
	while(q->length--)
	{
		/* Pop the first item off the list */
		p = q->first;
		q->first = p->next;
		
		av_packet_unref(&p->pkt);
		free(p);
	}
	
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
	
	return(0);
}

static void _packet_queue_free(_packet_queue_t *q)
{
	_packet_queue_flush(q);
	
	pthread_cond_destroy(&q->cond);
	pthread_mutex_destroy(&q->mutex);
}

static void _packet_queue_abort(_packet_queue_t *q)
{
	pthread_mutex_lock(&q->mutex);
	
	q->abort = 1;
	
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
}

static int _packet_queue_write(_packet_queue_t *q, AVPacket *pkt)
{
	_packet_queue_item_t *p;
	
	pthread_mutex_lock(&q->mutex);
	
	/* A NULL packet signals the end of the stream / file */
	if(pkt == NULL)
	{
		q->eof = 1;
	}
	else
	{
		/* Limit the size of the queue */
		while(q->abort == 0 && q->size + pkt->size + sizeof(_packet_queue_item_t) > MAX_QUEUE_SIZE)
		{
			pthread_cond_wait(&q->cond, &q->mutex);
		}
		
		if(q->abort == 1)
		{
			/* Abort was called while waiting for the queue size to drop */
			av_packet_unref(pkt);
			
			pthread_cond_signal(&q->cond);
			pthread_mutex_unlock(&q->mutex);
			
			return(-2);
		}
		
		/* Allocate memory for queue item and copy packet */
		p = malloc(sizeof(_packet_queue_item_t));
		p->pkt = *pkt;
		p->next = NULL;
		
		/* Add the item to the end of the queue */
		if(q->length == 0)
		{
			q->first = p;
		}
		else
		{
			q->last->next = p;
		}
		
		q->last = p;
		q->length++;
		q->size += pkt->size + sizeof(_packet_queue_item_t);
	}
	
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
	
	return(0);
}

static int _packet_queue_read(_packet_queue_t *q, AVPacket *pkt)
{
	_packet_queue_item_t *p;
	
	pthread_mutex_lock(&q->mutex);
	
	while(q->length == 0)
	{
		if(q->abort == 1 || q->eof == 1)
		{
			pthread_mutex_unlock(&q->mutex);
			return(q->abort == 1 ? -2 : -1);
		}
		
		pthread_cond_wait(&q->cond, &q->mutex);
	}
	
	p = q->first;
	
	*pkt = p->pkt;
	q->first = p->next;
	q->length--;
	q->size -= pkt->size + sizeof(_packet_queue_item_t);
	
	free(p);
	
	pthread_cond_signal(&q->cond);
	pthread_mutex_unlock(&q->mutex);
	
	return(0);
}

static int _frame_dbuffer_init(_frame_dbuffer_t *d)
{
	d->ready = 0;
	d->repeat = 0;
	d->abort = 0;
	
	d->frame[0] = av_frame_alloc();
	d->frame[1] = av_frame_alloc();
	
	if(!d->frame[0] || !d->frame[1])
	{
		av_frame_free(&d->frame[0]);
		av_frame_free(&d->frame[1]);
		return(-1);
	}
	
	pthread_mutex_init(&d->mutex, NULL);
	pthread_cond_init(&d->cond, NULL);
	
	return(0);
}

static void _frame_dbuffer_free(_frame_dbuffer_t *d)
{
	pthread_cond_destroy(&d->cond);
	pthread_mutex_destroy(&d->mutex);
	
	av_frame_free(&d->frame[0]);
	av_frame_free(&d->frame[1]);
}

static void _frame_dbuffer_abort(_frame_dbuffer_t *d)
{
	pthread_mutex_lock(&d->mutex);
	
	d->abort = 1;
	
	pthread_cond_signal(&d->cond);
	pthread_mutex_unlock(&d->mutex);
}

static AVFrame *_frame_dbuffer_back_buffer(_frame_dbuffer_t *d)
{
	AVFrame *frame;
	pthread_mutex_lock(&d->mutex);
	
	/* Wait for the ready flag to be unset */
	while(d->ready != 0 && d->abort == 0)
	{
		pthread_cond_wait(&d->cond, &d->mutex);
	}
	
	frame = d->frame[1];
	
	pthread_mutex_unlock(&d->mutex);
	
	return(frame);
}

static void _frame_dbuffer_ready(_frame_dbuffer_t *d, int repeat)
{
	pthread_mutex_lock(&d->mutex);
	
	/* Wait for the ready flag to be unset */
	while(d->ready != 0 && d->abort == 0)
	{
		pthread_cond_wait(&d->cond, &d->mutex);
	}
	
	d->ready = 1;
	d->repeat = repeat;
	
	pthread_cond_signal(&d->cond);
	pthread_mutex_unlock(&d->mutex);
}

static AVFrame *_frame_dbuffer_flip(_frame_dbuffer_t *d)
{
	AVFrame *frame;
	
	pthread_mutex_lock(&d->mutex);
	
	/* Wait for a flag to be set */
	while(d->ready == 0 && d->abort == 0)
	{
		pthread_cond_wait(&d->cond, &d->mutex);
	}
	
	/* Die if it was the abort flag */
	if(d->abort != 0)
	{
		pthread_mutex_unlock(&d->mutex);
		return(NULL);
	}
	
	/* Swap the frames if we're not repeating */
	if(d->repeat == 0)
	{
		frame       = d->frame[1];
		d->frame[1] = d->frame[0];
		d->frame[0] = frame;
	}
	
	frame = d->frame[0];
	d->ready = 0;
	
	/* Signal we're finished and release the mutex */
	pthread_cond_signal(&d->cond);
	pthread_mutex_unlock(&d->mutex);
	
	return(frame);
}

static void *_input_thread(void *arg)
{
	av_ffmpeg_t *av = (av_ffmpeg_t *) arg;
	AVPacket pkt;
	int r;
	
	//fprintf(stderr, "_input_thread(): Starting\n");
	
	/* Fetch packets from the source */
	while(av->thread_abort == 0)
	{
		r = av_read_frame(av->format_ctx, &pkt);
		
		if(r == AVERROR(EAGAIN))
		{
			av_usleep(10000);
			continue;
		}
		else if(r < 0)
		{
			/* FFmpeg input EOF or error. Break out */
			break;
		}
		
		if(av->video_stream && pkt.stream_index == av->video_stream->index)
		{
			_packet_queue_write(&av->video_queue, &pkt);
		}
		else if(av->audio_stream && pkt.stream_index == av->audio_stream->index)
		{
			_packet_queue_write(&av->audio_queue, &pkt);
		}
		else if(av->subtitle_stream && pkt.stream_index == av->subtitle_stream->index && (av->s->conf.subtitles || av->s->conf.txsubtitles))
		{
			AVSubtitle sub;
			int got_frame;
			
			r = avcodec_decode_subtitle2(av->subtitle_codec_ctx, &sub, &got_frame, &pkt);
			
			if(got_frame)
			{
				int s;
				
				if(sub.format == SUB_TEXT)
				{
					/* Load text subtitle into buffer */
					load_text_subtitle(av->s->av_sub, pkt.pts + sub.start_display_time, sub.end_display_time, sub.rects[0]->ass);
				}
				else if(sub.format == SUB_BITMAP)
				{
					uint32_t *bitmap;
					int max_bitmap_width = 0;
					int max_bitmap_height = 0;
					int bitmap_scale;
					int x, y, pos, last_pos;
					last_pos = pos = 0;
					
					for(s = 0; s < sub.num_rects; s++)
					{
						/* Scale bitmap to video width */
						bitmap_scale = sub.rects[s]->w / av->s->active_width < 1 ? 1 : round(sub.rects[s]->w / av->s->active_width);
						
						/* Get maximum width */
						max_bitmap_width = MAX(max_bitmap_width, sub.rects[s]->w / bitmap_scale);
						
						/* Get total height of all rects */
						max_bitmap_height += sub.rects[s]->h / bitmap_scale;
					}
					
					/* Give it some memory */
					bitmap = malloc(max_bitmap_width * max_bitmap_height * sizeof(uint32_t));
					
					/* Set all pixels to black */
					memset(bitmap, 0, max_bitmap_width * max_bitmap_height * sizeof(uint32_t));
					
					for(s = sub.num_rects - 1; s >= 0; s--)
					{	
						for (x = 0; x < sub.rects[s]->w; x++)
						{
							for (y = 0; y < sub.rects[s]->h; y++)
							{
								/* Bitmap position */
								pos = (y / bitmap_scale * max_bitmap_width + x / bitmap_scale) + last_pos;
								
								/* Colour index */
								char c = sub.rects[s]->data[0][y * sub.rects[s]->w + x];
								
								char r = sub.rects[s]->data[1][c * 4 + 0];
								char g = sub.rects[s]->data[1][c * 4 + 1];
								char b = sub.rects[s]->data[1][c * 4 + 2];
								char a = sub.rects[s]->data[1][c * 4 + 3];
								if(c) 
								{
									bitmap[pos] = (a << 24 | r << 16 | g << 8 | b << 0);
								}
							}
						}
						last_pos = pos;
					}
					
					load_bitmap_subtitle(av->s->av_sub, av->s, max_bitmap_width, max_bitmap_height, pkt.pts + sub.start_display_time, sub.end_display_time, bitmap);
					
					free(bitmap);
				}
				
				avsubtitle_free(&sub);
			}
			else if(r != AVERROR(EAGAIN))
			{
				/* avcodec_receive_frame returned an EOF or error, abort thread */
				//break;
			}
			
			av_packet_unref(&pkt);
		}
		else
		{
			av_packet_unref(&pkt);
		}
	}
	
	/* Set the EOF flag in the queues */
	_packet_queue_write(&av->video_queue, NULL);
	_packet_queue_write(&av->audio_queue, NULL);
	
	//fprintf(stderr, "_input_thread(): Ending\n");
	
	return(NULL);
}

static void *_video_decode_thread(void *arg)
{
	av_ffmpeg_t *av = (av_ffmpeg_t *) arg;
	AVPacket pkt, *ppkt = NULL;
	AVFrame *frame;
	int r;
	
	//fprintf(stderr, "_video_decode_thread(): Starting\n");
	
	frame = av_frame_alloc();
	
	/* Fetch video packets from the queue and decode */
	while(av->thread_abort == 0)
	{
		if(ppkt == NULL)
		{
			r = _packet_queue_read(&av->video_queue, &pkt);
			if(r == -2)
			{
				/* Thread is aborting */
				break;
			}
			
			ppkt = (r >= 0 ? &pkt : NULL);
		}
		
		r = avcodec_send_packet(av->video_codec_ctx, ppkt);
		
		if(ppkt != NULL && r != AVERROR(EAGAIN))
		{
			av_packet_unref(ppkt);
			ppkt = NULL;
		}
		
		if(r < 0 && r != AVERROR(EAGAIN))
		{
			/* avcodec_send_packet() has failed, abort thread */
			break;
		}
		
		r = avcodec_receive_frame(av->video_codec_ctx, frame);
		
		if(r == 0)
		{
			/* Push the decoded frame into the filtergraph */
			if (av_buffersrc_add_frame(av->vbuffersrc_ctx, frame) < 0) 
			{
				printf( "Error while feeding the video filtergraph\n");
			}

			/* Pull filtered frame from the filtergraph */ 
			if(av_buffersink_get_frame(av->vbuffersink_ctx, frame) < 0) 
			{
				printf( "Error while sourcing the video filtergraph\n");
			}
			
			/* We have received a frame! */
			av_frame_ref(_frame_dbuffer_back_buffer(&av->in_video_buffer), frame);
			_frame_dbuffer_ready(&av->in_video_buffer, 0);
			
		}
		else if(r != AVERROR(EAGAIN))
		{
			/* avcodec_receive_frame returned an EOF or error, abort thread */
			break;
		}
	}
	
	_frame_dbuffer_abort(&av->in_video_buffer);
	
	av_frame_free(&frame);
	
	//fprintf(stderr, "_video_decode_thread(): Ending\n");
	
	return(NULL);
}

static void *_video_scaler_thread(void *arg)
{
	av_ffmpeg_t *av = (av_ffmpeg_t *) arg;
	AVFrame *frame, *oframe;
	AVRational ratio;
	int64_t pts;
	
	/* Fetch video frames and pass them through the scaler */
	while((frame = _frame_dbuffer_flip(&av->in_video_buffer)) != NULL)
	{
		pts = frame->best_effort_timestamp;
		
		if(pts != AV_NOPTS_VALUE)
		{
			pts  = av_rescale_q(pts, av->video_stream->time_base, av->video_time_base);
			// fprintf(stderr, "Time base %i\n", pts);
			pts -= av->video_start_time;
			
			if(pts < 0)
			{
				/* This frame is in the past. Skip it */
				av_frame_unref(frame);
				continue;
			}
			
			while(pts > 0)
			{
				/* This frame is in the future. Repeat the previous one */
				_frame_dbuffer_ready(&av->out_video_buffer, 1);
				av->video_start_time++;
				pts--;
			}
		}
		
		if(av->seekflag < 2) av->seekflag++;
		
		oframe = _frame_dbuffer_back_buffer(&av->out_video_buffer);
		
		sws_scale(
			av->sws_ctx,
			(uint8_t const * const *) frame->data,
			frame->linesize,
			0,
			av->video_codec_ctx->height,
			oframe->data,
			oframe->linesize
		);
		
		ratio = frame->sample_aspect_ratio;
		
		if(ratio.num == 0 || ratio.den == 0)
		{
			/* Default to square pixels if the ratio looks odd */
			ratio = (AVRational) { 1, 1 };
		}
		
		/* Adjust the pixel ratio for the scaled image */
		av_reduce(
			&oframe->sample_aspect_ratio.num,
			&oframe->sample_aspect_ratio.den,
			frame->width * ratio.num * oframe->height,
			frame->height * ratio.den * oframe->width,
			INT_MAX
		);
		
		/* Print logo, if enabled */
		if(av->s->conf.logo)
		{
			overlay_image((uint32_t *) oframe->data[0], &av->s->vid_logo, av->s->active_width, av->s->conf.active_lines, av->s->vid_logo.position);
		}
		
		if(av->s->conf.timestamp)
		{
			char timestr[20];
			int toffset;
			toffset = 0;
			
			/* Hack to resolve time calculation differences between Windows and *nix */
			#ifndef WIN32
				toffset = 3600;
			#endif
			
			time_t diff = time(0) - av->s->conf.timestamp  + (av->s->conf.position * 60) - toffset;
			struct tm *d = localtime(&diff);
			sprintf(timestr, "%02d:%02d:%02d", d->tm_hour, d->tm_min, d->tm_sec);
			print_generic_text(	av->font[1],
								(uint32_t *) oframe->data[0],
								timestr,
								10, 90, 1, 0, 0, 0);
			
			fprintf(stderr,"\r%s", timestr);
		}
		
		/* Print subtitles, if enabled */
		if(av->s->conf.subtitles || av->s->conf.txsubtitles) 
		{
			if(get_subtitle_type(av->s->av_sub) == SUB_TEXT)
			{
				/* best_effort_timestamp is very flaky - not really a good measure of current position and doesn't work some of the time */
				char fmt[256];
				sprintf(fmt,"%s", get_text_subtitle(av->s->av_sub, frame->best_effort_timestamp / (av->video_stream->time_base.den / 1000)));
				
				if(av->s->conf.subtitles) print_subtitle(av->font[0], (uint32_t *) oframe->data[0], fmt);
				
				if(av->s->conf.txsubtitles && strcmp(current_text, fmt) != 0)
				{
					strcpy(current_text, fmt);
					update_teletext_subtitle(fmt, &av->s->tt.service);
				}
			}
			else if(av->s->conf.subtitles)
			{
				int w, h;
				uint32_t *bitmap = get_bitmap_subtitle(av->s->av_sub, frame->best_effort_timestamp, &w, &h);
				
				if(w > 0) display_bitmap_subtitle(av->font[0], (uint32_t *) oframe->data[0], w, h, bitmap);
			}
		}
		
		av_frame_unref(frame);
		
		_frame_dbuffer_ready(&av->out_video_buffer, 0);
		av->video_start_time++;
	}
	
	_frame_dbuffer_abort(&av->out_video_buffer);
	
	// fprintf(stderr, "_video_scaler_thread(): Ending\n");
	
	return(NULL);
}

static uint32_t *_av_ffmpeg_read_video(void *private, float *ratio)
{
	av_ffmpeg_t *av = private;
	AVFrame *frame;
	
	if(av->video_stream == NULL)
	{
		return(NULL);
	}
	
	frame = _frame_dbuffer_flip(&av->out_video_buffer);
	if(!frame)
	{
		/* EOF or abort */
		av->video_eof = 1;
		return(NULL);
	}
	
	if(ratio)
	{
		/* Default to 4:3 ratio if it can't be calculated */
		*ratio = 4.0 / 3.0;
		
		if(frame->sample_aspect_ratio.den > 0 && frame->height > 0)
		{
			if(!av->s->conf.letterbox && !av->s->conf.pillarbox)
			{
				*ratio = (float) av->video_codec_ctx->width / av->video_codec_ctx->height;
			}
		}
		
		/*
		if(av->s->conf.letterbox || av->s->conf.pillarbox)
		{
			*ratio  = (float) frame->sample_aspect_ratio.num / frame->sample_aspect_ratio.den;
			*ratio *= (float) frame->width / frame->height;
		}
		*/
		
	}
	
	if(!av->seekflag)
	{
		if(av->bstat)
		{
			if(av->background == 0x50) av->bstat = 0;
			av->background++;
		}
		else
		{
			if(av->background == 0x25) av->bstat = 1;
			av->background--;
		}

		memset(frame->data[0], av->background, vid_get_framebuffer_length(av->s));
		print_generic_text(	av->font[2], (uint32_t *) frame->data[0],
							"SEEKING VIDEO",
							50, 47, 1, 0, 0, 0);
		
		print_generic_text(	av->font[2], (uint32_t *) frame->data[0],
							"PLEASE WAIT",
							50, 53, 1, 0, 0, 0);

		/* Print logo, if enabled */
		if(av->s->conf.logo)
		{
			overlay_image((uint32_t *) frame->data[0], &av->s->vid_logo, av->s->active_width, av->s->conf.active_lines, av->s->vid_logo.position);
		}
	}
	
	return ((uint32_t *) frame->data[0]);
}

static void *_audio_decode_thread(void *arg)
{
	/* TODO: This function is virtually identical to _video_decode_thread(),
	 *       they should probably be combined */
	av_ffmpeg_t *av = (av_ffmpeg_t *) arg;
	AVPacket pkt, *ppkt = NULL;
	AVFrame *frame;
	int r;
	
	//fprintf(stderr, "_audio_decode_thread(): Starting\n");
	
	frame = av_frame_alloc();
	
	/* Fetch audio packets from the queue and decode */
	while(av->thread_abort == 0)
	{
		if(ppkt == NULL)
		{
			r = _packet_queue_read(&av->audio_queue, &pkt);
			if(r == -2)
			{
				/* Thread is aborting */
				break;
			}
			
			ppkt = (r >= 0 ? &pkt : NULL);
		}
		
		r = avcodec_send_packet(av->audio_codec_ctx, ppkt);
		
		if(ppkt != NULL && r != AVERROR(EAGAIN))
		{
			av_packet_unref(ppkt);
			ppkt = NULL;
		}
		
		r = avcodec_receive_frame(av->audio_codec_ctx, frame);
		
		if(r == 0)
		{
			/* Push the decoded frame into the filtergraph */
			if (av_buffersrc_add_frame(av->abuffersrc_ctx, frame) < 0) 
			{
				fprintf(stderr, "Error while feeding the audio filtergraph\n");
			}

			/* Pull filtered frame from the filtergraph */ 
			if(av_buffersink_get_frame(av->abuffersink_ctx, frame) < 0) 
			{
				fprintf(stderr, "Error while sourcing the video filtergraph\n");
			}
			
			/* We have received a frame! */
			av_frame_ref(_frame_dbuffer_back_buffer(&av->in_audio_buffer), frame);
			_frame_dbuffer_ready(&av->in_audio_buffer, 0);
		}
		else if(r != AVERROR(EAGAIN))
		{
			/* avcodec_receive_frame returned an EOF or error, abort thread */
			break;
		}
	}
	
	_frame_dbuffer_abort(&av->in_audio_buffer);
	
	av_frame_free(&frame);
	
	//fprintf(stderr, "_audio_decode_thread(): Ending\n");
	
	return(NULL);
}

static void *_audio_scaler_thread(void *arg)
{
	av_ffmpeg_t *av = (av_ffmpeg_t *) arg;
	AVFrame *frame, *oframe;
	int64_t pts, next_pts;
	uint8_t const *data[AV_NUM_DATA_POINTERS];
	int r, count, drop;
	
	//fprintf(stderr, "_audio_scaler_thread(): Starting\n");
	
	/* Fetch audio frames and pass them through the resampler */
	while((frame = _frame_dbuffer_flip(&av->in_audio_buffer)) != NULL)
	{
		pts = frame->best_effort_timestamp;
		drop = 0;
		
		if(pts != AV_NOPTS_VALUE)
		{
			pts      = av_rescale_q(pts, av->audio_stream->time_base, av->audio_time_base);
			pts     -= av->audio_start_time;
			next_pts = pts + frame->nb_samples;
			
			if(next_pts <= 0)
			{
				/* This frame is in the past. Skip it */
				av_frame_unref(frame);
				continue;
			}
			
			if(pts < -av->allowed_error)
			{
				/* Trim this frame */
				drop = -pts;
				//swr_drop_input(av->swr_ctx, -pts); /* It would be nice if this existed */
			}
			else if(pts > av->allowed_error)
			{
				/* This frame is in the future. Send silence to fill the gap */
				r = swr_inject_silence(av->swr_ctx, pts);
				av->audio_start_time += pts;
			}
		}
		
		count = frame->nb_samples;
		
		count -= drop;
		_audio_offset(
			data,
			(const uint8_t **) frame->data,
			drop,
			av->audio_codec_ctx->channels,
			av->audio_codec_ctx->sample_fmt
		);
		
		do
		{
			oframe = _frame_dbuffer_back_buffer(&av->out_audio_buffer);
			r = swr_convert(
				av->swr_ctx,
				oframe->data,
				av->out_frame_size,
				count ? data : NULL,
				count
			);
			if(r == 0) break;
			
			oframe->nb_samples = r;
			
			_frame_dbuffer_ready(&av->out_audio_buffer, 0);
			
			av->audio_start_time += count;
			count = 0;
		}
		while(r > 0);
		
		av_frame_unref(frame);
	}
	
	_frame_dbuffer_abort(&av->out_audio_buffer);
	
	//fprintf(stderr, "_audio_scaler_thread(): Ending\n");
	
	return(NULL);
}

static int16_t *_av_ffmpeg_read_audio(void *private, size_t *samples)
{
	av_ffmpeg_t *av = private;
	AVFrame *frame;
	
	if(av->audio_stream == NULL)
	{
		return(NULL);
	}
	
	frame = _frame_dbuffer_flip(&av->out_audio_buffer);
	if(!frame)
	{
		/* EOF or abort */
		av->audio_eof = 1;
		return(NULL);
	}
	
	*samples = frame->nb_samples;
	
	return((int16_t *) frame->data[0]);
}

static int _av_ffmpeg_eof(void *private)
{
	av_ffmpeg_t *av = private;
	
	if((av->video_stream && !av->video_eof) ||
	   (av->audio_stream && !av->audio_eof))
	{
		return(0);
	}
	
	return(1);
}

static int _av_ffmpeg_close(void *private)
{
	av_ffmpeg_t *av = private;
	
	av->thread_abort = 1;
	_packet_queue_abort(&av->video_queue);
	_packet_queue_abort(&av->audio_queue);
	
	pthread_join(av->input_thread, NULL);
	
	if(av->video_stream != NULL)
	{
		_frame_dbuffer_abort(&av->in_video_buffer);
		_frame_dbuffer_abort(&av->out_video_buffer);
		
		pthread_join(av->video_decode_thread, NULL);
		pthread_join(av->video_scaler_thread, NULL);
		
		_packet_queue_free(&av->video_queue);
		_frame_dbuffer_free(&av->in_video_buffer);
		
		av_freep(&av->out_video_buffer.frame[0]->data[0]);
		av_freep(&av->out_video_buffer.frame[1]->data[0]);
		_frame_dbuffer_free(&av->out_video_buffer);
		
		avcodec_free_context(&av->video_codec_ctx);
		sws_freeContext(av->sws_ctx);
	}
	
	if(av->audio_stream != NULL)
	{
		_frame_dbuffer_abort(&av->in_audio_buffer);
		_frame_dbuffer_abort(&av->out_audio_buffer);
		
		pthread_join(av->audio_decode_thread, NULL);
		pthread_join(av->audio_scaler_thread, NULL);
		
		_packet_queue_free(&av->audio_queue);
		_frame_dbuffer_free(&av->in_audio_buffer);
		
		//av_freep(&av->out_audio_buffer.frame[0]->data[0]);
		//av_freep(&av->out_audio_buffer.frame[1]->data[0]);
		_frame_dbuffer_free(&av->out_audio_buffer);
		
		avcodec_free_context(&av->audio_codec_ctx);
		swr_free(&av->swr_ctx);
	}
	
	avformat_close_input(&av->format_ctx);
	
	free(av);
	
	return(HACKTV_OK);
}

int av_ffmpeg_open(vid_t *s, char *input_url)
{
	av_ffmpeg_t *av;
	AVCodec *codec;
	AVRational time_base;
	int64_t start_time = 0;
	int r;
	int i;
	
	/* Default ratio */
	float source_ratio = 4.0 / 3.0;
	
	av = calloc(1, sizeof(av_ffmpeg_t));
	if(!av)
	{
		return(HACKTV_OUT_OF_MEMORY);
	}
	
	av->width = s->active_width;
	av->height = s->conf.active_lines;
	
	/* Use 'pipe:' for stdin */
	if(strcmp(input_url, "-") == 0)
	{
		input_url = "pipe:";
	}
	
	/* Open the video */
	if((r = avformat_open_input(&av->format_ctx, input_url, NULL, NULL)) < 0)
	{
		fprintf(stderr, "Error opening file '%s'\n", input_url);
		_print_ffmpeg_error(r);
		return(HACKTV_ERROR);
	}
	
	/* Read stream info from the file */
	if(avformat_find_stream_info(av->format_ctx, NULL) < 0)
	{
		fprintf(stderr, "Error reading stream information from file\n");
		return(HACKTV_ERROR);
	}
	
	/* Dump some useful information to stderr */
	fprintf(stderr, "Opening '%s'...\n", input_url);
	av_dump_format(av->format_ctx, 0, input_url, 0);
	
	/* Find the first video and audio streams */
	/* TODO: Allow the user to select streams by number or name */
	av->video_stream = NULL;
	av->audio_stream = NULL;
	av->subtitle_stream = NULL;
	
	for(i = 0; i < av->format_ctx->nb_streams; i++)
	{
		if(av->video_stream == NULL && av->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
		{
			av->video_stream = av->format_ctx->streams[i];
		}
		
		if(s->audio && av->audio_stream == NULL && av->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
		{
			if(av->format_ctx->streams[i]->codecpar->channels <= 0) continue;
			av->audio_stream = av->format_ctx->streams[i];
		}
		
		if(av->subtitle_stream == NULL && av->format_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_SUBTITLE)
		{
			av->subtitle_stream = av->format_ctx->streams[s->conf.txsubtitles >= i && s->conf.txsubtitles < av->format_ctx->nb_streams? s->conf.txsubtitles : i];
			av->subtitle_stream = av->format_ctx->streams[s->conf.subtitles >= i && s->conf.subtitles < av->format_ctx->nb_streams? s->conf.subtitles : i];
		}
	}
	
	/* At minimum we need either a video or audio stream */
	if(av->video_stream == NULL && av->audio_stream == NULL)
	{
		fprintf(stderr, "No video or audio streams found\n");
		return(HACKTV_ERROR);
	}
	
	if(av->video_stream != NULL)
	{
		fprintf(stderr, "Using video stream %d.\n", av->video_stream->index);
		
		/* Create the video's time_base using the current TV mode's frames per second.
		 * Numerator and denominator are swapped as ffmpeg uses seconds per frame. */
		av->video_time_base.num = s->conf.frame_rate_den;
		av->video_time_base.den = s->conf.frame_rate_num;
		if(s->conf.interlace) av->video_time_base.den *= 2;
		
		/* Use the video's start time as the reference */
		time_base = av->video_stream->time_base;
		start_time = av->video_stream->start_time;
		
		/* Get a pointer to the codec context for the video stream */
		av->video_codec_ctx = avcodec_alloc_context3(NULL);
		if(!av->video_codec_ctx)
		{
			return(HACKTV_OUT_OF_MEMORY);
		}
		
		if(avcodec_parameters_to_context(av->video_codec_ctx, av->video_stream->codecpar) < 0)
		{
			return(HACKTV_ERROR);
		}
		
		av->video_codec_ctx->thread_count = 0; /* Let ffmpeg decide number of threads */
		
		/* Find the decoder for the video stream */
		codec = avcodec_find_decoder(av->video_codec_ctx->codec_id);
		if(codec == NULL)
		{
			fprintf(stderr, "Unsupported video codec\n");
			return(HACKTV_ERROR);
		}
		
		/* Open video codec */
		if(avcodec_open2(av->video_codec_ctx, codec, NULL) < 0)
		{
			fprintf(stderr, "Error opening video codec\n");
			return(HACKTV_ERROR);
		}
		
		/* Video filter starts here */
		
		/* Video filter declarations */
		char *_vfi;
		char *_filter_args;
		enum AVPixelFormat pix_fmts[] = {AV_PIX_FMT_RGB32 };
			
		AVFilterGraph *vfilter_graph;

		/* Deprecated - to be removed in later versions */
		#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
		avfilter_register_all();
		#endif
		
		AVBufferSinkParams *buffersink_params;
		const AVFilter *vbuffersrc  = avfilter_get_by_name("buffer");
		const AVFilter *vbuffersink = avfilter_get_by_name("buffersink");
		AVFilterInOut *vinputs  = avfilter_inout_alloc();
		AVFilterInOut *voutputs = avfilter_inout_alloc();
		vfilter_graph = avfilter_graph_alloc();

		asprintf(&_filter_args,"video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=%d/%d",
			av->video_codec_ctx->width, av->video_codec_ctx->height, av->video_codec_ctx->pix_fmt,
			av->video_stream->r_frame_rate.num, av->video_stream->r_frame_rate.den,
			av->video_codec_ctx->sample_aspect_ratio.num, av->video_codec_ctx->sample_aspect_ratio.den);

		if(avfilter_graph_create_filter(&av->vbuffersrc_ctx, vbuffersrc, "in",_filter_args, NULL, vfilter_graph) < 0) 
		{
			fprintf(stderr, "Cannot create video buffer source\n");
			return(HACKTV_ERROR);
		}
		
		/* Buffer video sink to terminate the filter chain */
		buffersink_params = av_buffersink_params_alloc();
		buffersink_params->pixel_fmts = pix_fmts;
		
		if(avfilter_graph_create_filter(&av->vbuffersink_ctx, vbuffersink, "out", NULL, buffersink_params, vfilter_graph) < 0) 
		{
			fprintf(stderr,"Cannot create video buffer sink\n");
			return(HACKTV_ERROR);
		}
	
		/* Endpoints for the filter graph. */
		voutputs->name       = av_strdup("in");
		voutputs->filter_ctx = av->vbuffersrc_ctx;
		voutputs->pad_idx    = 0;
		voutputs->next       = NULL;
		
		vinputs->name       = av_strdup("out");
		vinputs->filter_ctx = av->vbuffersink_ctx;
		vinputs->pad_idx    = 0;
		vinputs->next       = NULL;
		
		/* Calculate letterbox padding for widescreen videos, if necessary */ 
		int video_width_ws = s->conf.active_lines * (16.0 / 9.0); 
		int source_width = av->video_codec_ctx->width;
		int source_height = av->video_codec_ctx->height;
		
		int video_width = s->conf.active_lines * (4.0 / 3.0); 
		
		source_ratio = (float) source_width / (float) source_height;
		int ws = source_ratio >= (14.0 / 9.0) ? 1 : 0;	
		
		char *_vid_filter;
		
		/* Default states */
		asprintf(&_vid_filter,"null");
		
		if(ws)
		{
			if(s->conf.letterbox)
			{
				asprintf(&_vid_filter,"pad = 'iw:iw / (%i / %i) : 0 : (oh - ih) / 2', scale = %i:%i", video_width, s->conf.active_lines, source_width, source_height);
			}
			else if(s->conf.pillarbox)
			{
				asprintf(&_vid_filter,"crop = out_w = in_h * (4.0 / 3.0) : out_h = in_h, scale = %i:%i", source_width, source_height);
			}
			else
			{
				if((float) video_width_ws / (float) s->conf.active_lines <= source_ratio)
				{
					asprintf(&_vid_filter,"pad = 'iw:iw / (%i/%i) : 0 : (oh-ih) / 2', scale = %i:%i", video_width_ws, s->conf.active_lines, source_width, source_height);
				}
				else
				{
					asprintf(&_vid_filter,"pad = 'ih * (%i / %i) : ih : (ow-iw) / 2 : 0', scale = %i:%i", video_width_ws, s->conf.active_lines, source_width, source_height);
				}
			}
		}
		
		asprintf(&_vfi, "[in]%s[out]", _vid_filter);
		
		const char *vfilter_descr = _vfi;

		if(avfilter_graph_parse_ptr(vfilter_graph, vfilter_descr, &vinputs, &voutputs, NULL) < 0)
		{
			fprintf(stderr, "Cannot parse filter graph\n");
			return(HACKTV_ERROR);
		}
		
		if(avfilter_graph_config(vfilter_graph, NULL) < 0) 
		{
			fprintf(stderr, "Cannot configure filter graph\n");
			return(HACKTV_ERROR);
		}
		
		av_free(buffersink_params);
		avfilter_inout_free(&vinputs);
		avfilter_inout_free(&voutputs);
		
		/* Video filter ends here */
		
		/* Initialise SWS context for software scaling */
		av->sws_ctx = sws_getContext(
			av->video_codec_ctx->width,
			av->video_codec_ctx->height,
			av->video_codec_ctx->pix_fmt,
			s->active_width,
			s->conf.active_lines,
			AV_PIX_FMT_RGB32,
			SWS_BICUBIC,
			NULL,
			NULL,
			NULL
		);
		
		if(!av->sws_ctx)
		{
			return(HACKTV_OUT_OF_MEMORY);
		}
		
		av->video_eof = 0;
	}
	else
	{
		fprintf(stderr, "No video streams found.\n");
	}
	
	if(av->audio_stream != NULL)
	{
		fprintf(stderr, "Using audio stream %d.\n", av->audio_stream->index);
		
		/* Get a pointer to the codec context for the video stream */
		av->audio_codec_ctx = avcodec_alloc_context3(NULL);
		if(!av->audio_codec_ctx)
		{
			return(HACKTV_ERROR);
		}
		
		if(avcodec_parameters_to_context(av->audio_codec_ctx, av->audio_stream->codecpar) < 0)
		{
			return(HACKTV_ERROR);
		}
		
		av->audio_codec_ctx->thread_count = 0; /* Let ffmpeg decide number of threads */
		
		/* Find the decoder for the audio stream */
		codec = avcodec_find_decoder(av->audio_codec_ctx->codec_id);
		if(codec == NULL)
		{
			fprintf(stderr, "Unsupported audio codec\n");
			return(HACKTV_ERROR);
		}
		
		/* Open audio codec */
		if(avcodec_open2(av->audio_codec_ctx, codec, NULL) < 0)
		{
			fprintf(stderr, "Error opening audio codec\n");
			return(HACKTV_ERROR);
		}
		
		/* Audio filter graph here */
		char *_afi;
		char *_afilter_args;
		AVFilterGraph *afilter_graph;
		
		/* Deprecated - to be removed in later versions */
		#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
		avfilter_register_all();
		#endif
		
		const AVFilter *abuffersrc  = avfilter_get_by_name("abuffer");
		const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
		AVFilterInOut *aoutputs = avfilter_inout_alloc();
		AVFilterInOut *ainputs  = avfilter_inout_alloc();
		afilter_graph = avfilter_graph_alloc();

		asprintf(&_afilter_args, "time_base=%d/%d:sample_rate=%d:sample_fmt=%s:channel_layout=0x%" PRIx64,
			av->audio_codec_ctx->time_base.num, av->audio_codec_ctx->time_base.den, av->audio_codec_ctx->sample_rate,
			av_get_sample_fmt_name(av->audio_codec_ctx->sample_fmt),
			av->audio_codec_ctx->channel_layout);
	
		if(avfilter_graph_create_filter(&av->abuffersrc_ctx, abuffersrc, "in", _afilter_args, NULL, afilter_graph) < 0) 
		{
			fprintf(stderr, "Cannot create audio buffer source\n");
			return(HACKTV_ERROR);
		}
		
		if(avfilter_graph_create_filter(&av->abuffersink_ctx, abuffersink, "out", NULL, NULL, afilter_graph) < 0) 
		{
			fprintf(stderr, "Cannot create audio buffer sink\n");
			return(HACKTV_ERROR);
		}

		/* Endpoints for the audio filter graph. */
		aoutputs->name       = av_strdup("in");
		aoutputs->filter_ctx = av->abuffersrc_ctx;
		aoutputs->pad_idx    = 0;
		aoutputs->next       = NULL;

		ainputs->name       = av_strdup("out");
		ainputs->filter_ctx = av->abuffersink_ctx;
		ainputs->pad_idx    = 0;
		ainputs->next       = NULL;
		
		char fmt[5];
		sprintf(fmt,"%s", av_get_sample_fmt_name(av->audio_codec_ctx->sample_fmt));
		asprintf(&_afi,
				"[in]%s[downmix],[downmix]volume=%f:precision=%s[out]",
				s->conf.downmix ? "pan=stereo|FL < FC + 0.30*FL + 0.30*BL|FR < FC + 0.30*FR + 0.30*BR" : "anull",
				s->conf.volume,
				fmt[0] == 'f' ? "float" : fmt[0] == 'd' ? "double" : "fixed"
		);
		
		const char *afilter_descr = _afi;
		
		if (avfilter_graph_parse_ptr(afilter_graph, afilter_descr, &ainputs, &aoutputs, NULL) < 0)
		{
			fprintf(stderr,"Cannot parse filter graph %s\n", _afi);
			return(HACKTV_ERROR);
		}
		
		if (avfilter_graph_config(afilter_graph, NULL) < 0) 
		{
			printf("Cannot configure filter graph\n");
			return(HACKTV_ERROR);
		}
		
		avfilter_inout_free(&ainputs);
		avfilter_inout_free(&aoutputs);
		
		/* Create the audio time_base using the source sample rate */
		av->audio_time_base.num = 1;
		av->audio_time_base.den = av->audio_codec_ctx->sample_rate;
		
		/* Use the audio's start time as the reference if no video was detected */
		if(av->video_stream == NULL)
		{
			time_base = av->audio_stream->time_base;
			start_time = av->audio_stream->start_time;
		}
		
		/* Prepare the resampler */
		av->swr_ctx = swr_alloc();
		if(!av->swr_ctx)
		{
			return(HACKTV_OUT_OF_MEMORY);
		}
		
		if(!av->audio_codec_ctx->channel_layout)
		{
			/* Set the default layout for codecs that don't specify any */
			av->audio_codec_ctx->channel_layout = av_get_default_channel_layout(av->audio_codec_ctx->channels);
		}
		
		/* Channel layout changes to stereo if using downmix option */
		av_opt_set_int(av->swr_ctx, "in_channel_layout",    s->conf.downmix ? AV_CH_LAYOUT_STEREO : av->audio_codec_ctx->channel_layout, 0);
		av_opt_set_int(av->swr_ctx, "in_sample_rate",       av->audio_codec_ctx->sample_rate, 0);
		av_opt_set_sample_fmt(av->swr_ctx, "in_sample_fmt", av->audio_codec_ctx->sample_fmt, 0);
		
		av_opt_set_int(av->swr_ctx, "out_channel_layout",    AV_CH_LAYOUT_STEREO, 0);
		av_opt_set_int(av->swr_ctx, "out_sample_rate",       HACKTV_AUDIO_SAMPLE_RATE, 0);
		av_opt_set_sample_fmt(av->swr_ctx, "out_sample_fmt", AV_SAMPLE_FMT_S16, 0);
		
		if(swr_init(av->swr_ctx) < 0)
		{
			fprintf(stderr, "Failed to initialise the resampling context\n");
			return(HACKTV_ERROR);
		}
		
		av->audio_eof = 0;
	}
	else
	{
		fprintf(stderr, "No audio streams found.\n");
	}
	
	if(av->subtitle_stream != NULL)
	{
		fprintf(stderr, "Using subtitle stream %d.\n", av->subtitle_stream->index);
		
		/* Get a pointer to the codec context for the subtitle stream */
		av->subtitle_codec_ctx = avcodec_alloc_context3(NULL);
		if(!av->subtitle_codec_ctx)
		{
			return(HACKTV_OUT_OF_MEMORY);
		}
		
		if(avcodec_parameters_to_context(av->subtitle_codec_ctx, av->subtitle_stream->codecpar) < 0)
		{
			return(HACKTV_ERROR);
		}
		
		av->subtitle_codec_ctx->thread_count = 0; /* Let ffmpeg decide number of threads */
		av->subtitle_codec_ctx->pkt_timebase = av->subtitle_stream->time_base;
		
		/* Find the decoder for the subtitle stream */
		codec = avcodec_find_decoder(av->subtitle_codec_ctx->codec_id);
		if(codec == NULL)
		{
			fprintf(stderr, "Unsupported subtitle codec\n");
			return(HACKTV_ERROR);
		}
		
		/* Open subtitle codec */
		if(avcodec_open2(av->subtitle_codec_ctx, codec, NULL) < 0)
		{
			fprintf(stderr, "Error opening subtitle codec\n");
			return(HACKTV_ERROR);
		}
		
		av->subtitle_eof = 0;
		if(s->conf.subtitles || s->conf.txsubtitles) subs_init_ffmpeg(s);
		
		/* Initialise fonts here */
		if(font_init(s, 38, source_ratio) !=0)
		{
			return(HACKTV_ERROR);
		};
		
		av->bstat = 0;

		av->font[0] = s->av_font;
	}
	else
	{
		fprintf(stderr, "No subtitle streams found.\n");
		
		/* Initialise subtitles - here because it's already supplied with the filename for video */
		/* Should really be moved somewhere else */
		if(s->conf.subtitles || s->conf.txsubtitles)
		{
			if(subs_init_file(input_url, s) != HACKTV_OK)
			{
				s->conf.subtitles = 0;
				s->conf.txsubtitles = 0;
				return(HACKTV_ERROR);
			}
			
			/* Initialise fonts here */
			if(font_init(s, 38, source_ratio) < 0)
			{
				s->conf.subtitles = 0;
				s->conf.txsubtitles = 0;
				return(HACKTV_ERROR);
			}
			
			av->font[0] = s->av_font;
		}
	}
	
	if(start_time == AV_NOPTS_VALUE)
	{
		start_time = 0;
	}
	
	/* Seek stuff here */
	int64_t request_timestamp = (60.0 * s->conf.position) / av_q2d(time_base) + start_time;
	
	/* Calculate the start time for each stream */
	if(av->video_stream != NULL)
	{
		if (s->conf.position > 0) 
		{
			av->video_start_time = av_rescale_q(request_timestamp, time_base, av->video_time_base);
			av_seek_frame(av->format_ctx, av->video_stream->index, request_timestamp, 0);
		}
		else
		{
			av->video_start_time = av_rescale_q(start_time, time_base, av->video_time_base);
		}
	}
	
	if(av->audio_stream != NULL)
	{
		av->audio_start_time = av_rescale_q(s->conf.position ? request_timestamp : start_time, time_base, av->audio_time_base);
	}
	
	if(s->conf.timestamp)
	{
		s->conf.timestamp = time(0);
		
		if(font_init(s, 40, source_ratio) != VID_OK)
		{
			s->conf.timestamp = 0;
		};
		
		av->font[1] = s->av_font;
	}
	
	if(s->conf.logo)
	{
		/* Normalise ratio */
		float ratio = source_ratio >= (14.0 / 9.0) ? 16.0/9.0 : 4.0/3.0;
		ratio = s->conf.pillarbox || s->conf.letterbox ? 4.0/3.0 : ratio;
		
		if(load_png(&s->vid_logo, s->active_width, s->conf.active_lines, s->conf.logo, 0.75, ratio, IMG_LOGO) == HACKTV_ERROR)
		{
			s->conf.logo = NULL;
		}
	}
	
	/* Generic font */
	if(font_init(s, 56, source_ratio) == VID_OK)
	{
		av->font[2] = s->av_font;
	};
		
	/* Register the callback functions */
	av->s = s;
	s->av_private = av;
	s->av_read_video = _av_ffmpeg_read_video;
	s->av_read_audio = _av_ffmpeg_read_audio;
	s->av_eof = _av_ffmpeg_eof;
	s->av_close = _av_ffmpeg_close;
	
	/* Start the threads */
	av->thread_abort = 0;
	_packet_queue_init(&av->video_queue);
	_packet_queue_init(&av->audio_queue);
	
	if(av->video_stream != NULL)
	{
		_frame_dbuffer_init(&av->in_video_buffer);
		_frame_dbuffer_init(&av->out_video_buffer);
		
		/* Allocate memory for the output frame buffers */
		for(i = 0; i < 2; i++)
		{
			av->out_video_buffer.frame[i]->width = s->active_width;
			av->out_video_buffer.frame[i]->height = s->conf.active_lines;
			
			r = av_image_alloc(
				av->out_video_buffer.frame[i]->data,
				av->out_video_buffer.frame[i]->linesize,
				s->active_width, s->conf.active_lines,
				AV_PIX_FMT_RGB32, 1
			);
		}
		
		r = pthread_create(&av->video_decode_thread, NULL, &_video_decode_thread, (void *) av);
		if(r != 0)
		{
			fprintf(stderr, "Error starting video decoder thread.\n");
			return(HACKTV_ERROR);
		}
		r = pthread_create(&av->video_scaler_thread, NULL, &_video_scaler_thread, (void *) av);
		if(r != 0)
		{
			fprintf(stderr, "Error starting video scaler thread.\n");
			return(HACKTV_ERROR);
		}
	}
	
	if(av->audio_stream != NULL)
	{
		_frame_dbuffer_init(&av->in_audio_buffer);
		_frame_dbuffer_init(&av->out_audio_buffer);
		
		/* Calculate the number of samples needed for output */
		av->out_frame_size = av_rescale_rnd(
			av->audio_codec_ctx->frame_size, /* Can this be trusted? */
			HACKTV_AUDIO_SAMPLE_RATE,
			av->audio_codec_ctx->sample_rate,
			AV_ROUND_UP
		);
		
		if(av->out_frame_size <= 0)
		{
			av->out_frame_size = HACKTV_AUDIO_SAMPLE_RATE;
		}
		
		/* Calculate the allowed error in input samples, +/- 20ms */
		av->allowed_error = av_rescale_q(AV_TIME_BASE * 0.020, AV_TIME_BASE_Q, av->audio_time_base);
		
		for(i = 0; i < 2; i++)
		{
			av->out_audio_buffer.frame[i]->format = AV_SAMPLE_FMT_S16;
			av->out_audio_buffer.frame[i]->channel_layout = AV_CH_LAYOUT_STEREO;
			av->out_audio_buffer.frame[i]->sample_rate = HACKTV_AUDIO_SAMPLE_RATE;
			av->out_audio_buffer.frame[i]->nb_samples = av->out_frame_size;
			
			r = av_frame_get_buffer(av->out_audio_buffer.frame[i], 0);
			if(r < 0)
			{
				fprintf(stderr, "Error allocating output audio buffer %d\n", i);
				return(HACKTV_OUT_OF_MEMORY);
			}
		}
		
		r = pthread_create(&av->audio_decode_thread, NULL, &_audio_decode_thread, (void *) av);
		if(r != 0)
		{
			fprintf(stderr, "Error starting audio decoder thread.\n");
			return(HACKTV_ERROR);
		}
		
		r = pthread_create(&av->audio_scaler_thread, NULL, &_audio_scaler_thread, (void *) av);
		if(r != 0)
		{
			fprintf(stderr, "Error starting audio resampler thread.\n");
			return(HACKTV_ERROR);
		}
	}
	
	r = pthread_create(&av->input_thread, NULL, &_input_thread, (void *) av);
	if(r != 0)
	{
		fprintf(stderr, "Error starting input thread.\n");
		return(HACKTV_ERROR);
	}
	
	return(HACKTV_OK);
}

void av_ffmpeg_init(void)
{
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(58, 9, 100)
	av_register_all();
#endif
	avdevice_register_all();
	avformat_network_init();
}

void av_ffmpeg_deinit(void)
{
	avformat_network_deinit();
}

