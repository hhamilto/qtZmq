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

Persistent<FunctionTemplate> socketConstructor;

class Socket : public node::ObjectWrap {
  void *context;
  void *subscriber;
  uv_poll_t *poll_handle_;
  Persistent<Function> onMessage_cb;
  static NAN_METHOD(DoReceive);
  static NAN_METHOD(Connect);
  static NAN_METHOD(SetOnMessage);
  static NAN_METHOD(Close);
  static NAN_METHOD(New);
  public:
    static void Init(){
    Local<FunctionTemplate> tpl = NanNew<FunctionTemplate>(Socket::New);
    NanAssignPersistent(socketConstructor, tpl);
    tpl->SetClassName(NanNew<String>("Socket"));
    tpl->InstanceTemplate()->SetInternalFieldCount(1);
    //NODE_SET_PROTOTYPE_METHOD(tpl, "New", Socket::New);
    NODE_SET_PROTOTYPE_METHOD(tpl, "connect", Socket::Connect);
    NODE_SET_PROTOTYPE_METHOD(tpl, "doReceive", Socket::DoReceive);
    NODE_SET_PROTOTYPE_METHOD(tpl, "onMessage", Socket::SetOnMessage);
    NODE_SET_PROTOTYPE_METHOD(tpl, "close", Socket::Close);
  }
};

void UV_PollCallback(uv_poll_t* handle, int status, int events);


extern "C" void
init(Handle<Object> theExports) {

  Socket::Init();
  v8::Local<v8::FunctionTemplate> constructorHandle =
      NanNew(socketConstructor);

  theExports->Set(NanNew<String>("Socket"), constructorHandle->GetFunction());
}

NAN_METHOD(Socket::New) {
  NanScope();
  Socket* socket = new Socket();
  socket->Wrap(args.This());
  NanReturnValue(args.This());

}

NAN_METHOD(Socket::SetOnMessage) {
  NanScope();
  Socket* socket = ObjectWrap::Unwrap<Socket>(args.This());

  Local<Value> callback_v = args[0]; //NanObjectWrapHandle(socket)->Get(NanNew("onMessage"));

  if (!callback_v->IsFunction()) {
    NanThrowError(NanError("onMessage callback was not a function. Perhaps its undefined?"));
    return;
  }

  NanAssignPersistent(socket->onMessage_cb, callback_v.As<Function>());
  NanReturnUndefined(); 
}

NAN_METHOD(Socket::Close) {
  NanScope();

  Socket* socket = ObjectWrap::Unwrap<Socket>(args.This());
  while (true) {
    int rc =  zmq_close(socket->subscriber);
    if (rc != 0) {
      if (zmq_errno()==EINTR) {
        continue;
      }
      NanThrowError(ErrorMessage());
      return;
    } else {
      break;
    }
  }

  while (true) {
    int rc =  zmq_ctx_term(socket->context);
    if (rc != 0) {
      if (zmq_errno()==EINTR) {
        continue;
      }
      NanThrowError(ErrorMessage());
      return;
    } else {
      break;
    }
  }

  socket->Unref();
  NanReturnUndefined();
}

NAN_METHOD(Socket::Connect) {
  NanScope();
  Socket* socket = ObjectWrap::Unwrap<Socket>(args.This());
  socket->Ref();

  char* connectAddress = **(new NanUtf8String(args[0]));
  socket->context = zmq_ctx_new();

  //  Socket to talk to server
  socket->subscriber = zmq_socket(socket->context, ZMQ_SUB);
  while (true) {
    int rc = zmq_connect(socket->subscriber, connectAddress);
    if (rc != 0) {
      if (zmq_errno()==EINTR) {
        continue;
      }
      NanThrowError(ErrorMessage());
      return;
    } else {
      break;
    }
  }

  char filter = 0;
  while (true) {
    int rc = zmq_setsockopt(socket->subscriber, ZMQ_SUBSCRIBE, &filter, 0);
    if (rc != 0) {
      if (zmq_errno()==EINTR) {
        continue;
      }
      NanThrowError(ErrorMessage());
      return;
    } else {
      break;
    }
  }

  socket->poll_handle_ = new uv_poll_t;
  socket->poll_handle_->data = socket;
  
  uv_os_sock_t os_socket;
  size_t len = sizeof(uv_os_sock_t);

  if (zmq_getsockopt(socket->subscriber, ZMQ_FD, &os_socket, &len)) {
    throw std::runtime_error(ErrorMessage());
  }
  uv_poll_init_socket(uv_default_loop(), socket->poll_handle_, os_socket);
  uv_poll_start(socket->poll_handle_, UV_READABLE, UV_PollCallback);

  NanReturnUndefined(); 
}



void UV_PollCallback(uv_poll_t* handle, int status, int events){
  Socket* s = static_cast<Socket*>(handle->data);
  NanMakeCallback(NanObjectWrapHandle(s), NanObjectWrapHandle(s)->Get(NanNew("doReceive")).As<Function>(), 0, NULL);
}

static void FreeCallback(char* data, void* message) {
  if (zmq_msg_close((zmq_msg_t*)message) < 0)
    throw std::runtime_error(ErrorMessage());
  free(message);
}

NAN_METHOD(Socket::DoReceive) {
  NanScope();
  Socket* socket = ObjectWrap::Unwrap<Socket>(args.This());
  

  Local<Array> message_buffers = NanNew<Array>(2);
  int message_part_count = 0;
  while(true){

    //printf("qoooooooO2!\n");

    zmq_msg_t* message = (zmq_msg_t*) malloc(sizeof( zmq_msg_t));
    zmq_msg_init(message);
    while (true) {
      int rc = zmq_msg_recv(message, socket->subscriber, ZMQ_DONTWAIT);
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
    if (zmq_getsockopt(socket->subscriber, ZMQ_RCVMORE, &more_to_receive, &len) < 0) {
      NanThrowError(ExceptionFromError());
      return;
    }

    //printf("qoooooooO5!%d\n", more_to_receive);
    if(!more_to_receive){
      break;
    }
  }

  Local<Value> argv[] = {message_buffers};
  Local<Function> local_cb = NanNew<Function>(socket->onMessage_cb);
  NanMakeCallback(NanObjectWrapHandle(socket), local_cb, 1, argv);

  NanReturnUndefined(); 
}

NODE_MODULE(zmq, init)
