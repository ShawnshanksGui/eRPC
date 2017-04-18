/**
 * @file rpc_connect_handlers.cc
 * @brief Handlers for session management connect requests and responses.
 */
#include "rpc.h"
#include <algorithm>

namespace ERpc {

// We need to handle all types of errors in remote arguments that the client can
// make when calling create_session(), which cannot check for such errors.
template <class TTr>
void Rpc<TTr>::handle_connect_req_st(typename Nexus<TTr>::SmWorkItem *wi) {
  assert(in_creator());
  assert(wi != nullptr && wi->peer != nullptr);

  SessionMgmtPkt *sm_pkt = wi->sm_pkt;
  assert(sm_pkt != nullptr &&
         sm_pkt->pkt_type == SessionMgmtPktType::kConnectReq);

  // Ensure that server fields known by the client were filled correctly
  assert(strcmp(sm_pkt->server.hostname, nexus->hostname.c_str()) == 0);
  assert(sm_pkt->server.rpc_id == rpc_id);
  assert(sm_pkt->server.secret == sm_pkt->client.secret);

  // Create the basic issue message
  char issue_msg[kMaxIssueMsgLen];
  sprintf(issue_msg, "eRPC Rpc %u: Received connect request from %s. Issue",
          rpc_id, sm_pkt->client.name().c_str());

  // Check that the transport matches
  Transport::TransportType pkt_tr_type = sm_pkt->server.transport_type;
  if (pkt_tr_type != transport->transport_type) {
    erpc_dprintf("%s: Invalid transport %s. Sending response.\n", issue_msg,
                 Transport::get_transport_name(pkt_tr_type).c_str());
    enqueue_sm_resp(wi, SessionMgmtErrType::kInvalidTransport);
    return;
  }

  // Check if the requested physical port is correct
  if (sm_pkt->server.phy_port != phy_port) {
    erpc_dprintf("%s: Invalid server port %u. Sending response.\n", issue_msg,
                 sm_pkt->server.phy_port);
    enqueue_sm_resp(wi, SessionMgmtErrType::kInvalidRemotePort);
    return;
  }

  // Ensure that we don't have a session as the serrver with this client
  for (Session *old_ssn : session_vec) {
    if (old_ssn != nullptr) {
      assert(strcmp(old_ssn->client.hostname, sm_pkt->client.hostname) != 0 ||
             (old_ssn->client.rpc_id != sm_pkt->client.rpc_id));
    }
  }

  // Check if we are allowed to create another session
  if (session_vec.size() == kMaxSessionsPerThread) {
    erpc_dprintf("%s: Reached session limit %zu. Sending response.\n",
                 issue_msg, kMaxSessionsPerThread);
    enqueue_sm_resp(wi, SessionMgmtErrType::kTooManySessions);
    return;
  }

  // Try to resolve the client's routing info into the packet. If session
  // creation succeeds, we'll copy it to the server's session endpoint.
  Transport::RoutingInfo *client_rinfo = &(sm_pkt->client.routing_info);
  erpc_dprintf("eRPC Rpc %u: Resolving client's routing info %s.\n", rpc_id,
               TTr::routing_info_str(client_rinfo).c_str());

  bool resolve_success = transport->resolve_remote_routing_info(client_rinfo);
  if (!resolve_success) {
    erpc_dprintf("%s: Unable to resolve routing info %s. Sending response.\n",
                 issue_msg, TTr::routing_info_str(client_rinfo).c_str());
    enqueue_sm_resp(wi, SessionMgmtErrType::kRoutingResolutionFailure);
    return;
  }

  // If we are here, create a new session and fill preallocated MsgBuffers.
  // XXX: Use pool?
  Session *session =
      new Session(Session::Role::kServer, SessionState::kConnected);
  for (size_t i = 0; i < Session::kSessionReqWindow; i++) {
    MsgBuffer &msgbuf_i = session->sslot_arr[i].pre_resp_msgbuf;
    msgbuf_i = alloc_msg_buffer(TTr::kMaxDataPerPkt);

    if (msgbuf_i.buf == nullptr) {
      // We haven't assigned a session number or allocated non-prealloc
      // MsgBuffers yet, so just free prealloc MsgBuffers 0 -- (i - 1).
      for (size_t j = 0; j < i; j++) {
        MsgBuffer &msgbuf_j = session->sslot_arr[j].pre_resp_msgbuf;
        assert(msgbuf_j.buf != nullptr);
        free_msg_buffer(msgbuf_j);
      }

      erpc_dprintf("%s: Failed to allocate prealloc MsgBuffer.\n", issue_msg);
      enqueue_sm_resp(wi, SessionMgmtErrType::kOutOfMemory);
      return;
    }
  }

  // Set the server endpoint metadata fields in the received packet, which we
  // will then send back to the client.
  sm_pkt->server.session_num = session_vec.size();
  transport->fill_local_routing_info(&(sm_pkt->server.routing_info));

  // Save endpoint metadata from pkt. This saves the resolved routing info.
  session->server = sm_pkt->server;
  session->client = sm_pkt->client;

  session->local_session_num = sm_pkt->server.session_num;
  session->remote_session_num = sm_pkt->client.session_num;

  session_vec.push_back(session);  // Add to list of all sessions

  erpc_dprintf("%s: None. Sending response.\n", issue_msg);
  enqueue_sm_resp(wi, SessionMgmtErrType::kNoError);
  return;
}

template <class TTr>
void Rpc<TTr>::handle_connect_resp_st(SessionMgmtPkt *sm_pkt) {
  assert(in_creator());
  assert(sm_pkt != nullptr);
  assert(sm_pkt->pkt_type == SessionMgmtPktType::kConnectResp);
  assert(session_mgmt_err_type_is_valid(sm_pkt->err_type));

  // Create the basic issue message using only the packet
  char issue_msg[kMaxIssueMsgLen];
  sprintf(issue_msg,
          "eRPC Rpc %u: Received connect response from %s for session %u. "
          "Issue",
          rpc_id, sm_pkt->server.name().c_str(), sm_pkt->client.session_num);

  // Try to locate the requester session and do some sanity checks
  uint16_t session_num = sm_pkt->client.session_num;
  assert(session_num < session_vec.size());

  Session *session = session_vec[session_num];
  assert(session != nullptr);
  assert(session->is_client());
  assert(session->state == SessionState::kConnectInProgress);
  assert(session->client_info.sm_request_pending);
  assert(session->client == sm_pkt->client);

  // We don't have the server's session number locally yet, so we cannot use
  // SessionEndpoint comparator to compare server endpoint metadata.
  assert(strcmp(session->server.hostname, sm_pkt->server.hostname) == 0);
  assert(session->server.rpc_id == sm_pkt->server.rpc_id);
  assert(session->server.session_num == kInvalidSessionNum);

  session->client_info.sm_request_pending = false;

  // If the connect response has an error, the server has not allocated a
  // Session. Mark the session as disconnected and invoke callback.
  if (sm_pkt->err_type != SessionMgmtErrType::kNoError) {
    erpc_dprintf("%s: Error %s.\n", issue_msg,
                 session_mgmt_err_type_str(sm_pkt->err_type).c_str());

    session->state = SessionState::kDisconnected;
    session_mgmt_handler(session->local_session_num,
                         SessionMgmtEventType::kConnectFailed, sm_pkt->err_type,
                         context);
    bury_session_st(session);
    return;
  }

  // If we are here, the server has created a session endpoint.
  //
  // Try to resolve the server's routing information into the packet. If this
  // fails, invoke kConnectFailed callback.
  Transport::RoutingInfo *srv_routing_info = &(sm_pkt->server.routing_info);
  erpc_dprintf("eRPC Rpc %u: Resolving server's routing info %s.\n", rpc_id,
               TTr::routing_info_str(srv_routing_info).c_str());

  bool resolve_success;
  if (!testing_fail_resolve_remote_rinfo_client) {
    resolve_success = transport->resolve_remote_routing_info(srv_routing_info);
  } else {
    resolve_success = false;  // Inject error for testing
  }

  if (!resolve_success) {
    erpc_dprintf("%s: Client failed to resolve server routing info.\n",
                 issue_msg);

    // The server has allocated a Session, so try to free server resources by
    // disconnecting. The user will only get the kConnectFailed callback, i.e.,
    // no callback will be invoked when we get the disconnect response.
    session->client_info.sm_callbacks_disabled = true;

    // Save server metadata for when we receieve the disconnect response
    session->server = sm_pkt->server;

    // Do what destroy_session() does with a kConnected session
    session->state = SessionState::kDisconnectInProgress;

    erpc_dprintf(
        "eRPC Rpc %u: Sending callback-less disconnect request for "
        "session %u, and invoking kConnectFailed callback\n",
        rpc_id, session->local_session_num);

    session->client_info.sm_request_pending = true;
    enqueue_sm_req(session, SessionMgmtPktType::kDisconnectReq);

    session_mgmt_handler(
        session->local_session_num, SessionMgmtEventType::kConnectFailed,
        SessionMgmtErrType::kRoutingResolutionFailure, context);

    return;
  }

  // Save server endpoint metadata. This saves the resolved routing info.
  session->server = sm_pkt->server;
  session->remote_session_num = session->server.session_num;
  session->state = SessionState::kConnected;

  erpc_dprintf("%s: None. Session connected.\n", issue_msg);
  session_mgmt_handler(session->local_session_num,
                       SessionMgmtEventType::kConnected,
                       SessionMgmtErrType::kNoError, context);
}

}  // End ERpc
