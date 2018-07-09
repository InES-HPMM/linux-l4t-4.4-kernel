/*
 * NVIDIA Tegra Video Input Device
 *
 * Copyright (c) 2015-2018, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Bryan Wu <pengw@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/atomic.h>
#include <linux/bitmap.h>
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/nvhost.h>
#include <linux/lcm.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_graph.h>
#include <linux/sched.h>
#include <linux/slab.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-dev.h>
#include <media/v4l2-fh.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>
#include <media/tegra-v4l2-camera.h>
#include <media/camera_common.h>
#include <media/tegra_camera_platform.h>
#include <media/v4l2-dv-timings.h>

#include <linux/clk/tegra.h>

#include "mc_common.h"
#include "vi/vi.h"
#include "mipical/mipi_cal.h"
#include "nvcsi/nvcsi.h"

#define TPG_CSI_GROUP_ID	10

static s64 queue_init_ts;

/*
 * Update the timestamp of the buffer
 */
void set_timestamp(struct tegra_channel_buffer *buf,
				const struct timespec *ts)
{
	buf->buf.timestamp.tv_sec = ts->tv_sec;
	buf->buf.timestamp.tv_usec = ts->tv_nsec / NSEC_PER_USEC;
}

static void gang_buffer_offsets(struct tegra_channel *chan)
{
	int i;
	u32 offset = 0;

	for (i = 0; i < chan->total_ports; i++) {
		switch (chan->gang_mode) {
		case CAMERA_NO_GANG_MODE:
		case CAMERA_GANG_L_R:
		case CAMERA_GANG_R_L:
			offset = chan->gang_bytesperline;
			break;
		case CAMERA_GANG_T_B:
		case CAMERA_GANG_B_T:
			offset = chan->gang_sizeimage;
			break;
		default:
			offset = 0;
		}
		offset = ((offset + TEGRA_SURFACE_ALIGNMENT - 1) &
					~(TEGRA_SURFACE_ALIGNMENT - 1));
		chan->buffer_offset[i] = i * offset;
	}
}

static u32 gang_mode_width(enum camera_gang_mode gang_mode,
					unsigned int width)
{
	if ((gang_mode == CAMERA_GANG_L_R) ||
		(gang_mode == CAMERA_GANG_R_L))
		return width >> 1;
	else
		return width;
}

static u32 gang_mode_height(enum camera_gang_mode gang_mode,
					unsigned int height)
{
	if ((gang_mode == CAMERA_GANG_T_B) ||
		(gang_mode == CAMERA_GANG_B_T))
		return height >> 1;
	else
		return height;
}

static void update_gang_mode_params(struct tegra_channel *chan)
{
	chan->gang_width = gang_mode_width(chan->gang_mode,
						chan->format.width);
	chan->gang_height = gang_mode_height(chan->gang_mode,
						chan->format.height);
	chan->gang_bytesperline = ((chan->gang_width *
					chan->fmtinfo->bpp.numerator) /
					chan->fmtinfo->bpp.denominator);
	chan->gang_sizeimage = chan->gang_bytesperline *
					chan->format.height;
	gang_buffer_offsets(chan);
}

static void update_gang_mode(struct tegra_channel *chan)
{
	int width = chan->format.width;
	int height = chan->format.height;

	/*
	 * At present only 720p, 1080p and 4k resolutions
	 * are supported and only 4K requires gang mode
	 * Update this code with CID for future extensions
	 * Also, validate width and height of images based
	 * on gang mode and surface stride alignment
	 */
	if ((width > 1920) && (height > 1080)) {
		chan->gang_mode = CAMERA_GANG_L_R;
		chan->valid_ports = chan->total_ports;
	} else {
		chan->gang_mode = CAMERA_NO_GANG_MODE;
		chan->valid_ports = 1;
	}

	update_gang_mode_params(chan);
}

static u32 get_aligned_buffer_size(struct tegra_channel *chan,
		u32 bytesperline, u32 height)
{
	u32 height_aligned;
	u32 temp_size, size;

	height_aligned = roundup(height, chan->height_align);
	temp_size = bytesperline * height_aligned;
	size = roundup(temp_size, chan->size_align);

	return size;
}

static void tegra_channel_fmt_align(struct tegra_channel *chan,
				const struct tegra_video_format *vfmt,
				u32 *width, u32 *height, u32 *bytesperline)
{
	unsigned int min_width;
	unsigned int max_width;
	unsigned int min_bpl;
	unsigned int max_bpl;
	unsigned int temp_width;
	unsigned int align, fmt_align;
	unsigned int temp_bpl;
	unsigned int bpl;
	unsigned int numerator, denominator;
	const struct tegra_frac *bpp = &vfmt->bpp;

	/* Init, if un-init */
	if (!*width || !*height) {
		*width = chan->format.width;
		*height = chan->format.height;
	}

	denominator = (!bpp->denominator) ? 1 : bpp->denominator;
	numerator = (!bpp->numerator) ? 1 : bpp->numerator;

	bpl = (*width * numerator) / denominator;
	if (!*bytesperline)
		*bytesperline = bpl;

	/* The transfer alignment requirements are expressed in bytes. Compute
	 * the minimum and maximum values, clamp the requested width and convert
	 * it back to pixels.
	 * use denominator for base width alignment when >1.
	 * use bytesperline to adjust width for applicaton related requriements.
	 */
	fmt_align = (denominator == 1) ? numerator : 1;
	align = lcm(chan->width_align, fmt_align);
	min_width = roundup(TEGRA_MIN_WIDTH, align);
	max_width = rounddown(TEGRA_MAX_WIDTH, align);
	temp_width = roundup(bpl, align);

	*width = (clamp(temp_width, min_width, max_width) * denominator) /
			numerator;
	*height = clamp(*height, TEGRA_MIN_HEIGHT, TEGRA_MAX_HEIGHT);

	/* Clamp the requested bytes per line value. If the maximum bytes per
	 * line value is zero, the module doesn't support user configurable line
	 * sizes. Override the requested value with the minimum in that case.
	 */
	min_bpl = bpl;
	max_bpl = rounddown(TEGRA_MAX_WIDTH, chan->stride_align);
	temp_bpl = roundup(*bytesperline, chan->stride_align);

	*bytesperline = clamp(temp_bpl, min_bpl, max_bpl);
}

static void tegra_channel_update_format(struct tegra_channel *chan,
		u32 width, u32 height, u32 fourcc,
		const struct tegra_frac *bpp,
		u32 preferred_stride)
{
	u32 denominator = (!bpp->denominator) ? 1 : bpp->denominator;
	u32 numerator = (!bpp->numerator) ? 1 : bpp->numerator;
	u32 bytesperline = (width * numerator / denominator);

	chan->format.width = width;
	chan->format.height = height;
	chan->format.pixelformat = fourcc;
	chan->format.bytesperline = preferred_stride ?: bytesperline;

	tegra_channel_fmt_align(chan, chan->fmtinfo,
				&chan->format.width,
				&chan->format.height,
				&chan->format.bytesperline);

	/* Calculate the sizeimage per plane */
	chan->format.sizeimage = get_aligned_buffer_size(chan,
			chan->format.bytesperline, chan->format.height);

	if (fourcc == V4L2_PIX_FMT_NV16)
		chan->format.sizeimage *= 2;
}

