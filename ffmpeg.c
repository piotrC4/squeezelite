/* 
 *  Squeezelite - lightweight headless squeezebox emulator
 *
 *  (c) Adrian Smith 2012, 2013, triode1@btinternet.com
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "squeezelite.h"

#if FFMPEG

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>

// we try to load a range of ffmpeg library versions
// note that this file must be compiled with header files of the same major version as the library loaded
// (as structs accessed may change between major versions)

#define LIBAVUTIL   "libavutil.so"
#define LIBAVUTIL_MAX 52
#define LIBAVUTIL_MIN 51

#define LIBAVCODEC  "libavcodec.so"
#define LIBAVCODEC_MAX 55
#define LIBAVCODEC_MIN 53

#define LIBAVFORMAT "libavformat.so"
#define LIBAVFORMAT_MAX 55
#define LIBAVFORMAT_MIN 53


#define READ_SIZE  4096 * 4   // this is large enough to ensure ffmpeg always gets new data when decode is called
#define WRITE_SIZE 256 * 1024 // FIXME - make smaller, but still to absorb max wma output

// FIXME - do we need to align these params as per ffmpeg on i386? 
#define attribute_align_arg

struct ff_s {
	// state for ffmpeg decoder
	bool wma;
	u8_t wma_mmsh;
	u8_t wma_playstream;
	u8_t wma_metadatastream;
	u8_t *readbuf;
	bool end_of_stream;
	AVInputFormat *input_format;
	AVFormatContext *formatC;
	AVCodecContext *codecC;
	AVFrame *frame;
	AVPacket *avpkt;
	unsigned mmsh_bytes_left;
	unsigned mmsh_bytes_pad;
	unsigned mmsh_packet_len;
	// library versions
	unsigned avcodec_v, avformat_v, avutil_v;
	// ffmpeg symbols to be dynamically loaded from libavcodec
	unsigned (* avcodec_version)(void);
	AVCodec * (* avcodec_find_decoder)(int);
	int attribute_align_arg (* avcodec_open2)(AVCodecContext *, const AVCodec *, AVDictionary **);
	AVFrame * (* avcodec_alloc_frame)(void);
	void (* avcodec_free_frame)(AVFrame *);
	int attribute_align_arg (* avcodec_decode_audio4)(AVCodecContext *, AVFrame *, int *, const AVPacket *);
	// ffmpeg symbols to be dynamically loaded from libavformat
	unsigned (* avformat_version)(void);
	AVFormatContext * (* avformat_alloc_context)(void);
	void (* avformat_free_context)(AVFormatContext *);
	int (* avformat_open_input)(AVFormatContext **, const char *, AVInputFormat *, AVDictionary **);
	int (* avformat_find_stream_info)(AVFormatContext *, AVDictionary **);
	AVIOContext * (* avio_alloc_context)(unsigned char *, int, int,	void *,
		int (*read_packet)(void *, uint8_t *, int), int (*write_packet)(void *, uint8_t *, int), int64_t (*seek)(void *, int64_t, int));
	void (* av_init_packet)(AVPacket *);
	void (* av_free_packet)(AVPacket *);
	int (* av_read_frame)(AVFormatContext *, AVPacket *);
	AVInputFormat * (* av_find_input_format)(const char *);
	void (* av_register_all)(void);
	// ffmpeg symbols to be dynamically loaded from libavutil
	unsigned (* avutil_version)(void);
	void (* av_log_set_callback)(void (*)(void*, int, const char*, va_list));
	void (* av_log_set_level)(int);
	int  (* av_strerror)(int, char *, size_t);
	void * (* av_malloc)(size_t);
	void (* av_free)(void *);
};

static struct ff_s *ff;

extern log_level loglevel;

extern struct buffer *streambuf;
extern struct buffer *outputbuf;
extern struct streamstate stream;
extern struct outputstate output;
extern struct decodestate decode;
extern struct processstate process;

#define LOCK_S   mutex_lock(streambuf->mutex)
#define UNLOCK_S mutex_unlock(streambuf->mutex)
#define LOCK_O   mutex_lock(outputbuf->mutex)
#define UNLOCK_O mutex_unlock(outputbuf->mutex)
#if PROCESS
#define LOCK_O_direct   if (decode.direct) mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct if (decode.direct) mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    if (decode.direct) { x }
#define IF_PROCESS(x)   if (!decode.direct) { x }
#else
#define LOCK_O_direct   mutex_lock(outputbuf->mutex)
#define UNLOCK_O_direct mutex_unlock(outputbuf->mutex)
#define IF_DIRECT(x)    { x }
#define IF_PROCESS(x)
#endif

// our own version of useful error function not included in earlier ffmpeg versions
static char *av__err2str(errnum) {
	static char buf[64];
	ff->av_strerror(errnum, buf, 64); 
	return buf;
}

// parser to extract asf data packet length from asf header
const u8_t header_guid[16] = { 0x30, 0x26, 0xB2, 0x75, 0x8E, 0x66, 0xCF, 0x11, 0xA6, 0xD9, 0x00, 0xAA, 0x00, 0x62, 0xCE, 0x6C };
const u8_t file_props_guid[16] = { 0xA1, 0xDC, 0xAB, 0x8C, 0x47, 0xA9, 0xCF, 0x11, 0x8E, 0xE4, 0x00, 0xC0, 0x0C, 0x20, 0x53, 0x65 };

static int _parse_packlen(void) {
	int bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	u8_t *ptr = streambuf->readp;
	int remain = 1;

	while (bytes >= 24 && remain > 0) {
		u32_t len = *(ptr+16) | *(ptr+17) << 8 | *(ptr+18) << 16 | *(ptr+19) << 24; // assume msb 32 bits are 0
		if (!memcmp(ptr, header_guid, 16) && bytes >= 30) {
			ptr    += 30;
			bytes  -= 30;
			remain = len - 30;
			continue;
		}
		if (!memcmp(ptr, file_props_guid, 16) && len == 104) {
			u32_t packlen = *(ptr+92) | *(ptr+93) << 8 | *(ptr+94) << 16 | *(ptr+95) << 24;
			LOG_INFO("asf packet len: %u", packlen);
			return packlen;
		}
		ptr    += len;
		bytes  -= len;
		remain -= len;
	}

	LOG_WARN("could not parse packet length");
	return 0;
}

static int _read_data(void *opaque, u8_t *buffer, int buf_size) {
	LOCK_S;

	size_t bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
	ff->end_of_stream = (stream.state <= DISCONNECT && bytes == 0);
	bytes = min(bytes, buf_size);

	// for chunked wma extract asf header and data frames from framing structure
	// pad asf data frames to size of packet extracted from asf header
	if (ff->wma_mmsh) {
		unsigned chunk_type = 0, chunk_len = 0;
		
		if (ff->mmsh_bytes_left) {
			// bytes remaining from previous frame
			if (bytes >= ff->mmsh_bytes_left) {
				bytes = ff->mmsh_bytes_left;
				ff->mmsh_bytes_left = 0;
			} else {
				ff->mmsh_bytes_left -= bytes;
			}
		} else if (ff->mmsh_bytes_pad) {
			// add padding for previous frame
			bytes = min(ff->mmsh_bytes_pad, buf_size);
			memset(buffer, 0, bytes);
			ff->mmsh_bytes_pad -= bytes;
			UNLOCK_S;
			return bytes;
		} else if (bytes >= 12) {
			// new chunk header
			chunk_type = (*(streambuf->readp) & 0x7f) | *(streambuf->readp + 1) << 8;
			chunk_len = *(streambuf->readp + 2) | *(streambuf->readp + 3) << 8;
			_buf_inc_readp(streambuf, 12);
			bytes -= 12;
		} else if (_buf_used(streambuf) >= 12) {
			// new chunk header split over end of streambuf, read in two
			u8_t header[12];
			memcpy(header, streambuf->readp, bytes);
			_buf_inc_readp(streambuf, bytes);
			memcpy(header + bytes, streambuf->readp, 12 - bytes);
			_buf_inc_readp(streambuf, 12 - bytes);
			chunk_type = (header[0] & 0x7f) | header[1] << 8;
			chunk_len  = header[2] | header[3] << 8;
			bytes = min(_buf_used(streambuf), _buf_cont_read(streambuf));
			bytes = min(bytes, buf_size);
		} else {
			// should not get here...
			LOG_ERROR("chunk parser stalled bytes: %u %u", bytes, _buf_used(streambuf));
			UNLOCK_S;
			return 0;
		}
		
		if (chunk_type && chunk_len) {
			if (chunk_type == 0x4824) {
				// asf header - parse packet length
				ff->mmsh_packet_len = _parse_packlen();
				ff->mmsh_bytes_pad = 0;
			} else if (chunk_type == 0x4424 && ff->mmsh_packet_len) {
				// asf data packet - add padding
				ff->mmsh_bytes_pad = ff->mmsh_packet_len - chunk_len + 8;
			} else {
				LOG_INFO("unknown chunk: %04x", chunk_type);
				// other packet - no padding
				ff->mmsh_bytes_pad = 0;
			}
	
			if (chunk_len - 8 <= bytes) {
				bytes = chunk_len - 8;
				ff->mmsh_bytes_left = 0;
			} else {
				ff->mmsh_bytes_left = chunk_len - 8 - bytes;
			}
		}

	}

	memcpy(buffer, streambuf->readp, bytes);

	_buf_inc_readp(streambuf, bytes);

	if (ff->mmsh_bytes_pad && bytes + ff->mmsh_bytes_pad < buf_size) {
		memset(buffer + bytes, 0, ff->mmsh_bytes_pad);
		bytes += ff->mmsh_bytes_pad;
		ff->mmsh_bytes_pad = 0;
	}

	UNLOCK_S;

	return bytes;
}

static decode_state ff_decode(void) {
	int r, len, got_frame;
	AVPacket pkt_c;
	s32_t *optr = NULL;

	if (decode.new_stream) {

		AVIOContext *avio;
		AVStream *av_stream;
		AVCodec *codec;
		int o;
		int audio_stream = -1;

		ff->mmsh_bytes_left = ff->mmsh_bytes_pad = ff->mmsh_packet_len = 0;

		if (!ff->readbuf) {
			ff->readbuf = ff->av_malloc(READ_SIZE +  FF_INPUT_BUFFER_PADDING_SIZE);
		}

		avio = ff->avio_alloc_context(ff->readbuf, READ_SIZE, 0, NULL, _read_data, NULL, NULL);
		avio->seekable = 0;

		ff->formatC = ff->avformat_alloc_context();
		if (ff->formatC == NULL) {
			LOG_ERROR("null context");
			return DECODE_ERROR;
		}

		ff->formatC->pb = avio;
		ff->formatC->flags |= AVFMT_FLAG_CUSTOM_IO | AVFMT_FLAG_NOPARSE;

		o = ff->avformat_open_input(&ff->formatC, "", ff->input_format, NULL);
		if (o < 0) {
			LOG_WARN("avformat_open_input: %d %s", o, av__err2str(o));
			return DECODE_ERROR;
		}

		LOG_INFO("format: name:%s lname:%s", ff->formatC->iformat->name, ff->formatC->iformat->long_name);
	
		o = ff->avformat_find_stream_info(ff->formatC, NULL);
		if (o < 0) {
			LOG_WARN("avformat_find_stream_info: %d %s", o, av__err2str(o));
			return DECODE_ERROR;
		}
		
		if (ff->wma && ff->wma_playstream < ff->formatC->nb_streams) {
			if (ff->formatC->streams[ff->wma_playstream]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
				LOG_INFO("using wma stream sent from server: %i", ff->wma_playstream);
				audio_stream = ff->wma_playstream;
			}
		}

		if (audio_stream == -1) {
			int i;
			for (i = 0; i < ff->formatC->nb_streams; ++i) {
				if (ff->formatC->streams[i]->codec->codec_type == AVMEDIA_TYPE_AUDIO) {
					audio_stream = i;
					LOG_INFO("found stream: %i", i);
					break;
				}
			}
		}

		if (audio_stream == -1) {
			LOG_WARN("no audio stream found");
			return DECODE_ERROR;
		}

		av_stream = ff->formatC->streams[audio_stream];

		ff->codecC = av_stream->codec;

		codec = ff->avcodec_find_decoder(ff->codecC->codec_id);

		ff->avcodec_open2(ff->codecC, codec, NULL);

		ff->frame = ff->avcodec_alloc_frame();

		ff->avpkt = ff->av_malloc(sizeof(AVPacket));
		if (ff->avpkt == NULL) {
			LOG_ERROR("can't allocate avpkt");
			return DECODE_ERROR;
		}

		ff->av_init_packet(ff->avpkt);
		ff->avpkt->data = NULL;
		ff->avpkt->size = 0;

		LOCK_O;
		LOG_INFO("setting track_start");
		output.next_sample_rate = decode_newstream(ff->codecC->sample_rate, output.max_sample_rate);
		output.track_start = outputbuf->writep;
		if (output.fade_mode) _checkfade(true);
		decode.new_stream = false;
		UNLOCK_O;
	}

	got_frame = 0;

	if ((r = ff->av_read_frame(ff->formatC, ff->avpkt)) < 0) {
		if (r == AVERROR_EOF) {
			if (ff->end_of_stream) {
				LOG_INFO("decode complete");
				return DECODE_COMPLETE;
			} else {
				LOG_INFO("codec end of file");
			}
		} else {
			LOG_ERROR("av_read_frame error: %i %s", r, av__err2str(r));
		}
		return DECODE_RUNNING;
	}

	// clone packet as we are adjusting it
	pkt_c = *ff->avpkt;

	IF_PROCESS(
		optr = (s32_t *)process.inbuf;
		process.in_frames = 0;
	);

	while (pkt_c.size > 0 || got_frame) {

		len = ff->avcodec_decode_audio4(ff->codecC, ff->frame, &got_frame, &pkt_c);
		if (len < 0) {
			LOG_ERROR("avcodec_decode_audio4 error: %i %s", len, av__err2str(len));
			return DECODE_RUNNING;
		}

		pkt_c.data += len;
		pkt_c.size -= len;
		
		if (got_frame) {
			
			s16_t *iptr16 = (s16_t *)ff->frame->data[0];
			s32_t *iptr32 = (s32_t *)ff->frame->data[0];
			s16_t *iptr16l = (s16_t *)ff->frame->data[0];
			s16_t *iptr16r = (s16_t *)ff->frame->data[1];
			s32_t *iptr32l = (s32_t *)ff->frame->data[0];
			s32_t *iptr32r = (s32_t *)ff->frame->data[1];
			float *iptrfl = (float *)ff->frame->data[0];
			float *iptrfr = (float *)ff->frame->data[1];

			frames_t frames = ff->frame->nb_samples;

			LOG_SDEBUG("got audio channels: %u samples: %u format: %u", ff->codecC->channels, ff->frame->nb_samples,
					   ff->codecC->sample_fmt);
			
			LOCK_O_direct;

			while (frames > 0) {
				frames_t count;
				frames_t f;
				
				IF_DIRECT(
					optr = (s32_t *)outputbuf->writep;
					f = min(_buf_space(outputbuf), _buf_cont_write(outputbuf)) / BYTES_PER_FRAME;
					f = min(f, frames);
				);

				IF_PROCESS(
					if (process.in_frames + frames > process.max_in_frames) {
						LOG_WARN("exceeded process buffer size - dropping frames");
						break;
					}
					f = frames;	   
				);

				count = f;
				
				if (ff->codecC->channels == 2) {
					if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16) {
						while (count--) {
							*optr++ = *iptr16++ << 16;
							*optr++ = *iptr16++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32) {
						while (count--) {
							*optr++ = *iptr32++;
							*optr++ = *iptr32++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16P) {
						while (count--) {
							*optr++ = *iptr16l++ << 16;
							*optr++ = *iptr16r++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32P) {
						while (count--) {
							*optr++ = *iptr32l++;
							*optr++ = *iptr32r++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_FLTP) {
						while (count--) {
							double scaledl = *iptrfl++ * 0x7fffffff;
							double scaledr = *iptrfr++ * 0x7fffffff;
							if (scaledl > 2147483647.0) scaledl = 2147483647.0;
							if (scaledl < -2147483648.0) scaledl = -2147483648.0;
							if (scaledr > 2147483647.0) scaledr = 2147483647.0;
							if (scaledr < -2147483648.0) scaledr = -2147483648.0;
							*optr++ = (s32_t)scaledl;
							*optr++ = (s32_t)scaledr;
						}
					} else {
						LOG_WARN("unsupported sample format: %u", ff->codecC->sample_fmt);
					}
				} else if (ff->codecC->channels == 1) {
					if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16) {
						while (count--) {
							*optr++ = *iptr16   << 16;
							*optr++ = *iptr16++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32) {
						while (count--) {
							*optr++ = *iptr32;
							*optr++ = *iptr32++;						
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S16P) {
						while (count--) {
							*optr++ = *iptr16l << 16;
							*optr++ = *iptr16l++ << 16;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_S32P) {
						while (count--) {
							*optr++ = *iptr32l;
							*optr++ = *iptr32l++;
						}
					} else if (ff->codecC->sample_fmt == AV_SAMPLE_FMT_FLTP) {
						while (count--) {
							double scaled = *iptrfl++ * 0x7fffffff;
							if (scaled > 2147483647.0) scaled = 2147483647.0;
							if (scaled < -2147483648.0) scaled = -2147483648.0;
							*optr++ = (s32_t)scaled;
							*optr++ = (s32_t)scaled;
						}
					} else {
						LOG_WARN("unsupported sample format: %u", ff->codecC->sample_fmt);
					}
				} else {
					LOG_WARN("unsupported number of channels");
				}
				
				frames -= f;
				
				IF_DIRECT(
					_buf_inc_writep(outputbuf, f * BYTES_PER_FRAME);
				);

				IF_PROCESS(
					process.in_frames += f;
				);
			}
			
			UNLOCK_O_direct;
		}
	}

	ff->av_free_packet(ff->avpkt);

	return DECODE_RUNNING;
}

static void _free_ff_data(void) {
	if (ff->formatC) {
		if (ff->formatC->pb) ff->av_free(ff->formatC->pb);
		ff->avformat_free_context(ff->formatC);
		ff->formatC = NULL;
	}

	if (ff->frame) {
		// ffmpeg version dependant free function
		ff->avcodec_free_frame ? ff->avcodec_free_frame(ff->frame) : ff->av_free(ff->frame);
		ff->frame = NULL;
	}

	if (ff->avpkt) {
		ff->av_free_packet(ff->avpkt);
		ff->av_free(ff->avpkt);
		ff->avpkt = NULL;
	}
}

static void ff_open_wma(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_free_ff_data();

	ff->input_format = ff->av_find_input_format("asf");
	if (ff->input_format == NULL) {
		LOG_ERROR("asf format not supported by ffmpeg library");
	}

	ff->wma = true;
	ff->wma_mmsh = size - '0';
	ff->wma_playstream = rate - 1;
	ff->wma_metadatastream = chan != '?' ? chan : 0;

	LOG_INFO("open wma chunking: %u playstream: %u metadatastream: %u", ff->wma_mmsh, ff->wma_playstream, ff->wma_metadatastream);
}

static void ff_open_alac(u8_t size, u8_t rate, u8_t chan, u8_t endianness) {
	_free_ff_data();

	ff->input_format = ff->av_find_input_format("mp4");
	if (ff->input_format == NULL) {
		LOG_ERROR("mp4 format not supported by ffmpeg library");
	}

	ff->wma = false;
	ff->wma_mmsh = 0;

	LOG_INFO("open alac");
}

static void ff_close(void) {
	_free_ff_data();

	if (ff->readbuf) {
		ff->av_free(ff->readbuf); 
		ff->readbuf = NULL;
	}
}

static bool load_ff() {
	void *handle_codec = NULL, *handle_format = NULL, *handle_util = NULL;
	char name[30];
	char *err;
	int i;

	// attempt to load newest known versions of libraries first
	for (i = LIBAVCODEC_MAX; i >= LIBAVCODEC_MIN && !handle_codec; --i) {
		sprintf(name, "%s.%d", LIBAVCODEC, i);
		handle_codec = dlopen(name, RTLD_NOW);
	}
	if (!handle_codec) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	for (i = LIBAVFORMAT_MAX; i >= LIBAVFORMAT_MIN && !handle_format; --i) {
		sprintf(name, "%s.%d", LIBAVFORMAT, i);
		handle_format = dlopen(name, RTLD_NOW);
	}
	if (!handle_format) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	for (i = LIBAVUTIL_MAX; i >= LIBAVUTIL_MIN && !handle_util; --i) {
		sprintf(name, "%s.%d", LIBAVUTIL, i);
		handle_util = dlopen(name, RTLD_NOW);
	}
	if (!handle_util) {
		LOG_INFO("dlerror: %s", dlerror());
		return false;
	}

	ff = malloc(sizeof(struct ff_s));
	memset(ff, 0, sizeof(struct ff_s));

	ff->avcodec_version = dlsym(handle_codec, "avcodec_version");
	ff->avcodec_find_decoder = dlsym(handle_codec, "avcodec_find_decoder");
	ff->avcodec_open2 = dlsym(handle_codec, "avcodec_open2");
	ff->avcodec_alloc_frame = dlsym(handle_codec, "avcodec_alloc_frame");
	ff->avcodec_free_frame = dlsym(handle_codec, "avcodec_free_frame");
	ff->avcodec_decode_audio4 = dlsym(handle_codec, "avcodec_decode_audio4");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}
	
	ff->avcodec_v = ff->avcodec_version();
	LOG_INFO("loaded "LIBAVCODEC" (%u.%u.%u)", ff->avcodec_v >> 16, (ff->avcodec_v >> 8) & 0xff, ff->avcodec_v & 0xff);
	if (ff->avcodec_v >> 16 != LIBAVCODEC_VERSION_MAJOR) {
		LOG_WARN("error: library major version (%u) differs from build headers (%u)", ff->avcodec_v >> 16, LIBAVCODEC_VERSION_MAJOR);
		return false;
	}

 	ff->avformat_version = dlsym(handle_format, "avformat_version");
 	ff->avformat_alloc_context = dlsym(handle_format, "avformat_alloc_context");
 	ff->avformat_free_context = dlsym(handle_format, "avformat_free_context");
 	ff->avformat_open_input = dlsym(handle_format, "avformat_open_input");
 	ff->avformat_find_stream_info = dlsym(handle_format, "avformat_find_stream_info");
 	ff->avio_alloc_context = dlsym(handle_format, "avio_alloc_context");
 	ff->av_init_packet = dlsym(handle_format, "av_init_packet");
 	ff->av_free_packet = dlsym(handle_format, "av_free_packet");
 	ff->av_read_frame = dlsym(handle_format, "av_read_frame");
 	ff->av_find_input_format= dlsym(handle_format, "av_find_input_format");
	ff->av_register_all = dlsym(handle_format, "av_register_all");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	ff->avformat_v = ff->avformat_version();
	LOG_INFO("loaded "LIBAVFORMAT" (%u.%u.%u)", ff->avformat_v >> 16, (ff->avformat_v >> 8) & 0xff, ff->avformat_v & 0xff);
	if (ff->avformat_v >> 16 != LIBAVFORMAT_VERSION_MAJOR) {
		LOG_WARN("error: library major version (%u) differs from build headers (%u)", ff->avformat_v >> 16, LIBAVFORMAT_VERSION_MAJOR);
		return false;
	}

	ff->avutil_version = dlsym(handle_util, "avutil_version");
	ff->av_log_set_callback = dlsym(handle_util, "av_log_set_callback");
	ff->av_log_set_level = dlsym(handle_util, "av_log_set_level");
	ff->av_strerror = dlsym(handle_util, "av_strerror");
	ff->av_malloc = dlsym(handle_util, "av_malloc");
	ff->av_free = dlsym(handle_util, "av_free");

	if ((err = dlerror()) != NULL) {
		LOG_INFO("dlerror: %s", err);		
		return false;
	}

	ff->avutil_v = ff->avutil_version();
	LOG_INFO("loaded "LIBAVUTIL" (%u.%u.%u)", ff->avutil_v >> 16, (ff->avutil_v >> 8) & 0xff, ff->avutil_v & 0xff);
	if (ff->avutil_v >> 16 != LIBAVUTIL_VERSION_MAJOR) {
		LOG_WARN("error: library major version (%u) differs from build headers (%u)", ff->avutil_v >> 16, LIBAVUTIL_VERSION_MAJOR);
		return false;
	}

	return true;
}

static int ff_log_level = 0;

void av_err_callback(void *avcl, int level, const char *fmt, va_list vl) {
	if (level > ff_log_level) return;
	fprintf(stderr, "%s ffmpeg: ", logtime());
	vfprintf(stderr, fmt, vl);
	fflush(stderr);
}

static bool registered = false;

struct codec *register_ff(const char *codec) {
	if (!registered) {

		if (!load_ff()) {
			return NULL;
		}

		switch (loglevel) {
		case lERROR:
			ff_log_level = AV_LOG_ERROR; break;
		case lWARN:
			ff_log_level = AV_LOG_WARNING; break;
		case lINFO:
			ff_log_level = AV_LOG_INFO; break;
		case lDEBUG:
			ff_log_level = AV_LOG_VERBOSE; break;
		default: break;
		}

		ff->av_log_set_callback(av_err_callback);

		ff->av_register_all();
		
		registered = true;
	}

	if (!strcmp(codec, "wma")) {

		static struct codec ret = { 
			'w',         // id
			"wma,wmap,wmal", // types
			READ_SIZE,   // min read
			WRITE_SIZE,  // min space
			ff_open_wma, // open
			ff_close,    // close
			ff_decode,   // decode
		};
		
		return &ret;
	}

	if (!strcmp(codec, "alc")) {

		static struct codec ret = { 
			'l',         // id
			"alc",       // types
			READ_SIZE,   // min read
			WRITE_SIZE,  // min space
			ff_open_alac,// open
			ff_close,    // close
			ff_decode,   // decode
		};
		
		return &ret;
	}

	return NULL;
}

#endif