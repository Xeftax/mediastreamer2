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

#include <algorithm>
#include <cstring>

#include <sys/system_properties.h>

#include <ortp/b64.h>

#include "filter-wrapper/decoding-filter-wrapper.h"
#include "h264-nal-unpacker.h"
#include "h264-utils.h"
#include "h26x-decoder-filter.h"

#include "media-codec-h264-decoder.h"

using namespace b64;
using namespace std;

namespace mediastreamer {

MediaCodecH264Decoder::MediaCodecH264Decoder() : MediaCodecDecoder("video/avc") {
	DeviceInfo info = getDeviceInfo();
	ms_message("MediaCodecH264Decoder: got device info: %s", info.toString().c_str());
	if (find(_tvDevices.cbegin(), _tvDevices.cend(), info) != _tvDevices.cend()) {
		ms_message("MediaCodecH264Decoder: found exact device, enabling reset on new SPS/PPS mode");
		_resetOnPsReceiving = true;
	} else {
		for (auto it = _tvDevices.cbegin(); it != _tvDevices.cend(); it++) {
			if (info.weakEquals(*it)) {
				ms_message(
				    "MediaCodecH264Decoder: found matching manufacturer/platform, enabling reset on new SPS/PPS mode");
				_resetOnPsReceiving = true;
				return;
			}
		}
	}
}

MediaCodecH264Decoder::~MediaCodecH264Decoder() {
	if (_lastSps) freemsg(_lastSps);
}

bool MediaCodecH264Decoder::setParameterSets(MSQueue *parameterSet, uint64_t timestamp) {
	if (_resetOnPsReceiving) {
		for (mblk_t *m = ms_queue_peek_first(parameterSet); !ms_queue_end(parameterSet, m);
		     m = ms_queue_next(parameterSet, m)) {
			MSH264NaluType type = ms_h264_nalu_get_type(m);
			if (type == MSH264NaluTypeSPS && isNewPps(m)) {
				int32_t curWidth, curHeight;
				AMediaFormat_getInt32(_format, "width", &curWidth);
				AMediaFormat_getInt32(_format, "height", &curHeight);
				MSVideoSize vsize = ms_h264_sps_get_video_size(m);
				if (vsize.width != curWidth || vsize.height != curHeight) {
					ms_message(
					    "MediaCodecDecoder: restarting decoder because the video size has changed (%dx%d->%dx%d)",
					    curWidth, curHeight, vsize.width, vsize.height);
					AMediaFormat_setInt32(_format, "width", vsize.width);
					AMediaFormat_setInt32(_format, "height", vsize.height);
					stopImpl();
					startImpl();
				}
			}
		}
	}
	return MediaCodecDecoder::setParameterSets(parameterSet, timestamp);
}

bool MediaCodecH264Decoder::DeviceInfo::operator==(const DeviceInfo &info) const {
	return this->manufacturer == info.manufacturer && this->model == info.model && this->platform == info.platform;
}

bool MediaCodecH264Decoder::DeviceInfo::weakEquals(const DeviceInfo &info) const {
	return this->manufacturer == info.manufacturer && this->platform == info.platform;
}

std::string MediaCodecH264Decoder::DeviceInfo::toString() const {
	ostringstream os;
	os << "{ '" << this->manufacturer << "', '" << this->model << "', '" << this->platform << "' }";
	return os.str();
}

bool MediaCodecH264Decoder::isNewPps(mblk_t *sps) {
	if (_lastSps == nullptr) {
		_lastSps = dupmsg(sps);
		return true;
	}
	const size_t spsSize = size_t(sps->b_wptr - sps->b_rptr);
	const size_t lastSpsSize = size_t(_lastSps->b_wptr - _lastSps->b_rptr);
	if (spsSize != lastSpsSize || memcmp(_lastSps->b_rptr, sps->b_rptr, spsSize) != 0) {
		freemsg(_lastSps);
		_lastSps = dupmsg(sps);
		return true;
	}
	return false;
}

MediaCodecH264Decoder::DeviceInfo MediaCodecH264Decoder::getDeviceInfo() {
	const size_t propSize = 256;
	char manufacturer[propSize];
	char model[propSize];
	char platform[propSize];

	if (__system_property_get("ro.product.manufacturer", manufacturer) < 0) manufacturer[0] = '\0';
	if (__system_property_get("ro.product.model", model) < 0) model[0] = '\0';
	if (__system_property_get("ro.board.platform", platform) < 0) platform[0] = '\0';

	return DeviceInfo({manufacturer, model, platform});
}

class MediaCodecH264DecoderFilterImpl : public H26xDecoderFilter {
public:
	MediaCodecH264DecoderFilterImpl(MSFilter *f) : H26xDecoderFilter(f, new MediaCodecH264Decoder()) {
	}
	~MediaCodecH264DecoderFilterImpl() {
		if (_sps) freemsg(_sps);
		if (_pps) freemsg(_pps);
	}

