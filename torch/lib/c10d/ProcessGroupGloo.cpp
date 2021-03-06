#include "ProcessGroupGloo.hpp"

#include <gloo/allgather.h>
#include <gloo/allreduce.h>
#include <gloo/barrier.h>
#include <gloo/broadcast.h>
#include <gloo/gather.h>
#include <gloo/reduce.h>
#include <gloo/scatter.h>

#ifdef USE_CUDA
#include <ATen/cuda/CUDAEvent.h>
#include <ATen/cuda/CUDAGuard.h>
#include <ATen/cuda/CUDAStream.h>
#include <ATen/cuda/Exceptions.h>
#include <ATen/cuda/PinnedMemoryAllocator.h>
#endif

#include <gloo/rendezvous/context.h>
#include <gloo/transport/tcp/device.h>

#define GENERATE_ALL_TYPES(type, func, args...)        \
  switch (type) {                                      \
    case ::at::ScalarType::Float:                      \
      func<float>(args);                               \
      break;                                           \
    case ::at::ScalarType::Double:                     \
      func<double>(args);                              \
      break;                                           \
    case ::at::ScalarType::Half:                       \
      func<gloo::float16>(args);                       \
      break;                                           \
    case ::at::ScalarType::Char:                       \
      func<int8_t>(args);                              \
      break;                                           \
    case ::at::ScalarType::Byte:                       \
      func<uint8_t>(args);                             \
      break;                                           \
    case ::at::ScalarType::Int:                        \
      func<int32_t>(args);                             \
      break;                                           \
    case ::at::ScalarType::Long:                       \
      func<int64_t>(args);                             \
      break;                                           \
    default:                                           \
      throw std::runtime_error("Invalid scalar type"); \
  }

namespace c10d {

namespace {

// Wrap c10d store as Gloo store
class GlooStore : public ::gloo::rendezvous::Store {
 public:
  GlooStore(const std::shared_ptr<::c10d::Store>& store) : store_(store) {}

  void set(const std::string& key, const std::vector<char>& value) override {
    std::vector<uint8_t> tmp(value.begin(), value.end());
    store_->set(key, tmp);
  }

  std::vector<char> get(const std::string& key) override {
    auto value = store_->get(key);
    return std::vector<char>(value.begin(), value.end());
  }

  void wait(const std::vector<std::string>& keys) override {
    store_->wait(keys, Store::kDefaultTimeout);
  }

  void wait(
      const std::vector<std::string>& keys,
      const std::chrono::milliseconds& timeout) override {
    store_->wait(keys, timeout);
  }

 protected:
  std::shared_ptr<::c10d::Store> store_;
};

typedef void (*ReduceFunc)(void*, const void*, const void*, size_t);

template <typename T>
ReduceFunc toFunction(const ReduceOp& r) {
  switch (r) {
    case ReduceOp::SUM:
      return ReduceFunc(&::gloo::sum<T>);
    case ReduceOp::PRODUCT:
      return ReduceFunc(&::gloo::product<T>);
    case ReduceOp::MIN:
      return ReduceFunc(&::gloo::min<T>);
    case ReduceOp::MAX:
      return ReduceFunc(&::gloo::max<T>);
    case ReduceOp::UNUSED:
      break;
  }

  throw std::runtime_error("Unhandled ReduceOp");
}

template <typename T, typename O>
void setInputs(O& opts, std::vector<at::Tensor>& tensors) {
  opts.setInputs(getDataPointers<T>(tensors), tensors[0].numel());
}

template <typename T, typename O>
void setInput(O& opts, at::Tensor& tensor) {
  opts.setInput(getDataPointer<T>(tensor), tensor.numel());
}

template <typename T, typename O>
void setOutputs(O& opts, std::vector<at::Tensor>& tensors) {
  opts.setOutputs(getDataPointers<T>(tensors), tensors[0].numel());
}

template <typename T, typename O>
void setOutput(O& opts, at::Tensor& tensor) {
  opts.setOutput(getDataPointer<T>(tensor), tensor.numel());
}

#ifdef USE_CUDA

at::Tensor pinnedLike(at::Tensor& tensor) {
  auto& type = tensor.type().toBackend(at::Backend::CPU);
  auto* allocator = at::cuda::getPinnedMemoryAllocator();
  return type.tensorWithAllocator(tensor.sizes(), tensor.strides(), allocator);
}

// This function initializes a vector of CUDA streams, one for every
// tensor in the input vector, and ensures that these streams are
// synchronized with the current default streams. This is needed so
// that new work on the new streams is serialized w.r.t. all operations
// on the tensors in the input vector.
void initializeStreamsEvents(
    std::vector<at::Tensor>& inputs,
    std::vector<at::cuda::CUDAStream>& streams,
    std::vector<at::cuda::CUDAEvent>& events) {
  at::cuda::OptionalCUDAGuard guard;
  streams.reserve(inputs.size());
  events.resize(inputs.size());
  for (size_t i = 0; i < inputs.size(); i++) {
    guard.set_index(inputs[i].get_device());
    // Record event on current stream
    events[i].record(at::cuda::getCurrentCUDAStream());
    // Get a non-default stream to execute asynchronous CUDA operations
    // on for this input. This ensures that the default stream used
    // by the caller is not occupied by c10d related operations.
    streams.push_back(at::cuda::getStreamFromPool(
        /* isHighPriority */ true, inputs[i].get_device()));
    // Ensure the new stream is synchronized with the current stream.
    events[i].block(streams[i]);
  }
}

#endif

} // namespace

ProcessGroupGloo::SendWork::SendWork(
    at::Tensor& tensor,
    std::unique_ptr<::gloo::transport::UnboundBuffer> buffer)
    : tensor_(tensor), buffer_(std::move(buffer)) {}

void ProcessGroupGloo::SendWork::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  try {
    buffer_->waitSend();
  } catch (...) {
    exception_ = std::current_exception();
  }

