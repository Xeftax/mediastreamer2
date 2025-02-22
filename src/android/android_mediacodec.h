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
#include "media/NdkMediaCodec.h"
#include "mediastreamer2/mscommon.h"
#include "mediastreamer2/msvideo.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
	int format;
	int width;
	int height;
	MSRect crop_rect;
	uint64_t timestamp;
	int nplanes;
	int row_strides[4];
	int pixel_strides[4];
	uint8_t *buffers[4];
	void *priv_ptr;
} AMediaImage;

media_status_t AMediaCodec_reset(AMediaCodec *codec);
void AMediaCodec_setParams(AMediaCodec *codec, const AMediaFormat *fmt);
bool AMediaCodec_getInputImage(AMediaCodec *codec, int index, AMediaImage *image);
bool AMediaCodec_getOutputImage(AMediaCodec *codec, int index, AMediaImage *image);
void AMediaImage_close(AMediaImage *image);
bool_t AMediaImage_isAvailable(void);

bool_t AMediaCodec_checkCodecAvailability(const char *mime);

#ifdef __cplusplus
} // extern "C"
#endif
