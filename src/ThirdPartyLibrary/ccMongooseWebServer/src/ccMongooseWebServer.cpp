/*
 * ccMongooseWebServer.cpp
 *
 *  Created on: 2015. 11. 8.
 *      Author: kmansoo
 */

#include <functional>

#include "ccCore/ccString.h"

#include "ccMongooseWebServer.h"
#include "ccMongooseWebServerRequest.h"
#include "ccMongooseWebServerResponse.h"
#include "ccMongooseWebsocket.h"

namespace Luna {

static struct mg_serve_http_opts s_http_server_opts;

ccMongooseWebServer::ccMongooseWebServer(const std::string& name, const std::string& ports, const std::string& root_path, std::shared_ptr<ccWebServerPageDirectory> page_directory)
    : ccWebServer(name, ports, root_path, page_directory)
    , is_stop_thread_(false)
    , mgr_(NULL)
    , con_(NULL)
{
}

ccMongooseWebServer::~ccMongooseWebServer()
{
  stop();

  if (mgr_ != NULL)
    delete mgr_;

  mgr_ = NULL;
  con_ = NULL;
}

void ccMongooseWebServer::set_upload_event(const std::string& path)
{
  if (con_ == NULL)
    return;

  mg_register_http_endpoint(con_, path.c_str(), ev_handler_upload);
}

bool ccMongooseWebServer::start()
{
  if (mgr_ != NULL)
    return false;

  mgr_ = new struct mg_mgr;

  mg_mgr_init(mgr_, NULL);

  mgr_->user_data = this;
  con_ = mg_bind(mgr_, httpPorts_.c_str(), ev_handler);

  // Set up HTTP server parameters
  mg_set_protocol_http_websocket(con_);

  s_http_server_opts.document_root = webRootPath_.c_str(); // Serve current directory
  s_http_server_opts.enable_directory_listing = "yes";

  printf("Starting web server on port %s\n", httpPorts_.c_str());

  is_stop_thread_ = false;

  poll_thread_ = new std::thread(std::bind(&ccMongooseWebServer::do_run_thread, this));

  init_server();

  return true;
}

bool ccMongooseWebServer::stop()
{
  if (poll_thread_) {
    is_stop_thread_ = true;

    poll_thread_->join();

    delete poll_thread_;
  }

  if (mgr_ == NULL)
    return false;

  mg_mgr_free(mgr_);

  delete mgr_;

  mgr_ = NULL;
  con_ = NULL;

  return true;
}

void ccMongooseWebServer::do_run_thread()
{
  while (is_stop_thread_ == false)
    mg_mgr_poll(mgr_, 200);
}

void ccMongooseWebServer::ev_handler(struct mg_connection* nc, int ev, void* p)
{
  bool bIsBuiltinProcess = true;

  ccMongooseWebServer* pServer = (ccMongooseWebServer*)nc->mgr->user_data;

  /* HTTP and websocket events. void *ev_data is described in a comment. */
  switch (ev) {
  case MG_EV_HTTP_REQUEST: /* struct http_message * */

    if (pServer->eventListener_ != NULL) {
      auto pRequest = std::make_shared<ccMongooseWebServerRequest>(nc, (http_message*)p);
      auto pResponse = std::make_shared<ccMongooseWebServerResponse>(nc);

      bIsBuiltinProcess = !pServer->eventListener_->on_web_request(pRequest, pResponse);
    }

    if (bIsBuiltinProcess)
      mg_serve_http(nc, (struct http_message*)p, s_http_server_opts);
    break;

  case MG_EV_HTTP_REPLY: /* struct http_message * */
    break;

  case MG_EV_HTTP_CHUNK: /* struct http_message * */
    break;

  case MG_EV_SSI_CALL: /* char * */
    break;

  case MG_EV_WEBSOCKET_HANDSHAKE_REQUEST: { /* NULL */
    struct http_message* hm = (struct http_message*)p;

    std::string request_uri(hm->uri.p, hm->uri.len);

    if (pServer->eventListener_->on_new_websocket_request(request_uri) == true) {
      auto new_websocket = std::make_shared<ccMongooseWebsocket>(request_uri, nc);

      pServer->eventListener_->on_websocket_created(new_websocket);
    } else
      nc->flags |= MG_F_CLOSE_IMMEDIATELY;
  } break;

  case MG_EV_WEBSOCKET_HANDSHAKE_DONE: /* NULL */
    pServer->eventListener_->on_websocket_connected(nc->sock);
    break;

  case MG_EV_WEBSOCKET_FRAME: { /* struct websocket_message * */
    struct websocket_message* wm = (struct websocket_message*)p;

    pServer->eventListener_->on_websocket_received_data(nc->sock, (const char*)wm->data, wm->size, (wm->flags & WEBSOCKET_OP_TEXT) == WEBSOCKET_OP_TEXT);
  } break;

  case MG_EV_WEBSOCKET_CONTROL_FRAME: /* struct websocket_message * */
    break;

  case MG_EV_CLOSE:
    //  is websocket ?
    if (nc->flags & MG_F_IS_WEBSOCKET) {
      int sock = nc->sock;

      if (sock == -1)
        sock = pServer->eventListener_->on_websocket_check_instance(nc);

      pServer->eventListener_->on_websocket_disconnected(sock);
    }
    break;
  }
}

void ccMongooseWebServer::ev_handler_upload(struct mg_connection* nc, int ev, void* p)
{
  ccMongooseWebServer* pServer = (ccMongooseWebServer*)nc->mgr->user_data;

  struct mg_http_multipart_part* mp = (struct mg_http_multipart_part*)p;

  switch (ev) {
  case MG_EV_HTTP_MULTIPART_REQUEST: {
    http_message* hm = (http_message*)p;

    pServer->last_multipart_request_uri_.assign(hm->uri.p, hm->uri.len);
    break;
  }

  case MG_EV_HTTP_PART_BEGIN: {
    std::shared_ptr<ccWebServerFileUploadPage> page = pServer->find_file_upload_page(pServer->last_multipart_request_uri_);

    ccWebServerFileUploadAgent* agent = NULL;

    if (page != NULL)
      agent = page->create_file_upload_agent();

    if (agent == NULL) {
      mg_printf(nc,
          "HTTP/1.1 403 Forbidden\r\n"
          "Content-Type: text/plain\r\n"
          "Connection: close\r\n\r\n");

      nc->flags |= MG_F_SEND_AND_CLOSE;
      break;
    }

    mp->user_data = (void*)agent;
    agent->on_file_info(mp->file_name, mp->data.len);
    break;
  }

  case MG_EV_HTTP_PART_DATA: {
    ccWebServerFileUploadAgent* agent = (ccWebServerFileUploadAgent*)mp->user_data;

    if (agent)
      agent->on_receive_data(mp->data.p, mp->data.len);
    break;
  }

  case MG_EV_HTTP_PART_END: {
    ccWebServerFileUploadAgent* agent = (ccWebServerFileUploadAgent*)mp->user_data;

    if (agent) {
      agent->on_finish_upload();

      if (agent->get_parent())
        agent->get_parent()->destroy_file_upload_agent(agent);
    }

    mg_printf(nc,
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Connection: close\r\n\r\n"
        "Written %ld of POST data to a temp file\n\n",
        100l);
    nc->flags |= MG_F_SEND_AND_CLOSE;
    break;
  }
  }
}
}

//
//int main(void) {
//
//  for (;;) {
//    mg_mgr_poll(&mgr, 1000);
//  }
//  mg_mgr_free(&mgr);
//
//  return 0;
//}
