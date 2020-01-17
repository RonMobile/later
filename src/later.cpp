#include "later.h"
#include <Rcpp.h>
#include <map>
#include <queue>
#include <boost/shared_ptr.hpp>
#include <boost/make_shared.hpp>
#include <boost/scope_exit.hpp>
#include "debug.h"
#include "utils.h"
#include "threadutils.h"

#include "callback_registry.h"
#include "interrupt.h"

// Declare platform-specific functions that are implemented in
// later_posix.cpp and later_win32.cpp.
// [[Rcpp::export]]
void ensureInitialized();
uint64_t doExecLater(boost::shared_ptr<CallbackRegistry> callbackRegistry, Rcpp::Function callback, double delaySecs, bool resetTimer);
uint64_t doExecLater(boost::shared_ptr<CallbackRegistry> callbackRegistry, void (*callback)(void*), void* data, double delaySecs, bool resetTimer);

static size_t exec_callbacks_reentrancy_count = 0;

class ProtectCallbacks {
public:
  ProtectCallbacks() {
    exec_callbacks_reentrancy_count++;
  }
  ~ProtectCallbacks() {
    exec_callbacks_reentrancy_count--;
  }
};

// Returns number of frames on the call stack. Basically just a wrapper for
// base::sys.nframe(). Note that this can report that an error occurred if the
// user sends an interrupt while the `sys.nframe()` function is running. I
// believe that the only reason that it should set errorOccurred is because of
// a user interrupt.
int sys_nframe() {
  ASSERT_MAIN_THREAD()
  SEXP e, result;
  int errorOccurred, value;

  BEGIN_SUSPEND_INTERRUPTS {
    PROTECT(e = Rf_lang1(Rf_install("sys.nframe")));
    PROTECT(result = R_tryEval(e, R_BaseEnv, &errorOccurred));

    if (errorOccurred) {
      value = -1;
    } else {
      value = INTEGER(result)[0];
    }

    UNPROTECT(2);
  } END_SUSPEND_INTERRUPTS;

  return value;
}

// Returns true if execCallbacks is executing, or sys.nframes() returns 0.
bool at_top_level() {
  ASSERT_MAIN_THREAD()
  if (exec_callbacks_reentrancy_count != 0)
    return false;

  int nframe = sys_nframe();
  if (nframe == -1) {
    throw Rcpp::exception("Error occurred while calling sys.nframe()");
  }
  return nframe == 0;
}

// ============================================================================
// Current event loop
// ============================================================================
static int current_loop_id = 0;

int setCurrentLoop(int loop_id) {
  current_loop_id = loop_id;
}

int getCurrentLoop() {
  return current_loop_id;
}

// ============================================================================
// Callback registry
// ============================================================================

// Each callback registry represents one event loop. Note that traversing and
// modifying callbackRegistries should always be guarded with
// callbackRegistriesMutex, to avoid races between threads.
std::map<int, boost::shared_ptr<CallbackRegistry> > callbackRegistries;

// Functions that search or manipulate callbackRegistries must use this mutex.
// Some functions also have ASSERT_MAIN_THREAD in order to make sure that
// R-related code always runs on the main thread.
Mutex callbackRegistriesMutex(tct_mtx_plain | tct_mtx_recursive);

// [[Rcpp::export]]
bool existsCallbackRegistry(int loop) {
  Guard guard(callbackRegistriesMutex);
  return (callbackRegistries.find(loop) != callbackRegistries.end());
}

// Gets a callback registry by ID.
boost::shared_ptr<CallbackRegistry> getCallbackRegistry(int loop) {
  Guard guard(callbackRegistriesMutex);
  if (!existsCallbackRegistry(loop)) {
    // This function can be called from different threads so we can't use
    // Rcpp::stop().
    throw std::runtime_error("Callback registry (loop) " + toString(loop) + " not found.");
  }
  return callbackRegistries[loop];
}

