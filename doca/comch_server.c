#include "comch_server.h"

#include <time.h>
#include <stdlib.h>
#include <string.h>

#include "common.h"
#include "object.h"
#include "comch_common.h"
#include "dpa.h"      /* teardown_pod_dma (DOCA_ARCH_DPU only) */
#include "dpu_proxy.h"
#include "workload_grant.h"
#include "control_scope.h"
#include "dmesh_l7.h"   /* l7_control_event: admission accounting */

#include <doca_pe.h>
#include <doca_comch.h>
#include <doca_buf_array.h>
#include <doca_mmap.h>
#include <doca_log.h>
#include <openssl/rand.h>

DOCA_LOG_REGISTER(COMCH_SERVER);

int
dmesh_registration_should_expire(uint64_t connected_ns, uint64_t now_ns,
				    int registered, int disconnect_pending,
				    uint64_t timeout_ns)
{
	return connected_ns != 0 && !registered && !disconnect_pending &&
	       now_ns >= connected_ns && now_ns - connected_ns >= timeout_ns;
}

/* The L7 layer owns the metrics surface these outcomes are exported through.
 * A build that links no L7 layer — the Host transport library — resolves this
 * weak definition instead and drops the accounting. */
__attribute__((weak)) void
l7_control_event(const char *kind, const char *reason)
{
	(void)kind;
	(void)reason;
}

/* Likewise: a build with no L7 layer holds no policy watches to drop. */
__attribute__((weak)) void
l7_inbound_forget(int worker_id, const char *workload)
{
	(void)worker_id;
	(void)workload;
}

static int resolve_dns_label(const char *text, size_t len)
{
	if (len == 0 || len > 63 || text[0] == '-' || text[len - 1] == '-')
		return 0;
	for (size_t i = 0; i < len; i++) {
		unsigned char c = (unsigned char)text[i];
		if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
			return 0;
	}
	return 1;
}

int dmesh_resolve_service_key(const char *namespace_name, const char *query,
			      char *out, size_t out_len)
{
	if (namespace_name == NULL || query == NULL || out == NULL || out_len == 0)
		return -1;
	const char *dot = strchr(query, '.');
	const char *ns = dot != NULL ? dot + 1 : namespace_name;
	size_t name_len = dot != NULL ? (size_t)(dot - query) : strlen(query);
	size_t ns_len = strlen(ns);
	if ((dot != NULL && strchr(dot + 1, '.') != NULL) ||
	    !resolve_dns_label(query, name_len) || !resolve_dns_label(ns, ns_len) ||
	    ns_len + 1 + name_len + 1 > out_len)
		return -1;
	snprintf(out, out_len, "%.*s/%.*s", (int)ns_len, ns,
	         (int)name_len, query);
	return 0;
}

static void server_send_task_completion_callback(struct doca_comch_task_send *task,
						 union doca_data task_user_data,
						 union doca_data ctx_user_data)
{
	struct objects *objs;
	void *payload_copy = task_user_data.ptr;

	objs = (struct objects *)ctx_user_data.ptr;
	doca_pool_release(&objs->send_tasks_in_flight);
	if (payload_copy != NULL)
		free(payload_copy);
	doca_task_free(doca_comch_task_send_as_task(task));
}

static void server_send_task_completion_err_callback(struct doca_comch_task_send *task,
						     union doca_data task_user_data,
						     union doca_data ctx_user_data)
{
	struct objects *objs = (struct objects *)ctx_user_data.ptr;
	void *payload_copy = task_user_data.ptr;

	doca_pool_release(&objs->send_tasks_in_flight);
	DOCA_LOG_ERR("Server send task failed: %s",
		     doca_error_get_name(doca_task_get_status(doca_comch_task_send_as_task(task))));
	if (payload_copy != NULL)
		free(payload_copy);
	doca_task_free(doca_comch_task_send_as_task(task));
}

/* Handle a message received by the Comch server. */
static void server_message_recv_callback(struct doca_comch_event_msg_recv *event,
					 uint8_t *recv_buffer,
					 uint32_t msg_len,
					 struct doca_comch_connection *comch_connection)
{
	union doca_data user_data;
	struct doca_comch_server *comch_server;
	struct objects *objs;
	doca_error_t result;

	(void)event;