static void tegra_channel_fmts_bitmap_init(struct tegra_channel *chan)
{
	int ret, pixel_format_index = 0, init_code = 0;
	struct v4l2_subdev *subdev = chan->subdev_on_csi;
	struct v4l2_subdev_format fmt = {};
	struct v4l2_subdev_mbus_code_enum code = {
		.which = V4L2_SUBDEV_FORMAT_ACTIVE,
	};

	bitmap_zero(chan->fmts_bitmap, MAX_FORMAT_NUM);

	/*
	 * Initialize all the formats available from
	 * the sub-device and extract the corresponding
	 * index from the pre-defined video formats and initialize
	 * the channel default format with the active code
	 * Index zero as the only sub-device is sensor
	 */
	while (1) {
		ret = v4l2_subdev_call(subdev, pad, enum_mbus_code,
				       NULL, &code);
		if (ret < 0)
			/* no more formats */
			break;

		pixel_format_index =
			tegra_core_get_idx_by_code(chan, code.code, 0);
		while (pixel_format_index >= 0) {
			bitmap_set(chan->fmts_bitmap, pixel_format_index, 1);
			/* Set init_code to the first matched format */
			if (!init_code)
				init_code = code.code;
			/* Look for other formats with the same mbus code */
			pixel_format_index = tegra_core_get_idx_by_code(chan,
				code.code, pixel_format_index + 1);
		}

		code.index++;
	}

	if (!init_code) {
		pixel_format_index =
			tegra_core_get_idx_by_code(chan, TEGRA_VF_DEF, 0);
		if (pixel_format_index >= 0) {
			bitmap_set(chan->fmts_bitmap, pixel_format_index, 1);
			init_code = TEGRA_VF_DEF;
		}
	}
		/* Get the format based on active code of the sub-device */
	ret = v4l2_subdev_call(subdev, pad, get_fmt, NULL, &fmt);
	if (ret)
		return;

	/* Initiate the channel format to the first matched format */
	chan->fmtinfo =
		tegra_core_get_format_by_code(chan, fmt.format.code, 0);
	v4l2_fill_pix_format(&chan->format, &fmt.format);
	tegra_channel_update_format(chan, chan->format.width,
				chan->format.height,
				chan->fmtinfo->fourcc,
				&chan->fmtinfo->bpp, 0);

	if (chan->total_ports > 1)
		update_gang_mode(chan);
}

/*
 * -----------------------------------------------------------------------------
 * Tegra channel frame setup and capture operations
 * -----------------------------------------------------------------------------
 */

void release_buffer(struct tegra_channel *chan, struct tegra_channel_buffer* buf)
{
	struct vb2_v4l2_buffer* vbuf = &buf->buf;
	s64 frame_arrived_ts = 0;
	/* release one frame */
	vbuf->sequence = chan->sequence++;
	vbuf->field = V4L2_FIELD_NONE;
	vb2_set_plane_payload(&vbuf->vb2_buf,
		0, chan->format.sizeimage);

	/*
	 * WAR to force buffer state if capture state is not good
	 * WAR - After sync point timeout or error frame capture
	 * the second buffer is intermittently frame of zeros
	 * with no error status or padding.
	 */
	if (chan->capture_state != CAPTURE_GOOD || vbuf->sequence < 2) {
		buf->state = VB2_BUF_STATE_ERROR;
	}

	if (chan->sequence == 1) {
		/*
		 * Evaluate the initial capture latency between videobuf2 queue
		 * and first captured frame release to user-space.
		 */
		frame_arrived_ts = ktime_to_ms(ktime_get());
		dev_dbg(&chan->video.dev, "%s: capture init latency is %lld ms\n",
			__func__, (frame_arrived_ts - queue_init_ts));
	}

	dev_dbg(&chan->video.dev,
		"%s: release buf[%p] frame[%d] to user-space\n",
		__func__, buf, chan->sequence);
	vb2_buffer_done(&vbuf->vb2_buf, buf->state);
}

/*
 * `buf` has been successfully setup to receive a frame and is
 * "in flight" through the VI hardware. We are currently waiting
 * on it to be filled. Moves the pointer into the `release` list
 * for the release thread to wait on.
 */
void enqueue_inflight(struct tegra_channel *chan,
		struct tegra_channel_buffer *buf)
{
	/* Put buffer into the release queue */
	spin_lock(&chan->release_lock);
	list_add_tail(&buf->queue, &chan->release);
	spin_unlock(&chan->release_lock);

	/* Wake up kthread for release */
	wake_up_interruptible(&chan->release_wait);
}

void tegra_channel_ec_close(struct tegra_mc_vi *vi)
{
	struct tegra_channel *chan;

	/* clear all channles sync point fifo context */
	list_for_each_entry(chan, &vi->vi_chans, list) {
		memset(&chan->syncpoint_fifo[0], 0, TEGRA_CSI_BLOCKS);
	}
}

struct tegra_channel_buffer* dequeue_inflight(struct tegra_channel* chan)
{

	struct tegra_channel_buffer *buf = NULL;

	spin_lock(&chan->release_lock);
	if (list_empty(&chan->release)) {
		spin_unlock(&chan->release_lock);
		return NULL;
	}

	buf = list_entry(chan->release.next,
			 struct tegra_channel_buffer, queue);

	if(buf) {
		list_del_init(&buf->queue);
	}
	spin_unlock(&chan->release_lock);
	return buf;
}

struct tegra_channel_buffer *dequeue_buffer(struct tegra_channel *chan)
{
	struct tegra_channel_buffer *buf = NULL;

	spin_lock(&chan->start_lock);
	if (list_empty(&chan->capture))
		goto done;

	buf = list_entry(chan->capture.next,
			 struct tegra_channel_buffer, queue);
	list_del_init(&buf->queue);

done:
	spin_unlock(&chan->start_lock);
	return buf;
}

/*
 * -----------------------------------------------------------------------------
 * videobuf2 queue operations
 * -----------------------------------------------------------------------------
 */
static int
tegra_channel_queue_setup(struct vb2_queue *vq, const void *parg,
		     unsigned int *nbuffers, unsigned int *nplanes,
		     unsigned int sizes[], void *alloc_ctxs[])
{
	const struct v4l2_format *fmt = parg;
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	/* Make sure the image size is large enough. */
	if (fmt && fmt->fmt.pix.sizeimage < chan->format.sizeimage)
		return -EINVAL;

	*nplanes = 1;

	sizes[0] = fmt ? fmt->fmt.pix.sizeimage : chan->format.sizeimage;
	alloc_ctxs[0] = chan->alloc_ctx;

	/* Make sure minimum number of buffers are passed */
	if (*nbuffers < (QUEUED_BUFFERS - 1))
		*nbuffers = QUEUED_BUFFERS - 1;

	return 0;
}

static int tegra_channel_buffer_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct tegra_channel *chan = vb2_get_drv_priv(vb->vb2_queue);
	struct tegra_channel_buffer *buf = to_tegra_channel_buffer(vbuf);

	buf->chan = chan;
	vb2_set_plane_payload(&vbuf->vb2_buf, 0, chan->format.sizeimage);
#if defined(CONFIG_VIDEOBUF2_DMA_CONTIG)
	buf->addr = vb2_dma_contig_plane_dma_addr(vb, 0);
#endif

	return 0;
}

static void tegra_channel_buffer_queue(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct tegra_channel *chan = vb2_get_drv_priv(vb->vb2_queue);
	struct tegra_channel_buffer *buf = to_tegra_channel_buffer(vbuf);

	/* for bypass mode - do nothing */
	if (chan->bypass)
		return;

	if (!queue_init_ts) {
		/*
		 * Record videobuf2 queue initial timestamp.
		 * Note: latency is accurate when streaming is already turned ON
		 */
		queue_init_ts = ktime_to_ms(ktime_get());
	}

	/* Put buffer into the capture queue */
	spin_lock(&chan->start_lock);
	list_add_tail(&buf->queue, &chan->capture);
	spin_unlock(&chan->start_lock);

	/* Wait up kthread for capture */
	wake_up_interruptible(&chan->start_wait);
}

