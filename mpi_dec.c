/*
Copyright (c) 2017, Martin Cerveny

All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:
	* Redistributions of source code must retain the above copyright
	  notice, this list of conditions and the following disclaimer.
	* Redistributions in binary form must reproduce the above copyright
	  notice, this list of conditions and the following disclaimer in the
	  documentation and/or other materials provided with the distribution.
	* Neither the name of the copyright holder nor the
	  names of its contributors may be used to endorse or promote products
	  derived from this software without specific prior written permission.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY
DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
(INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
(INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

#define MODULE_TAG "mpi_dec"

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>
#include <stropts.h>
#include <inttypes.h>
#include <signal.h>

#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <linux/videodev2.h>

#include <rk_mpi.h>

#define READ_BUF_SIZE (SZ_1M)
#define MAX_FRAMES 24		// min 16 and 20+ recommended (mpp/readme.txt) ?

typedef struct hdr_static_metadata {
	uint16_t eotf;
	uint16_t type;
	uint16_t display_primaries_x[3];
	uint16_t display_primaries_y[3];
	uint16_t white_point_x;
	uint16_t white_point_y;
	uint16_t max_mastering_display_luminance;
	uint16_t min_mastering_display_luminance;
	uint16_t max_fall;
	uint16_t max_cll;
	uint16_t min_cll;
} hdr_static_metadata;

struct {
	int fd;
	drmModeAtomicReqPtr request;
	
	uint32_t plane_id, crtc_id, connector_id;
	drmModePropertyPtr plane_props[32];
	drmModePropertyPtr connector_props[16];

	hdr_static_metadata hdr_panel;

	int frm_eos;
	
	int crtc_width;
	int crtc_height;
	RK_U32 frm_width;
	RK_U32 frm_height;
	int fb_x, fb_y, fb_width, fb_height;
	
	pthread_mutex_t mutex;
	pthread_cond_t cond;
	int fb_id;
	int skipped_frames;
} drm;

struct {
	MppCtx		  ctx;
	MppApi		  *mpi;
	
	struct timespec first_frame_ts;

	MppBufferGroup	frm_grp;
	struct {
		int prime_fd;
		int fb_id;
		uint32_t handle;
	} frame_to_drm[MAX_FRAMES];
} mpi;

enum supported_eotf_type {
	TRADITIONAL_GAMMA_SDR = 0,
	TRADITIONAL_GAMMA_HDR,
	SMPTE_ST2084,
	HLG,
	FUTURE_EOTF
};

enum drm_hdmi_output_type {
	DRM_HDMI_OUTPUT_DEFAULT_RGB,
	DRM_HDMI_OUTPUT_YCBCR444,
	DRM_HDMI_OUTPUT_YCBCR422,
	DRM_HDMI_OUTPUT_YCBCR420,
	DRM_HDMI_OUTPUT_YCBCR_HQ,
	DRM_HDMI_OUTPUT_YCBCR_LQ,
	DRM_HDMI_OUTPUT_INVALID,
};


int drm_object_add_property(drmModeAtomicReq *request, uint32_t id, drmModePropertyPtr *props, char *name, uint64_t value)
{
   while (*props) {
       if (!strcasecmp(name, (*props)->name)) {
           return drmModeAtomicAddProperty(drm.request, id, (*props)->prop_id, value);
       }
       props++;
   }

   return -EINVAL;
}


// frame_thread
//
// - allocate DRM buffers and DRM FB based on frame size
// - pickup frame in blocking mode and output to screen overlay

void *frame_thread(void *param)
{
	int ret;
	int i;	
	MppFrame  frame  = NULL;
	int frid = 0;
	int frm_eos = 0;

	printf("FRAME THREAD START\n");
	while (!drm.frm_eos) {
		struct timespec ts, ats;
		
		assert(!frame);
		ret = mpi.mpi->decode_get_frame(mpi.ctx, &frame);
		assert(!ret);
		clock_gettime(CLOCK_MONOTONIC, &ats);
		//printf("GETFRAME %3d.%06d frid %d\n", ats.tv_sec, ats.tv_nsec/1000, frid);
		if (frame) {
			if (mpp_frame_get_info_change(frame)) {
				// new resolution
				assert(!mpi.frm_grp);

				drm.frm_width = mpp_frame_get_width(frame);
				drm.frm_height = mpp_frame_get_height(frame);
				RK_U32 hor_stride = mpp_frame_get_hor_stride(frame);
				RK_U32 ver_stride = mpp_frame_get_ver_stride(frame);
				MppFrameFormat fmt = mpp_frame_get_fmt(frame);
				assert((fmt == MPP_FMT_YUV420SP) || (fmt == MPP_FMT_YUV420SP_10BIT));	// only supported for testing MPP_FMT_YUV420SP[_10BIT] == DRM_FORMAT_NV12[_10]

				printf("frame changed %d(%d)x%d(%d)\n", drm.frm_width, hor_stride, drm.frm_height, ver_stride);
#if 0				
				// position overlay, expand to full screen
				drm.fb_x = 0;
				drm.fb_y = 0;
				drm.fb_width = drm.crtc_width;
				drm.fb_height = drm.crtc_height;
#else				
				// position overlay, scale to pixel ratio 1:1
				float crt_ratio = (float)drm.crtc_width/drm.crtc_height;
				float frame_ratio = (float)drm.frm_width/drm.frm_height;
				
				if (crt_ratio>frame_ratio) {
					drm.fb_width = frame_ratio/crt_ratio*drm.crtc_width;
					drm.fb_height = drm.crtc_height;
					drm.fb_x = (drm.crtc_width-drm.fb_width)/2;
					drm.fb_y = 0;
				}
				else {
					drm.fb_width = drm.crtc_width;
					drm.fb_height = crt_ratio/frame_ratio*drm.crtc_height;
					drm.fb_x = 0;
					drm.fb_y = (drm.crtc_height-drm.fb_height)/2;
				}
#endif				

				// create new external frame group and allocate (commit flow) new DRM buffers and DRM FB
				assert(!mpi.frm_grp);
				ret = mpp_buffer_group_get_external(&mpi.frm_grp, MPP_BUFFER_TYPE_DRM);
				assert(!ret);					
				for (i=0; i<MAX_FRAMES; i++) {
					
					// new DRM buffer
					struct drm_mode_create_dumb dmcd;
					memset(&dmcd, 0, sizeof(dmcd));
					dmcd.bpp = fmt==MPP_FMT_YUV420SP?8:10;
					dmcd.width = hor_stride;
					dmcd.height = ver_stride*2; // documentation say not v*2/3 but v*2 (additional info included)
					do {
						ret = ioctl(drm.fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcd);
					} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
					assert(!ret);
					assert(dmcd.pitch==(fmt==MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8));
					assert(dmcd.size==(fmt == MPP_FMT_YUV420SP?hor_stride:hor_stride*10/8)*ver_stride*2);
					mpi.frame_to_drm[i].handle = dmcd.handle;
					
					// commit DRM buffer to frame group
					struct drm_prime_handle dph;
					memset(&dph, 0, sizeof(struct drm_prime_handle));
					dph.handle = dmcd.handle;
					dph.fd = -1;
					do {
						ret = ioctl(drm.fd, DRM_IOCTL_PRIME_HANDLE_TO_FD, &dph);
					} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
					assert(!ret);
					MppBufferInfo info;
					memset(&info, 0, sizeof(info));
					info.type = MPP_BUFFER_TYPE_DRM;
					info.size = dmcd.width*dmcd.height;
					info.fd = dph.fd;
					ret = mpp_buffer_commit(mpi.frm_grp, &info);
					assert(!ret);
					mpi.frame_to_drm[i].prime_fd = info.fd; // dups fd						
					if (dph.fd != info.fd) {
						ret = close(dph.fd);
						assert(!ret);
					}

					// allocate DRM FB from DRM buffer
					uint32_t handles[4], pitches[4], offsets[4];
					memset(handles, 0, sizeof(handles));
					memset(pitches, 0, sizeof(pitches));
					memset(offsets, 0, sizeof(offsets));
					handles[0] = mpi.frame_to_drm[i].handle;
					offsets[0] = 0;
					pitches[0] = hor_stride;						
					handles[1] = mpi.frame_to_drm[i].handle;
					offsets[1] = hor_stride * ver_stride;
					pitches[1] = hor_stride;
					ret = drmModeAddFB2(drm.fd, drm.frm_width, drm.frm_height, fmt==MPP_FMT_YUV420SP?DRM_FORMAT_NV12:DRM_FORMAT_NV12_10, handles, pitches, offsets, &mpi.frame_to_drm[i].fb_id, 0);
					assert(!ret);
				}

				// register external frame group
				ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_EXT_BUF_GROUP, mpi.frm_grp);
				ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_INFO_CHANGE_READY, NULL);

				// compute HDR metadata
				MppFrameColorTransferCharacteristic colorTrc = mpp_frame_get_color_trc(frame);
				MppFrameColorPrimaries colorPri = mpp_frame_get_color_primaries(frame);
				MppFrameColorSpace colorSpc = mpp_frame_get_colorspace(frame);

				printf("COLOR primaries %d transfer %d space %d\n", colorPri, colorTrc, colorSpc);

				hdr_static_metadata hdr_source;
				uint64_t color_space = V4L2_COLORSPACE_DEFAULT;
				bzero(&hdr_source, sizeof(hdr_source));  // default all zero (SDR+not set primaries)

				switch (colorPri) {
					// https://github.com/mpv-player/mpv/blob/d7d670fcbf3974894429e5693e76536f0d2fe847/video/csputils.c#L349
					case MPP_FRAME_PRI_RESERVED0:
					case MPP_FRAME_PRI_UNSPECIFIED:
						break;
					case MPP_FRAME_PRI_BT709:
				            	hdr_source.display_primaries_x[0] = 0.640*50000;
				            	hdr_source.display_primaries_y[0] = 0.330*50000;
				            	hdr_source.display_primaries_x[1] = 0.300*50000;
				            	hdr_source.display_primaries_y[1] = 0.600*50000;
				            	hdr_source.display_primaries_x[2] = 0.150*50000;
				            	hdr_source.display_primaries_y[2] = 0.060*50000;
				            	hdr_source.white_point_x = 0.31271*50000;
						hdr_source.white_point_y = 0.32902*50000;
						break;
					case MPP_FRAME_PRI_BT2020:
				            	hdr_source.display_primaries_x[0] = 0.708*50000;
				            	hdr_source.display_primaries_y[0] = 0.292*50000;
				            	hdr_source.display_primaries_x[1] = 0.170*50000;
				            	hdr_source.display_primaries_y[1] = 0.797*50000;
				            	hdr_source.display_primaries_x[2] = 0.131*50000;
				            	hdr_source.display_primaries_y[2] = 0.046*50000;
				            	hdr_source.white_point_x = 0.31271*50000;
						hdr_source.white_point_y = 0.32902*50000;
						break;

					case MPP_FRAME_PRI_RESERVED:
					case MPP_FRAME_PRI_BT470M:
					case MPP_FRAME_PRI_BT470BG:
					case MPP_FRAME_PRI_SMPTE170M:
					case MPP_FRAME_PRI_SMPTE240M:
					case MPP_FRAME_PRI_FILM:
					case MPP_FRAME_PRI_SMPTEST428_1:
					default: assert(!colorPri);
				}

				switch (colorTrc) {
					// https://github.com/mpv-player/mpv/blob/d7d670fcbf3974894429e5693e76536f0d2fe847/video/csputils.c#L187
					case MPP_FRAME_TRC_RESERVED0:
					case MPP_FRAME_TRC_UNSPECIFIED:
						break;
					case MPP_FRAME_TRC_SMPTEST2084:
						hdr_source.eotf = SMPTE_ST2084;
						break;
					case MPP_FRAME_TRC_ARIB_STD_B67:	
						hdr_source.eotf = HLG;
						break;
					case MPP_FRAME_TRC_BT709:
					case MPP_FRAME_TRC_SMPTE170M:
					case MPP_FRAME_TRC_SMPTE240M:
					case MPP_FRAME_TRC_BT1361_ECG:
					case MPP_FRAME_TRC_BT2020_10:
					case MPP_FRAME_TRC_BT2020_12:
						hdr_source.eotf = TRADITIONAL_GAMMA_SDR;
						break;

					case MPP_FRAME_TRC_RESERVED:
					case MPP_FRAME_TRC_GAMMA22:
					case MPP_FRAME_TRC_GAMMA28:
					case MPP_FRAME_TRC_LINEAR:
					case MPP_FRAME_TRC_LOG:
					case MPP_FRAME_TRC_LOG_SQRT:
					case MPP_FRAME_TRC_IEC61966_2_4:
					case MPP_FRAME_TRC_IEC61966_2_1:
					case MPP_FRAME_TRC_SMPTEST428_1:
					default: assert(!colorTrc);
				}


				switch (colorSpc) {
					case MPP_FRAME_SPC_UNSPECIFIED:
						break;
					case MPP_FRAME_SPC_RGB:		// patch
						switch (colorPri) {
							case MPP_FRAME_PRI_BT709:
								color_space = V4L2_COLORSPACE_REC709; break;
							case MPP_FRAME_PRI_BT2020:
								if (hdr_source.eotf!=TRADITIONAL_GAMMA_SDR) color_space = V4L2_COLORSPACE_BT2020; break;
						}
						break;
					case MPP_FRAME_SPC_BT709:
						color_space = V4L2_COLORSPACE_REC709; break;
					case MPP_FRAME_SPC_BT470BG:
						color_space = V4L2_COLORSPACE_470_SYSTEM_BG; break;
					case MPP_FRAME_SPC_SMPTE170M:
						color_space = V4L2_COLORSPACE_SMPTE170M; break;
					case MPP_FRAME_SPC_SMPTE240M:
						color_space = V4L2_COLORSPACE_SMPTE240M; break;

					case MPP_FRAME_SPC_YCOCG:
					case MPP_FRAME_SPC_FCC:
					case MPP_FRAME_SPC_BT2020_NCL:
					case MPP_FRAME_SPC_BT2020_CL:
					case MPP_FRAME_SPC_RESERVED:
					default: assert(!colorSpc);
				}

				hdr_source.type = 0; // Static Metadata Type 1

				// send atomic
				drmModeAtomicSetCursor(drm.request, 0);

				uint32_t hdr_source_id;
				ret = drmModeCreatePropertyBlob(drm.fd, &hdr_source, sizeof(hdr_source), &hdr_source_id);
				assert(!ret);

				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "FB_ID", mpi.frame_to_drm[0].fb_id);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "CRTC_ID", drm.crtc_id);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "SRC_X", 0 << 16);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "SRC_Y", 0 << 16);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "SRC_W", drm.frm_width << 16);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "SRC_H", drm.frm_height << 16);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "CRTC_X", drm.fb_x);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "CRTC_Y", drm.fb_y);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "CRTC_W", drm.fb_width);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "CRTC_H", drm.fb_height);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "ZPOS", 0);
				assert(ret>0);
				ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "COLOR_SPACE", color_space);
				assert(ret>0);


				if (drm.hdr_panel.eotf) {
					// HDR panel
					ret = drm_object_add_property(drm.request, drm.connector_id, drm.connector_props, "HDR_SOURCE_METADATA", hdr_source_id);
					assert(ret>0);
					ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "EOTF", hdr_source.eotf);
					assert(ret>0);
				} else {
					// SDR panel
					ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "EOTF", hdr_source.eotf? TRADITIONAL_GAMMA_HDR : TRADITIONAL_GAMMA_SDR);
					assert(ret>0);
				}

				ret = drm_object_add_property(drm.request, drm.connector_id, drm.connector_props, "hdmi_output_format", DRM_HDMI_OUTPUT_YCBCR_LQ);
				assert(ret>0);

				ret = drmModeAtomicCommit(drm.fd, drm.request, DRM_MODE_ATOMIC_NONBLOCK, NULL);
				assert(!ret);

				drmModeDestroyPropertyBlob(drm.fd, hdr_source_id);

				
			} else {
				// regular framer received
				if (!mpi.first_frame_ts.tv_sec) {
					ts = ats;
					mpi.first_frame_ts = ats;
				}

				MppBuffer buffer = mpp_frame_get_buffer(frame);					
				if (buffer) {
					// find fb_id by frame prime_fd
					MppBufferInfo info;
					ret = mpp_buffer_info_get(buffer, &info);
					assert(!ret);
					for (i=0; i<MAX_FRAMES; i++) {
						if (mpi.frame_to_drm[i].prime_fd == info.fd) break;
					}
					assert(i!=MAX_FRAMES);

					uint64_t time_us=(ats.tv_sec - mpi.first_frame_ts.tv_sec)*1000000ll + ((ats.tv_nsec - mpi.first_frame_ts.tv_nsec)/1000ll) % 1000000ll;
					printf("FRAME time %d.%06d render_time %6jd us fps=(time(%jd us)/frame(%d)) %3.2f\n",
							ats.tv_sec, ats.tv_nsec/1000,
							(ats.tv_sec - ts.tv_sec)*1000000ll + (((ats.tv_nsec - ts.tv_nsec)/1000ll) % 1000000ll),
							time_us, frid, (float)frid/((float)time_us*0.000001));

					ts = ats;
					frid++;
					
					// send DRM FB to display thread
					ret = pthread_mutex_lock(&drm.mutex);
					assert(!ret);
					if (drm.fb_id) drm.skipped_frames++;
					drm.fb_id = mpi.frame_to_drm[i].fb_id;
					ret = pthread_cond_signal(&drm.cond);
					assert(!ret);
					ret = pthread_mutex_unlock(&drm.mutex);
					assert(!ret);
					
				} else printf("FEAME no buff\n");
			}
			
			drm.frm_eos = mpp_frame_get_eos(frame);
			mpp_frame_deinit(&frame);
			frame = NULL;
		} else assert(0);
	}
	printf("FRAME THREAD END\n");
	return NULL;
}

// frame_thread
//
// wait and display last DRM FB (some may be skipped)

void *display_thread(void *param)
{
	int ret;	
	printf("DISPLAY THREAD START\n");

	while (!drm.frm_eos) {
		int fb_id;
		
		ret = pthread_mutex_lock(&drm.mutex);
		assert(!ret);
		while (drm.fb_id==0) {
			pthread_cond_wait(&drm.cond, &drm.mutex);
			assert(!ret);
			if (drm.fb_id == 0 && drm.frm_eos) {
				ret = pthread_mutex_unlock(&drm.mutex);
				assert(!ret);
				goto end;
			}
		}	
		fb_id = drm.fb_id;
		if (drm.skipped_frames) printf("DISPLAY skipped %d\n", drm.skipped_frames);
		drm.fb_id=0;
		drm.skipped_frames=0;
		ret = pthread_mutex_unlock(&drm.mutex);
		assert(!ret);

		if (param==NULL) {
			// show DRM FB in plane

			drmModeAtomicSetCursor(drm.request, 0);

			ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "FB_ID", fb_id);
			assert(ret>0);

			ret = drmModeAtomicCommit(drm.fd, drm.request, DRM_MODE_ATOMIC_NONBLOCK, NULL);
			assert(!ret);

		}
	}
end:	
	printf("DISPLAY THREAD END\n");
}

// signal

int signal_flag = 0;

void sig_handler(int signum)
{
	printf("Received signal %d\n", signum);
	signal_flag++;
}

// main

int main(int argc, char **argv)
{
	int ret;	
	int i, j;

	////////////////////////////////// PARAMETER SETUP

	if (!(argc == 3 || argc == 4))  {
		printf("usage: %s raw_filename mpp_coding_id [nodisplay]\n\n", argv[0]);
		mpp_show_support_format();
		return 0;
	}

	int data_fd=open(argv[1], O_RDONLY);
	assert(data_fd > 0);
		
	MppCodingType mpp_type = (MppCodingType)atoi(argv[2]);
	ret = mpp_check_support_format(MPP_CTX_DEC, mpp_type);
	assert(!ret);
	// MPP_VIDEO_CodingMJPEG only in advanced mode
	assert(mpp_type != MPP_VIDEO_CodingMJPEG);

	////////////////////////////////// SIGNAL SETUP

	signal(SIGINT, sig_handler);
	signal(SIGPIPE, sig_handler);
	
	//////////////////////////////////  DRM SETUP
	
	drm.fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC);
	assert(drm.fd >= 0);

	ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1); // overlays, primary, cursor
	assert(!ret);
	ret = drmSetClientCap(drm.fd, DRM_CLIENT_CAP_ATOMIC, 1); // atomic properties
	assert(!ret);
	drm.request = drmModeAtomicAlloc();
	assert(drm.request);

	drmModeRes *resources = drmModeGetResources(drm.fd);
	assert(resources);

 	//uint64_t prime;
	//ret = drmGetCap(drm.fd, DRM_CAP_PRIME, &prime);
	//assert(!ret);

	// find active monitor
	drmModeConnector *connector;
	for (i = 0; i < resources->count_connectors; ++i) {
		connector = drmModeGetConnector(drm.fd, resources->connectors[i]);
		if (!connector)
			continue;
		if (connector->connection == DRM_MODE_CONNECTED && connector->count_modes > 0) {
			printf("CONNECTOR type: %d id: %d\n", connector->connector_type, connector->connector_id);
			break;
		}
		drmModeFreeConnector(connector);
	}
	assert(i < resources->count_connectors);
	drm.connector_id = connector->connector_id;

	drmModeObjectPropertiesPtr props = drmModeObjectGetProperties(drm.fd, connector->connector_id, DRM_MODE_OBJECT_CONNECTOR);
	assert(props);
	assert(props->count_props<(sizeof(drm.connector_props)/sizeof(drm.connector_props[0])));
	for (i = 0; i < props->count_props; i++) {
		drmModePropertyPtr prop = drmModeGetProperty(drm.fd, props->props[i]);
		if (!prop) continue;
		printf("CONN PROP: %s=%"PRIu64, prop->name, props->prop_values[i]);
		if (drm_property_type_is(prop, DRM_MODE_PROP_ENUM)) {
			printf(" (");
			for (j = 0; j < prop->count_enums; j++)
				printf(" %s=%"PRIu64, prop->enums[j].name, prop->enums[j].value);
			printf(" )\n");
		}	
		else if (drm_property_type_is(prop, DRM_MODE_PROP_RANGE)) {
			printf(" [");
			for (j = 0; j < prop->count_values; j++)
				printf(" %"PRIu64, prop->values[j]);
			printf(" ]\n");
		}
		else printf("\n");
		if (!strcasecmp(prop->name, "HDR_PANEL_METADATA")) {
			assert(drm_property_type_is(prop, DRM_MODE_PROP_BLOB));
			drmModePropertyBlobPtr hdr_panel_metadata = drmModeGetPropertyBlob(drm.fd, props->prop_values[i]);
			assert(hdr_panel_metadata);
			assert(hdr_panel_metadata->length == sizeof(hdr_static_metadata));
			drm.hdr_panel = *((hdr_static_metadata *)hdr_panel_metadata->data);
			printf("HDR CONNECTOR EOTF 0x%x\n", drm.hdr_panel.eotf);
		}
		drm.connector_props[i]=prop;
	}
	drmModeFreeObjectProperties(props);

	drmModeEncoder *encoder;	
	for (i = 0; i < resources->count_encoders; ++i) {
		encoder = drmModeGetEncoder(drm.fd, resources->encoders[i]);
		if (!encoder)
			continue;
		if (encoder->encoder_id == connector->encoder_id) {
			printf("ENCODER id: %d\n", encoder->encoder_id);
			break;
		}
		drmModeFreeEncoder(encoder);
	}
	assert(i < resources->count_encoders);

	drmModeCrtcPtr crtc;	
	for (i = 0; i < resources->count_crtcs; ++i) {
		if (resources->crtcs[i] == encoder->crtc_id) {
			crtc = drmModeGetCrtc(drm.fd, resources->crtcs[i]);
			assert(crtc);
			break;
		}
	}
	assert(i < resources->count_crtcs);
	drm.crtc_id = crtc->crtc_id;
	drm.crtc_width = crtc->width;
	drm.crtc_height = crtc->height;
	uint32_t crtc_bit = (1 << i);

	drmModePlaneRes *plane_resources =  drmModeGetPlaneResources(drm.fd);
	assert(plane_resources);
	
	drmModePlane *plane;
		
	// search for OVERLAY (for active conector, unused, NV12 support)
	for (i = 0; i < plane_resources->count_planes; i++) {		
		plane = drmModeGetPlane(drm.fd, plane_resources->planes[i]);
		if (!plane) continue;
		printf("PLID: %d\n", plane->plane_id);
			
		for (j=0; j<plane->count_formats; j++)
			if (plane->formats[j] ==  DRM_FORMAT_NV12) break; // supported both DRM_FORMAT_NV12 and DRM_FORMAT_NV12_10
		if (j==plane->count_formats) continue;
			
		if (plane->possible_crtcs & crtc_bit) {
			props = drmModeObjectGetProperties(drm.fd, plane_resources->planes[i], DRM_MODE_OBJECT_PLANE);
			if (!props) continue;
			assert(props->count_props<(sizeof(drm.plane_props)/sizeof(drm.plane_props[0])));
			
			for (j = 0; j < props->count_props ; j++) {
				drmModePropertyPtr prop = drmModeGetProperty(drm.fd, props->props[j]);
				if (!prop) continue;
				printf("PLANE PROP: %s=%"PRIu64 "\n", prop->name, props->prop_values[j]);
				if (!strcmp(prop->name, "type") && props->prop_values[j] == DRM_PLANE_TYPE_PRIMARY) { // or OVERLAY
					drm.plane_id = plane->plane_id;
				}
				drm.plane_props[j] = prop;
			}
			drmModeFreeObjectProperties(props);
			if (drm.plane_id) break;
			for (j = 0; j < props->count_props ; j++) {
				drmModeFreeProperty(drm.plane_props[j]);
				drm.plane_props[j] = NULL;
			}
		}
		drmModeFreePlane(plane);
	}
	assert(drm.plane_id);
	printf("PLANE id %d\n", drm.plane_id);

	
	////////////////////////////////////////////// MPI SETUP

	MppPacket packet;
	void *pkt_buf = malloc(READ_BUF_SIZE);
	assert(pkt_buf);
	ret = mpp_packet_init(&packet, pkt_buf, READ_BUF_SIZE);
	assert(!ret);

	ret = mpp_create(&mpi.ctx, &mpi.mpi);
	assert(!ret);

	// decoder split mode (multi-data-input) need to be set before init
	int param = 1;
	ret = mpi.mpi->control(mpi.ctx, MPP_DEC_SET_PARSER_SPLIT_MODE, &param);
	assert(!ret);

	//mpp_env_set_u32("mpi_debug", 0x1);
	//mpp_env_set_u32("mpp_buffer_debug", 0xf);
	//mpp_env_set_u32("h265d_debug", 0xfff);

	ret = mpp_init(mpi.ctx, MPP_CTX_DEC, mpp_type);
	assert(!ret);


	// blocked/wait read of frame in thread
	param = MPP_POLL_BLOCK;
	ret = mpi.mpi->control(mpi.ctx, MPP_SET_OUTPUT_BLOCK, &param);
	assert(!ret);
	
	ret = pthread_mutex_init(&drm.mutex, NULL);
	assert(!ret);
	ret = pthread_cond_init(&drm.cond, NULL);
	assert(!ret);

	pthread_t tid_frame, tid_display;
	ret = pthread_create(&tid_frame, NULL, frame_thread, NULL);
	assert(!ret);
	ret = pthread_create(&tid_display, NULL, display_thread, argc==4?argv[3]:NULL);
	assert(!ret);

	////////////////////////////////////////////// MAIN LOOP
	
	while (1) {
		do {
			ret=read(data_fd, pkt_buf, READ_BUF_SIZE);
		} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
		assert(ret>=0);
		int read_size = ret;
		static RK_S64 pts, dts;
		if (read_size && !signal_flag) {
			mpp_packet_set_pos(packet, pkt_buf);
			mpp_packet_set_length(packet, read_size);
			
			while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
				// buffer 4 packet is hardcoded (actual is MPP_DEC_GET_STREAM_COUNT) and does not support blocking write
				usleep(10000);
			}
			
			RK_U32 s_cnt, v_cnt;
			ret = mpi.mpi->control(mpi.ctx, MPP_DEC_GET_STREAM_COUNT, &s_cnt);
			assert(!ret);			
			ret = mpi.mpi->control(mpi.ctx, MPP_DEC_GET_VPUMEM_USED_COUNT, &v_cnt);
			assert(!ret);	
			struct timespec ts;
			clock_gettime(CLOCK_MONOTONIC, &ts);	
			printf("PACKET SEND %d.%06d S %d V %d\n", ts.tv_sec, ts.tv_nsec/1000, s_cnt, v_cnt);	
		}
		else {
			printf("PACKET EOS\n");
			mpp_packet_set_eos(packet);
			mpp_packet_set_pos(packet, pkt_buf);
			mpp_packet_set_length(packet, 0);
			while (MPP_OK != (ret = mpi.mpi->decode_put_packet(mpi.ctx, packet))) {
				usleep(10000);
			}
			break;
		}
	}
	close(data_fd);

	////////////////////////////////////////////// MPI CLEANUP

	ret = pthread_join(tid_frame, NULL);
	assert(!ret);
	
	ret = pthread_mutex_lock(&drm.mutex);
	assert(!ret);	
	ret = pthread_cond_signal(&drm.cond);
	assert(!ret);	
	ret = pthread_mutex_unlock(&drm.mutex);
	assert(!ret);	

	ret = pthread_join(tid_display, NULL);
	assert(!ret);	
	
	ret = pthread_cond_destroy(&drm.cond);
	assert(!ret);
	ret = pthread_mutex_destroy(&drm.mutex);
	assert(!ret);

	ret = mpi.mpi->reset(mpi.ctx);
	assert(!ret);

	if (mpi.frm_grp) {
		ret = mpp_buffer_group_put(mpi.frm_grp);
		assert(!ret);
		mpi.frm_grp = NULL;
		for (i=0; i<MAX_FRAMES; i++) {
			ret = drmModeRmFB(drm.fd, mpi.frame_to_drm[i].fb_id);
			assert(!ret);
			struct drm_mode_destroy_dumb dmdd;
			memset(&dmdd, 0, sizeof(dmdd));
			dmdd.handle = mpi.frame_to_drm[i].handle;
			do {
				ret = ioctl(drm.fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdd);
			} while (ret == -1 && (errno == EINTR || errno == EAGAIN));
			assert(!ret);
		}
	}
		
	mpp_packet_deinit(&packet);
	mpp_destroy(mpi.ctx);
	free(pkt_buf);
	
	////////////////////////////////////////////// DRM CLEANUP

	// reset plane+connector
	drmModeAtomicSetCursor(drm.request, 0);
	assert(!ret);
	ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "FB_ID", 0);
	assert(ret>0);
	ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "COLOR_SPACE", V4L2_COLORSPACE_DEFAULT);
	assert(ret>0);
	ret = drm_object_add_property(drm.request, drm.plane_id, drm.plane_props, "EOTF", TRADITIONAL_GAMMA_SDR);
	assert(ret>0);
	ret = drm_object_add_property(drm.request, drm.connector_id, drm.connector_props, "HDR_SOURCE_METADATA", 0);
	assert(ret>0);
	ret = drm_object_add_property(drm.request, drm.connector_id, drm.connector_props, "hdmi_output_format", DRM_HDMI_OUTPUT_DEFAULT_RGB);
	assert(ret>0);
	ret = drmModeAtomicCommit(drm.fd, drm.request, DRM_MODE_ATOMIC_NONBLOCK, NULL);
	assert(!ret);
	
	drmModeFreePlane(plane);
	drmModeFreePlaneResources(plane_resources);
	drmModeFreeEncoder(encoder);
	drmModePropertyBlobPtr panel_metadata_prop;
	drmModeFreeConnector(connector);
	drmModeFreeCrtc(crtc);
	drmModeFreeResources(resources);
	drmModeAtomicFree(drm.request);
	close(drm.fd);

	return 0;
}

