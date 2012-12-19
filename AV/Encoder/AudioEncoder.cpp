/*
Copyright (c) 2012 Maarten Baert <maarten-baert@hotmail.com>

This file is part of SimpleScreenRecorder.

SimpleScreenRecorder is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.

SimpleScreenRecorder is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with SimpleScreenRecorder.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "StdAfx.h"
#include "AudioEncoder.h"

#include "Logger.h"
#include "AVWrapper.h"
#include "Muxer.h"

AudioEncoder::AudioEncoder(Logger* logger, Muxer* muxer, const QString& codec_name, const std::vector<std::pair<QString, QString> >& codec_options,
						   unsigned int bit_rate, unsigned int sample_rate)
	: BaseEncoder(logger, muxer) {

	m_bit_rate = bit_rate;
	m_sample_rate = sample_rate;

	// start the encoder
	AVDictionary *options = NULL;
	try {
		for(unsigned int i = 0; i < codec_options.size(); ++i) {
			av_dict_set(&options, qPrintable(codec_options[i].first), qPrintable(codec_options[i].second), 0);
		}
		CreateCodec(qPrintable(codec_name), &options);
		av_dict_free(&options);
	} catch(...) {
		av_dict_free(&options);
		throw;
	}

#if !SSR_USE_AVCODEC_ENCODE_AUDIO2
	// allocate a temporary buffer
	if(GetCodecContext()->frame_size == 0) {
		// This is really weird, the old API uses the size of the *output* buffer to determine the number of
		// input samples if the number of input samples (i.e. frame_size) is not fixed (i.e. frame_size == 0).
		m_temp_buffer.resize(1024 * GetCodecContext()->channels * av_get_bits_per_sample(GetCodecContext()->codec_id) / 8);
	} else {
		m_temp_buffer.resize(std::max(FF_MIN_BUFFER_SIZE, 256 * 1024));
	}
#endif

}

unsigned int AudioEncoder::GetRequiredFrameSize() {
#if SSR_USE_AVCODEC_ENCODE_AUDIO2
	return (GetCodecContext()->codec->capabilities & CODEC_CAP_VARIABLE_FRAME_SIZE)? 1024 : GetCodecContext()->frame_size;
#else
	return (GetCodecContext()->frame_size == 0)? 1024 : GetCodecContext()->frame_size;
#endif
}

void AudioEncoder::FillCodecContext() {

	GetCodecContext()->bit_rate = m_bit_rate;
	GetCodecContext()->sample_rate = m_sample_rate;
	GetCodecContext()->channels = 2;
	GetCodecContext()->sample_fmt = AV_SAMPLE_FMT_S16;

}

bool AudioEncoder::EncodeFrame(AVFrameWrapper* frame) {

#if SSR_USE_AVCODEC_ENCODE_AUDIO2

	// allocate a packet
	std::unique_ptr<AVPacketWrapper> packet(new AVPacketWrapper());

	// encode the frame
	int got_packet;
	if(avcodec_encode_audio2(GetCodecContext(), packet.get(), frame, &got_packet) < 0) {
		GetLogger()->LogError("[AudioEncoder::EncodeFrame] Error: Encoding of audio frame failed!");
		throw LibavException();
	}

	// do we have a packet?
	if(got_packet) {

		// send the packet to the muxer
		GetMuxer()->AddPacket(GetStreamIndex(), std::move(packet));
		return true;

	} else {
		return false;
	}

#else

#if SSR_USE_AVFRAME_FORMAT
	// check the format
	if(frame != NULL && frame->format != AV_SAMPLE_FMT_S16) {
		GetLogger()->LogError("[AudioEncoder::EncodeFrame] Error: Audio frame uses format " + QString::number(frame->format) + " instead of " + QString::number(AV_SAMPLE_FMT_S16) + " (AV_SAMPLE_FMT_S16)!");
		throw LibavException();
	}
#endif

	// encode the frame
	short *data = (frame == NULL)? NULL : (short*) frame->data[0];
	int bytes_encoded = avcodec_encode_audio(GetCodecContext(), m_temp_buffer.data(), m_temp_buffer.size(), data);
	if(bytes_encoded < 0) {
		GetLogger()->LogError("[AudioEncoder::EncodeFrame] Error: Encoding of audio frame failed!");
		throw LibavException();
	}

	// do we have a packet?
	if(bytes_encoded > 0) {

		// allocate a packet
		std::unique_ptr<AVPacketWrapper> packet(new AVPacketWrapper(bytes_encoded));

		// copy the data
		memcpy(packet->data, m_temp_buffer.data(), bytes_encoded);

		// set the timestamp and flags
		// note: pts will be rescaled and stream_index will be set by Muxer
		if(GetCodecContext()->coded_frame != NULL && GetCodecContext()->coded_frame->pts != (int64_t) AV_NOPTS_VALUE)
			packet->pts = GetCodecContext()->coded_frame->pts;
		if(GetCodecContext()->coded_frame->key_frame)
			packet->flags |= AV_PKT_FLAG_KEY;

		// send the packet to the muxer
		GetMuxer()->AddPacket(GetStreamIndex(), std::move(packet));
		return true;

	} else {
		return false;
	}

#endif

}
