/*
 * Tegra Video Input 2 device common APIs
 *
 * Copyright (c) 2016-2018, NVIDIA CORPORATION.  All rights reserved.
 *
 * Author: Bryan Wu <pengw@nvidia.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */
#include <linux/device.h>
#include <linux/nvhost.h>
#include <linux/tegra-powergate.h>
#include <linux/kthread.h>
#include <linux/freezer.h>
#include <media/tegra_camera_platform.h>
#include "nvhost_acm.h"
#include "camera/vi/mc_common.h"
#include "camera/csi/csi2_fops.h"
#include "vi2_formats.h"
#include "vi/vi.h"

#define DEFAULT_FRAMERATE	30
#define DEFAULT_CSI_FREQ	204000000
#define BPP_MEM		2
#define NUM_PPC		2
#define VI_CSI_CLK_SCALE	110

extern void tegra_channel_queued_buf_done(struct tegra_channel *chan,
					  enum vb2_buffer_state state);
static void tegra_channel_stop_kthreads(struct tegra_channel *chan);
extern int tegra_channel_set_stream(struct tegra_channel *chan, bool on);
extern struct tegra_channel_buffer *dequeue_buffer(struct tegra_channel *chan);
extern void enqueue_inflight(struct tegra_channel *chan,
                             struct tegra_channel_buffer *buf);
extern struct tegra_channel_buffer *dequeue_inflight(struct tegra_channel *chan);
extern int tegra_channel_set_power(struct tegra_channel *chan, bool on);
extern void release_buffer(struct tegra_channel *chan, struct tegra_channel_buffer* buf);
extern void set_timestamp(struct tegra_channel_buffer *buf,
				const struct timespec *ts);

static void vi_write(struct tegra_mc_vi *vi, unsigned int addr, u32 val)
{
	writel(val, vi->iomem + addr);
}

static u32 tegra_channel_read(struct tegra_channel *chan,
			unsigned int addr)
{
	return readl(chan->vi->iomem + addr);
}

static void tegra_channel_write(struct tegra_channel *chan,
			unsigned int addr, u32 val)
{
	writel(val, chan->vi->iomem + addr);
}

/* CSI registers */
static void csi_write(struct tegra_channel *chan, unsigned int index,
			unsigned int addr, u32 val)
{
	writel(val, chan->csibase[index] + addr);
}

static u32 csi_read(struct tegra_channel *chan, unsigned int index,
					unsigned int addr)
{
	return readl(chan->csibase[index] + addr);
}

static void vi_channel_syncpt_init(struct tegra_channel *chan)
{
	int i;

	for (i = 0; i < chan->total_ports; i++) {
		chan->syncpt[i][0] =
			nvhost_get_syncpt_client_managed(chan->vi->ndev, "vi");
		chan->syncpt[i][1] =
			nvhost_get_syncpt_client_managed(chan->vi->ndev, "vi");
	}
}

static void vi_channel_syncpt_free(struct tegra_channel *chan)
{
	int i;

	for (i = 0; i < chan->total_ports; i++) {
		nvhost_syncpt_put_ref_ext(chan->vi->ndev, chan->syncpt[i][0]);
		nvhost_syncpt_put_ref_ext(chan->vi->ndev, chan->syncpt[i][1]);
	}
}

void vi2_init_video_formats(struct tegra_channel *chan)
{
	int i;

	chan->num_video_formats = ARRAY_SIZE(vi2_video_formats);
	for (i = 0; i < chan->num_video_formats; i++)
		chan->video_formats[i] = &vi2_video_formats[i];
}

int tegra_vi2_s_ctrl(struct v4l2_ctrl *ctrl)
{
	struct tegra_channel *chan = container_of(ctrl->handler,
				struct tegra_channel, ctrl_handler);
	int err = 0;

	switch (ctrl->id) {
	case TEGRA_CAMERA_CID_WRITE_ISPFORMAT:
		chan->write_ispformat = ctrl->val;
		break;
	default:
		dev_err(&chan->video.dev, "%s:Not valid ctrl\n", __func__);
		return -EINVAL;
	}

	return err;
}

static const struct v4l2_ctrl_ops vi2_ctrl_ops = {
	.s_ctrl	= tegra_channel_s_ctrl,
};

static const struct v4l2_ctrl_config vi2_custom_ctrls[] = {
	{
		.ops = &vi2_ctrl_ops,
		.id = TEGRA_CAMERA_CID_WRITE_ISPFORMAT,
		.name = "Write ISP format",
		.type = V4L2_CTRL_TYPE_BOOLEAN,
		.def = 0,
		.min = 0,
		.step = 1,
		.max = 1,
	},
};