// [[Rcpp::export]]
bool createCallbackRegistry(int loop, int parent_loop) {
  ASSERT_MAIN_THREAD()
  Guard guard(callbackRegistriesMutex);
  if (existsCallbackRegistry(loop)) {
    Rcpp::stop("Can't create event loop %d because it already exists.", loop);
  }
  if (parent_loop == -1) {
    callbackRegistries[loop] = boost::make_shared<CallbackRegistry>(loop);
  } else {
    // Register this loop with parent
    boost::shared_ptr<CallbackRegistry> parent = getCallbackRegistry(parent_loop);
    callbackRegistries[loop] = boost::make_shared<CallbackRegistry>(loop, parent);
    parent->children.push_back(callbackRegistries[loop]);
  }
  return true;
}


bool deletingCallbackRegistry = false;
// [[Rcpp::export]]
bool deleteCallbackRegistry(int loop) {
  ASSERT_MAIN_THREAD()
  // Detect re-entrant calls to this function and just return in that case.
  // This can hypothetically happen if as we're deleting a callback registry,
  // an R finalizer runs while we're removing references to the Rcpp::Function
  // objects, and the finalizer calls this function. Since this function is
  // always called from the same thread, we don't have to worry about races
  // with this variable.
  if (deletingCallbackRegistry) {
    return false;
  }

  Guard guard(callbackRegistriesMutex);

  deletingCallbackRegistry = true;
  BOOST_SCOPE_EXIT(void) {
    deletingCallbackRegistry = false;
  } BOOST_SCOPE_EXIT_END

  if (!existsCallbackRegistry(loop)) {
    return false;
  }

  // This is in a block so that the lifetime of the shared_ptrs is limited to
  // inside. It would be nicer to do this stuff in the CallbackRegistry
  // destructor, but we can't, because we can't reliably use shard_ptr from
  // inside the destructor; we need to some pointer comparison, but you can't
  // call shared_from_this() from inside the destructor.
  {
    boost::shared_ptr<CallbackRegistry> registry = getCallbackRegistry(loop);
    boost::shared_ptr<CallbackRegistry> parent = registry->parent;
    if (parent != nullptr) {
      // Remove this registry from the parent's list of children.
      for (std::vector<boost::shared_ptr<CallbackRegistry>>::iterator it = parent->children.begin();
           it != parent->children.end();
           ++it)
      {
        if (*it == registry) {
          parent->children.erase(it);
        }
      }

      // Graft this registry's children onto the parent's children.
      // TODO: If there's no parent, do the children get orphaned?
      parent->children.insert(
        parent->children.end(), registry->children.begin(), registry->children.end()
      );
    }
  }

  int n = callbackRegistries.erase(loop);

  if (n == 0) return false;
  else return true;
}

// [[Rcpp::export]]
Rcpp::List list_queue_(int loop) {
  ASSERT_MAIN_THREAD()
  Guard guard(callbackRegistriesMutex);
  return getCallbackRegistry(loop)->list();
}


// Execute callbacks for a single event loop.
bool execCallbacksOne(
  bool runAll,
  boost::shared_ptr<CallbackRegistry> callback_registry,
  Timestamp now
) {
  ASSERT_MAIN_THREAD()
  // execCallbacks can be called directly from C code, and the callbacks may
  // include Rcpp code. (Should we also call wrap?)
  Rcpp::RNGScope rngscope;
  ProtectCallbacks pcscope;

  do {
    // We only take one at a time, because we don't want to lose callbacks if
    // one of the callbacks throws an error
    std::vector<Callback_sp> callbacks = callback_registry->take(1, now);
    if (callbacks.size() == 0) {
      break;
    }
    // This line may throw errors!
    callbacks[0]->invoke_wrapped();
  } while (runAll);

  // TODO: Recurse, but pass along `now`.
  // I think there's no need to lock this since it's only modified from the
  // main thread. But need to check.
  std::vector<boost::shared_ptr<CallbackRegistry> > children = callback_registry->children;
  for (std::vector<boost::shared_ptr<CallbackRegistry> >::iterator it = children.begin();
       it != children.end();
       ++it)
  {
    execCallbacksOne(true, *it, now);
  }

  return true;
}


