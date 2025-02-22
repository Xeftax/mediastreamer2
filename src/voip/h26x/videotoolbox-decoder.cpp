/*
 * Copyright (c) 2010-2022 Belledonne Communications SARL.
 *
 * This file is part of mediastreamer2
 * (see https://gitlab.linphone.org/BC/public/mediastreamer2).
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU Affero General Public License as
 * published by the Free Software Foundation, either version 3 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Affero General Public License for more details.
 *
 * You should have received a copy of the GNU Affero General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "videotoolbox-utils.h"

#include "videotoolbox-decoder.h"

#define vt_dec_log(level, fmt, ...) ms_##level("VideoToolboxDecoder: " fmt, ##__VA_ARGS__)
#define vt_dec_message(fmt, ...) vt_dec_log(message, fmt, ##__VA_ARGS__)
#define vt_dec_warning(fmt, ...) vt_dec_log(warning, fmt, ##__VA_ARGS__)
#define vt_dec_error(fmt, ...) vt_dec_log(error, fmt, ##__VA_ARGS__)
#define vt_dec_debug(fmt, ...) vt_dec_log(debug, fmt, ##__VA_ARGS__)

using namespace std;

namespace mediastreamer {

VideoToolboxDecoder::VideoToolboxDecoder(const string &mime) : H26xDecoder(mime) {
	_pixbufAllocator = ms_yuv_buf_allocator_new();
	const H26xToolFactory &factory = H26xToolFactory::get(mime);
	_psStore.reset(factory.createParameterSetsStore());
	_naluHeader.reset(factory.createNaluHeader());
}

VideoToolboxDecoder::~VideoToolboxDecoder() {
	if (_session) destroyDecoder();
	ms_yuv_buf_allocator_free(_pixbufAllocator);
}

bool VideoToolboxDecoder::feed(MSQueue *encodedFrame, uint64_t timestamp) {
	try {
		_psStore->extractAllPs(encodedFrame);
		if (_psStore->hasNewParameters()) {
			_psStore->acknowlege();
			if (_session) destroyDecoder();
		}
		if (ms_queue_empty(encodedFrame)) return true;
		if (!_psStore->psGatheringCompleted()) throw runtime_error("need more parameter sets");
		if (_session == nullptr) createDecoder();
		if (_freeze) {
			for (const mblk_t *nalu = ms_queue_peek_first(encodedFrame); !ms_queue_end(encodedFrame, nalu);
			     nalu = ms_queue_next(encodedFrame, nalu)) {
				_naluHeader->parse(nalu->b_rptr);
				if (_naluHeader->getAbsType().isKeyFramePart()) {
					_freeze = false;
					break;
				}
			}
		}
		if (!_freeze) {
			decodeFrame(encodedFrame, timestamp);
			return true;
		}
		return false; /* We can't decode without a new key rame, returning false will trigger the sending of a PLI */
	} catch (const runtime_error &e) {
		ms_error("VideoToolboxDecoder: %s", e.what());
		ms_error("VideoToolboxDecoder: feeding failed");
		if (typeid(e) == typeid(InvalidSessionError)) {
			destroyDecoder();
		}
		return false;
	}
}

VideoDecoder::Status VideoToolboxDecoder::fetch(mblk_t *&frame) {
	std::lock_guard<std::mutex> lck(_mutex);
	if (_queue.empty()) {
		frame = nullptr;
		return NoFrameAvailable;
	} else {
		frame = _queue.front().getData();
		_queue.pop_front();
		return frame ? NoError : DecodingFailure;
	}
}

void VideoToolboxDecoder::createDecoder() {
	OSStatus status;
	VTDecompressionOutputCallbackRecord dec_cb = {outputCb, this};

	vt_dec_message("creating a decoding session");

	/*
	 * Since the decoder might be destroyed and recreated during the lifetime of the filter (for example
	 * when SPS/PPS change because of video size changed by remote encoder), we must reset the _destroying flag.
	 */
	_mutex.lock();
	if (_destroying) _destroying = false;
	_mutex.unlock();
	formatDescFromSpsPps();

	CFMutableDictionaryRef decoder_params = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, NULL, NULL);