int vi2_add_ctrls(struct tegra_channel *chan)
{
	int i;

	/* Add vi2 custom controls */
	for (i = 0; i < ARRAY_SIZE(vi2_custom_ctrls); i++) {
		v4l2_ctrl_new_custom(&chan->ctrl_handler,
			&vi2_custom_ctrls[i], NULL);
		if (chan->ctrl_handler.error) {
			dev_err(chan->vi->dev,
				"Failed to add %s ctrl\n",
				vi2_custom_ctrls[i].name);
			return chan->ctrl_handler.error;
		}
	}

	return 0;
}

static struct tegra_csi_channel *find_linked_csi_channel(
	struct tegra_channel *chan, struct tegra_csi_device *csi)
{
	struct tegra_csi_channel *csi_it;
	struct tegra_csi_channel *csi_chan = NULL;
	int i;
	/* Find connected csi_channel */
	list_for_each_entry(csi_it, &csi->csi_chans, list) {
		for (i = 0; i < chan->num_subdevs; i++) {
			if (chan->subdev[i] == &csi_it->subdev) {
				csi_chan = csi_it;
				break;
			}
		}
	}
	return csi_chan;
}
static int tegra_channel_capture_setup(struct tegra_channel *chan)
{
	u32 height = chan->format.height;
	u32 width = chan->format.width;
	u32 format = chan->fmtinfo->img_fmt;
	u32 data_type = chan->fmtinfo->img_dt;
	u32 word_count = tegra_core_get_word_count(width, chan->fmtinfo);
	u32 bypass_pixel_transform = 1;
	int index;

	if (chan->valid_ports > 1) {
		height = chan->gang_height;
		width = chan->gang_width;
		word_count = tegra_core_get_word_count(width, chan->fmtinfo);
	}

	if (chan->pg_mode ||
	   (chan->write_ispformat == TEGRA_ISP_FORMAT) ||
	   (chan->fmtinfo->vf_code == TEGRA_VF_YUV422) ||
	   (chan->fmtinfo->vf_code == TEGRA_VF_RGB888))
		bypass_pixel_transform = 0;

	for (index = 0; index < chan->valid_ports; index++) {
		csi_write(chan, index, TEGRA_VI_CSI_ERROR_STATUS, 0xFFFFFFFF);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DEF,
		  (bypass_pixel_transform << BYPASS_PXL_TRANSFORM_OFFSET) |
		  (format << IMAGE_DEF_FORMAT_OFFSET));
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DT, data_type);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_SIZE_WC, word_count);
		csi_write(chan, index, TEGRA_VI_CSI_IMAGE_SIZE,
			(height << IMAGE_SIZE_HEIGHT_OFFSET) | width);
	}

	return 0;
}

static int tegra_channel_enable_stream(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_csi_channel *csi_chan = NULL;
	struct tegra_csi_device *csi = chan->vi->csi;
	/*
	 * enable pad power and perform calibration before arming
	 * single shot for first frame after the HW setup is complete
	 */
	/* Find connected csi_channel */
	csi_chan = find_linked_csi_channel(chan, csi);

	/* start streaming */
	ret = tegra_channel_set_stream(chan, true);
	return ret;
}

static void tegra_channel_ec_init(struct tegra_channel *chan)
{
	/*
	 * error recover initialization sequence
	 * set timeout as 200 ms, use default if fps not available
	 * Time limit allow CSI to capture good frames and drop error frames
	 * Timeout units is jiffies, 1 jiffy = 10ms
	 * TODO: Get frame rate from sub-device and adopt timeout
	 */
	chan->timeout = 20;

	/*
	 * Sync point FIFO full blocks host interface
	 * Below setting enables SW to process error recovery
	 */
	tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT_CNTRL, 0x100);
}


static void tegra_channel_clear_singleshot(struct tegra_channel *chan,
						int index)
{
	/* clear single shot */
	csi_write(chan, index, TEGRA_VI_CSI_SW_RESET, 0xF);
	csi_write(chan, index, TEGRA_VI_CSI_SW_RESET, 0x0);
}