  completed_ = true;
  if (exception_) {
    std::rethrow_exception(exception_);
  }
}

ProcessGroupGloo::RecvWork::RecvWork(
    at::Tensor& tensor,
    std::unique_ptr<::gloo::transport::UnboundBuffer> buffer)
    : tensor_(tensor), buffer_(std::move(buffer)), srcRank_(-1) {}

int ProcessGroupGloo::RecvWork::sourceRank() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return srcRank_;
}

void ProcessGroupGloo::RecvWork::wait() {
  std::unique_lock<std::mutex> lock(mutex_);
  try {
    buffer_->waitRecv(&srcRank_);
  } catch (...) {
    exception_ = std::current_exception();
  }

  completed_ = true;
  if (exception_) {
    std::rethrow_exception(exception_);
  }
}

ProcessGroupGloo::Options::Options()
    : timeout(std::chrono::milliseconds(10 * 1000)),
      threads(2),
      cacheNumAlgorithmEntries(1) {}

ProcessGroupGloo::ProcessGroupGloo(
    const std::shared_ptr<Store>& store,
    int rank,
    int size,
    Options options)
    : ProcessGroup(rank, size),
      store_(new GlooStore(store)),
      stop_(false),
      collectiveCounter_(0) {
  auto& devices = options.devices;
  if (devices.empty()) {
    throw std::runtime_error("No device(s) specified");
  }

  for (auto& device : options.devices) {
    auto context = std::make_shared<::gloo::rendezvous::Context>(rank_, size_);
    context->setTimeout(options.timeout);
    context->connectFullMesh(*store_, device);
    contexts_.push_back(std::move(context));
  }

  // Every worker thread stores the AsyncWork object it's currently
  // working on in the workInProgress_ vector. It must have size equal
  // to the number of workers such that they can simply index into it
  // using the worker index they are started with.
  workInProgress_.resize(options.threads);

  threads_.resize(options.threads);
  for (size_t i = 0; i < threads_.size(); i++) {
    threads_[i] = std::thread(&ProcessGroupGloo::runLoop, this, i);
  }
}

ProcessGroupGloo::~ProcessGroupGloo() {
  std::unique_lock<std::mutex> lock(workMutex_);
  while (!workQueue_.empty()) {
    workConsumeCV_.wait(lock);
  }

  // Queue is empty, signal stop
  stop_ = true;

  // Release lock to allow threads to terminate
  workProduceCV_.notify_all();
  lock.unlock();

  // Wait for worker threads to terminate
  for (auto& thread : threads_) {
    thread.join();
  }
}

uint32_t ProcessGroupGloo::nextTag() {
  return collectiveCounter_++;
}

