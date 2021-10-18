/* SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright(c) 2021 Realtek Semiconductor Corp. All rights reserved.
 *
 * Author: Ming Jen Tai <mingjen_tai@realtek.com>
 */

#include <sof/audio/buffer.h>
#include <sof/audio/component.h>
#include <sof/audio/format.h>
#include <sof/audio/pipeline.h>
#include <sof/audio/rtnr/rtnr.h>
#include <sof/common.h>
#include <sof/debug/panic.h>
#include <sof/ipc/msg.h>
#include <sof/lib/alloc.h>
#include <sof/lib/memory.h>
#include <sof/lib/uuid.h>
#include <sof/list.h>
#include <sof/platform.h>
#include <sof/string.h>
#include <sof/ut.h>
#include <sof/trace/trace.h>
#include <ipc/control.h>
#include <ipc/stream.h>
#include <ipc/topology.h>
#include <user/trace.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>

#include <sof/audio/rtnr/rtklib/include/RTK_MA_API.h>

#define MicNum 2
#define SpkNum  2

#define RTNR_BLK_LENGTH			4 /* Must be power of 2 */
#define RTNR_BLK_LENGTH_MASK	(RTNR_BLK_LENGTH - 1)
#define RTNR_MAX_SOURCES		1 /* Microphone stream */

static const struct comp_driver comp_rtnr;

/** \brief RTNR processing functions map item. */
struct rtnr_func_map {
	enum sof_ipc_frame fmt; /**< source frame format */
	rtnr_func func; /**< processing function */
};

/* UUID 5c7ca334-e15d-11eb-ba80-0242ac130004 */
DECLARE_SOF_RT_UUID("rtnr", rtnr_uuid, 0x5c7ca334, 0xe15d, 0x11eb, 0xba, 0x80,
		    0x02, 0x42, 0xac, 0x13, 0x00, 0x04);

DECLARE_TR_CTX(rtnr_tr, SOF_UUID(rtnr_uuid), LOG_LEVEL_INFO);

/* Generic processing */

/* Called by the processing library for debugging purpose */
void rtnr_printf(int a, int b, int c, int d, int e)
{
	switch (a) {
	case 0xa:
		comp_cl_info(&comp_rtnr, "rtnr_printf 1st=%08x, 2nd=%08x, 3rd=%08x, 4st=%08x",
					b, c, d, e);
		break;

	case 0xb:
		comp_cl_info(&comp_rtnr, "rtnr_printf 1st=%08x, 2nd=%08x, 3rd=%08x, 4st=%08x",
					b, c, d, e);
		break;

	case 0xc:
		comp_cl_warn(&comp_rtnr, "rtnr_printf 1st=%08x, 2nd=%08x, 3rd=%08x, 4st=%08x",
					b, c, d, e);
		break;

	case 0xd:
		comp_cl_dbg(&comp_rtnr, "rtnr_printf 1st=%08x, 2nd=%08x, 3rd=%08x, 4st=%08x",
					b, c, d, e);
		break;

	case 0xe:
		comp_cl_err(&comp_rtnr, "rtnr_printf 1st=%08x, 2nd=%08x, 3rd=%08x, 4st=%08x",
					b, c, d, e);
		break;

	default:
		break;
	}
}

void *rtk_rballoc(unsigned int flags, unsigned int caps, unsigned int bytes)
{
	return rballoc(flags, caps, bytes);
}

void rtk_rfree(void *ptr)
{
	rfree(ptr);
}

#if CONFIG_FORMAT_S16LE

static void rtnr_s16_default(struct comp_dev *dev, const struct audio_stream **sources,
							struct audio_stream *sink, int frames)

{
	struct comp_data *cd = comp_get_drvdata(dev);

	RTKMA_API_S16_Default(cd->rtk_agl, sources, sink, frames,
						0, 0, 0,
						0, 0);
}

#endif /* CONFIG_FORMAT_S16LE */

#if CONFIG_FORMAT_S24LE

static void rtnr_s24_default(struct comp_dev *dev, const struct audio_stream **sources,
							struct audio_stream *sink, int frames)

{
	struct comp_data *cd = comp_get_drvdata(dev);