static void tegra_channel_vi_csi_recover(struct tegra_channel *chan)
{
	u32 error_val = tegra_channel_read(chan,
					TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR);
	u32 frame_start, mw_ack_done;
	int index, valid_ports = chan->valid_ports;
	struct tegra_csi_channel *csi_chan;
	struct tegra_csi_device *csi = chan->vi->csi;

	/* Disable clock gating to enable continuous clock */
	tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, DISABLE);
	/* Find connected csi_channel */
	csi_chan = find_linked_csi_channel(chan, csi);

	/* clear CSI state */
	for (index = 0; index < valid_ports; index++) {
		tegra_csi_error_recover(csi_chan, index);
		csi_write(chan, index,
				TEGRA_VI_CSI_IMAGE_DEF, 0);
		tegra_channel_clear_singleshot(chan, index);
	}

	/* clear VI errors */
	for (index = 0; index < valid_ports; index++) {
		frame_start = VI_CSI_PP_FRAME_START(chan->port[index]);
		if (error_val & frame_start)
			chan->syncpoint_fifo[index][0] = SYNCPT_FIFO_DEPTH;

		mw_ack_done = VI_CSI_MW_ACK_DONE(chan->port[index]);
		if (error_val & mw_ack_done)
			chan->syncpoint_fifo[index][1] = SYNCPT_FIFO_DEPTH;
	}
	/* clear FIFO error status */
	tegra_channel_write(chan,
		TEGRA_VI_CFG_VI_INCR_SYNCPT_ERROR, error_val);

	/* Enable clock gating so VI can be clock gated if necessary */
	tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, ENABLE);

	/* re-init VI and CSI */
	tegra_channel_capture_setup(chan);
	for (index = 0; index < valid_ports; index++) {
		csi2_stop_streaming(csi_chan, index);
		csi2_start_streaming(csi_chan, index);
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						chan->syncpt[index][0]);
		nvhost_syncpt_set_min_eq_max_ext(chan->vi->ndev,
						chan->syncpt[index][1]);
	}
}

static void tegra_channel_capture_error(struct tegra_channel *chan)
{
	u32 val;
	int index = 0;
	struct tegra_csi_channel *csi_chan;
	struct tegra_csi_device *csi = chan->vi->csi;

	/* Find connected csi_channel */
	csi_chan = find_linked_csi_channel(chan, csi);

	for (index = 0; index < chan->valid_ports; index++) {
		val = csi_read(chan, index, TEGRA_VI_CSI_ERROR_STATUS);
		dev_dbg(&chan->video.dev,
			"TEGRA_VI_CSI_ERROR_STATUS 0x%08x\n", val);
		tegra_csi_status(csi_chan, index);
	}
}

static void tegra_channel_ec_recover(struct tegra_channel *chan)
{
	tegra_channel_capture_error(chan);
	tegra_channel_vi_csi_recover(chan);
}

static int tegra_channel_error_status(struct tegra_channel *chan)
{
	u32 val;
	int err = 0;
	int index = 0;
	struct tegra_csi_channel *csi_chan;
	struct tegra_csi_device *csi = chan->vi->csi;

	/* Find connected csi_channel */
	csi_chan = find_linked_csi_channel(chan, csi);

	for (index = 0; index < chan->valid_ports; index++) {
		/* Ignore error based on resolution but reset status */
		val = csi_read(chan, index, TEGRA_VI_CSI_ERROR_STATUS);
		csi_write(chan, index, TEGRA_VI_CSI_ERROR_STATUS, val);
		err = tegra_csi_error(csi_chan, index);
	}

	if (err)
		dev_err(chan->vi->dev, "%s:error %x frame %d\n",
				__func__, err, chan->sequence);
	return err;
}

/*
 * Initialize the VI and request it to fill `buf` with the next
 * inbound frame. If we need to reset, do that now. Then enque
 * the buffer for the release thread.
 */
static int tegra_channel_capture_frame(struct tegra_channel *chan,
					struct tegra_channel_buffer *buf)
{
	struct timespec ts = {0, 0};
	int err = 0;
	u32 val, frame_start, mw_ack_done;
	int bytes_per_line = chan->format.bytesperline;
	int index = 0;
	u32 thresh[TEGRA_CSI_BLOCKS] = { 0 };
	u32 release_thresh[TEGRA_CSI_BLOCKS] = { 0 };
	int valid_ports = chan->valid_ports;
	int restart_version = 0;
	bool is_streaming = atomic_read(&chan->is_streaming);

