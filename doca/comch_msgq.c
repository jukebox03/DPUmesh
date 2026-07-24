#include "comch_msgq.h"

#ifdef DOCA_ARCH_DPU

#include "dpa.h"
#include "object.h"
#include "dpa_common.h"
#include "comch_common.h"
#include <doca_log.h>
#include <doca_pe.h>
#include <doca_build_config.h>

DOCA_LOG_REGISTER(COMCH_MSGQ);

doca_error_t 
init_comch_dpa_msgq(struct objects *objs, struct doca_pe *pe)
{
	doca_error_t result;

		/* Channel k binds to consumer_pes[k % A]. */
	int nb = objs->n_data_workers >= 1 ? objs->n_data_workers : 1;
	for (int k = 0; k < objs->num_dpa_threads; k++) {
		result = dmesh_doca_dpa_comch_create(objs, k);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("Failed to create DPA comch for EU %d.", k);
			return result;
		}

		struct doca_pe *chan_pe = (nb >= 2 && objs->consumer_pes[k % nb])
		                          ? objs->consumer_pes[k % nb] : pe;
		struct dmesh_doca_dpa_msgq_create_attr msgq_attr = {
			.dev = objs->dev,
			.dpa = objs->dpa,
			.max_num_msg = CC_DPA_MAX_MSG_NUM,
			.consumer_comp = objs->dpa_comches[k]->consumer_comp,
			.producer_comp = objs->dpa_comches[k]->producer_comp,
			.pe = chan_pe,
			.ctx_state_changed_cb = dmesh_doca_dpa_comch_msgq_ctx_state_changed_cb,
			.ctx_user_data = objs,
		};

		msgq_attr.is_send = true;
		result = dmesh_doca_dpa_msgq_create(&msgq_attr, &objs->dpa_comches[k]->send);
		if (result != DOCA_SUCCESS)
			return result;

		msgq_attr.is_send = false;
		result = dmesh_doca_dpa_msgq_create(&msgq_attr, &objs->dpa_comches[k]->recv);
		if (result != DOCA_SUCCESS)
			return result;
	}

	return DOCA_SUCCESS;
}
#endif /* DOCA_ARCH_DPU */