void ProcessGroupGloo::runLoop(int workerIndex) {
  std::unique_lock<std::mutex> lock(workMutex_);

  while (!stop_) {
    if (workQueue_.empty()) {
      workProduceCV_.wait(lock);
      continue;
    }

    auto work = std::move(workQueue_.front());
    workQueue_.pop_front();
    workConsumeCV_.notify_one();

    workInProgress_[workerIndex] = work;
    lock.unlock();
    AsyncWork::execute(std::move(work));
    lock.lock();
    workInProgress_[workerIndex] = nullptr;
  }
}

void ProcessGroupGloo::enqueue(std::shared_ptr<AsyncWork> work) {
  std::unique_lock<std::mutex> lock(workMutex_);
  workQueue_.push_back(std::move(work));
  workProduceCV_.notify_one();
}

namespace {

class AsyncBroadcastWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncBroadcastWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      uint32_t tag)
      : context(context),
        inputs(inputs),
        rootRank(rootRank),
        rootTensor(rootTensor),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  const int rootRank;
  const int rootTensor;
  const uint32_t tag;

  void broadcast(at::Tensor& tensor) {
    const auto& scalarType = tensor.scalar_type();
    gloo::BroadcastOptions opts(context);
    opts.setRoot(rootRank);
    opts.setTag(tag);
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, tensor);
    gloo::broadcast(opts);
  }

  void run() override {
    broadcast(inputs[rootTensor]);

    // Copy to non-root tensors
    for (size_t i = 0; i < inputs.size(); i++) {
      if (i == static_cast<size_t>(rootTensor)) {
        continue;
      }
      inputs[i].copy_(inputs[rootTensor]);
    }
  }
};

#ifdef USE_CUDA

class AsyncBroadcastCUDAWork : public AsyncBroadcastWork {
 public:
  AsyncBroadcastCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      uint32_t tag)
      : AsyncBroadcastWork(context, inputs, rootRank, rootTensor, tag) {
    initializeStreamsEvents(inputs, streams, events);

    // Create pinned host side tensors.
    tmp = pinnedLike(inputs[rootTensor]);
    at::cuda::OptionalCUDAStreamGuard guard;
    if (context->rank == rootRank) {
      guard.reset_stream(streams[rootTensor]);
      tmp.copy_(inputs[rootTensor], /* non_blocking */ true);
    }
  }

  void run() override {
    at::cuda::OptionalCUDAStreamGuard guard;

    // Synchronize with copy operation if applicable.
    if (context->rank == rootRank) {
      guard.reset_stream(streams[rootTensor]);
      AT_CUDA_CHECK(cudaStreamSynchronize(streams[rootTensor]));
    }

    // Run broadcast on host side tensors.
    broadcast(tmp);

    // Kick off copy back to the CUDA tensors.
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(streams[i]);
      inputs[i].copy_(tmp, /* non_blocking */ true);
      events[i].record(streams[i]);
    }
  }

  void synchronize() override {
    at::cuda::OptionalCUDAGuard guard;

    // Synchronize with the copy back to CUDA tensors.
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.set_index(inputs[i].get_device());
      events[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  at::Tensor tmp;
  std::vector<at::cuda::CUDAStream> streams;
  std::vector<at::cuda::CUDAEvent> events;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::broadcast(
    std::vector<at::Tensor>& inputs,
    const BroadcastOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::broadcast: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertRootTensor(invalidArgument, opts.rootTensor, inputs.size());
  assertDense(invalidArgument, inputs);
  assertTypeAndSizesMatch(invalidArgument, inputs);

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncBroadcastWork> work;
  auto& context = contexts_[0];
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncBroadcastWork>(
        context, inputs, opts.rootRank, opts.rootTensor, nextTag());
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncBroadcastCUDAWork>(
        context, inputs, opts.rootRank, opts.rootTensor, nextTag());
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }

  enqueue(work);
  return work;
}

namespace {

class AsyncAllreduceWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncAllreduceWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      ReduceOp reduceOp,
      uint32_t tag)
      : context(context), inputs(inputs), reduceOp(reduceOp), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  const ReduceOp reduceOp;
  const uint32_t tag;

  void allreduce(std::vector<at::Tensor>& tensors) {
    const auto& scalarType = tensors[0].scalar_type();
    gloo::AllreduceOptions opts(context);
    opts.setReduceFunction(getFunction(scalarType, reduceOp));
    opts.setTag(tag);
    GENERATE_ALL_TYPES(scalarType, setOutputs, opts, tensors);
    gloo::allreduce(opts);
  }

  void run() override {
    allreduce(inputs);
  }

  template <typename T>
  void getFunction(gloo::AllreduceOptions::Func& fn, const ReduceOp op) {
    fn = toFunction<T>(op);
  }

  gloo::AllreduceOptions::Func getFunction(
      const at::ScalarType& dtype,
      const ReduceOp op) {
    gloo::AllreduceOptions::Func fn;
    GENERATE_ALL_TYPES(dtype, getFunction, fn, op);
    return fn;
  }
};