	/* Init registers related to each frames */
	for (index = 0; index < valid_ports; index++) {
		/* Program buffer address by using surface 0 */
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_MSB, 0x0);
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_LSB,
			(buf->addr + chan->buffer_offset[index]));
		csi_write(chan, index,
			TEGRA_VI_CSI_SURFACE0_STRIDE, bytes_per_line);

		if (chan->fmtinfo->fourcc == V4L2_PIX_FMT_NV16) {
			/*
			 * Program surface 1 for UV plane,
			 * with offset sizeimage from Y plane
			 */
			csi_write(chan,
				index, TEGRA_VI_CSI_SURFACE1_OFFSET_MSB, 0x0);
			csi_write(chan, index,
				TEGRA_VI_CSI_SURFACE1_OFFSET_LSB,
				(buf->addr + chan->format.sizeimage / 2 +
				chan->buffer_offset[index]));
			csi_write(chan, index,
				TEGRA_VI_CSI_SURFACE1_STRIDE, bytes_per_line);
		}

		/* Program syncpoints */
		thresh[index] = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[index][0], 1);
		/* Do not arm sync points if FIFO had entries before */
		if (!chan->syncpoint_fifo[index][0]) {
			frame_start = VI_CSI_PP_FRAME_START(chan->port[index]);
			val = VI_CFG_VI_INCR_SYNCPT_COND(frame_start) |
				chan->syncpt[index][0];
			tegra_channel_write(chan,
				TEGRA_VI_CFG_VI_INCR_SYNCPT, val);
		} else
			chan->syncpoint_fifo[index][0]--;

		release_thresh[index] =
			nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[index][1], 1);
		/* Do not arm sync points if FIFO had entries before */
		if (!chan->syncpoint_fifo[index][1]) {
			mw_ack_done = VI_CSI_MW_ACK_DONE(chan->port[index]);
			val = VI_CFG_VI_INCR_SYNCPT_COND(mw_ack_done) |
				chan->syncpt[index][1];
			tegra_channel_write(chan,
				TEGRA_VI_CFG_VI_INCR_SYNCPT, val);
		} else
			chan->syncpoint_fifo[index][1]--;

		memcpy(&buf->thresh[0], &release_thresh[0],
			TEGRA_CSI_BLOCKS * sizeof(u32));
	}

	/* enable input stream once the VI registers are configured */
	/* NOTE:
	 * If a restart request has occured since the last time this
	 * function was called, then restart (i.e. re-setup) the VI
	 */
	restart_version = atomic_read(&chan->restart_version);
	if(restart_version != chan->capture_version || !is_streaming) {
		chan->capture_version = restart_version;
		/* TODO: added this call to ec_recover() here to prevent
		 * it from happening on two separate threads if there was a
		 * syncpt timeout on receive. Is this correct approach?
		 */
		tegra_channel_ec_recover(chan);

		err = tegra_channel_enable_stream(chan);
		if (err) {
			dev_err(&chan->video.dev,
				"failed to enable stream. ERROR: %d\n", err);

			buf->state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_ERROR;
			getrawmonotonic(&ts);
			set_timestamp(buf, &ts);
			release_buffer(chan, buf);
			return err;
		}
		/* Bit controls VI memory write, enable after all regs */
		for (index = 0; index < valid_ports; index++) {
			val = csi_read(chan, index,
					TEGRA_VI_CSI_IMAGE_DEF);
			csi_write(chan, index, TEGRA_VI_CSI_IMAGE_DEF,
					val | IMAGE_DEF_DEST_MEM);
		}
	}

	/* Ensure all CSI ports are ready with setup to avoid timing issue */
	for (index = 0; index < valid_ports; index++)
		csi_write(chan, index,
			TEGRA_VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);

	chan->capture_state = CAPTURE_GOOD;
	for (index = 0; index < valid_ports; index++) {
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
			chan->syncpt[index][0], thresh[index],
			chan->timeout, NULL, &ts);
		if (err) {
			dev_err(&chan->video.dev,
				"frame start syncpt timeout!%d\n", index);
			buf->state = VB2_BUF_STATE_ERROR;
			/* perform error recovery for timeout */
			tegra_channel_ec_recover(chan);
			chan->capture_state = CAPTURE_TIMEOUT;
			break;
		}
		dev_dbg(&chan->video.dev,
			"%s: vi2 got SOF syncpt buf[%p]\n", __func__, buf);
	}

	if (!err && !chan->pg_mode) {
		/* Marking error frames and resume capture */
		/* TODO: TPG has frame height short error always set */
		err = tegra_channel_error_status(chan);
		if (err) {
			buf->state = VB2_BUF_STATE_ERROR;
			chan->capture_state = CAPTURE_ERROR;
			/* do we have to run recover here ?? */
			/* tegra_channel_ec_recover(chan); */
		}
	}

	set_timestamp(buf, &ts);

	if(chan->capture_state == CAPTURE_GOOD) {
		/* Set the buffer version to match the current capture version */
		buf->version = chan->capture_version;
		enqueue_inflight(chan, buf);
	} else {
		release_buffer(chan, buf);
		atomic_inc(&chan->restart_version);
	}

	return 0;
}