	RTKMA_API_S24_Default(cd->rtk_agl, sources, sink, frames,
						0, 0, 0,
						0, 0);
}

#endif /* CONFIG_FORMAT_S24LE */

#if CONFIG_FORMAT_S32LE

static void rtnr_s32_default(struct comp_dev *dev, const struct audio_stream **sources,
							struct audio_stream *sink, int frames)

{
	struct comp_data *cd = comp_get_drvdata(dev);

	RTKMA_API_S32_Default(cd->rtk_agl, sources, sink, frames,
						0, 0, 0,
						0, 0);
}

#endif /* CONFIG_FORMAT_S32LE */

/* Processing functions table */
/*
 *	These functions copy data from source stream to internal queue before
 *	processing, and output data from inter queue to sink stream after
 *	processing.
 */
const struct rtnr_func_map rtnr_fnmap[] = {
#if CONFIG_FORMAT_S16LE
	{ SOF_IPC_FRAME_S16_LE, rtnr_s16_default },
#endif /* CONFIG_FORMAT_S16LE */
#if CONFIG_FORMAT_S24LE
	{ SOF_IPC_FRAME_S24_4LE, rtnr_s24_default },
#endif /* CONFIG_FORMAT_S24LE */
#if CONFIG_FORMAT_S32LE
	{ SOF_IPC_FRAME_S32_LE, rtnr_s32_default },
#endif /* CONFIG_FORMAT_S32LE */
};

const size_t rtnr_fncount = ARRAY_SIZE(rtnr_fnmap);

/**
 * \brief Retrieves an RTNR processing function matching
 *	  the source buffer's frame format.
 * \param fmt the frames' format of the source and sink buffers
 */
static rtnr_func rtnr_find_func(enum sof_ipc_frame fmt)
{
	int i;

	/* Find suitable processing function from map */
	for (i = 0; i < rtnr_fncount; i++) {
		if (fmt == rtnr_fnmap[i].fmt)
			return rtnr_fnmap[i].func;
	}

	return NULL;
}

static inline void rtnr_set_process(struct comp_dev *dev)
{
	comp_info(dev, "rtnr_set_process()");
	struct comp_data *cd = comp_get_drvdata(dev);

	cd->process_enable = true;
	RTKMA_API_Bypass(cd->rtk_agl, 0);
}

static inline void rtnr_set_bypass(struct comp_dev *dev)
{
	comp_info(dev, "rtnr_set_bypass()");
	struct comp_data *cd = comp_get_drvdata(dev);

	cd->process_enable = false;
	RTKMA_API_Bypass(cd->rtk_agl, 1);
}

static inline void rtnr_set_process_sample_rate(struct comp_dev *dev, uint32_t sample_rate)
{
	comp_dbg(dev, "rtnr_set_process_sample_rate()");
	struct comp_data *cd = comp_get_drvdata(dev);

	cd->process_sample_rate = sample_rate;
}

static int32_t rtnr_check_config_validity(struct comp_dev *dev,
									    struct comp_data *cd)
{
	struct sof_rtnr_config *p_config = comp_get_data_blob(cd->model_handler, NULL, NULL);
	int ret = 0;

	if (!p_config) {
		comp_err(dev, "rtnr_check_config_validity() error: invalid cd->model_handler");
		ret = -EINVAL;
	} else {
		comp_info(dev, "rtnr_check_config_validity() enabled: %d sample_rate:%d",
				p_config->params.enabled,
				p_config->params.sample_rate);

		if (p_config->params.enabled)
			rtnr_set_process(dev);
		else
			rtnr_set_bypass(dev);

		rtnr_set_process_sample_rate(dev, p_config->params.sample_rate);
	}

	return ret;
}


/*
 * End of RTNR setup code. Next the standard component methods.
 */

static struct comp_dev *rtnr_new(const struct comp_driver *drv,
				struct sof_ipc_comp *comp)
{
	struct comp_dev *dev;
	struct comp_data *cd;
	struct sof_ipc_comp_process *rtnr;
	struct sof_ipc_comp_process *ipc_rtnr = (struct sof_ipc_comp_process *)comp;
	int ret;

	comp_cl_info(&comp_rtnr, "rtnr_new()");