	comch_server = doca_comch_server_get_server_ctx(comch_connection);
	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(comch_server), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from ctx with error = %s", doca_error_get_name(result));
		return;
	}

	objs = (struct objects *)user_data.ptr;
	if (msg_len < 1) {
		DOCA_LOG_ERR("Received short control message from client: %u", msg_len);
		return;
	}

	/* Track the primary (first) client's connection */
	if (objs->connection == NULL)
		objs->connection = comch_connection;

	switch (recv_buffer[0]) {
	case DMESH_MSG_MMAP_EXPORT:
		if (msg_len <= sizeof(struct dmesh_mmap_msg)) {
			DOCA_LOG_ERR("Received invalid MMAP message from client");
			struct pod_state *pod = find_pod_by_connection(objs, comch_connection);
			if (pod != NULL)
				(void)server_publish_pod_init_result(objs, pod, DMESH_POD_INIT_MMAP_FAILED);
			return;
		}
		result = process_mmap_msg(objs, comch_connection,
		                          (struct dmesh_mmap_msg *)recv_buffer, msg_len);
		{
			struct pod_state *pod = find_pod_by_connection(objs, comch_connection);
			if (result != DOCA_SUCCESS) {
				if (pod != NULL) {
					int kmax = objs->k_rings > 0 ? objs->k_rings : 1;
					int lmax = objs->n_data_workers > 0 ? objs->n_data_workers : 1;
					enum dmesh_pod_init_result init_result =
						(pod->ring_mmap_count == kmax && pod->remote_mmap != NULL &&
						 pod->host_rx_mmap != NULL &&
						 pod->rev_ring_mmap_count == lmax)
							? DMESH_POD_INIT_DPA_FAILED
							: DMESH_POD_INIT_MMAP_FAILED;
					(void)server_publish_pod_init_result(objs, pod, init_result);
				}
			} else if (pod != NULL && pod_data_ready(pod)) {
				(void)server_publish_pod_init_result(objs, pod, DMESH_POD_INIT_READY);
			}
		}
		break;

	case DMESH_MSG_WORKLOAD_ASSERT: {
		const struct dmesh_workload_assert_msg *assertion =
			(const struct dmesh_workload_assert_msg *)recv_buffer;
		if (msg_len != sizeof(*assertion)) {
			DOCA_LOG_ERR("Received invalid WORKLOAD_ASSERT size: %u", msg_len);
			return;
		}
		struct pod_state *pod = find_pod_by_connection(objs, comch_connection);
		if (pod == NULL || !pod->registration_challenge_issued) {
			DOCA_LOG_ERR("WORKLOAD_ASSERT rejected: no connection challenge");
			return;
		}
		if (pod->registration_grant_verified ||
		    __atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE)) {
			DOCA_LOG_ERR("WORKLOAD_ASSERT rejected: duplicate for slot %d",
			             (int)(pod - objs->pods));
			return;
		}

		struct dmesh_assert_claims claims;
		/* The held generation is authoritative for this node's agent key;
		 * the installed keyring is only the bring-up fallback for a DPU
		 * that has not adopted any generation yet. */
		const uint8_t *node_public_key = NULL;
		if (!dmesh_topology_node_key(objs, objs->node_name,
		                             assertion->key_id, &node_public_key))
			node_public_key =
				dmesh_registration_find_key(objs, assertion->key_id);
		enum dmesh_grant_result vr = node_public_key == NULL ?
			DMESH_GRANT_BAD_KEY_ID : dmesh_assert_verify_v2(
				assertion, node_public_key, objs->node_name,
				pod->registration_nonce, (uint64_t)time(NULL),
				&claims);
		if (vr == DMESH_GRANT_OK &&
		    dmesh_registration_consume_grant(objs, assertion->assert_id) != 0) {
			vr = DMESH_GRANT_REPLAY;
			objs->registration_grants_replayed++;
		}
		if (vr != DMESH_GRANT_OK) {
			objs->registration_grants_rejected++;
			l7_control_event("assert", dmesh_grant_result_name(vr));
			DOCA_LOG_ERR("workload_assert result=reject reason=%s slot=%d key_id=%.*s "
			             "assert=%02x%02x%02x%02x rejected=%lu replayed=%lu",
			             dmesh_grant_result_name(vr), (int)(pod - objs->pods),
			             DMESH_GRANT_KEY_ID_MAX, assertion->key_id,
			             assertion->assert_id[0], assertion->assert_id[1],
			             assertion->assert_id[2], assertion->assert_id[3],
			             (unsigned long)objs->registration_grants_rejected,
			             (unsigned long)objs->registration_grants_replayed);
			return;
		}

		memcpy(pod->workload, claims.workload, sizeof(pod->workload));
		memcpy(pod->pod_uid, claims.pod_uid, sizeof(pod->pod_uid));
		memcpy(pod->namespace_name, claims.namespace_name,
		       sizeof(pod->namespace_name));
		memcpy(pod->service_account, claims.service_account,
		       sizeof(pod->service_account));
		memcpy(pod->pod_ip, claims.pod_ip, sizeof(pod->pod_ip));
		memcpy(pod->granted_service, claims.service_name,
		       sizeof(pod->granted_service));
		memcpy(pod->registration_grant_id, assertion->assert_id,
		       sizeof(pod->registration_grant_id));
		pod->registration_grant_verified = 1;
		objs->registration_grants_accepted++;
		l7_control_event("assert", "ok");
		DOCA_LOG_INFO("workload_assert result=accept slot=%d service='%s/%s' key_id=%.*s "
		              "assert=%02x%02x%02x%02x pod_uid=%s pod_ip=%s workload='%s' accepted=%lu",
		              (int)(pod - objs->pods), pod->namespace_name,
		              pod->granted_service[0] ? pod->granted_service : "(none)",
		              DMESH_GRANT_KEY_ID_MAX, assertion->key_id,
		              assertion->assert_id[0], assertion->assert_id[1],
		              assertion->assert_id[2], assertion->assert_id[3],
		              pod->pod_uid, pod->pod_ip, pod->workload,
		              (unsigned long)objs->registration_grants_accepted);
		break;
	}

	case DMESH_MSG_POD_REGISTER: {
		struct dmesh_register_msg *reg = (struct dmesh_register_msg *)recv_buffer;
		if (msg_len != sizeof(struct dmesh_register_msg) ||
		    memchr(reg->service_name, '\0', sizeof(reg->service_name)) == NULL) {
			DOCA_LOG_ERR("Received invalid REGISTER message");
			return;
		}
		int assigned = pods_register(objs, comch_connection, reg->pod_id,
		                             reg->service_name);
		if (assigned >= 0) {
			/* Reply with the assigned pod_id so the host can address itself.
			 * Non-blocking send (no PE re-entry from this callback). */
			struct dmesh_pod_assigned_msg am = { .type = DMESH_MSG_POD_ASSIGNED,
						     .landing_stripes = objs->n_data_workers,
						     .service_id = DMESH_SVC_NONE };
			am.pod_id = assigned;
			struct pod_state *assigned_pod =
				find_pod_by_connection(objs, comch_connection);
			if (assigned_pod != NULL)
				am.service_id = assigned_pod->service_id;
			doca_error_t sr = server_send_msg_to_conn(objs, comch_connection,
			                                          (const char *)&am, sizeof(am));
			if (sr != DOCA_SUCCESS)
				DOCA_LOG_ERR("POD_ASSIGNED send failed (pod_id=%d): %s",
				             assigned, doca_error_get_name(sr));
			/* REGISTER also requests the current terminal registration result. */
			struct pod_state *pod = find_pod_by_connection(objs, comch_connection);
			if (pod != NULL && pod->init_result != DMESH_POD_INIT_PENDING) {
				struct dmesh_pod_init_result_msg im = {
					.type = DMESH_MSG_POD_INIT_RESULT,
					.pod_id = pod->pod_id,
					.result = pod->init_result,
					.landing_stripes = pod->landing_stripes,
				};
				sr = server_send_msg_to_conn(objs, comch_connection,
				                             (const char *)&im, sizeof(im));
				if (sr != DOCA_SUCCESS)
					DOCA_LOG_WARN("POD_INIT_RESULT resend failed (pod_id=%d): %s",
					              pod->pod_id, doca_error_get_name(sr));
			}
		} else {
			struct dmesh_pod_init_result_msg im = {
				.type = DMESH_MSG_POD_INIT_RESULT,
				.pod_id = -1,
				.result = DMESH_POD_INIT_REGISTER_FAILED,
				.landing_stripes = objs->n_data_workers,
			};
			doca_error_t sr = server_send_msg_to_conn(objs, comch_connection,
			                                          (const char *)&im, sizeof(im));
			if (sr != DOCA_SUCCESS)
				DOCA_LOG_ERR("REGISTER failure result send failed: %s",
				             doca_error_get_name(sr));
		}
		break;
	}

	case DMESH_MSG_RESOLVE: {
		const struct dmesh_resolve_msg *req =
			(const struct dmesh_resolve_msg *)recv_buffer;
		if (msg_len != sizeof(*req) || req->version != 1 || req->by_name > 1 ||
		    req->reserved != 0 || req->reserved2 != 0 ||
		    memchr(req->name, '\0', sizeof(req->name)) == NULL) {
			DOCA_LOG_ERR("Received invalid RESOLVE message");
			return;
		}
		struct pod_state *pod = find_pod_by_connection(objs, comch_connection);
		if (pod == NULL) {
			DOCA_LOG_ERR("RESOLVE rejected: unknown connection");
			return;
		}
		struct dmesh_resolve_ack_msg ack;
		memset(&ack, 0, sizeof(ack));
		ack.type = DMESH_MSG_RESOLVE_ACK;
		ack.interned_svc = -1;
		const struct dmesh_gen_service *service = NULL;
		if (objs->topology.tables == NULL) {
			ack.status = 2;                       /* no generation held */
		} else {
			ack.status = 1;                       /* not meshed until found */
			ack.generation_le = objs->topology.tables->version;
			char service_key[DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX];
			if (req->by_name) {
				/* "name" resolves in the requester's own namespace,
				 * "name.namespace" is the DNS convention for a
				 * cross-namespace peer. */
				if (dmesh_resolve_service_key(pod->namespace_name, req->name,
				                              service_key,
				                              sizeof(service_key)) != 0) {
					service_key[0] = '\0';
				}
				if (service_key[0] != '\0')
					service = dmesh_topology_service(objs, service_key);
			} else {
				const uint8_t *ip = (const uint8_t *)&req->ipv4_be;
				const uint8_t *pb = (const uint8_t *)&req->port_be;
				uint32_t want_ip = ((uint32_t)ip[0] << 24) |
				                   ((uint32_t)ip[1] << 16) |
				                   ((uint32_t)ip[2] << 8) | ip[3];
				uint16_t want_port = (uint16_t)(((uint16_t)pb[0] << 8) | pb[1]);
				const struct dmesh_topology_tables *tables =
					objs->topology.tables;
				for (size_t s = 0; s < tables->service_count; s++) {
					if (tables->services[s].cluster_ip_be == want_ip &&
					    tables->services[s].port == want_port) {
						service = &tables->services[s];
						break;
					}
				}
			}
		}
		if (service != NULL && service->interned >= 0) {
			/* The generation names every cluster Service, meshed or not
			 * (the apiserver included). A facade connect() must only be
			 * lifted onto the mesh when the Service can be served there:
			 * that is a live registered backend on this node or an endpoint
			 * the generation places on another node. A name lookup
			 * is an explicit request for the mesh and stays as-is. */
			int served = req->by_name;
			if (!served) {
				int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
				for (int i = 0; i < n && !served; i++)
					served = __atomic_load_n(&objs->pods[i].registered,
					                         __ATOMIC_ACQUIRE) &&
					         objs->pods[i].service_id == service->interned;
				if (!served) {
					struct dmesh_endpoint_ref remote;
					served = dmesh_topology_remote_endpoints(
						objs, service->interned, objs->node_name, &remote, 1) > 0;
				}
			}
			if (served) {
				ack.status = 0;
				ack.interned_svc = service->interned;
				const char *slash = strchr(service->key, '/');
				snprintf(ack.namespace_name, sizeof(ack.namespace_name),
				         "%.*s", (int)(slash - service->key), service->key);
				snprintf(ack.service_name, sizeof(ack.service_name), "%s",
				         slash + 1);
			}
		}
		doca_error_t sr = server_send_msg_to_conn(objs, comch_connection,
		                                          (const char *)&ack,
		                                          sizeof(ack));
		if (sr != DOCA_SUCCESS)
			DOCA_LOG_ERR("RESOLVE_ACK send failed: %s",
			             doca_error_get_name(sr));
		break;
	}

	case DMESH_MSG_POD_UNREGISTER: {
		if (msg_len != sizeof(struct dmesh_pod_unregister_msg)) {
			DOCA_LOG_ERR("Received invalid POD_UNREGISTER message");
			return;
		}
		const struct dmesh_pod_unregister_msg *unreg =
			(const struct dmesh_pod_unregister_msg *)recv_buffer;
		if (pods_unregister_connection(objs, comch_connection,
		                               unreg->pod_id) != 0) {
			DOCA_LOG_ERR("POD_UNREGISTER rejected for pod_id=%d",
			             unreg->pod_id);
		}
		break;
	}

	default:

		DOCA_LOG_ERR("Received unknown message type from client: %u", recv_buffer[0]);
		break;
	}
}