static void tegra_channel_capture_done(struct tegra_channel *chan)
{
	struct timespec ts;
	int index, err;
	int bytes_per_line = chan->format.bytesperline;
	u32 val, mw_ack_done;
	u32 thresh[TEGRA_CSI_BLOCKS] = { 0 };
	struct tegra_channel_buffer *buf;

	/* dequeue buffer and return if no buffer exists */
	buf = dequeue_buffer(chan);
	if (!buf) {
		dev_dbg(&chan->video.dev, "%s: no buffers in queue\n", __func__);
		return;
	}

	for (index = 0; index < chan->valid_ports; index++) {
		/* Program buffer address by using surface 0 */
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_MSB, 0x0);
		csi_write(chan, index, TEGRA_VI_CSI_SURFACE0_OFFSET_LSB,
			(buf->addr + chan->buffer_offset[index]));
		csi_write(chan, index,
			TEGRA_VI_CSI_SURFACE0_STRIDE, bytes_per_line);

		if (chan->fmtinfo->fourcc == V4L2_PIX_FMT_NV16) {
			/*
			 * Program surface 1 for UV plane,
			 * with offset sizeimage from Y plane
			 */
			csi_write(chan, index,
				TEGRA_VI_CSI_SURFACE1_OFFSET_MSB, 0x0);
			csi_write(chan, index,
				TEGRA_VI_CSI_SURFACE1_OFFSET_LSB,
				(buf->addr + chan->format.sizeimage / 2 +
				chan->buffer_offset[index]));
			csi_write(chan, index,
				TEGRA_VI_CSI_SURFACE1_STRIDE, bytes_per_line);
		}

		/* Program syncpoints */
		thresh[index] = nvhost_syncpt_incr_max_ext(chan->vi->ndev,
					chan->syncpt[index][1], 1);
		mw_ack_done = VI_CSI_MW_ACK_DONE(chan->port[index]);
		val = VI_CFG_VI_INCR_SYNCPT_COND(mw_ack_done) |
				chan->syncpt[index][1];
		tegra_channel_write(chan, TEGRA_VI_CFG_VI_INCR_SYNCPT, val);
		if (!csi_read(chan, index, TEGRA_VI_CSI_SINGLE_SHOT)) {
			csi_write(chan, index,
				TEGRA_VI_CSI_SINGLE_SHOT, SINGLE_SHOT_CAPTURE);
		} else {
			dev_dbg(&chan->video.dev,
				"Syncpoint already enabled at capture done!%d\n", index);
		}
	}

	for (index = 0; index < chan->valid_ports; index++) {
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
			chan->syncpt[index][1], thresh[index],
			chan->timeout, NULL, &ts);
		if (err) {
			dev_err(&chan->video.dev,
				"MW_ACK_DONE syncpoint time out!%d\n", index);
			buf->state = VB2_BUF_STATE_ERROR;
			/* perform error recovery for timeout */
			tegra_channel_ec_recover(chan);
			break;
		}
		else {
			buf->state = VB2_BUF_STATE_DONE;
		}
	}
	set_timestamp(buf, &ts);
	release_buffer(chan, buf);

	/* Mark capture state to IDLE as capture is finished */
	chan->capture_state = CAPTURE_IDLE;
}

static void tegra_channel_release_frame(struct tegra_channel *chan,
					struct tegra_channel_buffer *buf)
{
	struct timespec ts = {0, 0};
	int index;
	int err = 0;
	int restart_version = 0;
	buf->state = VB2_BUF_STATE_DONE;

	/* If the frame capture was started on a different reset version
	 * than our current version than either a reset is imminent or
	 * it has already happened so don't bother waiting for the frame
	 * to complete. */
	restart_version = atomic_read(&chan->restart_version);
	if(buf->version != restart_version) {
		buf->state = VB2_BUF_STATE_ERROR;
		release_buffer(chan, buf);
		return;
	}

	for (index = 0; index < chan->valid_ports; index++) {
		err = nvhost_syncpt_wait_timeout_ext(chan->vi->ndev,
			chan->syncpt[index][1], buf->thresh[index],
			chan->timeout, NULL, &ts);
		if (err)
			dev_err(&chan->video.dev,
				"MW_ACK_DONE syncpoint time out!%d\n", index);

		dev_dbg(&chan->video.dev,
			"%s: vi2 got EOF syncpt buf[%p]\n", __func__, buf);
	}

	if (err) {
		buf->state = VB2_BUF_STATE_ERROR;
		/* perform error recovery for timeout */
		/* NOTE:
		 * disabling the following, it will happen in the
		 * capture thread on the next frame start due to the reset
		 * request we make by incrementing the reset counter

		tegra_channel_ec_recover(chan);
		chan->capture_state = CAPTURE_TIMEOUT;
		 */
		atomic_inc(&chan->restart_version);
	}
	release_buffer(chan, buf);
}