#ifdef USE_CUDA

class AsyncAllreduceCUDAWork : public AsyncAllreduceWork {
 public:
  AsyncAllreduceCUDAWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      ReduceOp reduceOp,
      uint32_t tag)
      : AsyncAllreduceWork(context, inputs, reduceOp, tag) {
    initializeStreamsEvents(inputs, streams, events);

    // Kick off copy from CUDA tensors to pinned CPU tensors.
    tmp.reserve(inputs.size());
    at::cuda::OptionalCUDAStreamGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.reset_stream(streams[i]);
      tmp.push_back(pinnedLike(inputs[i]).copy_(inputs[i], true));
    }
  }

  void run() override {
    // Synchronize with copy operations.
    at::cuda::OptionalCUDAGuard device_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      device_guard.set_index(inputs[i].get_device());
      AT_CUDA_CHECK(cudaStreamSynchronize(streams[i]));
    }

    // Run allreduce on host side tensors.
    allreduce(tmp);

    // Kick off copy back to the CUDA tensors.
    at::cuda::OptionalCUDAStreamGuard stream_guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      stream_guard.reset_stream(streams[i]);
      inputs[i].copy_(tmp[i], /* non_blocking */ true);
      events[i].record(streams[i]);
    }
  }

  void synchronize() override {
    // Synchronize with the copy back to CUDA tensors.
    at::cuda::OptionalCUDAGuard guard;
    for (size_t i = 0; i < inputs.size(); i++) {
      guard.set_index(static_cast<at::DeviceIndex>(inputs[i].get_device()));
      events[i].block(at::cuda::getCurrentCUDAStream());
    }
  }

  std::vector<at::Tensor> tmp;
  std::vector<at::cuda::CUDAStream> streams;
  std::vector<at::cuda::CUDAEvent> events;
};

#endif

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::allreduce(
    std::vector<at::Tensor>& inputs,
    const AllreduceOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::allreduce: " + msg);
  };

  assertNonEmpty(invalidArgument, inputs);
  assertDense(invalidArgument, inputs);
  assertTypeAndSizesMatch(invalidArgument, inputs);

  const auto& device = inputs[0].device();
  switch (device.type()) {
    case at::kCPU:
#ifdef USE_CUDA
    case at::kCUDA:
#endif
      break;
    default:
      invalidArgument("unsupported device type");
  }

  std::shared_ptr<AsyncAllreduceWork> work;
  auto& context = contexts_[0];
  if (device.type() == at::kCPU) {
    work = std::make_shared<AsyncAllreduceWork>(
        context, inputs, opts.reduceOp, nextTag());
#ifdef USE_CUDA
  } else if (device.type() == at::kCUDA) {
    work = std::make_shared<AsyncAllreduceCUDAWork>(
        context, inputs, opts.reduceOp, nextTag());
#endif
  } else {
    throw std::runtime_error("Invalid backend");
  }

  enqueue(work);
  return work;
}

