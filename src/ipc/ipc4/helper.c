// SPDX-License-Identifier: BSD-3-Clause
//
// Copyright(c) 2016 Intel Corporation. All rights reserved.
//
// Author: Liam Girdwood <liam.r.girdwood@linux.intel.com>
//         Keyon Jie <yang.jie@linux.intel.com>

#include <sof/audio/buffer.h>
#include <sof/audio/component_ext.h>
#include <sof/audio/pipeline.h>
#include <sof/common.h>
#include <sof/drivers/idc.h>
#include <sof/drivers/interrupt.h>
#include <sof/ipc/topology.h>
#include <sof/ipc/common.h>
#include <sof/ipc/msg.h>
#include <sof/ipc/driver.h>
#include <sof/ipc/schedule.h>
#include <sof/lib/alloc.h>
#include <sof/lib/cache.h>
#include <sof/lib/cpu.h>
#include <sof/lib/mailbox.h>
#include <sof/list.h>
#include <sof/platform.h>
#include <sof/sof.h>
#include <sof/spinlock.h>
#include <rimage/cavs/cavs_ext_manifest.h>
#include <rimage/sof/user/manifest.h>

#include <ipc4/header.h>
#include <ipc4/pipeline.h>
#include <ipc4/module.h>
#include <ipc4/error_status.h>
#include <ipc4/copier.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define IPC4_MOD_ID(x) ((x) >> 16)

extern struct tr_ctx comp_tr;

void ipc_build_stream_posn(struct sof_ipc_stream_posn *posn, uint32_t type,
			   uint32_t id)
{
}

void ipc_build_comp_event(struct sof_ipc_comp_event *event, uint32_t type,
			  uint32_t id)
{
}

void ipc_build_trace_posn(struct sof_ipc_dma_trace_posn *posn)
{
}

/* Function overwrites PCM parameters (frame_fmt, buffer_fmt, channels, rate)
 * with buffer parameters when specific flag is set.
 */
static void comp_update_params(uint32_t flag,
			       struct sof_ipc_stream_params *params,
			       struct comp_buffer *buffer)
{
	if (flag & BUFF_PARAMS_FRAME_FMT)
		params->frame_fmt = buffer->stream.frame_fmt;

	if (flag & BUFF_PARAMS_BUFFER_FMT)
		params->buffer_fmt = buffer->buffer_fmt;

	if (flag & BUFF_PARAMS_CHANNELS)
		params->channels = buffer->stream.channels;

	if (flag & BUFF_PARAMS_RATE)
		params->rate = buffer->stream.rate;
}

/* used with IPC4 - placeholder atm */
int comp_verify_params(struct comp_dev *dev, uint32_t flag,
		       struct sof_ipc_stream_params *params)
{
	struct list_item *buffer_list;
	struct list_item *source_list;
	struct list_item *sink_list;
	struct list_item *clist;
	struct list_item *curr;
	struct comp_buffer *sinkb;
	struct comp_buffer *buf;
	int dir = dev->direction;
	uint32_t flags = 0;

	if (!params) {
		comp_err(dev, "comp_verify_params(): !params");
		return -EINVAL;
	}

	source_list = comp_buffer_list(dev, PPL_DIR_UPSTREAM);
	sink_list = comp_buffer_list(dev, PPL_DIR_DOWNSTREAM);

	/* searching for endpoint component e.g. HOST, DETECT_TEST, which
	 * has only one sink or one source buffer.
	 */
	if (list_is_empty(source_list) != list_is_empty(sink_list)) {
		if (!list_is_empty(source_list))
			buf = list_first_item(&dev->bsource_list,
					      struct comp_buffer,
					      sink_list);
		else
			buf = list_first_item(&dev->bsink_list,
					      struct comp_buffer,
					      source_list);

		buffer_lock(buf, &flags);

		/* update specific pcm parameter with buffer parameter if
		 * specific flag is set.
		 */
		comp_update_params(flag, params, buf);

		/* overwrite buffer parameters with modified pcm
		 * parameters
		 */
		buffer_set_params(buf, params, BUFFER_UPDATE_FORCE);

		/* set component period frames */
		component_set_period_frames(dev, buf->stream.rate);

		buffer_unlock(buf, flags);
	} else {
		/* for other components we iterate over all downstream buffers
		 * (for playback) or upstream buffers (for capture).
		 */
		buffer_list = comp_buffer_list(dev, dir);
		clist = buffer_list->next;

		while (clist != buffer_list) {
			curr = clist;
			buf = buffer_from_list(curr, struct comp_buffer, dir);
			buffer_lock(buf, &flags);
			clist = clist->next;
			comp_update_params(flag, params, buf);
			buffer_set_params(buf, params, BUFFER_UPDATE_FORCE);
			buffer_unlock(buf, flags);
		}

		/* fetch sink buffer in order to calculate period frames */
		sinkb = list_first_item(&dev->bsink_list, struct comp_buffer,
					source_list);

		buffer_lock(sinkb, &flags);
		component_set_period_frames(dev, sinkb->stream.rate);
		buffer_unlock(sinkb, flags);
	}