	dev = comp_alloc(drv, COMP_SIZE(struct sof_ipc_comp_process));
	if (!dev)
		return NULL;

	rtnr = COMP_GET_IPC(dev, sof_ipc_comp_process);

	ret = memcpy_s(rtnr, sizeof(*rtnr), ipc_rtnr,
		       sizeof(struct sof_ipc_comp_process));
	assert(!ret);

	cd = rzalloc(SOF_MEM_ZONE_RUNTIME, 0, SOF_MEM_CAPS_RAM, sizeof(*cd));
	if (!cd)
		goto err;

	comp_set_drvdata(dev, cd);

	/* Handler for configuration data */
	cd->model_handler = comp_data_blob_handler_new(dev);
	if (!cd->model_handler) {
		comp_cl_err(&comp_rtnr, "rtnr_new(): comp_data_blob_handler_new() failed.");
		goto cd_fail;
	}

	/* Get configuration data */
	ret = comp_init_data_blob(cd->model_handler, ipc_rtnr->size, ipc_rtnr->data);
	if (ret < 0) {
		comp_cl_err(&comp_rtnr, "rtnr_new(): comp_init_data_blob() failed.");
		goto cd_fail;
	}

	/* Component defaults */
	cd->source_channel = 0;

    /* Check config */
	ret = rtnr_check_config_validity(dev, cd);
	if (ret < 0) {
		comp_cl_err(&comp_rtnr, "rtnr_new(): rtnr_check_config_validity() failed.");
		goto cd_fail;
	}

	cd->rtk_agl = RTKMA_API_Context_Create(cd->process_sample_rate);
	if (cd->rtk_agl == 0) {
		comp_cl_err(&comp_rtnr, "rtnr_new(): RTKMA_API_Context_Create failed.");
		goto cd_fail;
	}
	comp_cl_info(&comp_rtnr, "rtnr_new(): RTKMA_API_Context_Create succeeded.");

	/* Done. */
	dev->state = COMP_STATE_READY;
	return dev;

cd_fail:
	comp_data_blob_handler_free(cd->model_handler);
	rfree(cd);

err:
	rfree(dev);
	return NULL;
}

static void rtnr_free(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	comp_info(dev, "rtnr_free()");

	comp_data_blob_handler_free(cd->model_handler);

	RTKMA_API_Context_Free(cd->rtk_agl);

	rfree(cd);
	rfree(dev);
}

/* set component audio stream parameters */
static int rtnr_params(struct comp_dev *dev, struct sof_ipc_stream_params *params)
{
	int ret;
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sinkb;
	struct comp_buffer *sourceb;

	comp_info(dev, "rtnr_params()");

	ret = comp_verify_params(dev, 0, params);
	if (ret < 0) {
		comp_err(dev, "rtnr_params() error: comp_verify_params() failed.");
		return ret;
	}

	sourceb = list_first_item(&dev->bsource_list, struct comp_buffer,
							sink_list);
	sinkb = list_first_item(&dev->bsink_list, struct comp_buffer,
							source_list);

	/* set source/sink_frames/rate */
	cd->source_rate = sourceb->stream.rate;
	cd->sink_rate = sinkb->stream.rate;
	if (!cd->sink_rate) {
		comp_err(dev, "rtnr_nr_params(), zero sink rate");
		return -EINVAL;
	}

	/* Currently support 16kHz sample rate only. */
	switch (sourceb->stream.rate) {
	case 16000:
		comp_info(dev, "rtnr_params(), sample rate = 16000 kHz");
		break;
	case 48000:
		comp_info(dev, "rtnr_params(), sample rate = 48000 kHz");
		break;
	default:
		comp_err(dev, "rtnr_nr_params(), invalid sample rate(%d kHz)",
				 sourceb->stream.rate);
		return -EINVAL;
	}

	if (sourceb->stream.channels != 2 || sinkb->stream.channels != 2) {
		comp_err(dev, "rtnr_params(), source/sink stream must be 2 channels");
		return -EINVAL;
	}

	return 0;
}