/* Return all queued buffers back to videobuf2 */
void tegra_channel_queued_buf_done(struct tegra_channel *chan,
					  enum vb2_buffer_state state)
{
	struct tegra_channel_buffer *buf, *nbuf;
	spinlock_t *lock = &chan->start_lock;
	struct list_head *q = &chan->capture;
	spinlock_t *release_lock = &chan->release_lock;
	struct list_head *rel_q = &chan->release;

	spin_lock(lock);
	if(!list_empty(q)) {
		list_for_each_entry_safe(buf, nbuf, q, queue) {
			vb2_buffer_done(&buf->buf.vb2_buf, state);
			list_del(&buf->queue);
		}
	}
	spin_unlock(lock);

	/* delete release list */
	spin_lock(release_lock);
	if(!list_empty(rel_q)) {
		list_for_each_entry_safe(buf, nbuf, rel_q, queue) {
			vb2_buffer_done(&buf->buf.vb2_buf, state);
			list_del(&buf->queue);
		}
	}
	spin_unlock(release_lock);
}

#define __tegra_channel_device_call_subdevs_all_p(v4l2_dev, sd, cond, o,\
		f, args...)						\
({									\
	long __err = 0;							\
	long e = 0;							\
									\
	list_for_each_entry((sd), &(v4l2_dev)->subdevs, list) {		\
		if ((cond) && (sd)->ops->o && (sd)->ops->o->f)		\
			e = (sd)->ops->o->f((sd), ##args);		\
		if (!__err && e && e != -ENOIOCTLCMD)			\
			__err = e;					\
		e = 0;							\
	}								\
	__err;								\
})

/*
 * Call the specified callback for all subdevs matching grp_id (if 0, then
 * match them all), errors are ignored until the end, and the first error
 * encountered is returned. If the callback returns an error other than 0 or
 * -ENOIOCTLCMD, then return with that error code. Note that you cannot
 * add or delete a subdev while walking the subdevs list.
 */
#define tegra_channel_device_call_all(v4l2_dev, grpid, o, f, args...)	\
({									\
	struct v4l2_subdev *__sd;					\
	__tegra_channel_device_call_subdevs_all_p(v4l2_dev, __sd,	\
			!(grpid) || __sd->grp_id == (grpid), o, f,	\
			##args);					\
})

/*
 * -----------------------------------------------------------------------------
 * subdevice set/unset operations
 * -----------------------------------------------------------------------------
 */
int tegra_channel_set_stream(struct tegra_channel *chan, bool on)
{
	int num_sd;
	int ret = 0;
	int err = 0;
	struct v4l2_subdev *sd;

	if (atomic_read(&chan->is_streaming) == on)
		return 0;

	if (on) {
		/* Enable CSI before sensor. Reason is as follows:
		 * CSI is able to catch the very first clk transition.
		 * Ensure mipi calibration is done before transmission/first frame data.
		 * TODO:Ensure deskew is setup properly before first deskew sync signal.
		 */
		for (num_sd = 0; num_sd < chan->num_subdevs; num_sd++) {
			sd = chan->subdev[num_sd];

			err = v4l2_subdev_call(sd, video, s_stream, on);
			if (!ret && err < 0 && err != -ENOIOCTLCMD)
				ret = err;
		}
	} else {
		for (num_sd = chan->num_subdevs - 1; num_sd >= 0; num_sd--) {
			sd = chan->subdev[num_sd];

			err = v4l2_subdev_call(sd, video, s_stream, on);
			if (!ret && err < 0 && err != -ENOIOCTLCMD)
				ret = err;
		}
	}

	atomic_set(&chan->is_streaming, on);
	return ret;
}

int tegra_channel_set_power(struct tegra_channel *chan, bool on)
{
	int num_sd;
	int ret = 0;
	int err = 0;
	struct v4l2_subdev *sd;

	/* Power on CSI at the last to complete calibration of mipi lanes */
	for (num_sd = chan->num_subdevs - 1; num_sd >= 0; num_sd--) {
		sd = chan->subdev[num_sd];

		err = v4l2_subdev_call(sd, core, s_power, on);
		if (!ret && err < 0 && err != -ENOIOCTLCMD)
			ret = err;
	}

	return ret;
}

static int tegra_channel_start_streaming(struct vb2_queue *vq, u32 count)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	struct tegra_mc_vi *vi = chan->vi;

	if (vi->fops)
		return vi->fops->vi_start_streaming(vq, count);
	return 0;
}

static void tegra_channel_stop_streaming(struct vb2_queue *vq)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	struct tegra_mc_vi *vi = chan->vi;

	if (vi->fops)
		vi->fops->vi_stop_streaming(vq);

	/* Clean-up recorded videobuf2 queue initial timestamp */
	queue_init_ts = 0;
}

static const struct vb2_ops tegra_channel_queue_qops = {
	.queue_setup = tegra_channel_queue_setup,
	.buf_prepare = tegra_channel_buffer_prepare,
	.buf_queue = tegra_channel_buffer_queue,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
	.start_streaming = tegra_channel_start_streaming,
	.stop_streaming = tegra_channel_stop_streaming,
};

/* -----------------------------------------------------------------------------
 * V4L2 ioctls
 */

static int
tegra_channel_querycap(struct file *file, void *fh, struct v4l2_capability *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	cap->device_caps = V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING;
	cap->device_caps |= V4L2_CAP_EXT_PIX_FORMAT;
	cap->capabilities = cap->device_caps | V4L2_CAP_DEVICE_CAPS;

	strlcpy(cap->driver, "tegra-video", sizeof(cap->driver));
	strlcpy(cap->card, chan->video.name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform:%s:%u",
		 dev_name(chan->vi->dev), chan->port[0]);

	return 0;
}

static int
tegra_channel_enum_framesizes(struct file *file, void *fh,
			      struct v4l2_frmsizeenum *sizes)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;
	struct v4l2_subdev_frame_size_enum fse = {
		.index = sizes->index,
		.code = sizes->pixel_format,
	};
	int ret = 0;

	ret = v4l2_subdev_call(sd, pad, enum_frame_size, NULL, &fse);

	if (!ret) {
		sizes->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		sizes->discrete.width = fse.max_width;
		sizes->discrete.height = fse.max_height;
	}

	return ret;
}

static int
tegra_channel_enum_frameintervals(struct file *file, void *fh,
			      struct v4l2_frmivalenum *intervals)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;
	struct v4l2_subdev_frame_interval_enum fie = {
		.index = intervals->index,
		.code = intervals->pixel_format,
		.width = intervals->width,
		.height = intervals->height,
	};
	int ret = 0;

	ret = v4l2_subdev_call(sd, pad, enum_frame_interval, NULL, &fie);

	if (!ret) {
		intervals->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		intervals->discrete.numerator = fie.interval.numerator;
		intervals->discrete.denominator = fie.interval.denominator;
	}

	return ret;
}

static int
tegra_channel_enum_format(struct file *file, void *fh, struct v4l2_fmtdesc *f)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	unsigned int index = 0, i;
	unsigned long *fmts_bitmap = chan->fmts_bitmap;

	if (f->index >= bitmap_weight(fmts_bitmap, MAX_FORMAT_NUM))
		return -EINVAL;

	for (i = 0; i < f->index + 1; i++, index++)
		index = find_next_bit(fmts_bitmap, MAX_FORMAT_NUM, index);

	index -= 1;
	f->pixelformat = tegra_core_get_fourcc_by_idx(chan, index);
	tegra_core_get_description_by_idx(chan, index, f->description);

	return 0;
}