	return 0;
}


int32_t ipc_comp_pipe_id(const struct ipc_comp_dev *icd)
{
	switch (icd->type) {
	case COMP_TYPE_COMPONENT:
		return dev_comp_pipe_id(icd->cd);
	case COMP_TYPE_BUFFER:
		return icd->cb->pipeline_id;
	case COMP_TYPE_PIPELINE:
		return icd->pipeline->pipeline_id;
	default:
		tr_err(&ipc_tr, "Unknown ipc component type %u", icd->type);
		return -EINVAL;
	};

	return 0;
}

/* used with IPC4 new module - placeholder atm */
struct comp_dev *comp_new(struct sof_ipc_comp *comp)
{
	struct comp_ipc_config ipc_config;
	const struct comp_driver *drv;
	struct comp_dev *dev;

	drv = ipc4_get_comp_drv(IPC4_MOD_ID(comp->id));
	if (!drv)
		return NULL;

	if (ipc4_get_comp_dev(comp->id)) {
		tr_err(&ipc_tr, "comp %d exists", comp->id);
		return NULL;
	}

	memset(&ipc_config, 0, sizeof(ipc_config));
	ipc_config.id = comp->id;
	ipc_config.pipeline_id = comp->pipeline_id;
	ipc_config.core = comp->core;

	dev = drv->ops.create(drv, &ipc_config, (void *)MAILBOX_HOSTBOX_BASE);
	if (!dev)
		return NULL;

	ipc4_add_comp_dev(dev);

	return dev;
}

int ipc_pipeline_new(struct ipc *ipc, ipc_pipe_new *_pipe_desc)
{
	struct ipc4_pipeline_create *pipe_desc = ipc_from_pipe_new(_pipe_desc);
	struct ipc_comp_dev *ipc_pipe;
	struct pipeline *pipe;

	tr_err(&ipc_tr, "pipe_desc->instance_id = %u", (uint32_t)pipe_desc->header.r.instance_id);

	/* check whether pipeline id is already taken or in use */
	ipc_pipe = ipc_get_comp_by_ppl_id(ipc, COMP_TYPE_PIPELINE,
					  pipe_desc->header.r.instance_id);
	if (ipc_pipe) {
		tr_err(&ipc_tr, "ipc_pipeline_new(): pipeline id is already taken, pipe_desc->instance_id = %u",
		       (uint32_t)pipe_desc->header.r.instance_id);
		return IPC4_INVALID_RESOURCE_ID;
	}

	/* create the pipeline */
	pipe = pipeline_new(pipe_desc->header.r.instance_id,
			    pipe_desc->header.r.ppl_priority, 0);
	if (!pipe) {
		tr_err(&ipc_tr, "ipc_pipeline_new(): pipeline_new() failed");
		return IPC4_OUT_OF_MEMORY;
	}

	pipe->time_domain = SOF_TIME_DOMAIN_TIMER;
	/* 1ms */
	pipe->period = 1000;

	/* allocate the IPC pipeline container */
	ipc_pipe = rzalloc(SOF_MEM_ZONE_RUNTIME_SHARED, 0, SOF_MEM_CAPS_RAM,
			   sizeof(struct ipc_comp_dev));
	if (!ipc_pipe) {
		pipeline_free(pipe);
		return IPC4_OUT_OF_MEMORY;
	}

	ipc_pipe->pipeline = pipe;
	ipc_pipe->type = COMP_TYPE_PIPELINE;
	ipc_pipe->id = pipe_desc->header.r.instance_id;

	/* add new pipeline to the list */
	list_item_append(&ipc_pipe->list, &ipc->comp_list);

	return 0;
}