namespace {

class AsyncReduceWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncReduceWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& inputs,
      int rootRank,
      int rootTensor,
      ReduceOp reduceOp,
      uint32_t tag)
      : context(context),
        inputs(inputs),
        rootRank(rootRank),
        rootTensor(rootTensor),
        reduceOp(reduceOp),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> inputs;
  const int rootRank;
  const int rootTensor;
  const ReduceOp reduceOp;
  const uint32_t tag;

  void run() override {
    const auto& scalarType = inputs[0].scalar_type();
    gloo::ReduceOptions opts(context);
    opts.setRoot(rootRank);
    opts.setTag(tag);
    opts.setReduceFunction(getFunction(scalarType, reduceOp));
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, inputs[0]);
    gloo::reduce(opts);
  }

 protected:
  template <typename T>
  void getFunction(gloo::ReduceOptions::Func& fn, const ReduceOp op) {
    fn = toFunction<T>(op);
  }

  gloo::ReduceOptions::Func getFunction(
      const at::ScalarType& dtype,
      const ReduceOp op) {
    gloo::ReduceOptions::Func fn;
    GENERATE_ALL_TYPES(dtype, getFunction, fn, op);
    return fn;
  }
};

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::reduce(
    std::vector<at::Tensor>& inputs,
    const ReduceOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::reduce: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertRootTensor(invalidArgument, opts.rootTensor, inputs.size());
  assertSingleElement(invalidArgument, inputs);
  assertDense(invalidArgument, inputs);
  assertCPU(invalidArgument, inputs);

  auto work = std::make_shared<AsyncReduceWork>(
      contexts_[0],
      inputs,
      opts.rootRank,
      opts.rootTensor,
      opts.reduceOp,
      nextTag());
  enqueue(work);
  return work;
}

namespace {

class AsyncAllgatherWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncAllgatherWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      uint32_t tag)
      : context(context), outputs(outputs), inputs(inputs), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<std::vector<at::Tensor>> outputs;
  std::vector<at::Tensor> inputs;
  const uint32_t tag;

  void run() override {
    const auto& scalarType = inputs[0].scalar_type();
    gloo::AllgatherOptions opts(context);
    opts.setTag(tag);

    // Use single flattened input tensor.
    at::Tensor flatInputTensor = flattenDenseTensors(inputs);
    GENERATE_ALL_TYPES(scalarType, setInput, opts, flatInputTensor);

    // Use single flat output tensor.
    // The first dimension corresponds to the index into outputs[N],
    // so copying into the actual output later is easy.
    at::Tensor flatOutputTensor = newLikeFlat(outputs[0]);
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, flatOutputTensor);
    gloo::allgather(opts);

    // Unflatten into output tensors.
    for (size_t i = 0; i < outputs.size(); i++) {
      for (size_t j = 0; j < outputs[i].size(); j++) {
        outputs[i][j].copy_(flatOutputTensor[j]);
      }
    }
  }
};

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::allgather(
    std::vector<std::vector<at::Tensor>>& outputs,
    std::vector<at::Tensor>& inputs,
    const AllgatherOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::allgather: " + msg);
  };

  if (inputs.size() == 0) {
    invalidArgument("requires non-empty input tensor list");
  }

  if (inputs.size() != outputs.size()) {
    invalidArgument(
        "requires input/output tensor lists to have the same length");
  }

  for (size_t i = 0; i < outputs.size(); i++) {
    const auto expected = inputs.size() * getSize();
    const auto actual = outputs[i].size();
    if (actual != expected) {
      invalidArgument(
          "invalid output tensor list at index " + std::to_string(i) +
          " (expected length " + std::to_string(expected) + ", got " +
          std::to_string(actual) + ")");
    }
  }

  assertDense(invalidArgument, inputs);
  assertCPU(invalidArgument, inputs);

  // Expect all input/output tensors to have the same type and sizes
  const auto& type = inputs[0].type();
  const auto& sizes = inputs[0].sizes();
  assertTypeAndSizesMatch(invalidArgument, inputs, type, sizes);
  for (size_t i = 0; i < outputs.size(); i++) {
    assertTypeAndSizesMatch(invalidArgument, outputs[i], type, sizes);
  }

  auto work = std::make_shared<AsyncAllgatherWork>(
      contexts_[0], outputs, inputs, nextTag());
  enqueue(work);
  return work;
}