static doca_error_t
server_send_registration_challenge(struct objects *objs, struct pod_state *pod)
{
	if (pod == NULL || pod->connection == NULL)
		return DOCA_ERROR_INVALID_VALUE;
	if (!pod->registration_challenge_issued) {
		if (RAND_bytes(pod->registration_nonce,
		               sizeof(pod->registration_nonce)) != 1) {
			DOCA_LOG_ERR("Failed to create registration challenge nonce");
			return DOCA_ERROR_INITIALIZATION;
		}
		pod->registration_challenge_issued = 1;
	}
	if (pod->registration_challenge_sent)
		return DOCA_SUCCESS;

	struct dmesh_registration_challenge_msg challenge = {
		.type = DMESH_MSG_REG_CHALLENGE,
		.version = DMESH_ASSERT_VERSION,
		.trusted_required = 1,
	};
	memcpy(challenge.nonce, pod->registration_nonce,
	       sizeof(challenge.nonce));
	doca_error_t result = server_send_msg_to_conn(objs, pod->connection,
	                                              (const char *)&challenge,
	                                              sizeof(challenge));
	if (result == DOCA_SUCCESS)
		pod->registration_challenge_sent = 1;
	return result;
}

/* A new Comch connection: track it and issue its registration challenge. */
static void server_connection_event_callback(struct doca_comch_event_connection_status_changed *event,
					     struct doca_comch_connection *comch_connection,
					     uint8_t change_success)
{
	union doca_data user_data;
	struct doca_comch_server *comch_server;
	struct objects *objs;
	doca_error_t result;

	if (change_success == 0) {
		DOCA_LOG_ERR("Failed connection received");
		return;
	}

	(void)event;

	comch_server = doca_comch_server_get_server_ctx(comch_connection);

	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(comch_server), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed to get user data from ctx with error = %s", doca_error_get_name(result));
		return;
	}

	objs = (struct objects *)user_data.ptr;

	/* First connection is the primary */
	if (objs->connection == NULL)
		objs->connection = comch_connection;

	/* Add to pods table. A connection without a slot is kept only long enough
	 * for REGISTER to receive an explicit REGISTER_FAILED response. */
	if (pods_add_connection(objs, comch_connection) != 0)
		DOCA_LOG_ERR("New connection has no available pod slot");
	else {
		struct pod_state *pod = find_pod_by_connection(objs, comch_connection);
		doca_error_t challenge_result =
			server_send_registration_challenge(objs, pod);
		if (challenge_result != DOCA_SUCCESS)
			DOCA_LOG_WARN("Registration challenge send deferred: %s",
			              doca_error_get_name(challenge_result));
	}

}

/* A Comch disconnect: release the connection's pod slot (cleanup may follow). */
static void server_disconnection_event_callback(struct doca_comch_event_connection_status_changed *event,
						struct doca_comch_connection *comch_connection,
						uint8_t change_success)
{
	union doca_data user_data;
	struct doca_comch_server *comch_server;
	struct objects *objs;
	doca_error_t result;

	(void)event;

	if (change_success == 0)
		DOCA_LOG_ERR("Disconnection reported as failed; cleaning up anyway");

	comch_server = doca_comch_server_get_server_ctx(comch_connection);
	result = doca_ctx_get_user_data(doca_comch_server_as_ctx(comch_server), &user_data);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("disconnect: failed to get user data from ctx: %s",
			     doca_error_get_name(result));
		return;
	}
	objs = (struct objects *)user_data.ptr;

	if (objs->connection == comch_connection)
		objs->connection = NULL;

	if (pods_remove_connection(objs, comch_connection) != 0)
		DOCA_LOG_WARN("disconnect: no pod slot matched the disconnected connection");
}