static int
tegra_channel_g_edid(struct file *file, void *fh, struct v4l2_edid *edid)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	if (!v4l2_subdev_has_op(sd, pad, get_edid))
		return -ENOTTY;

	return v4l2_subdev_call(sd, pad, get_edid, edid);
}

static int
tegra_channel_s_edid(struct file *file, void *fh, struct v4l2_edid *edid)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	if (!v4l2_subdev_has_op(sd, pad, set_edid))
		return -ENOTTY;

	return v4l2_subdev_call(sd, pad, set_edid, edid);
}

static int
tegra_channel_g_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	if (!v4l2_subdev_has_op(chan->subdev_on_csi, video, g_dv_timings))
		return -ENOTTY;

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, g_dv_timings, timings);
}

static int
tegra_channel_s_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_bt_timings *bt = &timings->bt;
	struct v4l2_dv_timings curr_timings;
	int ret;

	if (!v4l2_subdev_has_op(chan->subdev_on_csi, video, s_dv_timings))
		return -ENOTTY;

	ret = tegra_channel_g_dv_timings(file, fh, &curr_timings);
	if (ret)
		return ret;

	if (v4l2_match_dv_timings(timings, &curr_timings, 0))
		return 0;

	if (vb2_is_busy(&chan->queue))
		return -EBUSY;

	ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, s_dv_timings, timings);

	if (!ret)
		tegra_channel_update_format(chan, bt->width, bt->height,
			chan->fmtinfo->fourcc, &chan->fmtinfo->bpp, 0);

	if (chan->total_ports > 1)
		update_gang_mode(chan);

	return ret;
}

static int
tegra_channel_query_dv_timings(struct file *file, void *fh,
		struct v4l2_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	if (!v4l2_subdev_has_op(chan->subdev_on_csi, video, query_dv_timings))
		return -ENOTTY;

	return v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, query_dv_timings, timings);
}

static int
tegra_channel_enum_dv_timings(struct file *file, void *fh,
		struct v4l2_enum_dv_timings *timings)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	if (!v4l2_subdev_has_op(sd, pad, enum_dv_timings))
		return -ENOTTY;

	return v4l2_subdev_call(sd, pad, enum_dv_timings, timings);
}

static int
tegra_channel_dv_timings_cap(struct file *file, void *fh,
		struct v4l2_dv_timings_cap *cap)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd = chan->subdev_on_csi;

	if (!v4l2_subdev_has_op(sd, pad, dv_timings_cap))
		return -ENOTTY;

	return v4l2_subdev_call(sd, pad, dv_timings_cap, cap);
}

int tegra_channel_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct tegra_channel *chan = container_of(ctrl->handler,
				struct tegra_channel, ctrl_handler);

	switch (ctrl->id) {
	case TEGRA_CAMERA_CID_VI_BYPASS_MODE:
		if (switch_ctrl_qmenu[ctrl->val] == SWITCH_ON)
			chan->bypass = true;
		else if (chan->vi->bypass) {
			dev_dbg(&chan->video.dev,
				"can't disable bypass mode\n");
			dev_dbg(&chan->video.dev,
				"because the VI/CSI is in bypass mode\n");
			chan->bypass = true;
		} else
			chan->bypass = false;
		break;
	case TEGRA_CAMERA_CID_OVERRIDE_ENABLE:
		{
			struct v4l2_subdev *sd = chan->subdev_on_csi;
			struct camera_common_data *s_data =
				to_camera_common_data(sd->dev);

			if (!s_data)
				break;
			if (switch_ctrl_qmenu[ctrl->val] == SWITCH_ON) {
				s_data->override_enable = true;
				dev_dbg(&chan->video.dev,
					"enable override control\n");
			} else {
				s_data->override_enable = false;
				dev_dbg(&chan->video.dev,
					"disable override control\n");
			}
		}
		break;
	case TEGRA_CAMERA_CID_VI_HEIGHT_ALIGN:
		chan->height_align = ctrl->val;
		tegra_channel_update_format(chan, chan->format.width,
				chan->format.height,
				chan->format.pixelformat,
				&chan->fmtinfo->bpp, 0);
		break;
	case TEGRA_CAMERA_CID_VI_SIZE_ALIGN:
		chan->size_align = size_align_ctrl_qmenu[ctrl->val];
		tegra_channel_update_format(chan, chan->format.width,
				chan->format.height,
				chan->format.pixelformat,
				&chan->fmtinfo->bpp, 0);
		break;
	case TEGRA_CAMERA_CID_WRITE_ISPFORMAT:
		chan->write_ispformat = ctrl->val;
		break;
	default:
		dev_err(&chan->video.dev, "%s: Invalid ctrl %u\n",
			__func__, ctrl->id);
		return -EINVAL;
	}

	return 0;
}

static const struct v4l2_ctrl_ops channel_ctrl_ops = {
	.s_ctrl	= tegra_channel_s_ctrl,
};

static const struct v4l2_ctrl_config common_custom_ctrls[] = {
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_VI_BYPASS_MODE,
		.name = "Bypass Mode",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.def = 0,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_OVERRIDE_ENABLE,
		.name = "Override Enable",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.def = 0,
		.min = 0,
		.max = ARRAY_SIZE(switch_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.qmenu_int = switch_ctrl_qmenu,
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_VI_HEIGHT_ALIGN,
		.name = "Height Align",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.min = 1,
		.max = 16,
		.step = 1,
		.def = 1,
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_VI_SIZE_ALIGN,
		.name = "Size Align",
		.type = V4L2_CTRL_TYPE_INTEGER_MENU,
		.def = 0,
		.min = 0,
		.max = ARRAY_SIZE(size_align_ctrl_qmenu) - 1,
		.menu_skip_mask = 0,
		.qmenu_int = size_align_ctrl_qmenu,
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_SENSOR_MODES,
		.name = "Sensor Modes",
		.type = V4L2_CTRL_TYPE_INTEGER,
		.flags = V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = MAX_NUM_SENSOR_MODES,
		.def = MAX_NUM_SENSOR_MODES,
		.step = 1,
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_SENSOR_SIGNAL_PROPERTIES,
		.name = "Sensor Signal Properties",
		.type = V4L2_CTRL_TYPE_U32,
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = 0xFFFFFFFF,
		.step = 1,
		.def = 0,
		.dims = { MAX_NUM_SENSOR_MODES,
			  SENSOR_SIGNAL_PROPERTIES_CID_SIZE },
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_SENSOR_IMAGE_PROPERTIES,
		.name = "Sensor Image Properties",
		.type = V4L2_CTRL_TYPE_U32,
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = 0xFFFFFFFF,
		.step = 1,
		.def = 0,
		.dims = { MAX_NUM_SENSOR_MODES,
			  SENSOR_IMAGE_PROPERTIES_CID_SIZE },
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_SENSOR_CONTROL_PROPERTIES,
		.name = "Sensor Control Properties",
		.type = V4L2_CTRL_TYPE_U32,
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = 0xFFFFFFFF,
		.step = 1,
		.def = 0,
		.dims = { MAX_NUM_SENSOR_MODES,
			  SENSOR_CONTROL_PROPERTIES_CID_SIZE },
	},
	{
		.ops = &channel_ctrl_ops,
		.id = TEGRA_CAMERA_CID_SENSOR_DV_TIMINGS,
		.name = "Sensor DV Timings",
		.type = V4L2_CTRL_TYPE_U32,
		.flags = V4L2_CTRL_FLAG_HAS_PAYLOAD |
			 V4L2_CTRL_FLAG_READ_ONLY,
		.min = 0,
		.max = 0xFFFFFFFF,
		.step = 1,
		.def = 0,
		.dims = { MAX_NUM_SENSOR_MODES,
			  SENSOR_DV_TIMINGS_CID_SIZE },
	},
};