static int rtnr_cmd_get_data(struct comp_dev *dev,
						struct sof_ipc_ctrl_data *cdata, int max_size)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int ret = 0;

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_BINARY:
		comp_info(dev, "rtnr_cmd_get_data(), SOF_CTRL_CMD_BINARY");
		ret = comp_data_blob_get_cmd(cd->model_handler, cdata, max_size);
		break;
	default:
		comp_err(dev, "rtnr_cmd_get_data() error: invalid command %d", cdata->cmd);
		ret = -EINVAL;
		break;
	}

	return ret;
}

static int rtnr_cmd_set_data(struct comp_dev *dev,
							struct sof_ipc_ctrl_data *cdata)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int ret = 0;

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_BINARY:
		comp_info(dev, "rtnr_cmd_set_data(), SOF_CTRL_CMD_BINARY");
		ret = comp_data_blob_set_cmd(cd->model_handler, cdata);
		break;
	default:
		comp_err(dev, "rtnr_cmd_set_data() error: invalid command %d", cdata->cmd);
		ret = -EINVAL;
		break;
	}

	if (ret >= 0)
		ret = rtnr_check_config_validity(dev, cd);

	return ret;
}

static int32_t rtnr_cmd_get_value(struct comp_dev *dev, struct sof_ipc_ctrl_data *cdata)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	int32_t j;
	int32_t ret = 0;

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_SWITCH:
		for (j = 0; j < cdata->num_elems; j++) {
			cdata->chanv[j].channel = j;
			cdata->chanv[j].value = cd->process_enable;
			comp_info(dev, "rtnr_cmd_get_value(), channel = %u, value = %u",
					cdata->chanv[j].channel,
					cdata->chanv[j].value);
		}
		break;
	default:
		comp_err(dev, "rtnr_cmd_get_value() error: invalid cdata->cmd %d", cdata->cmd);
		ret = -EINVAL;
		break;
	}
	return ret;
}

static int32_t rtnr_set_value(struct comp_dev *dev, struct sof_ipc_ctrl_data *cdata)
{
	uint32_t val = 0;
	int32_t j;

	for (j = 0; j < cdata->num_elems; j++) {
		val |= cdata->chanv[j].value;
		comp_info(dev, "rtnr_set_value(), value = %u", val);
	}

	if (val)
		rtnr_set_process(dev);
	else
		rtnr_set_bypass(dev);

	return 0;
}

static int32_t rtnr_cmd_set_value(struct comp_dev *dev, struct sof_ipc_ctrl_data *cdata)
{
	int32_t ret = 0;

	switch (cdata->cmd) {
	case SOF_CTRL_CMD_SWITCH:
		comp_dbg(dev, "rtnr_cmd_set_value(), SOF_CTRL_CMD_SWITCH, cdata->comp_id = %u",
				cdata->comp_id);
		ret = rtnr_set_value(dev, cdata);
		break;

	default:
		comp_err(dev, "rtnr_cmd_set_value() error: invalid cdata->cmd");
		ret = -EINVAL;
		break;
	}

	return ret;
}

/* used to pass standard and bespoke commands (with data) to component */
static int rtnr_cmd(struct comp_dev *dev, int cmd, void *data,
					int max_data_size)
{
	struct sof_ipc_ctrl_data *cdata = ASSUME_ALIGNED(data, 4);
	int ret = 0;

	comp_info(dev, "rtnr_cmd()");

	switch (cmd) {
	case COMP_CMD_SET_DATA:
		ret = rtnr_cmd_set_data(dev, cdata);
		break;
	case COMP_CMD_GET_DATA:
		ret = rtnr_cmd_get_data(dev, cdata, max_data_size);
		break;
	case COMP_CMD_SET_VALUE:
		ret = rtnr_cmd_set_value(dev, cdata);
		break;
	case COMP_CMD_GET_VALUE:
		ret = rtnr_cmd_get_value(dev, cdata);
		break;
	default:
		comp_err(dev, "rtnr_cmd() error: invalid command");
		return -EINVAL;
	}

	return ret;
}

static int rtnr_trigger(struct comp_dev *dev, int cmd)
{
	comp_info(dev, "rtnr_trigger() cmd: %d", cmd);

	return comp_set_state(dev, cmd);
}