doca_error_t
init_comch_ctrl_path_server(const char *server_name, struct objects *objs)
{
    doca_error_t result;
    struct doca_ctx *ctx;
    union doca_data user_data;
    uint32_t max_msg_size, max_rq_size;
	struct timespec ts = {
		.tv_nsec = SLEEP_IN_NANOS,
	};

	/* Prime task-pool counters before anything that can submit. */
	objects_init_task_pools(objs);

	/* create a progress engine */
    result = doca_pe_create(&(objs->pe));
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed creating pe with error = %s", doca_error_get_name(result));
        return result;
    }
	
    result = doca_comch_server_create(objs->dev, objs->rep_dev,
                server_name, &objs->cc_server);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to create server with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

    ctx = doca_comch_server_as_ctx(objs->cc_server);

    result = doca_pe_connect_ctx(objs->pe, ctx);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed adding pe context to server with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

    result = doca_comch_server_task_send_set_conf(objs->cc_server,
                server_send_task_completion_callback,
                server_send_task_completion_err_callback,
                CC_SEND_TASK_NUM);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed setting send task cbs with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

    result = doca_comch_server_event_msg_recv_register(objs->cc_server, server_message_recv_callback);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed adding message recv event cb with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

    result = doca_comch_server_event_connection_status_changed_register(objs->cc_server,
                                        server_connection_event_callback,
                                        server_disconnection_event_callback);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed adding connection status changed event cbs with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }                                        

    /* Config the data_path related events */
	result = doca_comch_server_event_consumer_register(objs->cc_server,
								dmesh_consumer_connected,
								dmesh_consumer_expired);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("Failed adding consumer event cb with error = %s", doca_error_get_name(result));
		goto setup_failed;
	}

    result = doca_comch_cap_get_max_msg_size(doca_dev_as_devinfo(objs->dev), &max_msg_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get max message size with error = %s", doca_error_get_name(result));
        goto setup_failed;
    } 

    result = doca_comch_cap_get_max_recv_queue_size(doca_dev_as_devinfo(objs->dev), &max_rq_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to get max recv queue size with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }
    
    result = doca_comch_server_set_max_msg_size(objs->cc_server, max_msg_size);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set max message size with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

    {
		uint32_t desired_rq = CC_SERVER_RECV_QUEUE_SIZE;
		if (desired_rq > max_rq_size) desired_rq = max_rq_size;
        result = doca_comch_server_set_recv_queue_size(objs->cc_server, desired_rq);
        if (result != DOCA_SUCCESS) {
            DOCA_LOG_ERR("Failed to set recv queue size (%u) with error = %s",
                         desired_rq, doca_error_get_name(result));
            goto setup_failed;
        }
    }

    user_data.ptr = (void *)objs;
    result = doca_ctx_set_user_data(ctx, user_data);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to set ctx user data with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

    result = doca_ctx_start(ctx);
    if (result != DOCA_SUCCESS) {
        DOCA_LOG_ERR("Failed to start server context with error = %s", doca_error_get_name(result));
        goto setup_failed;
    }

	while (objs->connection == NULL) {
		if (doca_pe_progress(objs->pe) == 0)
			nanosleep(&ts, &ts);
	}

    return DOCA_SUCCESS;

setup_failed:
    /* run_dpu_worker owns `objs` and calls cleanup_objects on every failure.
     * Preserve partially started state for the common stop→IDLE→destroy path. */
    return result;
}

/* ====================================================================
 * Send a raw message to a specific connection (comch control path)
 * ==================================================================== */

doca_error_t
server_send_msg_to_conn(struct objects *objs, struct doca_comch_connection *conn,
                        const char *msg, size_t len)
{
	doca_error_t result;
	struct doca_comch_task_send *task;
	void *msg_copy;
	union doca_data task_user_data;
	struct doca_task *task_obj;

	/* Capacity gate on our mirror of DOCA's send pool. If full, return
	 * DOCA_ERROR_AGAIN so the caller can retain the work and retry later.
	 * No retry loop here: PE re-entry from a callback is unsafe. */
	if (!doca_pool_try_acquire(&objs->send_tasks_in_flight, objs->send_tasks_max))
		return DOCA_ERROR_AGAIN;

	msg_copy = malloc(len);
	if (msg_copy == NULL) {
		DOCA_LOG_ERR("server_send_msg_to_conn: payload copy allocation failed");
		doca_pool_release(&objs->send_tasks_in_flight);
		return DOCA_ERROR_NO_MEMORY;
	}
	memcpy(msg_copy, msg, len);

	result = doca_comch_server_task_send_alloc_init(objs->cc_server, conn,
							msg_copy, len, &task);
	if (result != DOCA_SUCCESS) {
		DOCA_LOG_ERR("server_send_msg_to_conn: alloc failed: %s",
		             doca_error_get_name(result));
		doca_pool_release(&objs->send_tasks_in_flight);
		free(msg_copy);
		return result;
	}

	task_obj = doca_comch_task_send_as_task(task);
	task_user_data.ptr = msg_copy;
	doca_task_set_user_data(task_obj, task_user_data);

	result = doca_task_submit(task_obj);
	if (result != DOCA_SUCCESS) {
		/* Capacity gated ahead of time — should not hit this, but be safe */
		DOCA_LOG_ERR("server_send_msg_to_conn: submit failed: %s",
		             doca_error_get_name(result));
		doca_pool_release(&objs->send_tasks_in_flight);
		free(msg_copy);
		doca_task_free(task_obj);
		return result;
	}

	return DOCA_SUCCESS;
}

static doca_error_t
send_pod_init_result(struct objects *objs, struct pod_state *pod)
{
	if (pod == NULL || pod->connection == NULL ||
	    pod->init_result == DMESH_POD_INIT_PENDING)
		return DOCA_ERROR_INVALID_VALUE;
	if (pod->init_result_sent)
		return DOCA_SUCCESS;

	struct dmesh_pod_init_result_msg msg = {
		.type = DMESH_MSG_POD_INIT_RESULT,
		.pod_id = pod->pod_id,
		.result = pod->init_result,
		.landing_stripes = pod->landing_stripes,
	};
	doca_error_t result = server_send_msg_to_conn(objs, pod->connection,
	                                               (const char *)&msg, sizeof(msg));
	if (result == DOCA_SUCCESS) {
		pod->init_result_sent = 1;
	}
	return result;
}

doca_error_t
server_publish_pod_init_result(struct objects *objs, struct pod_state *pod,
                               enum dmesh_pod_init_result result)
{
	if (pod == NULL || result <= DMESH_POD_INIT_PENDING ||
	    result > DMESH_POD_INIT_DPA_FAILED)
		return DOCA_ERROR_INVALID_VALUE;

	/* A failure is terminal and can never be overwritten by a later setup retry.
	 * READY is likewise immutable once published. */
	if (pod->init_result == DMESH_POD_INIT_PENDING)
		pod->init_result = result;
	else if (pod->init_result != result)
		return DOCA_ERROR_BAD_STATE;

	return send_pod_init_result(objs, pod);
}