namespace {

class AsyncGatherWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncGatherWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::vector<at::Tensor>>& outputs,
      std::vector<at::Tensor>& inputs,
      int root,
      uint32_t tag)
      : context(context),
        outputs(outputs),
        inputs(inputs),
        root(root),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<std::vector<at::Tensor>> outputs;
  std::vector<at::Tensor> inputs;
  const int root;
  const uint32_t tag;

  void run() override {
    const auto scalarType = inputs[0].type().scalarType();
    gloo::GatherOptions opts(context);
    opts.setRoot(root);
    opts.setTag(tag);

    // Set single temporary tensor on root process.
    // This is later scattered to the separate output tensors.
    at::Tensor flatOutputTensor;
    if (context->rank == root) {
      flatOutputTensor = newLikeFlat(outputs[0]);
      GENERATE_ALL_TYPES(scalarType, setOutput, opts, flatOutputTensor);
    }

    // Set single input tensor on all processes.
    GENERATE_ALL_TYPES(scalarType, setInput, opts, inputs[0]);
    gloo::gather(opts);

    // Unflatten into output tensors on root process.
    if (context->rank == root) {
      for (size_t i = 0; i < outputs[0].size(); i++) {
        outputs[0][i].copy_(flatOutputTensor[i]);
      }
    }
  }
};

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::gather(
    std::vector<std::vector<at::Tensor>>& outputs,
    std::vector<at::Tensor>& inputs,
    const GatherOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::gather: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertSingleElementInput(invalidArgument, inputs);
  assertDense(invalidArgument, inputs);
  assertCPU(invalidArgument, inputs);

  if (getRank() == opts.rootRank) {
    if (outputs.size() != 1 ||
        outputs[0].size() != static_cast<size_t>(getSize())) {
      invalidArgument(
          "requires a single-element output list "
          "containing a list with <size> tensors");
    }

    const auto& type = inputs[0].type();
    const auto& sizes = inputs[0].sizes();
    assertTypeAndSizesMatch(invalidArgument, outputs[0], type, sizes);
  } else {
    if (outputs.size() != 0) {
      invalidArgument("requires empty output on non-root");
    }
  }

  auto work = std::make_shared<AsyncGatherWork>(
      contexts_[0], outputs, inputs, opts.rootRank, nextTag());
  enqueue(work);
  return work;
}

namespace {

class AsyncScatterWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncScatterWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<at::Tensor>& outputs,
      std::vector<std::vector<at::Tensor>>& inputs,
      int root,
      uint32_t tag)
      : context(context),
        outputs(outputs),
        inputs(inputs),
        root(root),
        tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<at::Tensor> outputs;
  std::vector<std::vector<at::Tensor>> inputs;
  const int root;
  const uint32_t tag;

  void run() override {
    const auto scalarType = outputs[0].type().scalarType();
    gloo::ScatterOptions opts(context);
    opts.setRoot(root);
    opts.setTag(tag);

    // Set list of input tensors on root process
    if (context->rank == root) {
      GENERATE_ALL_TYPES(scalarType, setInputs, opts, inputs[0]);
    }

    // Set single output tensor on all processes
    GENERATE_ALL_TYPES(scalarType, setOutput, opts, outputs[0]);
    gloo::scatter(opts);
  }
};

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::scatter(
    std::vector<at::Tensor>& outputs,
    std::vector<std::vector<at::Tensor>>& inputs,
    const ScatterOptions& opts) {
  static auto invalidArgument = [](const std::string& msg) {
    throw std::invalid_argument("ProcessGroupGloo::scatter: " + msg);
  };

  assertRootRank(invalidArgument, opts.rootRank, size_);
  assertSingleElementOutput(invalidArgument, outputs);
  assertDense(invalidArgument, outputs);
  assertCPU(invalidArgument, outputs);

  if (getRank() == opts.rootRank) {
    if (inputs.size() != 1 ||
        inputs[0].size() != static_cast<size_t>(getSize())) {
      invalidArgument(
          "requires a single-element input list "
          "containing a list with <size> tensors");
    }
    const auto& type = outputs[0].type();
    const auto& sizes = outputs[0].sizes();
    assertTypeAndSizesMatch(invalidArgument, inputs[0], type, sizes);
  } else {
    if (inputs.size() != 0) {
      invalidArgument("requires empty input on non-root");
    }
  }

  auto work = std::make_shared<AsyncScatterWork>(
      contexts_[0], outputs, inputs, opts.rootRank, nextTag());
  enqueue(work);
  return work;
}

at::Tensor& checkSingleTensor(std::vector<at::Tensor>& tensors) {
  if (tensors.size() != 1) {
    throw std::runtime_error("ProcessGroupGloo::send takes a single tensor");
  }
  auto& tensor = tensors[0];
  if (!tensor.is_contiguous()) {
    throw std::runtime_error("input tensor has to be contiguous");
  }
  if (tensor.is_sparse()) {
    throw std::runtime_error("input tensor has to be dense");
  }
  return tensor;
}