#define GET_TEGRA_CAMERA_CTRL(id, c)					\
do {									\
	c = v4l2_ctrl_find(&chan->ctrl_handler, TEGRA_CAMERA_CID_##id);	\
	if (!c) {							\
		dev_err(chan->vi->dev, "%s: could not find ctrl %s\n",	\
			__func__, "##id");				\
		return -EINVAL;						\
	}								\
} while (0)

static int tegra_channel_sensorprops_setup(struct tegra_channel *chan)
{
	const struct v4l2_subdev *sd = chan->subdev_on_csi;
	const struct camera_common_data *s_data =
			to_camera_common_data(sd->dev);
	const struct sensor_mode_properties *modes;
	struct v4l2_ctrl *ctrl_modes;
	struct v4l2_ctrl *ctrl_signalprops;
	struct v4l2_ctrl *ctrl_imageprops;
	struct v4l2_ctrl *ctrl_controlprops;
	struct v4l2_ctrl *ctrl_dvtimings;
	u32 i;

	GET_TEGRA_CAMERA_CTRL(SENSOR_MODES, ctrl_modes);
	GET_TEGRA_CAMERA_CTRL(SENSOR_SIGNAL_PROPERTIES, ctrl_signalprops);
	GET_TEGRA_CAMERA_CTRL(SENSOR_IMAGE_PROPERTIES, ctrl_imageprops);
	GET_TEGRA_CAMERA_CTRL(SENSOR_CONTROL_PROPERTIES, ctrl_controlprops);
	GET_TEGRA_CAMERA_CTRL(SENSOR_DV_TIMINGS, ctrl_dvtimings);

	ctrl_modes->val = s_data->sensor_props.num_modes;
	ctrl_modes->cur.val = s_data->sensor_props.num_modes;

	modes = s_data->sensor_props.sensor_modes;
	for (i = 0; i < s_data->sensor_props.num_modes; i++) {
		void *ptr = NULL;
		u32 size;

		size = sizeof(struct sensor_signal_properties);
		ptr = ctrl_signalprops->p_new.p + (i * size);
		memcpy(ptr, &modes[i].signal_properties, size);

		size = sizeof(struct sensor_image_properties);
		ptr = ctrl_imageprops->p_new.p + (i * size);
		memcpy(ptr, &modes[i].image_properties, size);

		size = sizeof(struct sensor_control_properties);
		ptr = ctrl_controlprops->p_new.p + (i * size);
		memcpy(ptr, &modes[i].control_properties, size);

		size = sizeof(struct sensor_dv_timings);
		ptr = ctrl_dvtimings->p_new.p + (i * size);
		memcpy(ptr, &modes[i].dv_timings, size);
	}
	ctrl_signalprops->p_cur.p = ctrl_signalprops->p_new.p;
	ctrl_imageprops->p_cur.p = ctrl_imageprops->p_new.p;
	ctrl_controlprops->p_cur.p = ctrl_controlprops->p_new.p;
	ctrl_dvtimings->p_cur.p = ctrl_dvtimings->p_new.p;

	return 0;
}

static int tegra_channel_setup_controls(struct tegra_channel *chan)
{
	int num_sd = 0;
	struct v4l2_subdev *sd = NULL;
	struct tegra_mc_vi *vi = chan->vi;
	int i;
	int ret = 0;

	/* Initialize the subdev and controls here at first open */
	sd = chan->subdev[num_sd];
	while ((sd = chan->subdev[num_sd++]) &&
		(num_sd <= chan->num_subdevs)) {
		/* Add control handler for the subdevice */
		ret = v4l2_ctrl_add_handler(&chan->ctrl_handler,
					sd->ctrl_handler, NULL);
		if (ret || chan->ctrl_handler.error)
			dev_err(chan->vi->dev,
				"Failed to add sub-device controls\n");
	}

	/* Add new custom controls */
	for (i = 0; i < ARRAY_SIZE(common_custom_ctrls); i++) {
		/* don't create override control for pg mode and hdmiin */
		if (common_custom_ctrls[i].id ==
			TEGRA_CAMERA_CID_OVERRIDE_ENABLE &&
			(chan->pg_mode || chan->hdmiin))
			continue;
		v4l2_ctrl_new_custom(&chan->ctrl_handler,
			&common_custom_ctrls[i], NULL);
		if (chan->ctrl_handler.error) {
			dev_err(chan->vi->dev,
				"Failed to add %s ctrl\n",
				common_custom_ctrls[i].name);
			return chan->ctrl_handler.error;
		}
	}

	vi->fops->vi_add_ctrls(chan);

	if (chan->pg_mode) {
		ret = v4l2_ctrl_add_handler(&chan->ctrl_handler,
					&chan->vi->ctrl_handler, NULL);
		if (ret || chan->ctrl_handler.error)
			dev_err(chan->vi->dev,
				"Failed to add VI controls\n");
	}

	/* setup the controls */
	ret = v4l2_ctrl_handler_setup(&chan->ctrl_handler);
	if (ret < 0)
		goto error;

	return 0;

error:
	v4l2_ctrl_handler_free(&chan->ctrl_handler);
	return ret;
}

static void tegra_channel_free_sensor_properties(
		const struct v4l2_subdev *sensor_sd)
{
	struct device *sensor_dev = sensor_sd->dev;
	struct camera_common_data *s_data = to_camera_common_data(sensor_dev);

	if (sensor_dev == NULL || s_data == NULL)
		return;

	if (s_data->sensor_props.sensor_modes)
		devm_kfree(sensor_dev, s_data->sensor_props.sensor_modes);

	s_data->sensor_props.sensor_modes = NULL;
}

static int tegra_channel_connect_sensor(
	struct tegra_channel *chan, struct v4l2_subdev *sensor_sd)
{
	struct device *sensor_dev;
	struct device_node *sensor_of_node;
	struct tegra_csi_device *csi_device;
	struct device_node *ep_node;

	if (!chan)
		return -EINVAL;

	if (!sensor_sd)
		return -EINVAL;

	sensor_dev = sensor_sd->dev;
	if (!sensor_dev)
		return -EINVAL;

	sensor_of_node = sensor_dev->of_node;
	if (!sensor_of_node)
		return -EINVAL;

	csi_device = tegra_get_mc_csi();
	WARN_ON(!csi_device);
	if (!csi_device)
		return -ENODEV;

	for_each_endpoint_of_node(sensor_of_node, ep_node) {
		struct device_node *csi_chan_of_node;
		struct tegra_csi_channel *csi_chan;

		csi_chan_of_node =
			of_graph_get_remote_port_parent(ep_node);

		list_for_each_entry(csi_chan, &csi_device->csi_chans, list)
			if (csi_chan->of_node == csi_chan_of_node)
				break;

		of_node_put(csi_chan_of_node);

		if (!csi_chan)
			continue;

		csi_chan->s_data =
			to_camera_common_data(chan->subdev_on_csi->dev);
		csi_chan->sensor_sd = chan->subdev_on_csi;
	}

	return 0;
}

int tegra_channel_init_subdevices(struct tegra_channel *chan)
{
	int ret = 0;
	struct media_entity *entity;
	struct media_pad *pad;
	struct v4l2_subdev *sd;
	int index = 0;
	int num_sd = 0;
	int grp_id = chan->pg_mode ? (TPG_CSI_GROUP_ID + chan->port[0] + 1)
		: chan->port[0] + 1;

	/* set_stream of CSI */
	pad = media_entity_remote_pad(&chan->pad);
	if (!pad)
		return -ENODEV;

	entity = pad->entity;
	sd = media_entity_to_v4l2_subdev(entity);
	v4l2_set_subdev_hostdata(sd, chan);
	chan->subdev[num_sd++] = sd;
	/* Add subdev name to this video dev name with vi-output tag*/
	snprintf(chan->video.name, sizeof(chan->video.name), "%s, %s",
		"vi-output", sd->name);
	sd->grp_id = grp_id;
	chan->grp_id = grp_id;
	index = pad->index - 1;
	while (index >= 0) {
		pad = &entity->pads[index];
		if (!(pad->flags & MEDIA_PAD_FL_SINK))
			break;

		pad = media_entity_remote_pad(pad);
		if (pad == NULL ||
		    media_entity_type(pad->entity) != MEDIA_ENT_T_V4L2_SUBDEV)
			break;

		if (num_sd >= MAX_SUBDEVICES)
			break;

		entity = pad->entity;
		sd = media_entity_to_v4l2_subdev(entity);
		v4l2_set_subdev_hostdata(sd, chan);
		sd->grp_id = grp_id;
		chan->subdev[num_sd++] = sd;
		/* Add subdev name to this video dev name with vi-output tag*/
		snprintf(chan->video.name, sizeof(chan->video.name), "%s, %s",
			"vi-output", sd->name);

		index = pad->index - 1;
	}
	chan->num_subdevs = num_sd;
	/*
	 * Each CSI channel has only one final remote source,
	 * Mark that subdev as subdev_on_csi
	 */
	chan->subdev_on_csi = sd;

	/* initialize the available formats */
	if (chan->num_subdevs)
		tegra_channel_fmts_bitmap_init(chan);

	chan->hdmiin = v4l2_subdev_has_op(chan->subdev_on_csi,
				video, s_dv_timings);

	ret = tegra_channel_setup_controls(chan);
	if (ret < 0) {
		dev_err(chan->vi->dev, "%s: failed to setup controls\n",
			__func__);
		goto fail;
	}

	/*
	 * If subdev on csi is csi or channel is in pg mode
	 * then don't look for sensor props
	 */
	if (strstr(chan->subdev_on_csi->name, "nvcsi") != NULL ||
			chan->pg_mode)
		return 0;


	if(!strncmp("tc358840", sd->name, 8))
		goto no_camera_data;

	ret = tegra_channel_sensorprops_setup(chan);
	if (ret < 0) {
		dev_err(chan->vi->dev, "%s: failed to setup sensor props\n",
			__func__);
		goto fail;
	}

no_camera_data:
	/* Add a link for the camera_common_data in the tegra_csi_channel. */
	ret = tegra_channel_connect_sensor(chan, chan->subdev_on_csi);
	if (ret < 0) {
		dev_err(chan->vi->dev,
			"%s: failed to connect sensor to channel\n", __func__);
		goto fail;
	}

	return 0;
fail:
	tegra_channel_free_sensor_properties(chan->subdev_on_csi);
	return ret;
}

static int
tegra_channel_get_format(struct file *file, void *fh,
			struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_pix_format *pix = &format->fmt.pix;

	*pix = chan->format;

	return 0;
}

static int
__tegra_channel_try_format(struct tegra_channel *chan,
			struct v4l2_pix_format *pix)
{
	const struct tegra_video_format *vfmt;
	struct v4l2_subdev_format fmt;
	struct v4l2_subdev *sd = chan->subdev_on_csi;
	int ret = 0;

	/* Use the channel format if pixformat is not supported */
	vfmt = tegra_core_get_format_by_fourcc(chan, pix->pixelformat);
	if (!vfmt) {
		pix->pixelformat = chan->format.pixelformat;
		vfmt = tegra_core_get_format_by_fourcc(chan, pix->pixelformat);
	}

	fmt.which = V4L2_SUBDEV_FORMAT_TRY;
	fmt.pad = 0;
	v4l2_fill_mbus_format(&fmt.format, pix, vfmt->code);

	ret = v4l2_subdev_call(sd, pad, set_fmt, NULL, &fmt);
	if (ret == -ENOIOCTLCMD)
		return -ENOTTY;

	v4l2_fill_pix_format(pix, &fmt.format);

	tegra_channel_fmt_align(chan, vfmt,
				&pix->width, &pix->height, &pix->bytesperline);
	pix->sizeimage = get_aligned_buffer_size(chan,
			pix->bytesperline, pix->height);
	if (chan->fmtinfo->fourcc == V4L2_PIX_FMT_NV16)
		pix->sizeimage *= 2;

	return ret;
}

static int
tegra_channel_try_format(struct file *file, void *fh,
			struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	return  __tegra_channel_try_format(chan, &format->fmt.pix);
}

static int
__tegra_channel_set_format(struct tegra_channel *chan,
			struct v4l2_pix_format *pix)
{
	const struct tegra_video_format *vfmt;
	struct v4l2_subdev_format fmt;
	struct v4l2_subdev *sd = chan->subdev_on_csi;
	int ret = 0;

	vfmt = tegra_core_get_format_by_fourcc(chan, pix->pixelformat);

	fmt.which = V4L2_SUBDEV_FORMAT_ACTIVE;
	fmt.pad = 0;
	v4l2_fill_mbus_format(&fmt.format, pix, vfmt->code);

	ret = v4l2_subdev_call(sd, pad, set_fmt, NULL, &fmt);
	if (ret == -ENOIOCTLCMD)
		return -ENOTTY;

	v4l2_fill_pix_format(pix, &fmt.format);

	if (!ret) {
		chan->format = *pix;
		chan->fmtinfo = vfmt;
		tegra_channel_update_format(chan, pix->width,
			pix->height, vfmt->fourcc, &vfmt->bpp,
			pix->bytesperline);

		*pix = chan->format;

		if (chan->total_ports > 1)
			update_gang_mode(chan);
	}

	return ret;
}

static int
tegra_channel_set_format(struct file *file, void *fh,
			struct v4l2_format *format)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	int ret = 0;

	/* get the suppod format by try_fmt */
	ret = __tegra_channel_try_format(chan, &format->fmt.pix);
	if (ret)
		return ret;

	if (vb2_is_busy(&chan->queue))
		return -EBUSY;

	return __tegra_channel_set_format(chan, &format->fmt.pix);
}