/* copy and process stream data from source to sink buffers */
static int rtnr_copy(struct comp_dev *dev)
{
	struct comp_buffer *sink;
	struct comp_buffer *source;
	int frames;
	int sink_bytes;
	int source_bytes;
	const struct audio_stream *sources_stream[RTNR_MAX_SOURCES] = { NULL };
	struct comp_data *cd = comp_get_drvdata(dev);

	comp_dbg(dev, "rtnr_copy()");

	source = list_first_item(&dev->bsource_list, struct comp_buffer, sink_list);

	/* put empty data into output queue*/
	RTKMA_API_First_Copy(cd->rtk_agl, cd->source_rate, source->stream.channels);

	sink = list_first_item(&dev->bsink_list, struct comp_buffer, source_list);
	frames = audio_stream_avail_frames(&source->stream, &sink->stream);

	/* Process integer multiple of RTNR internal block length */
	frames = frames & ~RTNR_BLK_LENGTH_MASK;

	comp_dbg(dev, "rtnr_copy() source->id: %d, frames = %d", source->id, frames);

	if (frames) {
		source_bytes = frames * audio_stream_frame_bytes(&source->stream);
		sink_bytes = frames * audio_stream_frame_bytes(&sink->stream);

		buffer_invalidate(source, source_bytes);

		/* Run processing function */

		/*
		 * Processing function uses an array of pointers to source streams
		 * as parameter.
		 */
		sources_stream[0] = &source->stream;
		cd->rtnr_func(dev, sources_stream, &sink->stream, frames);

		/*
		 * real process function of rtnr, consume/produce data from internal queue
		 * instead of component buffer
		 */
		RTKMA_API_Process(cd->rtk_agl, 0, cd->source_rate, MicNum);

		buffer_writeback(sink, sink_bytes);

		/* Track consume and produce */
		comp_update_buffer_consume(source, source_bytes);
		comp_update_buffer_produce(sink, sink_bytes);
	}


	return 0;
}

static int rtnr_prepare(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);
	struct comp_buffer *sinkb;
	int ret;

	comp_info(dev, "rtnr_prepare()");

	ret = comp_set_state(dev, COMP_TRIGGER_PREPARE);
	if (ret < 0)
		return ret;

	if (ret == COMP_STATUS_STATE_ALREADY_SET)
		return PPL_STATUS_PATH_STOP;

	/* Get sink data format */
	sinkb = list_first_item(&dev->bsink_list, struct comp_buffer, source_list);
	cd->sink_format = sinkb->stream.frame_fmt;

	/* Check source and sink PCM format and get copy function */
	comp_info(dev, "rtnr_prepare(), sink_format=%d", cd->sink_format);
	cd->rtnr_func = rtnr_find_func(cd->sink_format);
	if (!cd->rtnr_func) {
		comp_err(dev, "rtnr_prepare(): No suitable processing function found.");
		goto err;
	}

	/* Default on */
	cd->process_enable = true;

	/* Clear in/out buffers */
	RTKMA_API_Prepare(cd->rtk_agl);

	/* Initialize RTNR */
	return 0;

err:
	comp_set_state(dev, COMP_TRIGGER_RESET);
	return ret;
}

static int rtnr_reset(struct comp_dev *dev)
{
	struct comp_data *cd = comp_get_drvdata(dev);

	comp_info(dev, "rtnr_reset()");
	cd->sink_format = 0;
	cd->rtnr_func = NULL;
	comp_set_state(dev, COMP_TRIGGER_RESET);
	return 0;
}

static const struct comp_driver comp_rtnr = {
	.uid = SOF_RT_UUID(rtnr_uuid),
	.tctx = &rtnr_tr,
	.ops = {
		.create = rtnr_new,
		.free = rtnr_free,
		.params = rtnr_params,
		.cmd = rtnr_cmd,
		.trigger = rtnr_trigger,
		.copy = rtnr_copy,
		.prepare = rtnr_prepare,
		.reset = rtnr_reset,
	},
};

static SHARED_DATA struct comp_driver_info comp_rtnr_info = {
	.drv = &comp_rtnr,
};

UT_STATIC void sys_comp_rtnr_init(void)
{
	comp_register(platform_shared_get(&comp_rtnr_info, sizeof(comp_rtnr_info)));
}


DECLARE_MODULE(sys_comp_rtnr_init);