uint32_t checkTag(int32_t tag) {
  if (tag < 0) {
    throw std::runtime_error("Tag must be >= 0");
  }
  return (uint32_t)tag;
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::send(
    std::vector<at::Tensor>& tensors,
    int dstRank,
    int tag) {
  auto& tensor = checkSingleTensor(tensors);
  auto utag = checkTag(tag);
  auto ptr = tensor.data_ptr();
  auto size = tensor.numel() * tensor.type().elementSizeInBytes();

  // Construct unbound buffer.
  auto& context = contexts_[0];
  auto buf = context->createUnboundBuffer(ptr, size);
  buf->send(dstRank, utag);

  // The work captures the tensor to prevent it being deallocated and
  // the unbound buffer to synchronize on completion of the send.
  return std::make_shared<SendWork>(tensor, std::move(buf));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::recv(
    std::vector<at::Tensor>& tensors,
    int srcRank,
    int tag) {
  auto& tensor = checkSingleTensor(tensors);
  auto utag = checkTag(tag);
  auto ptr = tensor.data_ptr();
  auto size = tensor.numel() * tensor.type().elementSizeInBytes();

  // Construct unbound buffer.
  auto& context = contexts_[0];
  auto buf = context->createUnboundBuffer(ptr, size);
  buf->recv(srcRank, utag);

  // The work captures the tensor to prevent it being deallocated and
  // the unbound buffer to synchronize on completion of the recv.
  return std::make_shared<RecvWork>(tensor, std::move(buf));
}

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::recvAnysource(
    std::vector<at::Tensor>& tensors,
    int tag) {
  auto& tensor = checkSingleTensor(tensors);
  auto utag = checkTag(tag);
  auto ptr = tensor.data_ptr();
  auto size = tensor.numel() * tensor.type().elementSizeInBytes();

  // Construct unbound buffer.
  auto& context = contexts_[0];
  auto buf = context->createUnboundBuffer(ptr, size);

  // Build list of ranks that this operation can recv from. In these
  // bindings we don't differentiate between ranks and can receive
  // from any other process in the group.
  std::vector<int> srcRanks;
  srcRanks.resize(size_);
  for (auto i = 0; i < size_; i++) {
    srcRanks.push_back(i);
  }

  buf->recv(srcRanks, utag);

  // The work captures the tensor to prevent it being deallocated and
  // the unbound buffer to synchronize on completion of the recv.
  return std::make_shared<RecvWork>(tensor, std::move(buf));
}

namespace {

class AsyncBarrierWork : public ProcessGroupGloo::AsyncWork {
 public:
  AsyncBarrierWork(
      const std::shared_ptr<gloo::Context>& context,
      std::vector<std::weak_ptr<AsyncWork>> priorWork,
      uint32_t tag)
      : context(context), priorWork(std::move(priorWork)), tag(tag) {}

  std::shared_ptr<gloo::Context> context;
  std::vector<std::weak_ptr<AsyncWork>> priorWork;
  const uint32_t tag;

  void run() override {
    // Wait on prior work to complete
    for (auto& weakWork : priorWork) {
      auto work = weakWork.lock();
      if (work) {
        work->wait();
      }
    }

    gloo::BarrierOptions opts(context);
    opts.setTag(tag);
    gloo::barrier(opts);
  }
};

} // namespace

std::shared_ptr<ProcessGroup::Work> ProcessGroupGloo::barrier(
    const BarrierOptions& opts) {
  std::vector<std::weak_ptr<AsyncWork>> priorWork;

  // Snapshot all in progress and pending work as weak_ptr.
  // When executing a barrier, we need to ensure that all prior work
  // has completed before completing itself.
  {
    std::unique_lock<std::mutex> lock(workMutex_);
    priorWork.insert(
        priorWork.end(), workInProgress_.begin(), workInProgress_.end());
    priorWork.insert(priorWork.end(), workQueue_.begin(), workQueue_.end());
  }

  auto work = std::make_shared<AsyncBarrierWork>(
      contexts_[0], std::move(priorWork), nextTag());
  enqueue(work);
  return work;
}

std::unordered_map<int, int> ProcessGroupGloo::getGroupRank() {
  throw std::runtime_error("ProcessGroupGloo does not support getGroupRank");
}

} // namespace c10d