static int ipc_pipeline_module_free(uint32_t pipeline_id)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *icd;
	int ret;

	icd = ipc_get_comp_by_ppl_id(ipc, COMP_TYPE_COMPONENT, pipeline_id);
	while (icd) {
		ret = ipc_comp_free(ipc, icd->id);
		if (ret)
			return ret;

		icd = ipc_get_comp_by_ppl_id(ipc, COMP_TYPE_COMPONENT, pipeline_id);
	}

	return IPC4_SUCCESS;
}

int ipc_pipeline_free(struct ipc *ipc, uint32_t comp_id)
{
	struct ipc_comp_dev *ipc_pipe;
	int ret;

	/* check whether pipeline exists */
	ipc_pipe = ipc_get_comp_by_id(ipc, comp_id);
	if (!ipc_pipe)
		return -ENODEV;

	/* check core */
	if (!cpu_is_me(ipc_pipe->core))
		return ipc_process_on_core(ipc_pipe->core);

	ret = ipc_pipeline_module_free(ipc_pipe->pipeline->pipeline_id);
	if (ret) {
		tr_err(&ipc_tr, "ipc_pipeline_free(): module free () failed");
		return ret;
	}

	/* free buffer and remove from list */
	ret = pipeline_free(ipc_pipe->pipeline);
	if (ret < 0) {
		tr_err(&ipc_tr, "ipc_pipeline_free(): pipeline_free() failed");
		return IPC4_INVALID_RESOURCE_STATE;
	}

	ipc_pipe->pipeline = NULL;
	list_item_del(&ipc_pipe->list);
	rfree(ipc_pipe);

	return IPC4_SUCCESS;
}

int ipc_pipeline_complete(struct ipc *ipc, uint32_t comp_id)
{
	struct ipc_comp_dev *ipc_pipe;
	struct ipc_comp_dev *icd;
	struct pipeline *p;
	int ret;

	/* check whether pipeline exists */
	ipc_pipe = ipc_get_comp_by_id(ipc, comp_id);
	if (!ipc_pipe) {
		tr_err(&ipc_tr, "ipc: ipc_pipeline_complete looking for pipe component id %d failed",
		       comp_id);
		return -EINVAL;
	}

	/* check core */
	if (!cpu_is_me(ipc_pipe->core))
		return ipc_process_on_core(ipc_pipe->core);

	p = ipc_pipe->pipeline;

	/* find the scheduling component */
	icd = ipc_get_comp_by_id(ipc, p->sched_id);
	if (!icd) {
		tr_err(&ipc_tr, "ipc_pipeline_complete(): cannot find the scheduling component, p->sched_id = %u",
		       p->sched_id);
		return IPC4_INVALID_REQUEST;
	}

	if (icd->type != COMP_TYPE_COMPONENT) {
		tr_err(&ipc_tr, "ipc_pipeline_complete(): icd->type (%d) != COMP_TYPE_COMPONENT for pipeline scheduling component icd->id %d",
		       icd->type, icd->id);
		return IPC4_INVALID_REQUEST;
	}

	if (icd->core != ipc_pipe->core) {
		tr_err(&ipc_tr, "ipc_pipeline_complete(): icd->core (%d) != ipc_pipe->core (%d) for pipeline scheduling component icd->id %d",
		       icd->core, ipc_pipe->core, icd->id);
		return IPC4_INVALID_REQUEST;
	}

	p->sched_comp = icd->cd;

	ret = pipeline_complete(ipc_pipe->pipeline, p->source_comp, p->sink_comp);
	if (ret < 0)
		ret = IPC4_INVALID_REQUEST;

	return ret;
}

/* not used with IPC4 - placeholder atm */
int ipc_buffer_new(struct ipc *ipc, struct sof_ipc_buffer *desc)
{
	return 0;
}

/* not used with IPC4 - placeholder atm */
int ipc_buffer_free(struct ipc *ipc, uint32_t buffer_id)
{
	return 0;
}

