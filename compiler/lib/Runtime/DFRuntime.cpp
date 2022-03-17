// Part of the Concrete Compiler Project, under the BSD3 License with Zama
// Exceptions. See
// https://github.com/zama-ai/concrete-compiler-internal/blob/main/LICENSE.txt
// for license information.

/// This file implements the dataflow runtime. It encapsulates all of
/// the underlying communication, parallelism, etc. and only exposes a
/// simplified interface for code generation in runtime_api.h
/// This hides the details of implementation, including of the HPX
/// framework currently used, from the code generation side.

#ifdef CONCRETELANG_PARALLEL_EXECUTION_ENABLED

#include <hpx/barrier.hpp>
#include <hpx/future.hpp>
#include <hpx/hpx_start.hpp>
#include <hpx/hpx_suspend.hpp>
#include <hwloc.h>

#include "concretelang/Runtime/DFRuntime.hpp"
#include "concretelang/Runtime/distributed_generic_task_server.hpp"
#include "concretelang/Runtime/runtime_api.h"

std::vector<GenericComputeClient> gcc;
void *dl_handle;

KeyManager<LweBootstrapKey_u64> *_dfr_node_level_bsk_manager;
KeyManager<LweKeyswitchKey_u64> *_dfr_node_level_ksk_manager;

WorkFunctionRegistry *_dfr_node_level_work_function_registry;
std::list<void *> new_allocated;
std::list<void *> fut_allocated;
std::list<void *> m_allocated;
hpx::lcos::barrier *_dfr_jit_workfunction_registration_barrier;
hpx::lcos::barrier *_dfr_jit_phase_barrier;
std::atomic<uint64_t> init_guard = {0};

using namespace hpx;

void *_dfr_make_ready_future(void *in) {
  void *future = static_cast<void *>(
      new hpx::shared_future<void *>(hpx::make_ready_future(in)));
  m_allocated.push_back(in);
  fut_allocated.push_back(future);
  return future;
}

void *_dfr_await_future(void *in) {
  return static_cast<hpx::shared_future<void *> *>(in)->get();
}

void _dfr_deallocate_future_data(void *in) {
  delete[] static_cast<char *>(
      static_cast<hpx::shared_future<void *> *>(in)->get());
}

void _dfr_deallocate_future(void *in) {
  delete (static_cast<hpx::shared_future<void *> *>(in));
}

// Determine where new task should run.  For now just round-robin
// distribution - TODO: optimise.
static inline size_t _dfr_find_next_execution_locality() {
  static size_t num_nodes = hpx::get_num_localities().get();
  static std::atomic<std::size_t> next_locality{0};

  size_t next_loc = ++next_locality;

  return next_loc % num_nodes;
}

