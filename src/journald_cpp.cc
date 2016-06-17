/**
 * Native extension that logs to the systemd journal asyncronously. It also
 * supports a sync mode but that isn't yet implemented in the journald.js
 * lib.
 *
 * (C) 2012 Mark Theunissen
 * MIT (EXPAT) LICENCE
 *
 */

#include <node.h>
// http://stackoverflow.com/questions/30141137/use-libuv-function-in-node-js-0-12-x
#include <uv.h>
#include <v8.h>
#include <systemd/sd-journal.h>
#include <stdlib.h>

using namespace std;
using namespace v8;

void Async(const FunctionCallbackInfo<Value>& args);
void AsyncWork(uv_work_t* req);
void AsyncAfter(uv_work_t* req);

struct Baton {
  uv_work_t request;
  Persistent<Function> callback;
  bool error;
  int32_t result;
  struct iovec *iov;
  int argc;
  Isolate* isolate;
};

/**
 * Log string messages to the journal. Type checking of the args is performed
 * in the journald.js module. If the last argument is a function, we log
 * asyncronously and treat the function as a callback.
 */
void SdJournalSend(const FunctionCallbackInfo<Value>& args) {
  // http://stackoverflow.com/questions/27033241/v8-on-android-ndk-build-throws-error-v8handlescopehandlescope-is-prot
  Isolate* isolate = args.GetIsolate(); // the current VM instance
  int argc = args.Length();
  struct iovec *iov = NULL;
  bool isAsync = false;

  // If the last argument is a function, we need to exclude it from the
  // string processing.
  if (args[argc-1]->IsFunction()) {
    argc--;
    isAsync = true;
  }

  // Regardless of whether this is sync or async, we need to create the
  // iovector because we won't have access to the v8 API in the worker
  // thread.
  iov = (iovec*) malloc(argc * sizeof(struct iovec));
  if (!iov) {
    isolate->ThrowException(String::NewFromUtf8(isolate, "Out of memory"));
  }
  for (int i = 0; i < argc; ++i) {
    Local<String> v8str = args[i]->ToString();
    iov[i].iov_len = v8str->Length();
    iov[i].iov_base = (uint8_t*) malloc(v8str->Length() + 1);
    // https://bugs.chromium.org/p/v8/issues/detail?id=2588
    v8str->WriteOneByte((uint8_t*)iov[i].iov_base, 0, iov[i].iov_len);
  }

  // Divergent paths for sync and async.
  if (isAsync) {
    Local<Function> callback = Local<Function>::Cast(args[argc]);

    // This creates our work request, including the libuv struct.
    Baton* baton = new Baton();
    baton->error = false;
    baton->request.data = baton;
    // NOTE: Another change is that persistent handles are no longer copyable by default
    baton->callback.Reset(isolate, callback);
    baton->iov = iov;
    baton->argc = argc;
    baton->isolate = isolate;

    int status = uv_queue_work(uv_default_loop(), &baton->request, AsyncWork, (uv_after_work_cb) AsyncAfter);
    assert(status == 0);
  }
  else {
    sd_journal_sendv(iov, argc);
    for (int i = 0; i < argc; ++i) {
      free(iov[i].iov_base);
    }
    free(iov);
  }
}

/**
 * Perform the async work in the worker thread. No v8 API here.
 */
void AsyncWork(uv_work_t* req) {
  Baton* baton = static_cast<Baton*>(req->data);

  baton->result = sd_journal_sendv(baton->iov, baton->argc);

  for (int i = 0; i < baton->argc; ++i) {
    free(baton->iov[i].iov_base);
  }
  free(baton->iov);
}

/**
 * Once the async work completes.
 */
void AsyncAfter(uv_work_t* req) {
  Baton* baton = static_cast<Baton*>(req->data);
  Isolate* isolate = baton->isolate;
  HandleScope scope(isolate);
  Local<Function> callback = Local<Function>::New(isolate, baton->callback);

  // TODO: We don't yet set this error flag but the example code is
  // still here for future.
  if (baton->error) {
    Local<Value> err = Exception::Error(String::NewFromUtf8(isolate, "ERROR"));

    // Prepare the parameters for the callback function.
    const unsigned argc = 1;
    Local<Value> argv[argc] = { err };

    // Wrap the callback function call in a TryCatch so that we can call
    // node's FatalException afterwards. This makes it possible to catch
    // the exception from JavaScript land using the
    // process.on('uncaughtException') event.
    TryCatch try_catch;
    callback->Call(Null(isolate), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(isolate, try_catch);
    }
  }
  else {
    // If the operation succeeded, convention is to pass null as the
    // first argument before the result arguments, as the err parameter.
    const unsigned argc = 2;
    Local<Value> argv[argc] = {
      Null(isolate),
      Integer::New(isolate, baton->result)
    };

    // Wrap the callback function call in a TryCatch so that we can call
    // node's FatalException afterwards. This makes it possible to catch
    // the exception from JavaScript land using the
    // process.on('uncaughtException') event.
    TryCatch try_catch;
    callback->Call(Null(isolate), argc, argv);
    if (try_catch.HasCaught()) {
      node::FatalException(isolate, try_catch);
    }
  }

  // The callback is a permanent handle, so we have to dispose of it manually.
  baton->callback.Reset();
  delete baton;
}

void init(Local<Object> exports) {
  NODE_SET_METHOD(exports, "send", SdJournalSend);
}

NODE_MODULE(journald_cpp, init)