static struct comp_buffer *ipc4_create_buffer(struct comp_dev *src, struct comp_dev *sink,
					      uint32_t src_queue, uint32_t dst_queue)
{
	struct ipc4_base_module_cfg *src_cfg, *sink_cfg;
	struct comp_buffer *buffer = NULL;
	struct sof_ipc_buffer ipc_buf;
	int buf_size;

	src_cfg = (struct ipc4_base_module_cfg *)comp_get_drvdata(src);
	sink_cfg = (struct ipc4_base_module_cfg *)comp_get_drvdata(sink);

	buf_size = MAX(src_cfg->obs, sink_cfg->ibs);
	memset(&ipc_buf, 0, sizeof(ipc_buf));
	ipc_buf.size = buf_size;
	ipc_buf.comp.id = IPC4_COMP_ID(src_queue, dst_queue);
	ipc_buf.comp.pipeline_id = src->ipc_config.pipeline_id;
	ipc_buf.comp.core = src->ipc_config.core;
	buffer = buffer_new(&ipc_buf);

	return buffer;
}

static int ipc4_comp_to_buffer_connect(struct comp_dev *comp,
				       struct comp_buffer *buffer)
{
	uint32_t flags;

	if (!cpu_is_me(comp->ipc_config.core))
		return ipc_process_on_core(comp->ipc_config.core);

	irq_local_disable(flags);
	list_item_prepend(buffer_comp_list(buffer, PPL_CONN_DIR_COMP_TO_BUFFER),
			  comp_buffer_list(comp, PPL_CONN_DIR_COMP_TO_BUFFER));
	buffer_set_comp(buffer, comp, PPL_CONN_DIR_COMP_TO_BUFFER);
	comp_writeback(comp);

	dcache_writeback_invalidate_region(buffer, sizeof(*buffer));
	irq_local_enable(flags);

	return 0;
}

static int ipc4_buffer_to_comp_connect(struct comp_buffer *buffer,
				       struct comp_dev *comp)
{
	uint32_t flags;

	if (!cpu_is_me(comp->ipc_config.core))
		return ipc_process_on_core(comp->ipc_config.core);

	/* check if it's a connection between cores */
	if (buffer->core != comp->ipc_config.core) {
		dcache_invalidate_region(buffer, sizeof(*buffer));

		buffer->inter_core = true;

		if (!comp->is_shared) {
			comp = comp_make_shared(comp);
			if (!comp)
				return IPC4_OUT_OF_MEMORY;
		}
	}

	irq_local_disable(flags);
	list_item_prepend(buffer_comp_list(buffer, PPL_CONN_DIR_BUFFER_TO_COMP),
			  comp_buffer_list(comp, PPL_CONN_DIR_BUFFER_TO_COMP));
	buffer_set_comp(buffer, comp, PPL_CONN_DIR_BUFFER_TO_COMP);
	comp_writeback(comp);

	dcache_writeback_invalidate_region(buffer, sizeof(*buffer));
	irq_local_enable(flags);

	return 0;
}

int ipc_comp_connect(struct ipc *ipc, ipc_pipe_comp_connect *_connect)
{
	struct ipc4_module_bind_unbind *bu;
	struct comp_buffer *buffer = NULL;
	struct comp_dev *src, *sink;
	int src_id, sink_id;
	int ret;

	bu = (struct ipc4_module_bind_unbind *)_connect;
	src_id = IPC4_COMP_ID(bu->header.r.module_id, bu->header.r.instance_id);
	sink_id = IPC4_COMP_ID(bu->data.r.dst_module_id, bu->data.r.dst_instance_id);
	src = ipc4_get_comp_dev(src_id);
	sink = ipc4_get_comp_dev(sink_id);
	if (!src || !sink) {
		tr_err(&ipc_tr, "failed to find src %x, or dst %x", src_id, sink_id);
		return IPC4_INVALID_RESOURCE_ID;
	}

	buffer = ipc4_create_buffer(src, sink, bu->data.r.src_queue, bu->data.r.dst_queue);
	if (!buffer) {
		tr_err(&ipc_tr, "failed to allocate buffer to bind %d to %d", src_id, sink_id);
		return IPC4_OUT_OF_MEMORY;
	}

	ret = ipc4_comp_to_buffer_connect(src, buffer);
	if (ret < 0) {
		tr_err(&ipc_tr, "failed to connect src %d to internal buffer", src_id);
		goto err;
	}

	ret = ipc4_buffer_to_comp_connect(buffer, sink);
	if (ret < 0) {
		tr_err(&ipc_tr, "failed to connect internal buffer to sink %d", sink_id);
		goto err;
	}

	ret = comp_bind(src, bu);
	if (ret < 0)
		return IPC4_INVALID_RESOURCE_ID;

	ret = comp_bind(sink, bu);
	if (ret < 0)
		return IPC4_INVALID_RESOURCE_ID;

	return 0;

err:
	buffer_free(buffer);
	return IPC4_INVALID_RESOURCE_STATE;
}

