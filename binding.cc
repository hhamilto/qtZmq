/*
 * Copyright (c) 2011 Justin Tulloss
 * Copyright (c) 2010 Justin Tulloss
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <v8.h>
#include <node.h>
#include <node_version.h>
#include <node_buffer.h>
#include <zmq.h>
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <stdexcept>
#include <set>
#include "nan.h"

using namespace v8;
using namespace node;

//Lots of inspiration from here: https://github.com/JustinTulloss/zeromq.node

static inline const char*
ErrorMessage() {
  return zmq_strerror(zmq_errno());
}

static inline Handle<Value>
ExceptionFromError() {
  return NanError(ErrorMessage());
}


// module
void *context;
void *subscriber;
v8::Persistent<v8::Object> exports;
uv_poll_t *poll_handle_;

NAN_METHOD(DoReceive);
NAN_METHOD(Connect);

void UV_PollCallback(uv_poll_t* handle, int status, int events);

extern "C" void
init(Handle<Object> theExports) {
  NanAssignPersistent(exports, theExports);

  theExports->Set(NanNew<String>("doReceive"),
    NanNew<FunctionTemplate>(DoReceive)->GetFunction());
  theExports->Set(NanNew<String>("connect"),
    NanNew<FunctionTemplate>(Connect)->GetFunction());


}


NAN_METHOD(Connect) {
  NanScope();

  char* connectAddress = **(new NanUtf8String(args[0]));

  context = zmq_ctx_new();

  //  Socket to talk to server
  printf ("Connecting to hello world server…\n");
  subscriber = zmq_socket(context, ZMQ_SUB);
  int rc = zmq_connect(subscriber, connectAddress);
  if(rc != 0)
    printf("uhhh ohh \n");


  char filter = 0;
  rc = zmq_setsockopt (subscriber, ZMQ_SUBSCRIBE, &filter, 0);
  if(rc != 0)
    printf("uhhh ohh 2: %s\n", zmq_strerror(zmq_errno()));



  poll_handle_ = new uv_poll_t;

  uv_os_sock_t socket;
  size_t len = sizeof(uv_os_sock_t);

  if (zmq_getsockopt(subscriber, ZMQ_FD, &socket, &len)) {
    throw std::runtime_error(ErrorMessage());
  }
  //poll_handle_->data = this;
  uv_poll_init_socket(uv_default_loop(), poll_handle_, socket);
  uv_poll_start(poll_handle_, UV_READABLE, UV_PollCallback);

  NanReturnUndefined(); 
}



void UV_PollCallback(uv_poll_t* handle, int status, int events){
  v8::Local<v8::Object> exportsHandle =NanNew(exports);

  NanMakeCallback(exportsHandle, exportsHandle->Get(NanNew("doReceive")).As<Function>(), 0, NULL);
}

static void FreeCallback(char* data, void* message) {
  if (zmq_msg_close((zmq_msg_t*)message) < 0)
    throw std::runtime_error(ErrorMessage());
  free(message);
}

NAN_METHOD(DoReceive) {
  NanScope();

  //printf("qoooooooO!\n");
  v8::Local<v8::Object> exportsHandle =NanNew(exports);
  //Local<Object> localExports = 
  Local<Value> callback_v = exportsHandle->Get(NanNew("onMessage"));

  if (!callback_v->IsFunction()) {
    printf("wtdc\n");
    return;
  }

  Local<Array> message_buffers = NanNew<Array>(1);
  int message_part_count = 0;
  while(true){

    //printf("qoooooooO2!\n");

    zmq_msg_t* message = (zmq_msg_t*) malloc(sizeof( zmq_msg_t));
    zmq_msg_init(message);

    while (true) {
     // printf("qoooooooO3!\n");
      int rc = zmq_msg_recv(message, subscriber, ZMQ_DONTWAIT);
      if (rc < 0) {
        if (zmq_errno()==EINTR) {
          continue;
        }else if (zmq_errno()==EAGAIN) {
          //printf("wooo\n");
          if (zmq_msg_close(message) < 0)
            throw std::runtime_error(ErrorMessage());
          free(message);
          NanReturnUndefined(); 
          return;
        }
        NanThrowError(ErrorMessage());
        return;
      } else {
        break;
      }
    }

    //printf("qoooooooO4!\n");
    //printf("%d\n",)
    Local<Object> buf_obj = NanNewBufferHandle((char*)zmq_msg_data(message), zmq_msg_size(message), FreeCallback, message);

    message_buffers->Set(message_part_count,buf_obj);
    message_part_count++;

    int more_to_receive = 0;
    size_t len = sizeof(more_to_receive);
    if (zmq_getsockopt(subscriber, ZMQ_RCVMORE, &more_to_receive, &len) < 0) {
      NanThrowError(ExceptionFromError());
      return;
    }

 //   printf("qoooooooO5!%d\n", more_to_receive);
    if(!more_to_receive){
      break;
    }
  }

  Local<Value> argv[] = {message_buffers};
  NanMakeCallback(exportsHandle, callback_v.As<Function>(), 1, argv);

  NanReturnUndefined(); 
}

NODE_MODULE(zmq, init)