int
server_flush_pod_init_results(struct objects *objs)
{
	int submitted = 0;
	struct timespec now;
	uint64_t now_ns = 0;
	if (clock_gettime(CLOCK_MONOTONIC, &now) == 0)
		now_ns = (uint64_t)now.tv_sec * 1000000000ull +
		         (uint64_t)now.tv_nsec;
	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	for (int i = 0; i < n; i++) {
		struct pod_state *pod = &objs->pods[i];
		if (pod->connection != NULL && now_ns != 0 &&
		    dmesh_registration_should_expire(
			    pod->connected_ns, now_ns,
			    __atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE),
			    pod->registration_disconnect_pending,
			    DMESH_REGISTRATION_TIMEOUT_NS)) {
			doca_error_t result = doca_comch_server_disconnect(
				objs->cc_server, pod->connection);
			if (result == DOCA_SUCCESS) {
				pod->registration_disconnect_pending = 1;
				l7_control_event("registration-timeout", "disconnect");
				DOCA_LOG_WARN("disconnecting unauthenticated Comch slot %d "
				              "after 30s", i);
			} else if (result == DOCA_ERROR_AGAIN ||
			           result == DOCA_ERROR_IN_USE) {
				/* Keep pending clear: the next PE iteration retries. */
				l7_control_event("registration-timeout", "retry");
			} else {
				l7_control_event("registration-timeout", "error");
				DOCA_LOG_WARN("unauthenticated Comch disconnect failed: %s",
				              doca_error_get_name(result));
			}
		}
		if (pod->registration_disconnect_pending)
			continue;
		if (pod->connection != NULL && !pod->registration_challenge_sent)
			(void)server_send_registration_challenge(objs, pod);
		if (pod->connection == NULL ||
		    !__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE) ||
		    pod->init_result == DMESH_POD_INIT_PENDING ||
		    pod->init_result_sent)
			continue;
		if (send_pod_init_result(objs, pod) == DOCA_SUCCESS)
			submitted++;
	}
	return submitted;
}

/* ====================================================================
 * Pod connection management
 * ==================================================================== */

int
pods_add_connection(struct objects *objs, struct doca_comch_connection *conn)
{
	/* Reuse an unpublished, disconnected slot when available. The control PE is
	 * the sole writer, and num_pods publishes newly appended slots. */
	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	int idx = -1;
	for (int i = 0; i < n; i++) {
		if (objs->pods[i].connection == NULL &&
		    __atomic_load_n(&objs->pods[i].registered, __ATOMIC_ACQUIRE) == 0 &&
		    !__atomic_load_n(&objs->pods[i].cleanup_pending,
		                     __ATOMIC_ACQUIRE)) {
			idx = i;   /* recycle a freed slot */
			break;
		}
	}
	if (idx < 0) {
		/* LIVE cap = forward-ring capacity for the running config: one EU holds
		 * MAX_DPA_RINGS rings, so N EUs / K rings-per-pod admit MAX_DPA_RINGS*N/K pods
		 * (bounded by the pods[] array MAX_PODS). Raising DPUMESH_DPA_THREADS raises it. */
		int live_cap = MAX_DPA_RINGS * objs->num_dpa_threads / objs->k_rings;
		if (live_cap > MAX_PODS) live_cap = MAX_PODS;
		if (n >= live_cap) {
			DOCA_LOG_ERR("pods_add_connection: table full (live cap %d = %d rings × %d EU / %d K)",
			             live_cap, MAX_DPA_RINGS, objs->num_dpa_threads, objs->k_rings);
			return -1;
		}
		idx = n;
	}

	/* Nothing to release here: pods_remove_connection already unpublished every
	 * host-exported handle on this slot, and a never-used slot is memset to 0.
	 * The DPU-local staging (local_mmap/dma_buffer) is deliberately kept and
	 * REUSED by setup_pod_dma. */

	objs->pods[idx].connection = conn;
	struct timespec connected;
	objs->pods[idx].connected_ns =
		clock_gettime(CLOCK_MONOTONIC, &connected) == 0
			? (uint64_t)connected.tv_sec * 1000000000ull +
			  (uint64_t)connected.tv_nsec
			: 1;
	objs->pods[idx].registration_disconnect_pending = 0;
	objs->pods[idx].pod_id = -1;  /* not yet registered */
	objs->pods[idx].service_id = DMESH_SVC_NONE;
	objs->pods[idx].workload[0] = '\0';   /* the new tenant states its own */
	objs->pods[idx].pod_uid[0] = '\0';
	objs->pods[idx].namespace_name[0] = '\0';
	objs->pods[idx].service_account[0] = '\0';
	objs->pods[idx].pod_ip[0] = '\0';
	objs->pods[idx].granted_service[0] = '\0';
	memset(objs->pods[idx].registration_nonce, 0,
	       sizeof(objs->pods[idx].registration_nonce));
	memset(objs->pods[idx].registration_grant_id, 0,
	       sizeof(objs->pods[idx].registration_grant_id));
	objs->pods[idx].registration_challenge_issued = 0;
	objs->pods[idx].registration_challenge_sent = 0;
	objs->pods[idx].registration_grant_verified = 0;
	objs->pods[idx].registration_grant_consumed = 0;
	objs->pods[idx].membership_generation = 0;
	objs->pods[idx].membership_absences = 0;
	objs->pods[idx].revoked = 0;
	objs->pods[idx].landing_stripes = objs->n_data_workers;
	objs->pods[idx].rev_ring_mmap_count = 0;
	objs->pods[idx].rev_doorbell_pending_epoch = 0;
	objs->pods[idx].rev_doorbell_sent_epoch = 0;
	objs->pods[idx].init_result = DMESH_POD_INIT_PENDING;
	objs->pods[idx].init_result_sent = 0;
	objs->pods[idx].dpa_add_expected_mask = 0;
	objs->pods[idx].dpa_add_ack_mask = 0;
	objs->pods[idx].dpa_add_ack_failed = 0;
	objs->pods[idx].dpa_setup_complete = 0;
	objs->pods[idx].dpa_add_last_send_ns = 0;
	objs->pods[idx].cleanup_pending = 0;
	objs->pods[idx].cleanup_reply_sent = 0;
	objs->pods[idx].dpa_del_expected_mask = 0;
	objs->pods[idx].dpa_del_ack_mask = 0;
	objs->pods[idx].dpa_del_last_send_ns = 0;
	objs->pods[idx].proxy_producers_quiesced_mask = 0;
	objs->pods[idx].egress_quiesced_mask = 0;
	objs->pods[idx].egress_reclaim_fenced_mask = 0;
	for (int w = 0; w < MAX_ARM_WORKERS; w++)
		__atomic_store_n(&objs->pods[idx].egress_inflight_worker[w].v, 0,
		                 __ATOMIC_RELEASE);
	objs->pods[idx].egress_pending_emit = 0;
	objs->pods[idx].proxy_source_refs = 0;
	__atomic_store_n(&objs->pods[idx].registered, 0, __ATOMIC_RELEASE);
	if (idx == n)
		__atomic_store_n(&objs->num_pods, idx + 1, __ATOMIC_RELEASE);

	return 0;
}

static int
pod_has_imported_resources(const struct pod_state *pod)
{
	if (pod->remote_mmap || pod->host_rx_mmap)
		return 1;
	for (int w = 1; w < MAX_ARM_WORKERS; w++)
		if (pod->host_rx_worker_mmaps[w] != NULL)
			return 1;
	for (int j = 0; j < MAX_EU_PER_POD; j++)
		if (pod->buf_arrs[j] || pod->ring_mmaps[j] ||
		    pod->rev_ring_mmaps[j])
			return 1;
	return 0;
}