// Execute callbacks for an event loop and its children. Note that
// [[Rcpp::export]]
bool execCallbacks(double timeoutSecs, bool runAll, int loop) {
  ASSERT_MAIN_THREAD()

  boost::shared_ptr<CallbackRegistry> callback_registry;
  {
    // Only lock callbackRegistries for this scope so that when we're waiting
    // or running callbacks later in this function, we won't block other
    // threads from adding items to the registry.
    Guard guard(callbackRegistriesMutex);
    callback_registry = getCallbackRegistry(loop);
  }

  if (!callback_registry->wait(timeoutSecs, true)) {
    return false;
  }

  Timestamp now;
  execCallbacksOne(runAll, callback_registry, now);

  // TODO: Fix return value
  return true;
}


// This function is called from the input handler on Unix, or the Windows
// equivalent. It may throw exceptions.
//
// Invoke execCallbacks up to 20 times. At the first iteration where no work is
// done, terminate. We call this from the top level instead of just calling
// execCallbacks because the top level only gets called occasionally (every 10's
// of ms), so tasks that generate other tasks will execute surprisingly slowly.
//
// Example:
// promise_map(1:1000, function(i) {
//   message(i)
//   promise_resolve(i)
// })
bool execCallbacksForTopLevel() {
  bool any = false;
  for (size_t i = 0; i < 20; i++) {
    if (!execCallbacks(0, true, GLOBAL_LOOP))
      return any;
    any = true;
  }
  return any;
}

// [[Rcpp::export]]
bool idle(int loop) {
  ASSERT_MAIN_THREAD()
  Guard guard(callbackRegistriesMutex);
  return getCallbackRegistry(loop)->empty();
}

// [[Rcpp::export]]
std::string execLater(Rcpp::Function callback, double delaySecs, int loop) {
  ASSERT_MAIN_THREAD()
  ensureInitialized();
  Guard guard(callbackRegistriesMutex);
  uint64_t callback_id = doExecLater(getCallbackRegistry(loop), callback, delaySecs, true);

  // We have to convert it to a string in order to maintain 64-bit precision,
  // since R doesn't support 64 bit integers.
  return toString(callback_id);
}



bool cancel(uint64_t callback_id, int loop) {
  ASSERT_MAIN_THREAD()
  Guard guard(callbackRegistriesMutex);
  if (!existsCallbackRegistry(loop))
    return false;

  boost::shared_ptr<CallbackRegistry> reg = getCallbackRegistry(loop);
  if (!reg)
    return false;

  return reg->cancel(callback_id);
}

// [[Rcpp::export]]
bool cancel(std::string callback_id_s, int loop) {
  ASSERT_MAIN_THREAD()
  uint64_t callback_id;
  std::istringstream iss(callback_id_s);
  iss >> callback_id;

  // If the input is good (just a number with no other text) then eof will be
  // 1 and fail will be 0.
  if (! (iss.eof() && !iss.fail())) {
    return false;
  }

  return cancel(callback_id, loop);
}



// [[Rcpp::export]]
double nextOpSecs(int loop) {
  ASSERT_MAIN_THREAD()
  Guard guard(callbackRegistriesMutex);
  Optional<Timestamp> nextTime = getCallbackRegistry(loop)->nextTimestamp();
  if (!nextTime.has_value()) {
    return R_PosInf;
  } else {
    Timestamp now;
    return nextTime->diff_secs(now);
  }
}

// Schedules a C function to execute on the global loop. Returns callback ID
// on success, or 0 on error.
extern "C" uint64_t execLaterNative(void (*func)(void*), void* data, double delaySecs) {
  return execLaterNative2(func, data, delaySecs, GLOBAL_LOOP);
}

// Schedules a C function to execute on a specific event loop. Returns
// callback ID on success, or 0 on error.
extern "C" uint64_t execLaterNative2(void (*func)(void*), void* data, double delaySecs, int loop) {
  ensureInitialized();
  Guard guard(callbackRegistriesMutex);
  // This try is because getCallbackRegistry can throw, and if it happens on a
  // background thread the process will stop.
  try {
    return doExecLater(getCallbackRegistry(loop), func, data, delaySecs, true);
  } catch (...) {
    return 0;
  }
}

extern "C" int apiVersion() {
  return LATER_DLL_API_VERSION;
}