	void process() {
		if (_sps && _pps) {
			static_cast<H264NalUnpacker &>(*_unpacker).setOutOfBandSpsPps(_sps, _pps);
			_sps = nullptr;
			_pps = nullptr;
		}
		H26xDecoderFilter::process();
	}

	void addFmtp(const char *fmtp) {
		char value[256];
		if (fmtp_get_value(fmtp, "sprop-parameter-sets", value, sizeof(value))) {
			char *b64_sps = value;
			char *b64_pps = strchr(value, ',');

			if (b64_pps) {
				*b64_pps = '\0';
				++b64_pps;
				ms_message("Got sprop-parameter-sets : sps=%s , pps=%s", b64_sps, b64_pps);
				_sps = allocb(sizeof(value), 0);
				_sps->b_wptr += b64_decode(b64_sps, strlen(b64_sps), _sps->b_wptr, sizeof(value));
				_pps = allocb(sizeof(value), 0);
				_pps->b_wptr += b64_decode(b64_pps, strlen(b64_pps), _pps->b_wptr, sizeof(value));
			}
		}
	}

private:
	void updateSps(mblk_t *sps) {
		if (_sps) freemsg(_sps);
		_sps = dupb(sps);
	}

	void updatePps(mblk_t *pps) {
		if (_pps) freemsg(_pps);
		if (pps) _pps = dupb(pps);
		else _pps = nullptr;
	}

	bool checkSpsChange(mblk_t *sps) {
		bool ret = false;
		if (_sps) {
			ret = (msgdsize(sps) != msgdsize(_sps)) || (memcmp(_sps->b_rptr, sps->b_rptr, msgdsize(sps)) != 0);

			if (ret) {
				ms_message("MediaCodecDecoder: SPS changed ! %i,%i", (int)msgdsize(sps), (int)msgdsize(_sps));
				updateSps(sps);
				updatePps(nullptr);
			}
		} else {
			ms_message("MediaCodecDecoder: receiving first SPS");
			updateSps(sps);
		}
		return ret;
	}

	bool checkPpsChange(mblk_t *pps) {
		bool ret = false;
		if (_pps) {
			ret = (msgdsize(pps) != msgdsize(_pps)) || (memcmp(_pps->b_rptr, pps->b_rptr, msgdsize(pps)) != 0);

			if (ret) {
				ms_message("MediaCodecDecoder: PPS changed ! %i,%i", (int)msgdsize(pps), (int)msgdsize(_pps));
				updatePps(pps);
			}
		} else {
			ms_message("MediaCodecDecoder: receiving first PPS");
			updatePps(pps);
		}
		return ret;
	}

	mblk_t *_sps = nullptr;
	mblk_t *_pps = nullptr;
};

const std::vector<const MediaCodecH264Decoder::DeviceInfo> MediaCodecH264Decoder::_tvDevices = {
    {"Amlogic", "Quad-Core Enjoy TV Box", "gxl"},
    {"rockchip", "X9-LX", "rk3288"},
    {"rockchip", "rk3288", "rk3288"},
    {"rockchip", "rk3399", "rk3399"},
    {"rockchip", "rk3399pro", "rk3399pro"},
    {"rockchip", "rk3368", "rk3368"},
    {"rockchip", "Sasincomm S09", "rk3126c"},
    {"freescale", "Control4-imx8mm", "imx8"}};

} // namespace mediastreamer

using namespace mediastreamer;

MS_DECODING_FILTER_WRAPPER_METHODS_DECLARATION(MediaCodecH264Decoder);
MS_DECODING_FILTER_WRAPPER_DESCRIPTION_DECLARATION(MediaCodecH264Decoder,
                                                   MS_MEDIACODEC_H264_DEC_ID,
                                                   "A H264 decoder based on MediaCodec API.",
                                                   "H264",
                                                   MS_FILTER_IS_PUMP);