static void
pod_begin_cleanup(struct objects *objs, struct pod_state *pod)
{
	if (__atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE))
		return;

	/* Stop every producer before asking either DMA engine to quiesce. Keep all
	 * imported handles published in the private slot until both barriers pass. */
	__atomic_store_n(&pod->proxy_producers_quiesced_mask, 0,
	                 __ATOMIC_RELEASE);
	__atomic_store_n(&pod->egress_quiesced_mask, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&pod->egress_reclaim_fenced_mask, 0,
	                 __ATOMIC_RELEASE);
	__atomic_store_n(&pod->dma_ready, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&pod->registered, 0, __ATOMIC_RELEASE);
	if (pod->pod_id >= 0 && pod->pod_id < POD_ID_SPACE)
		__atomic_store_n(&objs->pod_id_to_slot[pod->pod_id], -1,
		                 __ATOMIC_RELEASE);
	pod->cleanup_reply_sent = 0;
	pod->cleanup_stall_report_ns = 0;
	struct timespec started;
	pod->cleanup_started_ns = clock_gettime(CLOCK_MONOTONIC, &started) == 0
		? (uint64_t)started.tv_sec * 1000000000ull + (uint64_t)started.tv_nsec
		: 0;
	__atomic_store_n(&pod->cleanup_pending, 1, __ATOMIC_RELEASE);
#ifdef DOCA_ARCH_DPU
	teardown_pod_dma(objs, pod);
#endif
}

/* Membership is consulted on its own cadence: the control loop runs on a 1 ms
 * backstop and the publisher installs generations far more slowly. */
#define MEMBERSHIP_CHECK_INTERVAL_NS 1000000000ull

/* How long a quiescence may run before it is reported as stalled. */
#define DMESH_CLEANUP_STALL_NS 5000000000ull

int
server_progress_membership(struct objects *objs)
{
	if (objs == NULL || !objs->membership_enabled)
		return 0;

	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
	if (now_ns < objs->membership_next_check_ns)
		return 0;
	objs->membership_next_check_ns = now_ns + MEMBERSHIP_CHECK_INTERVAL_NS;

	enum dmesh_membership_result result = dmesh_membership_refresh(objs);
	if (result == DMESH_MEMBERSHIP_UNCHANGED)
		return 0;
	if (result != DMESH_MEMBERSHIP_ADOPTED) {
		objs->membership_rejected++;
		l7_control_event("membership", dmesh_membership_result_name(result));
		DOCA_LOG_WARN("membership generation rejected: reason=%s generation=%lu rejected=%lu",
		              dmesh_membership_result_name(result),
		              (unsigned long)objs->membership_generation,
		              (unsigned long)objs->membership_rejected);
		return 0;
	}
	l7_control_event("membership", "ok");

	int revoked = 0;
	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	for (int i = 0; i < n; i++) {
		struct pod_state *pod = &objs->pods[i];
		if (!__atomic_load_n(&pod->registered, __ATOMIC_ACQUIRE) ||
		    __atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE) ||
		    pod->revoked || pod->pod_uid[0] == '\0')
			continue;
		/* A generation no newer than the one this registration was last
		 * judged against says nothing new about it. */
		if (objs->membership_generation <= pod->membership_generation)
			continue;
		pod->membership_generation = objs->membership_generation;
		if (dmesh_membership_contains(objs, pod->pod_uid,
		                              pod->granted_service)) {
			pod->membership_absences = 0;
			continue;
		}
		if (++pod->membership_absences < DMESH_MEMBERSHIP_ABSENCES_TO_REVOKE) {
			DOCA_LOG_INFO("membership absence %u for slot %d pod_id=%d pod_uid=%s",
			              pod->membership_absences, i, pod->pod_id, pod->pod_uid);
			continue;
		}
		pod->revoked = 1;
		objs->membership_revocations++;
		l7_control_event("revocation", "membership-withdrawn");
		DOCA_LOG_WARN("registration revoked: slot=%d pod_id=%d service_id=%d "
		              "pod_uid=%s generation=%lu revocations=%lu",
		              i, pod->pod_id, pod->service_id, pod->pod_uid,
		              (unsigned long)objs->membership_generation,
		              (unsigned long)objs->membership_revocations);
		pod_begin_cleanup(objs, pod);
		revoked++;
	}
	return revoked;
}

/* `drain` refuses new protected sessions; anything else opens admission. The
 * switch is a file so it can be set without restarting the proxy. */
int
server_progress_admission(struct objects *objs)
{
	if (objs == NULL || !objs->admission_enabled)
		return 0;

	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return 0;
	uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
	if (now_ns < objs->admission_next_check_ns)
		return 0;
	objs->admission_next_check_ns = now_ns + MEMBERSHIP_CHECK_INTERVAL_NS;

	char state[32] = {0};
	int drain = 0;
	FILE *file = fopen(objs->admission_path, "r");
	if (file != NULL) {
		if (fgets(state, sizeof(state), file) != NULL) {
			state[strcspn(state, "\r\n")] = '\0';
			drain = strcmp(state, "drain") == 0;
		}
		fclose(file);
	}
	/* An unreadable switch means open: a lost file must not stop admission. */
	if (drain == __atomic_load_n(&objs->admission_drain, __ATOMIC_RELAXED))
		return 0;
	__atomic_store_n(&objs->admission_drain, drain, __ATOMIC_RELAXED);
	l7_control_event("admission", drain ? "drain" : "open");
	DOCA_LOG_WARN("protected admission %s (refusals=%lu)",
	              drain ? "draining" : "open",
	              (unsigned long)objs->admission_drain_refusals);
	return 1;
}

int
pods_unregister_connection(struct objects *objs,
                           struct doca_comch_connection *conn,
                           int32_t pod_id)
{
	struct pod_state *pod = find_pod_by_connection(objs, conn);
	if (pod == NULL || pod->pod_id != pod_id)
		return -1;
	pod_begin_cleanup(objs, pod);
	return 0;
}

int
pods_remove_connection(struct objects *objs, struct doca_comch_connection *conn)
{
	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	for (int i = 0; i < n; i++) {
		struct pod_state *pod = &objs->pods[i];
		if (pod->connection != conn)
			continue;
		/* The inbound policy watches this Pod's registration opened end
		 * with it: their lifetime is the registration's, not the
		 * process's, so a Pod that leaves stops costing a watch. */
		if (pod->workload[0] != '\0')
			l7_inbound_forget(0, pod->workload);
		int32_t pod_id = pod->pod_id;
		if (pod_id >= 0 &&
		    (pod_has_imported_resources(pod) || pod->k_rings > 0) &&
		    !__atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE))
			pod_begin_cleanup(objs, pod);
		else {
			__atomic_store_n(&pod->dma_ready, 0, __ATOMIC_RELEASE);
			__atomic_store_n(&pod->registered, 0, __ATOMIC_RELEASE);
			if (pod_id >= 0 && pod_id < POD_ID_SPACE)
				__atomic_store_n(&objs->pod_id_to_slot[pod_id], -1,
				                 __ATOMIC_RELEASE);
		}
		pod->connection = NULL;
		pod->connected_ns = 0;
		pod->registration_disconnect_pending = 0;
		if (!__atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE))
			pod->pod_id = -1;
		return 0;
	}
	return -1;
}