static int tegra_channel_subscribe_event(struct v4l2_fh *fh,
				  const struct v4l2_event_subscription *sub)
{
	switch (sub->type) {
	case V4L2_EVENT_SOURCE_CHANGE:
		return v4l2_event_subscribe(fh, sub, 4, NULL);
	}
	return v4l2_ctrl_subscribe_event(fh, sub);
}

static int
tegra_channel_enum_input(struct file *file, void *fh, struct v4l2_input *inp)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);
	struct v4l2_subdev *sd_on_csi = chan->subdev_on_csi;
	int ret;

	if (inp->index)
		return -EINVAL;

	ret = v4l2_device_call_until_err(chan->video.v4l2_dev,
			chan->grp_id, video, g_input_status, &inp->status);

	if (ret == -ENODEV || sd_on_csi == NULL)
		return -ENODEV;

	inp->type = V4L2_INPUT_TYPE_CAMERA;
	if (v4l2_subdev_has_op(sd_on_csi, video, s_dv_timings)) {
		inp->capabilities = V4L2_IN_CAP_DV_TIMINGS;
		snprintf(inp->name,
			sizeof(inp->name), "HDMI %u",
			chan->port[0]);
	} else
		snprintf(inp->name,
			sizeof(inp->name), "Camera %u",
			chan->port[0]);

	return ret;
}