static int tegra_channel_kthread_capture_start(void *data)
{
	struct tegra_channel *chan = data;
	struct tegra_channel_buffer *buf;
	int err = 0;

	set_freezable();

	while (1) {

		try_to_freeze();

		wait_event_interruptible(chan->start_wait,
					 !list_empty(&chan->capture) ||
					 kthread_should_stop());

		if (kthread_should_stop())
			break;

		/* source is not streaming if error is non-zero */
		/* wait till kthread stop and dont DeQ buffers */
		if (err)
			continue;

		buf = dequeue_buffer(chan);
		if (!buf)
			continue;

		err = tegra_channel_capture_frame(chan, buf);
	}

	return 0;
}

static int tegra_channel_kthread_release(void *data)
{
	struct tegra_channel *chan = data;
	struct tegra_channel_buffer *buf;

	set_freezable();

	while (1) {

		try_to_freeze();

		wait_event_interruptible(chan->release_wait,
					 !list_empty(&chan->release) ||
					 kthread_should_stop());

		if (kthread_should_stop())
			break;

		buf = dequeue_inflight(chan);
		if (!buf)
			continue;

		tegra_channel_release_frame(chan, buf);
	}

	return 0;
}

static void tegra_channel_stop_kthreads(struct tegra_channel *chan)
{
	struct tegra_channel_buffer *buf;

	mutex_lock(&chan->stop_kthread_lock);
	/* Stop the kthread for capture */
	if (chan->kthread_capture_start) {
		kthread_stop(chan->kthread_capture_start);
		chan->kthread_capture_start = NULL;
	}

	if (chan->kthread_release) {
		if (!list_empty(&chan->release)) {
			buf = dequeue_inflight(chan);
			if (buf)
				tegra_channel_release_frame(chan, buf);
		}
		kthread_stop(chan->kthread_release);
		chan->kthread_release = NULL;
	}

	mutex_unlock(&chan->stop_kthread_lock);
}

static int tegra_channel_update_clknbw(
	struct tegra_channel *chan, u8 on) __maybe_unused;
static int tegra_channel_update_clknbw(struct tegra_channel *chan, u8 on)
{
	int ret = 0;
	unsigned long request_pixelrate;
	struct v4l2_subdev_frame_interval fie;
	unsigned long csi_freq = 0;

	fie.interval.denominator = DEFAULT_FRAMERATE;
	fie.interval.numerator = 1;

	if (v4l2_subdev_has_op(chan->subdev_on_csi,
				video, g_frame_interval))
		v4l2_subdev_call(chan->subdev_on_csi, video,
				g_frame_interval, &fie);
	else {
		if (v4l2_subdev_has_op(chan->subdev_on_csi,
				video, g_dv_timings)) {
			u32 total_width;
			u32 total_height;
			struct v4l2_dv_timings dvtimings;
			struct v4l2_bt_timings *timings = &dvtimings.bt;

			v4l2_subdev_call(chan->subdev_on_csi,
				video, g_dv_timings, &dvtimings);
			total_width = timings->width + timings->hfrontporch +
				timings->hsync + timings->hbackporch;
			total_height = timings->height + timings->vfrontporch +
				timings->vsync + timings->vbackporch;
			fie.interval.denominator = timings->pixelclock /
				(total_width * total_height);
		}
	}

	if (on) {
		/**
		 * TODO: use real sensor pixelrate
		 * See PowerService code
		 */
		request_pixelrate = (long long)(chan->format.width
				* chan->format.height
				* fie.interval.denominator / 100)
				* VI_CSI_CLK_SCALE;
		/* for PG, get csi frequency from nvhost */
		if (chan->pg_mode) {
			ret = nvhost_module_get_rate(
					chan->vi->csi->pdev, &csi_freq, 0);
			csi_freq = ret ? DEFAULT_CSI_FREQ : csi_freq;
		} else
			/* Use default csi4 frequency for t186 for now
			 * We can't get the frequency from nvhost because
			 * vi4 does not has access to csi4
			 */
			csi_freq = DEFAULT_CSI_FREQ;

		/* VI clk should be slightly faster than CSI clk*/
		ret = nvhost_module_set_rate(chan->vi->ndev, &chan->video,
				max(request_pixelrate,
				csi_freq * VI_CSI_CLK_SCALE * NUM_PPC / 100),
				0, NVHOST_PIXELRATE);
		if (ret) {
			dev_err(chan->vi->dev, "Fail to update vi clk\n");
			return ret;
		}
	} else {
		ret = nvhost_module_set_rate(chan->vi->ndev, &chan->video, 0, 0,
				NVHOST_PIXELRATE);
		if (ret) {
			dev_err(chan->vi->dev, "Fail to update vi clk\n");
			return ret;
		}
	}

	chan->requested_kbyteps = (on > 0 ? 1 : -1) *
		((long long)(chan->format.width * chan->format.height
		* fie.interval.denominator * BPP_MEM) * 115 / 100) / 1000;

	mutex_lock(&chan->vi->bw_update_lock);
	chan->vi->aggregated_kbyteps += chan->requested_kbyteps;
	ret = vi_v4l2_update_isobw(chan->vi->aggregated_kbyteps, 0);
	mutex_unlock(&chan->vi->bw_update_lock);
	if (ret)
		dev_info(chan->vi->dev,
		"WAR:Calculation not precise.Ignore BW request failure\n");
#if 0
	ret = vi4_v4l2_set_la(chan->vi->ndev, 0, 0);
	if (ret)
		dev_info(chan->vi->dev,
		"WAR:Calculation not precise.Ignore LA failure\n");
#endif
	return 0;
}