#ifdef DOCA_ARCH_DPU
static int
pod_destroy_imported_resources(struct pod_state *pod)
{
	/* DEL_ACK fenced the DPA handles; ARM quiescence fenced host-RX and credit
	 * reads. Destroy buf_arrs before the mmaps they were created over. */
	for (int j = 0; j < MAX_EU_PER_POD; j++) {
		if (pod->buf_arrs[j] != NULL) {
			doca_error_t result = doca_buf_arr_destroy(pod->buf_arrs[j]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("pod %d: buf_arr[%d] reclaim failed: %s",
				             pod->pod_id, j, doca_error_get_name(result));
				return 0;
			}
			pod->buf_arrs[j] = NULL;
		}
	}
	for (int j = 0; j < MAX_EU_PER_POD; j++) {
		if (pod->ring_mmaps[j] != NULL) {
			doca_error_t result = doca_mmap_destroy(pod->ring_mmaps[j]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("pod %d: ring mmap[%d] reclaim failed: %s",
				             pod->pod_id, j, doca_error_get_name(result));
				return 0;
			}
			pod->ring_mmaps[j] = NULL;
			pod->ring_host_addrs[j] = NULL;
		}
	}
	for (int j = 0; j < MAX_EU_PER_POD; j++) {
		if (pod->rev_ring_mmaps[j] != NULL) {
			doca_error_t result = doca_mmap_destroy(pod->rev_ring_mmaps[j]);
			if (result != DOCA_SUCCESS) {
				DOCA_LOG_ERR("pod %d: reverse ring mmap[%d] reclaim failed: %s",
				             pod->pod_id, j, doca_error_get_name(result));
				return 0;
			}
			pod->rev_ring_mmaps[j] = NULL;
			pod->rev_ring_host_addrs[j] = NULL;
		}
	}
	if (pod->remote_mmap != NULL) {
		doca_error_t result = doca_mmap_destroy(pod->remote_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("pod %d: TX mmap reclaim failed: %s",
			             pod->pod_id, doca_error_get_name(result));
			return 0;
		}
		pod->remote_mmap = NULL;
	}
	for (int w = 1; w < MAX_ARM_WORKERS; w++) {
		if (pod->host_rx_worker_mmaps[w] == NULL)
			continue;
		doca_error_t result = doca_mmap_destroy(pod->host_rx_worker_mmaps[w]);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("pod %d: worker %d RX mmap reclaim failed: %s",
			             pod->pod_id, w, doca_error_get_name(result));
			return 0;
		}
		pod->host_rx_worker_mmaps[w] = NULL;
	}
	if (pod->host_rx_mmap != NULL) {
		doca_error_t result = doca_mmap_destroy(pod->host_rx_mmap);
		if (result != DOCA_SUCCESS) {
			DOCA_LOG_ERR("pod %d: worker 0 RX mmap reclaim failed: %s",
			             pod->pod_id, doca_error_get_name(result));
			return 0;
		}
		pod->host_rx_mmap = NULL;
	}
	return 1;
}
#endif

#ifdef DOCA_ARCH_DPU
/* A quiescence that cannot finish holds its slot and its imported mappings.
 * Name the gate holding it, and keep naming it: a stall that outlives one
 * report is the case worth watching, and a single line cannot show whether the
 * gate moved. */
static void
pod_report_cleanup_stall(struct objects *objs, struct pod_state *pod,
                         int dma_done, int reclaimed)
{
	struct timespec now;
	if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
		return;
	uint64_t now_ns = (uint64_t)now.tv_sec * 1000000000ull + (uint64_t)now.tv_nsec;
	if (pod->cleanup_started_ns == 0 ||
	    now_ns - pod->cleanup_started_ns < DMESH_CLEANUP_STALL_NS)
		return;
	if (pod->cleanup_stall_report_ns != 0 &&
	    now_ns - pod->cleanup_stall_report_ns < DMESH_CLEANUP_STALL_NS)
		return;
	pod->cleanup_stall_report_ns = now_ns;
	uint32_t expected = __atomic_load_n(&pod->dpa_del_expected_mask, __ATOMIC_ACQUIRE);
	DOCA_LOG_WARN("pod cleanup stalled: pod_id=%d gate=%s connection=%s "
	              "k_rings=%d del_expected=0x%x del_acked=0x%x",
	              pod->pod_id,
	              !dma_done ? "dpa-ring-del" :
	              !reclaimed ? "proxy-reclaim" : "mapping-destroy",
	              pod->connection != NULL ? "live" : "gone", pod->k_rings,
	              expected,
	              __atomic_load_n(&pod->dpa_del_ack_mask, __ATOMIC_ACQUIRE));
	/* The DPA drops a ring ACK when the channel has no posted receive, and
	 * receives are withheld while a worker's completion queue is above its
	 * backpressure mark. */
	for (int w = 0; w < objs->n_data_workers; w++)
		DOCA_LOG_WARN("  worker %d: comp_queue=%u deferred_recv=%d", w,
		              comp_queue_usage(&objs->data_workers[w].queue),
		              objs->data_workers[w].num_deferred_recv);
	for (int eu = 0; eu < objs->num_dpa_threads && eu < MAX_DPA_EU; eu++)
		if ((expected & (1u << eu)) && objs->dpa_comches[eu] != NULL)
			DOCA_LOG_WARN("  EU %d: recv_posted=%d", eu,
			              __atomic_load_n(&objs->dpa_comches[eu]->recv.recv_posted,
			                              __ATOMIC_RELAXED));
}
#endif

int
server_progress_pod_cleanup(struct objects *objs)
{
#ifndef DOCA_ARCH_DPU
	(void)objs;
	return 0;
#else
	int progressed = 0;
	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	for (int i = 0; i < n; i++) {
		struct pod_state *pod = &objs->pods[i];
		if (!__atomic_load_n(&pod->cleanup_pending, __ATOMIC_ACQUIRE))
			continue;
		int dma_done = progress_teardown_pod_dma(objs, pod);
		int reclaimed = dma_done && px_pod_reclaim_ready(objs, i);
		int destroyed = reclaimed && pod_destroy_imported_resources(pod);
		if (!destroyed) {
			pod_report_cleanup_stall(objs, pod, dma_done, reclaimed);
			continue;
		}

		pod->ring_mmap_count = 0;
		pod->rev_ring_mmap_count = 0;
		pod->rev_doorbell_pending_epoch = 0;
		pod->rev_doorbell_sent_epoch = 0;
		pod->remote_addr = NULL;
		pod->remote_buf_size = 0;
		pod->host_rx_addr = NULL;
		pod->host_rx_buf_size = 0;
		pod->rq_depth = 0;
		pod->k_rings = 0;
		pod->landing_stripes = 0;
		pod->init_result = DMESH_POD_INIT_PENDING;
		pod->init_result_sent = 0;
		pod->dpa_add_expected_mask = 0;
		pod->dpa_add_ack_mask = 0;
		pod->dpa_add_ack_failed = 0;
		pod->dpa_setup_complete = 0;
		pod->dpa_add_last_send_ns = 0;

		if (pod->connection != NULL && !pod->cleanup_reply_sent) {
			struct dmesh_pod_quiesced_msg msg = {
				.type = DMESH_MSG_POD_QUIESCED,
				.pod_id = pod->pod_id,
			};
			doca_error_t result = server_send_msg_to_conn(
				objs, pod->connection, (const char *)&msg, sizeof(msg));
			if (result != DOCA_SUCCESS)
				continue; /* send-pool backpressure: retain and retry */
			pod->cleanup_reply_sent = 1;
		}

		__atomic_store_n(&pod->cleanup_pending, 0, __ATOMIC_RELEASE);
		if (pod->connection == NULL)
			pod->pod_id = -1;
		progressed++;
	}
	return progressed;
#endif
}