/// Runtime generic async_task.  Each first NUM_PARAMS pairs of
/// arguments in the variadic list corresponds to a void* pointer on a
/// hpx::future<void*> and the size of data within the future.  After
/// that come NUM_OUTPUTS pairs of hpx::future<void*>* and size_t for
/// the returns.
void _dfr_create_async_task(wfnptr wfn, size_t num_params, size_t num_outputs,
                            ...) {
  std::vector<void *> params;
  std::vector<size_t> param_sizes;
  std::vector<uint64_t> param_types;
  std::vector<void *> outputs;
  std::vector<size_t> output_sizes;
  std::vector<uint64_t> output_types;

  va_list args;
  va_start(args, num_outputs);
  for (size_t i = 0; i < num_params; ++i) {
    params.push_back(va_arg(args, void *));
    param_sizes.push_back(va_arg(args, uint64_t));
    param_types.push_back(va_arg(args, uint64_t));
  }
  for (size_t i = 0; i < num_outputs; ++i) {
    outputs.push_back(va_arg(args, void *));
    output_sizes.push_back(va_arg(args, uint64_t));
    output_types.push_back(va_arg(args, uint64_t));
  }
  va_end(args);

  for (size_t i = 0; i < num_params; ++i) {
    if (_dfr_get_arg_type(param_types[i] == _DFR_TASK_ARG_MEMREF)) {
      m_allocated.push_back(
          (void *)static_cast<StridedMemRefType<char, 1> *>(params[i])->data);
    }
  }

  // We pass functions by name - which is not strictly necessary in
  // shared memory as pointers suffice, but is needed in the
  // distributed case where the functions need to be located/loaded on
  // the node.
  auto wfnname =
      _dfr_node_level_work_function_registry->getWorkFunctionName((void *)wfn);
  hpx::future<hpx::future<OpaqueOutputData>> oodf;

  // In order to allow complete dataflow semantics for
  // communication/synchronization, we split tasks in two parts: an
  // execution body that is scheduled once all input dependences are
  // satisfied, which generates a future on a tuple of outputs, which
  // is then further split into a tuple of futures and provide
  // individual synchronization for each return independently.
  switch (num_params) {
  case 0:
    oodf = std::move(
        hpx::dataflow([wfnname, param_sizes, param_types, output_sizes,
                       output_types]() -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        }));
    break;

  case 1:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {param0.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0]));
    break;

  case 2:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {param0.get(), param1.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1]));
    break;

  case 3:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {param0.get(), param1.get(),
                                        param2.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2]));
    break;

  case 4:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {param0.get(), param1.get(),
                                        param2.get(), param3.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3]));
    break;

  case 5:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {param0.get(), param1.get(),
                                        param2.get(), param3.get(),
                                        param4.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4]));
    break;

  case 6:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {param0.get(), param1.get(),
                                        param2.get(), param3.get(),
                                        param4.get(), param5.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5]));
    break;

  case 7:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(), param3.get(),
              param4.get(), param5.get(), param6.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6]));
    break;

  case 8:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(), param3.get(),
              param4.get(), param5.get(), param6.get(), param7.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7]));
    break;

  case 9:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(),
              param3.get(), param4.get(), param5.get(),
              param6.get(), param7.get(), param8.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8]));
    break;

  case 10:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(), param3.get(),
              param4.get(), param5.get(), param6.get(), param7.get(),
              param8.get(), param9.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9]));
    break;

  case 11:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9,
                       hpx::shared_future<void *> param10)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(), param3.get(),
              param4.get(), param5.get(), param6.get(), param7.get(),
              param8.get(), param9.get(), param10.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9],
        *(hpx::shared_future<void *> *)params[10]));
    break;

  case 12:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9,
                       hpx::shared_future<void *> param10,
                       hpx::shared_future<void *> param11)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(),  param3.get(),
              param4.get(), param5.get(), param6.get(),  param7.get(),
              param8.get(), param9.get(), param10.get(), param11.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9],
        *(hpx::shared_future<void *> *)params[10],
        *(hpx::shared_future<void *> *)params[11]));
    break;

  case 13:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9,
                       hpx::shared_future<void *> param10,
                       hpx::shared_future<void *> param11,
                       hpx::shared_future<void *> param12)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(), param1.get(), param2.get(),  param3.get(),
              param4.get(), param5.get(), param6.get(),  param7.get(),
              param8.get(), param9.get(), param10.get(), param11.get(),
              param12.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9],
        *(hpx::shared_future<void *> *)params[10],
        *(hpx::shared_future<void *> *)params[11],
        *(hpx::shared_future<void *> *)params[12]));
    break;

  case 14:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9,
                       hpx::shared_future<void *> param10,
                       hpx::shared_future<void *> param11,
                       hpx::shared_future<void *> param12,
                       hpx::shared_future<void *> param13)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(),  param1.get(), param2.get(),  param3.get(),
              param4.get(),  param5.get(), param6.get(),  param7.get(),
              param8.get(),  param9.get(), param10.get(), param11.get(),
              param12.get(), param13.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9],
        *(hpx::shared_future<void *> *)params[10],
        *(hpx::shared_future<void *> *)params[11],
        *(hpx::shared_future<void *> *)params[12],
        *(hpx::shared_future<void *> *)params[13]));
    break;

  case 15:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9,
                       hpx::shared_future<void *> param10,
                       hpx::shared_future<void *> param11,
                       hpx::shared_future<void *> param12,
                       hpx::shared_future<void *> param13,
                       hpx::shared_future<void *> param14)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(),  param1.get(),  param2.get(),  param3.get(),
              param4.get(),  param5.get(),  param6.get(),  param7.get(),
              param8.get(),  param9.get(),  param10.get(), param11.get(),
              param12.get(), param13.get(), param14.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9],
        *(hpx::shared_future<void *> *)params[10],
        *(hpx::shared_future<void *> *)params[11],
        *(hpx::shared_future<void *> *)params[12],
        *(hpx::shared_future<void *> *)params[13],
        *(hpx::shared_future<void *> *)params[14]));
    break;

  case 16:
    oodf = std::move(hpx::dataflow(
        [wfnname, param_sizes, param_types, output_sizes,
         output_types](hpx::shared_future<void *> param0,
                       hpx::shared_future<void *> param1,
                       hpx::shared_future<void *> param2,
                       hpx::shared_future<void *> param3,
                       hpx::shared_future<void *> param4,
                       hpx::shared_future<void *> param5,
                       hpx::shared_future<void *> param6,
                       hpx::shared_future<void *> param7,
                       hpx::shared_future<void *> param8,
                       hpx::shared_future<void *> param9,
                       hpx::shared_future<void *> param10,
                       hpx::shared_future<void *> param11,
                       hpx::shared_future<void *> param12,
                       hpx::shared_future<void *> param13,
                       hpx::shared_future<void *> param14,
                       hpx::shared_future<void *> param15)
            -> hpx::future<OpaqueOutputData> {
          std::vector<void *> params = {
              param0.get(),  param1.get(),  param2.get(),  param3.get(),
              param4.get(),  param5.get(),  param6.get(),  param7.get(),
              param8.get(),  param9.get(),  param10.get(), param11.get(),
              param12.get(), param13.get(), param14.get(), param15.get()};
          OpaqueInputData oid(wfnname, params, param_sizes, param_types,
                              output_sizes, output_types);
          return gcc[_dfr_find_next_execution_locality()].execute_task(oid);
        },
        *(hpx::shared_future<void *> *)params[0],
        *(hpx::shared_future<void *> *)params[1],
        *(hpx::shared_future<void *> *)params[2],
        *(hpx::shared_future<void *> *)params[3],
        *(hpx::shared_future<void *> *)params[4],
        *(hpx::shared_future<void *> *)params[5],
        *(hpx::shared_future<void *> *)params[6],
        *(hpx::shared_future<void *> *)params[7],
        *(hpx::shared_future<void *> *)params[8],
        *(hpx::shared_future<void *> *)params[9],
        *(hpx::shared_future<void *> *)params[10],
        *(hpx::shared_future<void *> *)params[11],
        *(hpx::shared_future<void *> *)params[12],
        *(hpx::shared_future<void *> *)params[13],
        *(hpx::shared_future<void *> *)params[14],
        *(hpx::shared_future<void *> *)params[15]));
    break;

  default:
    HPX_THROW_EXCEPTION(hpx::no_success, "_dfr_create_async_task",
                        "Error: number of task parameters not supported.");
  }

  switch (num_outputs) {
  case 1:
    *((void **)outputs[0]) = new hpx::shared_future<void *>(hpx::dataflow(
        [](hpx::future<OpaqueOutputData> oodf_in) -> void * {
          return oodf_in.get().outputs[0];
        },
        oodf));
    fut_allocated.push_back(*((void **)outputs[0]));
    break;

  case 2: {
    hpx::future<hpx::tuple<void *, void *>> &&ft = hpx::dataflow(
        [](hpx::future<OpaqueOutputData> oodf_in)
            -> hpx::tuple<void *, void *> {
          std::vector<void *> outputs = std::move(oodf_in.get().outputs);
          return hpx::make_tuple<>(outputs[0], outputs[1]);
        },
        oodf);
    hpx::tuple<hpx::future<void *>, hpx::future<void *>> &&tf =
        hpx::split_future(std::move(ft));
    *((void **)outputs[0]) =
        (void *)new hpx::shared_future<void *>(std::move(hpx::get<0>(tf)));
    *((void **)outputs[1]) =
        (void *)new hpx::shared_future<void *>(std::move(hpx::get<1>(tf)));
    fut_allocated.push_back(*((void **)outputs[0]));
    fut_allocated.push_back(*((void **)outputs[1]));
    break;
  }

  case 3: {
    hpx::future<hpx::tuple<void *, void *, void *>> &&ft = hpx::dataflow(
        [](hpx::future<OpaqueOutputData> oodf_in)
            -> hpx::tuple<void *, void *, void *> {
          std::vector<void *> outputs = std::move(oodf_in.get().outputs);
          return hpx::make_tuple<>(outputs[0], outputs[1], outputs[2]);
        },
        oodf);
    hpx::tuple<hpx::future<void *>, hpx::future<void *>, hpx::future<void *>>
        &&tf = hpx::split_future(std::move(ft));
    *((void **)outputs[0]) =
        (void *)new hpx::shared_future<void *>(std::move(hpx::get<0>(tf)));
    *((void **)outputs[1]) =
        (void *)new hpx::shared_future<void *>(std::move(hpx::get<1>(tf)));
    *((void **)outputs[2]) =
        (void *)new hpx::shared_future<void *>(std::move(hpx::get<2>(tf)));
    fut_allocated.push_back(*((void **)outputs[0]));
    fut_allocated.push_back(*((void **)outputs[1]));
    fut_allocated.push_back(*((void **)outputs[2]));
    break;
  }
  default:
    HPX_THROW_EXCEPTION(hpx::no_success, "_dfr_create_async_task",
                        "Error: number of task outputs not supported.");
  }
}