/* when both module instances are parts of the same pipeline Unbind IPC would
 * be ignored by FW since FW does not support changing internal topology of pipeline
 * during run-time. The only way to change pipeline topology is to delete the whole
 * pipeline and create it in modified form.
 */
int ipc_comp_disconnect(struct ipc *ipc, ipc_pipe_comp_connect *_connect)
{
	struct ipc4_module_bind_unbind *bu;
	struct comp_buffer *buffer = NULL;
	struct comp_dev *src, *sink;
	struct list_item *sink_list;
	uint32_t src_id, sink_id, buffer_id;
	uint32_t flags;
	int ret;

	bu = (struct ipc4_module_bind_unbind *)_connect;
	src_id = IPC4_COMP_ID(bu->header.r.module_id, bu->header.r.instance_id);
	sink_id = IPC4_COMP_ID(bu->data.r.dst_module_id, bu->data.r.dst_instance_id);
	src = ipc4_get_comp_dev(src_id);
	sink = ipc4_get_comp_dev(sink_id);
	if (!src || !sink) {
		tr_err(&ipc_tr, "failed to find src %x, or dst %x", src_id, sink_id);
		return IPC4_INVALID_RESOURCE_ID;
	}

	if (src->pipeline == sink->pipeline)
		return IPC4_INVALID_REQUEST;

	buffer_id = IPC4_COMP_ID(bu->data.r.src_queue, bu->data.r.dst_queue);
	list_for_item(sink_list, &src->bsink_list) {
		struct comp_buffer *buf;

		buf = container_of(sink_list, struct comp_buffer, source_list);
		if (buf->id == buffer_id) {
			buffer = buf;
			break;
		}
	}

	if (!buffer)
		return IPC4_INVALID_RESOURCE_ID;

	irq_local_disable(flags);
	list_item_del(buffer_comp_list(buffer, PPL_CONN_DIR_COMP_TO_BUFFER));
	list_item_del(buffer_comp_list(buffer, PPL_CONN_DIR_BUFFER_TO_COMP));
	comp_writeback(src);
	comp_writeback(sink);
	irq_local_enable(flags);

	buffer_free(buffer);

	ret = comp_unbind(src, bu);
	if (ret < 0)
		return IPC4_INVALID_RESOURCE_ID;

	ret = comp_unbind(sink, bu);
	if (ret < 0)
		return IPC4_INVALID_RESOURCE_ID;

	return 0;
}

/* used with IPC4 - placeholder atm */
int ipc_comp_new(struct ipc *ipc, ipc_comp *_comp)
{
	return 0;
}

/* used with IPC4 - placeholder atm */
int ipc_comp_free(struct ipc *ipc, uint32_t comp_id)
{
	struct ipc_comp_dev *icd;

	icd = ipc_get_comp_by_id(ipc, comp_id);
	if (!icd)
		return IPC4_INVALID_RESOURCE_ID;

	/* check core */
	if (!cpu_is_me(icd->core))
		return ipc_process_on_core(icd->core);

	/* check state */
	if (icd->cd->state != COMP_STATE_READY)
		return IPC4_BAD_STATE;

	/* set pipeline sink/source/sched pointers to NULL if needed */
	if (icd->cd->pipeline) {
		if (icd->cd == icd->cd->pipeline->source_comp)
			icd->cd->pipeline->source_comp = NULL;
		if (icd->cd == icd->cd->pipeline->sink_comp)
			icd->cd->pipeline->sink_comp = NULL;
		if (icd->cd == icd->cd->pipeline->sched_comp)
			icd->cd->pipeline->sched_comp = NULL;
	}

	/* free component and remove from list */
	comp_free(icd->cd);

	icd->cd = NULL;

	list_item_del(&icd->list);
	rfree(icd);

	return 0;
}