int vi2_channel_start_streaming(struct vb2_queue *vq, u32 count)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	struct media_pipeline *pipe = chan->video.entity.pipe;
	int ret = 0;
	struct tegra_csi_channel *csi_chan = NULL;
	struct tegra_csi_device *csi = chan->vi->csi;
	struct v4l2_ctrl *override_ctrl;

	tegra_channel_ec_init(chan);

	/* Start the pipeline. */
	ret = media_entity_pipeline_start(&chan->video.entity, pipe);
	if (ret < 0)
		goto error_pipeline_start;

	if (chan->bypass) {
		ret = tegra_channel_set_stream(chan, true);
		if (ret < 0)
			goto error_set_stream;
		return ret;
	}
	chan->capture_state = CAPTURE_IDLE;
	/* Find connected csi_channel */
	csi_chan = find_linked_csi_channel(chan, csi);

	if (!csi_chan)
		goto error_set_stream;

	/* Note: Program VI registers after TPG, sensors and CSI streaming */
	ret = tegra_channel_capture_setup(chan);
	if (ret < 0)
		goto error_capture_setup;

	chan->sequence = 0;

	/* disable override for vi mode */
	override_ctrl = v4l2_ctrl_find(
		&chan->ctrl_handler, TEGRA_CAMERA_CID_OVERRIDE_ENABLE);
	if (!chan->pg_mode) {
		if (override_ctrl) {
			ret = v4l2_ctrl_s_ctrl(override_ctrl, false);
			if (ret < 0)
				dev_err(&chan->video.dev,
					"failed to disable override control\n");
		} else
			dev_err(&chan->video.dev,
				"No override control\n");
	}
	/* Update clock and bandwidth based on the format */
	tegra_channel_update_clknbw(chan, 1);

	/* Start kthread to capture data to buffer */
	chan->kthread_capture_start = kthread_run(
					tegra_channel_kthread_capture_start,
					chan, chan->video.name);
	if (IS_ERR(chan->kthread_capture_start)) {
		dev_err(&chan->video.dev,
			"failed to run kthread for capture start\n");
		ret = PTR_ERR(chan->kthread_capture_start);
		goto error_capture_setup;
	}

	/* Start thread to release buffers */
	chan->kthread_release = kthread_run(
					tegra_channel_kthread_release,
					chan, chan->video.name);
	if (IS_ERR(chan->kthread_release)) {
		dev_err(&chan->video.dev,
			"failed to run kthread for release\n");
		ret = PTR_ERR(chan->kthread_release);
		goto error_capture_setup;
	}

	return 0;

error_capture_setup:
	if (!chan->pg_mode)
		tegra_channel_set_stream(chan, false);
error_set_stream:
	if (!chan->pg_mode)
		media_entity_pipeline_stop(&chan->video.entity);
error_pipeline_start:
	vq->start_streaming_called = 0;
	tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_QUEUED);

	return ret;
}