/***************************/
/* JIT execution support.  */
/***************************/
void _dfr_try_initialize();
namespace {
static bool dfr_required_p = false;
static bool is_jit_p = false;
} // namespace
bool _dfr_set_required(bool is_required) {
  dfr_required_p = is_required;
  if (dfr_required_p)
    _dfr_try_initialize();
  return true;
}
void _dfr_set_jit(bool is_jit) { is_jit_p = is_jit; }
bool _dfr_is_jit() { return is_jit_p; }

static inline bool _dfr_is_root_node_impl() {
  static bool is_root_node_p =
      (!dfr_required_p || (hpx::find_here() == hpx::find_root_locality()));
  return is_root_node_p;
}

bool _dfr_is_root_node() { return _dfr_is_root_node_impl(); }

void _dfr_register_work_function(wfnptr wfn) {
  _dfr_node_level_work_function_registry->registerAnonymousWorkFunction(
      (void *)wfn);
}

/************************************/
/*  Initialization & Finalization.  */
/************************************/
static inline void _dfr_stop_impl() {
  if (_dfr_is_root_node())
    hpx::apply([]() { hpx::finalize(); });
  hpx::stop();
  if (!_dfr_is_root_node())
    exit(EXIT_SUCCESS);
}

static inline void _dfr_start_impl(int argc, char *argv[]) {
  dl_handle = dlopen(nullptr, RTLD_NOW);
  if (argc == 0) {
    unsigned long nCores, nOMPThreads, nHPXThreads;
    hwloc_topology_t topology;
    hwloc_topology_init(&topology);
    hwloc_topology_set_all_types_filter(topology, HWLOC_TYPE_FILTER_KEEP_NONE);
    hwloc_topology_set_type_filter(topology, HWLOC_OBJ_CORE,
                                   HWLOC_TYPE_FILTER_KEEP_ALL);
    hwloc_topology_load(topology);
    nCores = hwloc_get_nbobjs_by_type(topology, HWLOC_OBJ_CORE);
    if (nCores < 1)
      nCores = 1;
    nOMPThreads = 1;
    char *env = getenv("OMP_NUM_THREADS");
    if (env != nullptr) {
      nOMPThreads = strtoul(env, NULL, 10);
      if (nOMPThreads == 0)
        nOMPThreads = 1;
      if (nOMPThreads >= nCores)
        nOMPThreads = nCores;
    }
    std::string numOMPThreads = std::to_string(nOMPThreads);
    setenv("OMP_NUM_THREADS", numOMPThreads.c_str(), 0);
    nHPXThreads = nCores + 1 - nOMPThreads;
    env = getenv("DFR_NUM_THREADS");
    if (env != nullptr)
      nHPXThreads = strtoul(env, NULL, 10);
    nHPXThreads = (nHPXThreads) ? nHPXThreads : 1;
    std::string numHPXThreads = std::to_string(nHPXThreads);
    char *_argv[3] = {const_cast<char *>("__dummy_dfr_HPX_program_name__"),
                      const_cast<char *>("--hpx:threads"),
                      const_cast<char *>(numHPXThreads.c_str())};
    int _argc = 3;
    hpx::start(nullptr, _argc, _argv);
  } else {
    hpx::start(nullptr, argc, argv);
  }

  // Instantiate on each node
  new KeyManager<LweBootstrapKey_u64>();
  new KeyManager<LweKeyswitchKey_u64>();
  new WorkFunctionRegistry();
  _dfr_jit_workfunction_registration_barrier = new hpx::lcos::barrier(
      "wait_register_remote_work_functions", hpx::get_num_localities().get(),
      hpx::get_locality_id());
  _dfr_jit_phase_barrier = new hpx::lcos::barrier(
      "phase_barrier", hpx::get_num_localities().get(), hpx::get_locality_id());

  if (_dfr_is_root_node()) {
    // Create compute server components on each node - from the root
    // node only - and the corresponding compute client on the root
    // node.
    auto num_nodes = hpx::get_num_localities().get();
    gcc = hpx::new_<GenericComputeClient[]>(
              hpx::default_layout(hpx::find_all_localities()), num_nodes)
              .get();
  }
}