struct comp_buffer *buffer_new(struct sof_ipc_buffer *desc)
{
	struct comp_buffer *buffer;

	tr_info(&buffer_tr, "buffer new size 0x%x id %d.%d flags 0x%x",
		desc->size, desc->comp.pipeline_id, desc->comp.id, desc->flags);

	/* allocate buffer */
	buffer = buffer_alloc(desc->size, desc->caps, PLATFORM_DCACHE_ALIGN);
	if (buffer) {
		buffer->id = desc->comp.id;
		buffer->pipeline_id = desc->comp.pipeline_id;
		buffer->core = desc->comp.core;

		buffer->stream.underrun_permitted = desc->flags &
						    SOF_BUF_UNDERRUN_PERMITTED;
		buffer->stream.overrun_permitted = desc->flags &
						   SOF_BUF_OVERRUN_PERMITTED;

		memcpy_s(&buffer->tctx, sizeof(struct tr_ctx),
			 &buffer_tr, sizeof(struct tr_ctx));

		dcache_writeback_invalidate_region(buffer, sizeof(*buffer));
	}

	return buffer;
}

static const struct comp_driver *module_driver[FW_MAX_EXT_MODULE_NUM] = {0};

const struct comp_driver *ipc4_get_drv(uint8_t *uuid)
{
	struct comp_driver_list *drivers = comp_drivers_get();
	struct list_item *clist;
	const struct comp_driver *drv = NULL;
	struct comp_driver_info *info;
	uint32_t flags;

	irq_local_disable(flags);

	/* search driver list with UUID */
	list_for_item(clist, &drivers->list) {
		info = container_of(clist, struct comp_driver_info,
				    list);
		if (!memcmp(info->drv->uid, uuid, UUID_SIZE)) {
			tr_dbg(&comp_tr,
			       "found type %d, uuid %pU",
			       info->drv->type,
			       info->drv->tctx->uuid_p);
			drv = info->drv;
			goto out;
		}
	}

	tr_err(&comp_tr, "get_drv(): the provided UUID (%8x %8x %8x %8x) doesn't match to any driver!",
	       *(uint32_t *)(&uuid[0]),
	       *(uint32_t *)(&uuid[4]),
	       *(uint32_t *)(&uuid[8]),
	       *(uint32_t *)(&uuid[12]));

out:
	irq_local_enable(flags);
	return drv;
}

const struct comp_driver *ipc4_get_comp_drv(int module_id)
{
	struct sof_man_fw_desc *desc = (struct sof_man_fw_desc *)IMR_BOOT_LDR_MANIFEST_BASE;
	struct sof_man_module *mod;
	const struct comp_driver *drv;

	if (module_driver[module_id])
		return module_driver[module_id];

	/* skip basefw of module 0 in manifest */
	mod = (struct sof_man_module *)((char *)desc + SOF_MAN_MODULE_OFFSET(module_id));
	drv = ipc4_get_drv(mod->uuid);
	module_driver[module_id] = drv;

	return drv;
}

struct comp_dev *ipc4_get_comp_dev(uint32_t comp_id)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *icd;

	icd = ipc_get_comp_by_id(ipc, comp_id);
	if (!icd)
		return NULL;

	return icd->cd;
}

int ipc4_add_comp_dev(struct comp_dev *dev)
{
	struct ipc *ipc = ipc_get();
	struct ipc_comp_dev *icd;

	/* allocate the IPC component container */
	icd = rzalloc(SOF_MEM_ZONE_RUNTIME_SHARED, 0, SOF_MEM_CAPS_RAM,
		      sizeof(struct ipc_comp_dev));
	if (!icd) {
		tr_err(&ipc_tr, "ipc_comp_new(): alloc failed");
		rfree(icd);
		return IPC4_OUT_OF_MEMORY;
	}

	icd->cd = dev;
	icd->type = COMP_TYPE_COMPONENT;
	icd->core = dev->ipc_config.core;
	icd->id = dev->ipc_config.id;

	tr_err(&ipc_tr, "ipc4_add_comp_dev add  comp %x", icd->id);
	/* add new component to the list */
	list_item_append(&icd->list, &ipc->comp_list);

	return 0;
};