static int tegra_channel_g_input(struct file *file, void *priv, unsigned int *i)
{
	*i = 0;
	return 0;
}

static int tegra_channel_s_input(struct file *file, void *priv, unsigned int i)
{
	if (i > 0)
		return -EINVAL;
	return 0;
}

static int tegra_channel_log_status(struct file *file, void *priv)
{
	struct v4l2_fh *vfh = file->private_data;
	struct tegra_channel *chan = to_tegra_channel(vfh->vdev);

	v4l2_device_call_all(chan->video.v4l2_dev,
		chan->grp_id, core, log_status);
	return 0;
}
static const struct v4l2_ioctl_ops tegra_channel_ioctl_ops = {
	.vidioc_querycap		= tegra_channel_querycap,
	.vidioc_enum_framesizes		= tegra_channel_enum_framesizes,
	.vidioc_enum_frameintervals	= tegra_channel_enum_frameintervals,
	.vidioc_enum_fmt_vid_cap	= tegra_channel_enum_format,
	.vidioc_g_fmt_vid_cap		= tegra_channel_get_format,
	.vidioc_s_fmt_vid_cap		= tegra_channel_set_format,
	.vidioc_try_fmt_vid_cap		= tegra_channel_try_format,
	.vidioc_reqbufs			= vb2_ioctl_reqbufs,
	.vidioc_querybuf		= vb2_ioctl_querybuf,
	.vidioc_qbuf			= vb2_ioctl_qbuf,
	.vidioc_dqbuf			= vb2_ioctl_dqbuf,
	.vidioc_create_bufs		= vb2_ioctl_create_bufs,
	.vidioc_expbuf			= vb2_ioctl_expbuf,
	.vidioc_streamon		= vb2_ioctl_streamon,
	.vidioc_streamoff		= vb2_ioctl_streamoff,
	.vidioc_g_edid			= tegra_channel_g_edid,
	.vidioc_s_edid			= tegra_channel_s_edid,
	.vidioc_s_dv_timings		= tegra_channel_s_dv_timings,
	.vidioc_g_dv_timings		= tegra_channel_g_dv_timings,
	.vidioc_query_dv_timings	= tegra_channel_query_dv_timings,
	.vidioc_enum_dv_timings		= tegra_channel_enum_dv_timings,
	.vidioc_dv_timings_cap		= tegra_channel_dv_timings_cap,
	.vidioc_subscribe_event		= tegra_channel_subscribe_event,
	.vidioc_unsubscribe_event	= v4l2_event_unsubscribe,
	.vidioc_enum_input		= tegra_channel_enum_input,
	.vidioc_g_input			= tegra_channel_g_input,
	.vidioc_s_input			= tegra_channel_s_input,
	.vidioc_log_status		= tegra_channel_log_status,
};

static int tegra_channel_close(struct file *fp);
static int tegra_channel_open(struct file *fp)
{
	int ret;
	struct video_device *vdev = video_devdata(fp);
	struct tegra_channel *chan = video_get_drvdata(vdev);
	struct tegra_mc_vi *vi;
	struct tegra_csi_device *csi;

	mutex_lock(&chan->video_lock);
	ret = v4l2_fh_open(fp);
	if (ret || !v4l2_fh_is_singular_file(fp)) {
		mutex_unlock(&chan->video_lock);
		return ret;
	}

	if (chan->subdev[0] == NULL) {
		ret = -ENODEV;
		goto fail;
	}

	vi = chan->vi;
	csi = vi->csi;

	/* The first open then turn on power */
	if (vi->fops) {
		ret = vi->fops->vi_power_on(chan);
		if (ret < 0)
			goto fail;
	}

	chan->fh = (struct v4l2_fh *)fp->private_data;

	mutex_unlock(&chan->video_lock);
	return 0;

fail:
	_vb2_fop_release(fp, NULL);
	mutex_unlock(&chan->video_lock);
	return ret;
}

static int tegra_channel_close(struct file *fp)
{
	int ret = 0;
	struct video_device *vdev = video_devdata(fp);
	struct tegra_channel *chan = video_get_drvdata(vdev);
	struct tegra_mc_vi *vi = chan->vi;
	bool is_singular;

	mutex_lock(&chan->video_lock);
	is_singular = v4l2_fh_is_singular_file(fp);
	ret = _vb2_fop_release(fp, NULL);

	if (!is_singular) {
		mutex_unlock(&chan->video_lock);
		return ret;
	}
	vi->fops->vi_power_off(chan);

	mutex_unlock(&chan->video_lock);
	return ret;
}

/* -----------------------------------------------------------------------------
 * V4L2 file operations
 */
static const struct v4l2_file_operations tegra_channel_fops = {
	.owner		= THIS_MODULE,
	.unlocked_ioctl	= video_ioctl2,
	.open		= tegra_channel_open,
	.release	= tegra_channel_close,
	.read		= vb2_fop_read,
	.poll		= vb2_fop_poll,
	.mmap		= vb2_fop_mmap,
};

static int tegra_channel_csi_init(struct tegra_channel *chan)
{
	int idx = 0;
	struct tegra_mc_vi *vi = chan->vi;
	int ret = 0;

	chan->gang_mode = CAMERA_NO_GANG_MODE;
	chan->total_ports = 0;
	memset(&chan->port[0], INVALID_CSI_PORT, TEGRA_CSI_BLOCKS);
	memset(&chan->syncpoint_fifo[0], 0, TEGRA_CSI_BLOCKS);
	if (chan->pg_mode) {
		/* If VI has 4 existing channels, chan->id will start
		 * from 4 for the first TPG channel, which uses PORT_A(0).
		 * To get the correct PORT number, subtract existing number of
		 * channels from chan->id.
		 */
		chan->port[0] = chan->id - vi->num_channels;
		WARN_ON(chan->port[0] > TPG_CHANNELS);
		chan->numlanes = 2;
	} else {
		ret = tegra_vi_get_port_info(chan, vi->dev->of_node, chan->id);
		if (ret) {
			dev_err(vi->dev, "%s:Fail to parse port info\n",
					__func__);
			return ret;
		}
	}

	for (idx = 0; csi_port_is_valid(chan->port[idx]); idx++) {
		chan->total_ports++;
		/* maximum of 4 lanes are present per CSI block */
		chan->csibase[idx] = vi->iomem +
					TEGRA_VI_CSI_BASE(chan->port[idx]);
	}
	/* based on gang mode valid ports will be updated - set default to 1 */
	chan->valid_ports = chan->total_ports ? 1 : 0;
	return ret;
}