#if !TARGET_OS_IPHONE
	CFDictionarySetValue(decoder_params, kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder,
	                     kCFBooleanTrue);
#endif

	CFMutableDictionaryRef pixel_parameters =
	    CFDictionaryCreateMutable(kCFAllocatorDefault, 1, NULL, &kCFTypeDictionaryValueCallBacks);
	int32_t format = kCVPixelFormatType_420YpCbCr8Planar;
	CFNumberRef value = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &format);
	CFDictionarySetValue(pixel_parameters, kCVPixelBufferPixelFormatTypeKey, value);
	CFRelease(value);

	status = VTDecompressionSessionCreate(kCFAllocatorDefault, _formatDesc, decoder_params, pixel_parameters, &dec_cb,
	                                      &_session);
	CFRelease(pixel_parameters);
	CFRelease(decoder_params);
	if (status != noErr) {
		throw runtime_error("could not create the decoding context: " + toString(status));
	} else {
#if !TARGET_OS_IPHONE
		CFBooleanRef hardware_acceleration;
		status = VTSessionCopyProperty(_session, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder,
		                               kCFAllocatorDefault, &hardware_acceleration);
		if (status != noErr) {
			vt_dec_error("could not read kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder property: %s",
			             toString(status).c_str());
		} else {
			if (hardware_acceleration != NULL && CFBooleanGetValue(hardware_acceleration)) {
				vt_dec_message("hardware acceleration enabled");
			} else {
				vt_dec_warning("hardware acceleration not enabled");
			}
		}
		if (hardware_acceleration != NULL) CFRelease(hardware_acceleration);
#endif

#if TARGET_OS_IPHONE // kVTDecompressionPropertyKey_RealTime is only available on MacOSX after 10.10 version
		status = VTSessionSetProperty(_session, kVTDecompressionPropertyKey_RealTime, kCFBooleanTrue);
		if (status != noErr) {
			vt_dec_warning("could not be able to switch to real-time mode: %s", toString(status).c_str());
		}
#endif
	}
}

void VideoToolboxDecoder::destroyDecoder() {
	vt_dec_message("destroying decoder");

	// Notify the output callback that the decoder
	// is in an instable state from now. This is necessary
	// as the output callback may be called until the
	// decoding session is completely destroyed.
	_mutex.lock();
	_destroying = true;
	_mutex.unlock();

	VTDecompressionSessionWaitForAsynchronousFrames(_session);
	VTDecompressionSessionInvalidate(_session);
	CFRelease(_session);
	CFRelease(_formatDesc);
	_session = nullptr;
	_formatDesc = nullptr;
	_destroying = false;
}