/*  Start/stop functions to be called from within user code (or during
    JIT invocation).  These serve to pause/resume the runtime
    scheduler and to clean up used resources.  */
void _dfr_start() {
  // The first invocation will initialise the runtime. As each call to
  // _dfr_start is matched with _dfr_stop, if this is not hte first,
  // we need to resume the HPX runtime.
  uint64_t uninitialised = 0;
  uint64_t active = 1;
  uint64_t suspended = 2;
  if (init_guard.compare_exchange_strong(uninitialised, active))
    _dfr_start_impl(0, nullptr);
  else if (init_guard.compare_exchange_strong(suspended, active))
    hpx::resume();

  // If this is not the root node in a non-JIT execution, then this
  // node should only run the scheduler for any incoming work until
  // termination is flagged. If this is JIT, we need to run the
  // cancelled function which registers the work functions.
  if (!_dfr_is_root_node() && !_dfr_is_jit())
    _dfr_stop_impl();

  // TODO: conditional -- If this is the root node, and this is JIT
  // execution, we need to wait for the compute nodes to compile and
  // register work functions
  if (_dfr_is_root_node() && _dfr_is_jit()) {
    _dfr_jit_workfunction_registration_barrier->wait();
  }
}

// This function cannot be used to terminate the runtime as it is
// non-decidable if another computation phase will follow. Instead the
// _dfr_terminate function provides this facility and is normally
// called on exit from "main" when not using the main wrapper library.
void _dfr_stop() {
  // Non-root nodes synchronize here with the root to mark the point
  // where the root is free to send work out.
  // TODO: optimize this by moving synchro to local remote nodes
  // waiting in the scheduler for registration.
  if (!_dfr_is_root_node() /*&& _dfr_is_jit() /** implicitly true*/) {
    _dfr_jit_workfunction_registration_barrier->wait();
  }

  // The barrier is only needed to synchronize the different
  // computation phases when the compute nodes need to generate and
  // register new work functions in each phase.

  // TODO: this barrier may be removed based on how work function
  // registration is handled - but it is unlikely to result in much
  // gain as the root node would be waiting for the end of computation
  // on all remote nodes before reaching here anyway (dataflow
  // dependences).
  if (_dfr_is_jit()) {
    _dfr_jit_phase_barrier->wait();
  }

  // TODO: this can be removed along with the matching hpx::resume if
  // their overhead is larger than the benefit of pausing worker
  // threads outside of parallel regions - to be tested.
  uint64_t active = 1;
  uint64_t suspended = 2;
  if (init_guard.compare_exchange_strong(active, suspended))
    hpx::suspend();

  // TODO: until we have better unique identifiers for keys it is
  // safer to drop them in-between phases.
  _dfr_node_level_bsk_manager->clear_keys();
  _dfr_node_level_ksk_manager->clear_keys();

  while (!new_allocated.empty()) {
    delete[] static_cast<char *>(new_allocated.front());
    new_allocated.pop_front();
  }
  while (!fut_allocated.empty()) {
    delete static_cast<hpx::shared_future<void *> *>(fut_allocated.front());
    fut_allocated.pop_front();
  }
  while (!m_allocated.empty()) {
    free(m_allocated.front());
    m_allocated.pop_front();
  }
}