void vi2_channel_stop_streaming(struct vb2_queue *vq)
{
	struct tegra_channel *chan = vb2_get_drv_priv(vq);
	int index;
	bool is_streaming = atomic_read(&chan->is_streaming);
	struct tegra_csi_channel *csi_chan = NULL;
	struct tegra_csi_device *csi = chan->vi->csi;

	if (!chan->bypass) {
		tegra_channel_stop_kthreads(chan);
		/* wait for last frame memory write ack */
		if (is_streaming && chan->capture_state == CAPTURE_GOOD)
			tegra_channel_capture_done(chan);

		/* dequeue buffers back to app which are in capture queue */
		tegra_channel_queued_buf_done(chan, VB2_BUF_STATE_ERROR);

		/* Disable clock gating to enable continuous clock */
		tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, DISABLE);
		/* Find connected csi_channel */
		csi_chan = find_linked_csi_channel(chan, csi);
		if (!csi_chan)
			pr_err("%s, no csi_chan found\n", __func__);
		for (index = 0; index < chan->valid_ports; index++) {
			/* csi2_stop_streaming(csi_chan, index); */
			/* Always clear single shot if armed at close */
			if (csi_read(chan, index, TEGRA_VI_CSI_SINGLE_SHOT))
				tegra_channel_clear_singleshot(chan, index);
		}
		/* Enable clock gating so VI can be clock gated if necessary */
		tegra_channel_write(chan, TEGRA_VI_CFG_CG_CTRL, ENABLE);
	}

	tegra_channel_set_stream(chan, false);
	media_entity_pipeline_stop(&chan->video.entity);

	if (!chan->bypass)
		tegra_channel_update_clknbw(chan, 0);
}

int vi2_mfi_work(struct tegra_mc_vi *vi, int csiport)
{
	struct tegra_channel *it;
	int ret = 0;

	/* for vi2, the input argument is the actual CSI port itself */
	/* search the list and match the port */
	list_for_each_entry(it, &vi->vi_chans, list) {
		if (csiport == it->port[0]) {
			ret = v4l2_subdev_call(it->subdev_on_csi, core,
					sync, V4L2_SYNC_EVENT_FOCUS_POS);
			if (ret < 0 && ret != -ENOIOCTLCMD) {
				dev_err(vi->dev,
					"%s:channel failed\n", __func__);
				return ret;
			}
		}
	}

	return ret;
}

int tegra_vi2_power_on(struct tegra_mc_vi *vi)
{
	int ret;

	ret = nvhost_module_busy(vi->ndev);
	if (ret) {
		dev_err(vi->dev, "%s:nvhost module is busy\n", __func__);
		return ret;
	}

	vi_write(vi, TEGRA_VI_CFG_CG_CTRL, 1);

	ret = tegra_camera_emc_clk_enable();
	if (ret)
		goto err_emc_enable;

	return 0;

err_emc_enable:
	nvhost_module_idle(vi->ndev);

	return ret;
}

void tegra_vi2_power_off(struct tegra_mc_vi *vi)
{
	tegra_channel_ec_close(vi);
	tegra_camera_emc_clk_disable();
	nvhost_module_idle(vi->ndev);
}

int vi2_power_on(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_mc_vi *vi;
	struct vi *tegra_vi;
	struct tegra_csi_device *csi;

	vi = chan->vi;
	tegra_vi = vi->vi;
	csi = vi->csi;

	ret = nvhost_module_add_client(vi->ndev, &chan->video);
	if (ret)
		return ret;

	if (atomic_add_return(1, &vi->power_on_refcnt) == 1) {
		tegra_vi2_power_on(vi);
		if (chan->pg_mode)
			tegra_vi->tpg_opened = true;
		else
			tegra_vi->sensor_opened = true;
	}

	if ((atomic_add_return(1, &chan->power_on_refcnt) == 1))
		ret = tegra_channel_set_power(chan, 1);

	return ret;
}

void vi2_power_off(struct tegra_channel *chan)
{
	int ret = 0;
	struct tegra_mc_vi *vi;
	struct vi *tegra_vi;
	struct tegra_csi_device *csi;

	vi = chan->vi;
	tegra_vi = vi->vi;
	csi = vi->csi;

	if (atomic_dec_and_test(&chan->power_on_refcnt)) {
		ret = tegra_channel_set_power(chan, 0);
		if (ret < 0)
			dev_err(vi->dev, "Failed to power off subdevices\n");
	}

	/* The last release then turn off power */
	if (atomic_dec_and_test(&vi->power_on_refcnt)) {
		tegra_vi2_power_off(vi);
		if (vi->pg_mode)
			tegra_vi->tpg_opened = false;
		else
			tegra_vi->sensor_opened = false;
	}
	nvhost_module_remove_client(vi->ndev, &chan->video);
}

void vi2_syncpt_init(struct tegra_channel *chan)
{
	vi_channel_syncpt_init(chan);
}

void vi2_syncpt_free(struct tegra_channel *chan)
{
	vi_channel_syncpt_free(chan);
}