void VideoToolboxDecoder::decodeFrame(MSQueue *encodedFrame, uint64_t timestamp) {
	CMBlockBufferRef stream = nullptr;
	OSStatus status = CMBlockBufferCreateEmpty(kCFAllocatorDefault, 0, kCMBlockBufferAssureMemoryNowFlag, &stream);
	if (status != kCMBlockBufferNoErr) {
		throw runtime_error("failure while creating input buffer for decoder");
	}
	while (mblk_t *nalu = ms_queue_get(encodedFrame)) {
		CMBlockBufferRef nalu_block;
		size_t nalu_block_size = msgdsize(nalu) + _naluSizeLength;
		uint32_t nalu_size = htonl(msgdsize(nalu));

		CMBlockBufferCreateWithMemoryBlock(NULL, NULL, nalu_block_size, NULL, NULL, 0, nalu_block_size,
		                                   kCMBlockBufferAssureMemoryNowFlag, &nalu_block);
		CMBlockBufferReplaceDataBytes(&nalu_size, nalu_block, 0, _naluSizeLength);
		CMBlockBufferReplaceDataBytes(nalu->b_rptr, nalu_block, _naluSizeLength, msgdsize(nalu));
		CMBlockBufferAppendBufferReference(stream, nalu_block, 0, nalu_block_size, 0);
		CFRelease(nalu_block);
		freemsg(nalu);
	}
	if (!CMBlockBufferIsEmpty(stream)) {
		CMSampleBufferRef sample = NULL;
		CMSampleTimingInfo timing_info;
		timing_info.duration = kCMTimeInvalid;
		timing_info.presentationTimeStamp = CMTimeMake(timestamp, 1000);
		timing_info.decodeTimeStamp = CMTimeMake(timestamp, 1000);
		CMSampleBufferCreate(kCFAllocatorDefault, stream, TRUE, NULL, NULL, _formatDesc, 1, 1, &timing_info, 0, NULL,
		                     &sample);
		status = VTDecompressionSessionDecodeFrame(
		    _session, sample, kVTDecodeFrame_EnableAsynchronousDecompression | kVTDecodeFrame_1xRealTimePlayback, NULL,
		    NULL);
		CFRelease(sample);
		if (status != noErr) {
			CFRelease(stream);
			if (status == kVTInvalidSessionErr) {
				throw InvalidSessionError();
			} else {
				throw runtime_error("error while passing encoded frames to the decoder: " + toString(status));
			}
		}
	}
	CFRelease(stream);
}

void VideoToolboxDecoder::formatDescFromSpsPps() {
	try {
		unique_ptr<VideoToolboxUtilities> utils(VideoToolboxUtilities::create(_mime));
		CMFormatDescriptionRef format_desc = utils->createFormatDescription(*_psStore);
		CMVideoDimensions vsize = CMVideoFormatDescriptionGetDimensions(format_desc);
		vt_dec_message("new video format %dx%d", int(vsize.width), int(vsize.height));
		if (_formatDesc) CFRelease(_formatDesc);
		_formatDesc = format_desc;
	} catch (const AppleOSError &e) {
		throw runtime_error(string("cannot create format description: ") + e.what());
	}
}

void VideoToolboxDecoder::outputCb(void *decompressionOutputRefCon,
                                   BCTBX_UNUSED(void *sourceFrameRefCon),
                                   OSStatus status,
                                   BCTBX_UNUSED(VTDecodeInfoFlags infoFlags),
                                   CVImageBufferRef imageBuffer,
                                   BCTBX_UNUSED(CMTime presentationTimeStamp),
                                   BCTBX_UNUSED(CMTime presentationDuration)) {
	auto ctx = static_cast<VideoToolboxDecoder *>(decompressionOutputRefCon);

	std::lock_guard<std::mutex> lck(ctx->_mutex);

	if (ctx->_destroying) {
		return;
	}

	if (status != noErr || imageBuffer == nullptr) {
		vt_dec_error("fail to decode one frame: %s", toString(status).c_str());
		ctx->_queue.push_back(Frame());
		return;
	}

	MSPicture pixbuf_desc;
	CGSize vsize = CVImageBufferGetEncodedSize(imageBuffer);
	mblk_t *pixbuf = ms_yuv_buf_allocator_get(ctx->_pixbufAllocator, &pixbuf_desc, int(vsize.width), int(vsize.height));

	uint8_t *src_planes[4] = {0};
	int src_strides[4] = {0};
	CVPixelBufferLockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);
	for (size_t i = 0; i < 3; i++) {
		src_planes[i] = static_cast<uint8_t *>(CVPixelBufferGetBaseAddressOfPlane(imageBuffer, i));
		src_strides[i] = (int)CVPixelBufferGetBytesPerRowOfPlane(imageBuffer, i);
	}
	ms_yuv_buf_copy(src_planes, src_strides, pixbuf_desc.planes, pixbuf_desc.strides,
	                {int(vsize.width), int(vsize.height)});
	CVPixelBufferUnlockBaseAddress(imageBuffer, kCVPixelBufferLock_ReadOnly);

	ctx->_queue.push_back(Frame(pixbuf));
}

} // namespace mediastreamer