void _dfr_try_initialize() {
  // Initialize and immediately suspend the HPX runtime if not yet done.
  uint64_t uninitialised = 0;
  uint64_t suspended = 2;
  if (init_guard.compare_exchange_strong(uninitialised, suspended)) {
    _dfr_start_impl(0, nullptr);
    hpx::suspend();
  }
}

void _dfr_terminate() {
  uint64_t active = 1;
  uint64_t suspended = 2;
  uint64_t terminated = 3;
  if (init_guard.compare_exchange_strong(suspended, active))
    hpx::resume();
  if (init_guard.compare_exchange_strong(active, terminated))
    _dfr_stop_impl();
}

/*******************/
/*  Main wrapper.  */
/*******************/
extern "C" {
extern int main(int argc, char *argv[]) __attribute__((weak));
extern int __real_main(int argc, char *argv[]) __attribute__((weak));
int __wrap_main(int argc, char *argv[]) {
  int r;

  _dfr_try_initialize();
  // Run the actual main function. Within there should be a call to
  // _dfr_start to resume execution of the HPX scheduler if needed.
  r = __real_main(argc, argv);
  _dfr_terminate();

  return r;
}
}

/**********************/
/*  Debug interface.  */
/**********************/
size_t _dfr_debug_get_node_id() { return hpx::get_locality_id(); }

size_t _dfr_debug_get_worker_id() { return hpx::get_worker_thread_num(); }

void _dfr_debug_print_task(const char *name, size_t inputs, size_t outputs) {
  // clang-format off
  hpx::cout << "Task \"" << name << "\t\""
	    << " [" << inputs << " inputs, " << outputs << " outputs]"
	    << "  Executing on Node/Worker: " << _dfr_debug_get_node_id()
	    << " / " << _dfr_debug_get_worker_id() << "\n" << std::flush;
  // clang-format on
}

/// Generic utility function for printing debug info
void _dfr_print_debug(size_t val) {
  hpx::cout << "_dfr_print_debug : " << val << "\n" << std::flush;
}

#else // CONCRETELANG_PARALLEL_EXECUTION_ENABLED

#include "concretelang/Runtime/DFRuntime.hpp"

bool _dfr_set_required(bool is_required) { return !is_required; }
void _dfr_set_jit(bool) {}
bool _dfr_is_root_node() { return true; }
void _dfr_terminate() {}
#endif
