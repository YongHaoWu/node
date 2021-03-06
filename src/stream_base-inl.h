#ifndef SRC_STREAM_BASE_INL_H_
#define SRC_STREAM_BASE_INL_H_

#if defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#include "stream_base.h"

#include "node.h"
#include "env-inl.h"
#include "v8.h"

namespace node {

using v8::Signature;
using v8::External;
using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::HandleScope;
using v8::Local;
using v8::Object;
using v8::PropertyAttribute;
using v8::PropertyCallbackInfo;
using v8::String;
using v8::Value;

using AsyncHooks = Environment::AsyncHooks;

inline void StreamReq::AttachToObject(v8::Local<v8::Object> req_wrap_obj) {
  CHECK_EQ(req_wrap_obj->GetAlignedPointerFromInternalField(kStreamReqField),
           nullptr);
  req_wrap_obj->SetAlignedPointerInInternalField(kStreamReqField, this);
}

inline StreamReq* StreamReq::FromObject(v8::Local<v8::Object> req_wrap_obj) {
  return static_cast<StreamReq*>(
      req_wrap_obj->GetAlignedPointerFromInternalField(kStreamReqField));
}

inline void StreamReq::Dispose() {
  object()->SetAlignedPointerInInternalField(kStreamReqField, nullptr);
  delete this;
}

inline v8::Local<v8::Object> StreamReq::object() {
  return GetAsyncWrap()->object();
}

inline StreamListener::~StreamListener() {
  if (stream_ != nullptr)
    stream_->RemoveStreamListener(this);
}

inline void StreamListener::PassReadErrorToPreviousListener(ssize_t nread) {
  CHECK_NE(previous_listener_, nullptr);
  previous_listener_->OnStreamRead(nread, uv_buf_init(nullptr, 0));
}

inline void StreamListener::OnStreamAfterShutdown(ShutdownWrap* w, int status) {
  CHECK_NE(previous_listener_, nullptr);
  previous_listener_->OnStreamAfterShutdown(w, status);
}

inline void StreamListener::OnStreamAfterWrite(WriteWrap* w, int status) {
  CHECK_NE(previous_listener_, nullptr);
  previous_listener_->OnStreamAfterWrite(w, status);
}

inline StreamResource::~StreamResource() {
  while (listener_ != nullptr) {
    StreamListener* listener = listener_;
    listener->OnStreamDestroy();
    // Remove the listener if it didn’t remove itself. This makes the logic
    // logic in `OnStreamDestroy()` implementations easier, because they
    // may call generic cleanup functions which can just remove the
    // listener unconditionally.
    if (listener == listener_)
      RemoveStreamListener(listener_);
  }
}

inline void StreamResource::PushStreamListener(StreamListener* listener) {
  CHECK_NE(listener, nullptr);
  CHECK_EQ(listener->stream_, nullptr);

  listener->previous_listener_ = listener_;
  listener->stream_ = this;

  listener_ = listener;
}

inline void StreamResource::RemoveStreamListener(StreamListener* listener) {
  CHECK_NE(listener, nullptr);

  StreamListener* previous;
  StreamListener* current;

  // Remove from the linked list.
  for (current = listener_, previous = nullptr;
       /* No loop condition because we want a crash if listener is not found */
       ; previous = current, current = current->previous_listener_) {
    CHECK_NE(current, nullptr);
    if (current == listener) {
      if (previous != nullptr)
        previous->previous_listener_ = current->previous_listener_;
      else
        listener_ = listener->previous_listener_;
      break;
    }
  }

  listener->stream_ = nullptr;
  listener->previous_listener_ = nullptr;
}

inline uv_buf_t StreamResource::EmitAlloc(size_t suggested_size) {
#ifdef DEBUG
  v8::SealHandleScope handle_scope(v8::Isolate::GetCurrent());
#endif
  return listener_->OnStreamAlloc(suggested_size);
}

inline void StreamResource::EmitRead(ssize_t nread, const uv_buf_t& buf) {
#ifdef DEBUG
  v8::SealHandleScope handle_scope(v8::Isolate::GetCurrent());
#endif
  if (nread > 0)
    bytes_read_ += static_cast<uint64_t>(nread);
  listener_->OnStreamRead(nread, buf);
}

inline void StreamResource::EmitAfterWrite(WriteWrap* w, int status) {
#ifdef DEBUG
  v8::SealHandleScope handle_scope(v8::Isolate::GetCurrent());
#endif
  listener_->OnStreamAfterWrite(w, status);
}

inline void StreamResource::EmitAfterShutdown(ShutdownWrap* w, int status) {
#ifdef DEBUG
  v8::SealHandleScope handle_scope(v8::Isolate::GetCurrent());
#endif
  listener_->OnStreamAfterShutdown(w, status);
}

inline void StreamResource::EmitWantsWrite(size_t suggested_size) {
#ifdef DEBUG
  v8::SealHandleScope handle_scope(v8::Isolate::GetCurrent());
#endif
  listener_->OnStreamWantsWrite(suggested_size);
}

inline StreamBase::StreamBase(Environment* env) : env_(env) {
  PushStreamListener(&default_listener_);
}

inline Environment* StreamBase::stream_env() const {
  return env_;
}

inline int StreamBase::Shutdown(v8::Local<v8::Object> req_wrap_obj) {
  Environment* env = stream_env();

  HandleScope handle_scope(env->isolate());

  if (req_wrap_obj.IsEmpty()) {
    req_wrap_obj =
        env->shutdown_wrap_template()
            ->NewInstance(env->context()).ToLocalChecked();
    StreamReq::ResetObject(req_wrap_obj);
  }

  AsyncHooks::DefaultTriggerAsyncIdScope trigger_scope(GetAsyncWrap());
  ShutdownWrap* req_wrap = CreateShutdownWrap(req_wrap_obj);
  int err = DoShutdown(req_wrap);

  if (err != 0) {
    req_wrap->Dispose();
  }

  const char* msg = Error();
  if (msg != nullptr) {
    req_wrap_obj->Set(env->error_string(), OneByteString(env->isolate(), msg));
    ClearError();
  }

  return err;
}

inline StreamWriteResult StreamBase::Write(
    uv_buf_t* bufs,
    size_t count,
    uv_stream_t* send_handle,
    v8::Local<v8::Object> req_wrap_obj) {
  Environment* env = stream_env();
  int err;
  if (send_handle == nullptr) {
    err = DoTryWrite(&bufs, &count);
    if (err != 0 || count == 0) {
      return StreamWriteResult { false, err, nullptr };
    }
  }

  HandleScope handle_scope(env->isolate());

  if (req_wrap_obj.IsEmpty()) {
    req_wrap_obj =
        env->write_wrap_template()
            ->NewInstance(env->context()).ToLocalChecked();
    StreamReq::ResetObject(req_wrap_obj);
  }

  AsyncHooks::DefaultTriggerAsyncIdScope trigger_scope(GetAsyncWrap());
  WriteWrap* req_wrap = CreateWriteWrap(req_wrap_obj);

  err = DoWrite(req_wrap, bufs, count, send_handle);
  bool async = err == 0;

  if (!async) {
    req_wrap->Dispose();
    req_wrap = nullptr;
  }

  const char* msg = Error();
  if (msg != nullptr) {
    req_wrap_obj->Set(env->error_string(), OneByteString(env->isolate(), msg));
    ClearError();
  }

  return StreamWriteResult { async, err, req_wrap };
}

template <typename OtherBase>
SimpleShutdownWrap<OtherBase>::SimpleShutdownWrap(
    StreamBase* stream,
    v8::Local<v8::Object> req_wrap_obj)
  : ShutdownWrap(stream, req_wrap_obj),
    OtherBase(stream->stream_env(),
              req_wrap_obj,
              AsyncWrap::PROVIDER_SHUTDOWNWRAP) {
  Wrap(req_wrap_obj, static_cast<AsyncWrap*>(this));
}

template <typename OtherBase>
SimpleShutdownWrap<OtherBase>::~SimpleShutdownWrap() {
  ClearWrap(static_cast<AsyncWrap*>(this)->object());
}

inline ShutdownWrap* StreamBase::CreateShutdownWrap(
    v8::Local<v8::Object> object) {
  return new SimpleShutdownWrap<AsyncWrap>(this, object);
}

template <typename OtherBase>
SimpleWriteWrap<OtherBase>::SimpleWriteWrap(
    StreamBase* stream,
    v8::Local<v8::Object> req_wrap_obj)
  : WriteWrap(stream, req_wrap_obj),
    OtherBase(stream->stream_env(),
              req_wrap_obj,
              AsyncWrap::PROVIDER_WRITEWRAP) {
  Wrap(req_wrap_obj, static_cast<AsyncWrap*>(this));
}

template <typename OtherBase>
SimpleWriteWrap<OtherBase>::~SimpleWriteWrap() {
  ClearWrap(static_cast<AsyncWrap*>(this)->object());
}

inline WriteWrap* StreamBase::CreateWriteWrap(
    v8::Local<v8::Object> object) {
  return new SimpleWriteWrap<AsyncWrap>(this, object);
}

template <class Base>
void StreamBase::AddMethods(Environment* env,
                            Local<FunctionTemplate> t,
                            int flags) {
  HandleScope scope(env->isolate());

  enum PropertyAttribute attributes =
      static_cast<PropertyAttribute>(
          v8::ReadOnly | v8::DontDelete | v8::DontEnum);

  Local<Signature> signature = Signature::New(env->isolate(), t);

  Local<FunctionTemplate> get_fd_templ =
      FunctionTemplate::New(env->isolate(),
                            GetFD<Base>,
                            env->as_external(),
                            signature);

  Local<FunctionTemplate> get_external_templ =
      FunctionTemplate::New(env->isolate(),
                            GetExternal<Base>,
                            env->as_external(),
                            signature);

  Local<FunctionTemplate> get_bytes_read_templ =
      FunctionTemplate::New(env->isolate(),
                            GetBytesRead<Base>,
                            env->as_external(),
                            signature);

  t->PrototypeTemplate()->SetAccessorProperty(env->fd_string(),
                                              get_fd_templ,
                                              Local<FunctionTemplate>(),
                                              attributes);

  t->PrototypeTemplate()->SetAccessorProperty(env->external_stream_string(),
                                              get_external_templ,
                                              Local<FunctionTemplate>(),
                                              attributes);

  t->PrototypeTemplate()->SetAccessorProperty(env->bytes_read_string(),
                                              get_bytes_read_templ,
                                              Local<FunctionTemplate>(),
                                              attributes);

  env->SetProtoMethod(t, "readStart", JSMethod<Base, &StreamBase::ReadStartJS>);
  env->SetProtoMethod(t, "readStop", JSMethod<Base, &StreamBase::ReadStopJS>);
  if ((flags & kFlagNoShutdown) == 0)
    env->SetProtoMethod(t, "shutdown", JSMethod<Base, &StreamBase::Shutdown>);
  if ((flags & kFlagHasWritev) != 0)
    env->SetProtoMethod(t, "writev", JSMethod<Base, &StreamBase::Writev>);
  env->SetProtoMethod(t,
                      "writeBuffer",
                      JSMethod<Base, &StreamBase::WriteBuffer>);
  env->SetProtoMethod(t,
                      "writeAsciiString",
                      JSMethod<Base, &StreamBase::WriteString<ASCII> >);
  env->SetProtoMethod(t,
                      "writeUtf8String",
                      JSMethod<Base, &StreamBase::WriteString<UTF8> >);
  env->SetProtoMethod(t,
                      "writeUcs2String",
                      JSMethod<Base, &StreamBase::WriteString<UCS2> >);
  env->SetProtoMethod(t,
                      "writeLatin1String",
                      JSMethod<Base, &StreamBase::WriteString<LATIN1> >);
}


template <class Base>
void StreamBase::GetFD(const FunctionCallbackInfo<Value>& args) {
  // Mimic implementation of StreamBase::GetFD() and UDPWrap::GetFD().
  Base* handle;
  ASSIGN_OR_RETURN_UNWRAP(&handle,
                          args.This(),
                          args.GetReturnValue().Set(UV_EINVAL));

  StreamBase* wrap = static_cast<StreamBase*>(handle);
  if (!wrap->IsAlive())
    return args.GetReturnValue().Set(UV_EINVAL);

  args.GetReturnValue().Set(wrap->GetFD());
}

template <class Base>
void StreamBase::GetBytesRead(const FunctionCallbackInfo<Value>& args) {
  // The handle instance hasn't been set. So no bytes could have been read.
  Base* handle;
  ASSIGN_OR_RETURN_UNWRAP(&handle,
                          args.This(),
                          args.GetReturnValue().Set(0));

  StreamBase* wrap = static_cast<StreamBase*>(handle);
  // uint64_t -> double. 53bits is enough for all real cases.
  args.GetReturnValue().Set(static_cast<double>(wrap->bytes_read_));
}

template <class Base>
void StreamBase::GetExternal(const FunctionCallbackInfo<Value>& args) {
  Base* handle;
  ASSIGN_OR_RETURN_UNWRAP(&handle, args.This());

  StreamBase* wrap = static_cast<StreamBase*>(handle);
  Local<External> ext = External::New(args.GetIsolate(), wrap);
  args.GetReturnValue().Set(ext);
}


template <class Base,
          int (StreamBase::*Method)(const FunctionCallbackInfo<Value>& args)>
void StreamBase::JSMethod(const FunctionCallbackInfo<Value>& args) {
  Base* handle;
  ASSIGN_OR_RETURN_UNWRAP(&handle, args.Holder());

  StreamBase* wrap = static_cast<StreamBase*>(handle);
  if (!wrap->IsAlive())
    return args.GetReturnValue().Set(UV_EINVAL);

  AsyncHooks::DefaultTriggerAsyncIdScope trigger_scope(handle);
  args.GetReturnValue().Set((wrap->*Method)(args));
}


inline void ShutdownWrap::OnDone(int status) {
  stream()->EmitAfterShutdown(this, status);
  Dispose();
}

inline void WriteWrap::SetAllocatedStorage(char* data, size_t size) {
  CHECK_EQ(storage_, nullptr);
  storage_ = data;
  storage_size_ = size;
}

inline char* WriteWrap::Storage() {
  return storage_;
}

inline size_t WriteWrap::StorageSize() const {
  return storage_size_;
}

inline void WriteWrap::OnDone(int status) {
  stream()->EmitAfterWrite(this, status);
  Dispose();
}

inline void StreamReq::Done(int status, const char* error_str) {
  AsyncWrap* async_wrap = GetAsyncWrap();
  Environment* env = async_wrap->env();
  if (error_str != nullptr) {
    async_wrap->object()->Set(env->error_string(),
                              OneByteString(env->isolate(), error_str));
  }

  OnDone(status);
}

inline void StreamReq::ResetObject(v8::Local<v8::Object> obj) {
#ifdef DEBUG
  CHECK_GT(obj->InternalFieldCount(), StreamReq::kStreamReqField);
#endif
  ClearWrap(obj);
  obj->SetAlignedPointerInInternalField(StreamReq::kStreamReqField, nullptr);
}


}  // namespace node

#endif  // defined(NODE_WANT_INTERNALS) && NODE_WANT_INTERNALS

#endif  // SRC_STREAM_BASE_INL_H_