int tegra_channel_init(struct tegra_channel *chan)
{
	int ret;
	struct tegra_mc_vi *vi = chan->vi;

	ret = tegra_channel_csi_init(chan);
	if (ret)
		return ret;

	atomic_set(&chan->restart_version, 1);
	chan->capture_version = 0;
	chan->width_align = TEGRA_WIDTH_ALIGNMENT;
	chan->stride_align = TEGRA_STRIDE_ALIGNMENT;
	chan->num_subdevs = 0;
	mutex_init(&chan->video_lock);
	INIT_LIST_HEAD(&chan->capture);
	INIT_LIST_HEAD(&chan->entities);
	init_waitqueue_head(&chan->start_wait);
	spin_lock_init(&chan->start_lock);
	INIT_LIST_HEAD(&chan->release);
	init_waitqueue_head(&chan->release_wait);
	spin_lock_init(&chan->release_lock);
	mutex_init(&chan->stop_kthread_lock);
	atomic_set(&chan->is_streaming, DISABLE);
	spin_lock_init(&chan->capture_state_lock);

	/* Init video format */
	vi->fops->vi_init_video_formats(chan);
	chan->fmtinfo = tegra_core_get_default_format();
	tegra_channel_update_format(chan, TEGRA_DEF_WIDTH,
				TEGRA_DEF_HEIGHT,
				chan->fmtinfo->fourcc,
				&chan->fmtinfo->bpp, 0);

	chan->buffer_offset[0] = 0;

	/* Initialize the media entity... */
	chan->pad.flags = MEDIA_PAD_FL_SINK;

	ret = media_entity_init(&chan->video.entity, 1, &chan->pad, 0);
	if (ret < 0) {
		dev_err(&chan->video.dev, "failed to init video entity\n");
		return ret;
	}

	/* init control handler */
	ret = v4l2_ctrl_handler_init(&chan->ctrl_handler, MAX_CID_CONTROLS);
	if (chan->ctrl_handler.error) {
		dev_err(&chan->video.dev, "failed to init control handler\n");
		goto ctrl_init_error;
	}

	/* init video node... */
	chan->video.fops = &tegra_channel_fops;
	chan->video.v4l2_dev = &vi->v4l2_dev;
	chan->video.queue = &chan->queue;
	snprintf(chan->video.name, sizeof(chan->video.name), "%s-%s-%u",
		dev_name(vi->dev), chan->pg_mode ? "tpg" : "output",
		chan->port[0]);
	chan->video.vfl_type = VFL_TYPE_GRABBER;
	chan->video.vfl_dir = VFL_DIR_RX;
	chan->video.release = video_device_release_empty;
	chan->video.ioctl_ops = &tegra_channel_ioctl_ops;
	chan->video.ctrl_handler = &chan->ctrl_handler;
	chan->video.lock = &chan->video_lock;

	set_bit(_IOC_NR(VIDIOC_G_PRIORITY), chan->video.valid_ioctls);
	set_bit(_IOC_NR(VIDIOC_S_PRIORITY), chan->video.valid_ioctls);

	video_set_drvdata(&chan->video, chan);

#if defined(CONFIG_VIDEOBUF2_DMA_CONTIG)
	/* get the buffers queue... */
	chan->alloc_ctx = vb2_dma_contig_init_ctx(chan->vi->dev);
	if (IS_ERR(chan->alloc_ctx)) {
		dev_err(chan->vi->dev, "failed to init vb2 buffer\n");
		ret = -ENOMEM;
		goto vb2_init_error;
	}
#endif

	chan->queue.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
	chan->queue.io_modes = VB2_MMAP | VB2_DMABUF | VB2_READ | VB2_USERPTR;
	chan->queue.lock = &chan->video_lock;
	chan->queue.drv_priv = chan;
	chan->queue.buf_struct_size = sizeof(struct tegra_channel_buffer);
	chan->queue.ops = &tegra_channel_queue_qops;
#if defined(CONFIG_VIDEOBUF2_DMA_CONTIG)
	chan->queue.mem_ops = &vb2_dma_contig_memops;
#endif
	chan->queue.timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC
				   | V4L2_BUF_FLAG_TSTAMP_SRC_EOF;
	ret = vb2_queue_init(&chan->queue);
	if (ret < 0) {
		dev_err(chan->vi->dev, "failed to initialize VB2 queue\n");
		goto vb2_queue_error;
	}

	if (vi->fops->vi_syncpt_init)
		vi->fops->vi_syncpt_init(chan);

	chan->init_done = true;
	return 0;

vb2_queue_error:
#if defined(CONFIG_VIDEOBUF2_DMA_CONTIG)
	vb2_dma_contig_cleanup_ctx(chan->alloc_ctx);
vb2_init_error:
#endif
	v4l2_ctrl_handler_free(&chan->ctrl_handler);
ctrl_init_error:
	media_entity_cleanup(&chan->video.entity);
	return ret;
}

int tegra_channel_cleanup(struct tegra_channel *chan)
{
	if (chan->vi->fops->vi_syncpt_free)
		chan->vi->fops->vi_syncpt_free(chan);

	/* release embedded data buffer */
	if (chan->vi->emb_buf_size > 0) {
		dma_free_coherent(chan->vi->dev,
			chan->vi->emb_buf_size,
			chan->vi->emb_buf_addr, chan->vi->emb_buf);
		chan->vi->emb_buf_size = 0;
	}

	v4l2_ctrl_handler_free(&chan->ctrl_handler);
	vb2_queue_release(&chan->queue);
#if defined(CONFIG_VIDEOBUF2_DMA_CONTIG)
	vb2_dma_contig_cleanup_ctx(chan->alloc_ctx);
#endif

	media_entity_cleanup(&chan->video.entity);

	return 0;
}

int tegra_vi_channels_register(struct tegra_mc_vi *vi)
{
	int ret = 0;
	struct tegra_channel *it;
	int count = 0;

	list_for_each_entry(it, &vi->vi_chans, list) {
		struct v4l2_subdev *sd = it->subdev_on_csi;
		bool is_csi = false;

		if (sd) {
			/*
			 * If subdevice on csi is csi itself,
			 * then sensor subdevice is not connected
			 */
			is_csi = strstr(sd->name, "nvcsi") != NULL;

			if (is_csi)
				continue;
		} else
			continue;

		if (!it->init_done)
			continue;
		ret = video_register_device(&it->video, VFL_TYPE_GRABBER, -1);
		if (ret < 0) {
			dev_err(&it->video.dev, "failed to register %s\n",
				it->video.name);
			continue;
		}
		count++;
	}

	if (count == 0) {
		dev_err(vi->dev, "all channel register failed\n");
		return ret;
	}

	return 0;
}

void tegra_vi_channels_unregister(struct tegra_mc_vi *vi)
{
	struct tegra_channel *it;

	list_for_each_entry(it, &vi->vi_chans, list) {
		if (it->video.cdev != NULL)
			video_unregister_device(&it->video);
	}
}

int tegra_vi_mfi_work(struct tegra_mc_vi *vi, int channel)
{
	if (vi->fops)
		return vi->fops->vi_mfi_work(vi, channel);

	return 0;
}
EXPORT_SYMBOL(tegra_vi_mfi_work);

int tegra_vi_channels_init(struct tegra_mc_vi *vi)
{
	int ret = 0;
	struct tegra_channel *it;
	int count = 0;

	list_for_each_entry(it, &vi->vi_chans, list) {
		it->vi = vi;
		ret = tegra_channel_init(it);
		if (ret < 0) {
			dev_err(vi->dev, "channel init failed\n");
			continue;
		}
		count++;
	}

	if (count == 0) {
		dev_err(vi->dev, "all channel init failed\n");
		return ret;
	}

	return 0;
}
EXPORT_SYMBOL(tegra_vi_channels_init);
int tegra_vi_channels_cleanup(struct tegra_mc_vi *vi)
{
	int ret = 0, err = 0;
	struct tegra_channel *it;

	list_for_each_entry(it, &vi->vi_chans, list) {
		if (!it->init_done)
			continue;
		err = tegra_channel_cleanup(it);
		if (err < 0) {
			ret = err;
			dev_err(vi->dev, "channel cleanup failed, err %d\n",
					err);
		}
	}
	return ret;
}
EXPORT_SYMBOL(tegra_vi_channels_cleanup);