int
pods_register(struct objects *objs, struct doca_comch_connection *conn,
              int32_t pod_id, const char *service_name)
{
	if (pod_id < -1 || pod_id >= POD_ID_SPACE || service_name == NULL ||
	    strlen(service_name) >= DMESH_SVC_NAME_MAX) {
		DOCA_LOG_ERR("pods_register: invalid pod_id=%d name='%s'",
		             pod_id, service_name == NULL ? "(null)" : service_name);
		return -1;
	}

	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	for (int i = 0; i < n; i++) {
		if (objs->pods[i].connection != conn)
			continue;
		if (__atomic_load_n(&objs->pods[i].cleanup_pending, __ATOMIC_ACQUIRE)) {
			DOCA_LOG_WARN("pods_register: slot %d is still quiescing", i);
			return -1;
		}
		if (__atomic_load_n(&objs->pods[i].registered, __ATOMIC_ACQUIRE)) {
			/* Idempotent replay: one Comch connection owns one registration.
			 * The host uses this to recover a lost ASSIGNED/INIT_RESULT reply. */
			int32_t current_id = objs->pods[i].pod_id;
			if ((pod_id < 0 || pod_id == current_id) &&
			    strcmp(service_name, objs->pods[i].granted_service) == 0)
				return current_id;
			DOCA_LOG_ERR("pods_register: conflicting replay on slot %d", i);
			return -1;
		}
		/* The Service name is authoritative for identity: it must equal the
		 * one the connection's assertion named. */
		if (!objs->pods[i].registration_grant_verified ||
		    objs->pods[i].registration_grant_consumed ||
		    strcmp(service_name, objs->pods[i].granted_service) != 0) {
			DOCA_LOG_ERR("pods_register: trusted assertion missing/consumed or Service mismatch "
			             "on slot %d (requested='%s' asserted='%s')",
			             i, service_name, objs->pods[i].granted_service);
			return -1;
		}
		/* The node-local compact id is the DPU's own: interned from the
		 * held generation. Serving an identity requires the generation
		 * that defines it, so a Service the generation does not intern
		 * fails closed; a client-only registration needs no id. */
		int32_t service_id = DMESH_SVC_NONE;
		if (service_name[0] != '\0') {
			char service_key[DMESH_K8S_NAMESPACE_MAX + DMESH_SVC_NAME_MAX];
			snprintf(service_key, sizeof(service_key), "%.*s/%.*s",
			         (int)sizeof(objs->pods[i].namespace_name) - 1,
			         objs->pods[i].namespace_name,
			         (int)DMESH_SVC_NAME_MAX - 1, service_name);
			service_id = dmesh_topology_interned_id(objs, service_key);
			if (service_id < 0) {
				DOCA_LOG_ERR("pods_register: no interned id for %s "
				             "(no generation defines it): fails closed",
				             service_key);
				return -1;
			}
		}

			/* Negative pod_id requests a live pods[] slot index. */
		if (pod_id < 0)
			pod_id = i;
		if (__atomic_load_n(&objs->pod_id_to_slot[pod_id], __ATOMIC_ACQUIRE) >= 0) {
			DOCA_LOG_ERR("pods_register: pod_id=%d is already live", pod_id);
			return -1;
		}

		/* Publication order: write all fields first, then the gate.
		 * Readers that observe registered=1 (ACQUIRE load) are guaranteed
		 * to see the prior pod_id write. */
		objs->pods[i].pod_id = pod_id;
		objs->pods[i].service_id = service_id;
		objs->pods[i].registration_grant_consumed = 1;
		/* Judge this registration only against generations newer than the one
		 * the node published before it existed. */
		objs->pods[i].membership_generation = objs->membership_generation;
		objs->pods[i].membership_absences = 0;
		/* This timestamp belongs only to the unauthenticated handshake.  Clear
		 * it before publishing registration so a later UNREGISTER/cleanup (which
		 * clears registered) cannot be mistaken for a 30-second auth timeout. */
		objs->pods[i].connected_ns = 0;
		__atomic_store_n(&objs->pods[i].registered, 1, __ATOMIC_RELEASE);

		/* Publish the O(1) pod_id->slot map after registered=1, so a reader
		 * that observes the map entry also sees registered=1. find_pod_by_id
		 * re-validates registered + pod_id regardless. */
		if (pod_id >= 0 && pod_id < POD_ID_SPACE)
			__atomic_store_n(&objs->pod_id_to_slot[pod_id], i, __ATOMIC_RELEASE);

		/* The load balancer derives live backends from published pod fields. */

		/* Which interaction rules this registration carries. Every
		 * registration is assertion-verified either way; what the
		 * generation grades is the rules, and it is graded here so the
		 * decision is never taken from Pod input. */
		/* The mediated lookup, asked once here on the control thread. A
		 * data worker never asks: it reads the answer. */
		objs->pods[i].scope_state =
			(int8_t)dmesh_scope_query(objs, objs->pods[i].pod_uid);
		int protection = dmesh_topology_service_protection(objs, (int16_t)service_id);
		l7_control_event("registration",
		                 protection > 0 ? "protected"
		                                : (protection == 0 ? "unprotected" : "ungraded"));
		DOCA_LOG_INFO("pods_register: pod_id=%d service_id=%d protection=%s",
		              pod_id, service_id,
		              protection > 0 ? "protected"
		                             : (protection == 0 ? "unprotected" : "ungraded"));

		return pod_id;
	}
	DOCA_LOG_ERR("pods_register: connection not found");
	return -1;
}

struct pod_state *
find_pod_by_id(struct objects *objs, int32_t pod_id)
{
	/* Lock-free O(1) lookup via the pod_id->slot map. The map is only an
	 * accelerator: the registered ACQUIRE + pod_id re-check below remain the
	 * authority, so a stale/torn map entry can only yield a re-validated hit
	 * or NULL — never a wrong pod. See object.h pods[] concurrency model. */
	if (pod_id < 0 || pod_id >= POD_ID_SPACE)
		return NULL;
	int idx = __atomic_load_n(&objs->pod_id_to_slot[pod_id], __ATOMIC_ACQUIRE);
	if (idx < 0 || idx >= MAX_PODS)
		return NULL;
	struct pod_state *p = &objs->pods[idx];
	if (__atomic_load_n(&p->registered, __ATOMIC_ACQUIRE) && p->pod_id == pod_id)
		return p;
	return NULL;
}

struct pod_state *
find_pod_by_connection(struct objects *objs, struct doca_comch_connection *conn)
{
	/* Lock-free read keyed on the connection field (not the registered gate),
	 * used both pre-register and post-register. The connection pointer is
	 * NULL-ed in pods_remove_connection AFTER the registered=0 RELEASE store,
	 * so a non-NULL connection always corresponds to a valid slot. */
	int n = __atomic_load_n(&objs->num_pods, __ATOMIC_ACQUIRE);
	for (int i = 0; i < n; i++) {
		if (objs->pods[i].connection == conn)
			return &objs->pods[i];
	}
	return NULL;
}
