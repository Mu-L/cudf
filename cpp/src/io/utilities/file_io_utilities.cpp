/*
 * Copyright (c) 2021, NVIDIA CORPORATION.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include "file_io_utilities.hpp"
#include <cudf/detail/utilities/integer_utils.hpp>
#include <io/utilities/config_utils.hpp>

#include <rmm/device_buffer.hpp>

#include <dlfcn.h>

#include <fstream>
#include <numeric>

namespace cudf {
namespace io {
namespace detail {

size_t get_file_size(int file_descriptor)
{
  struct stat st;
  CUDF_EXPECTS(fstat(file_descriptor, &st) != -1, "Cannot query file size");
  return static_cast<size_t>(st.st_size);
}

file_wrapper::file_wrapper(std::string const& filepath, int flags)
  : fd(open(filepath.c_str(), flags)), _size{get_file_size(fd)}
{
  CUDF_EXPECTS(fd != -1, "Cannot open file " + filepath);
}

file_wrapper::file_wrapper(std::string const& filepath, int flags, mode_t mode)
  : fd(open(filepath.c_str(), flags, mode)), _size{get_file_size(fd)}
{
  CUDF_EXPECTS(fd != -1, "Cannot open file " + filepath);
}

file_wrapper::~file_wrapper() { close(fd); }

#ifdef CUFILE_FOUND

cufile_config::cufile_config() : policy{getenv_or("LIBCUDF_CUFILE_POLICY", default_policy)}
{
  if (is_enabled()) {
    // Modify the config file based on the policy
    auto const config_file_path = getenv_or(json_path_env_var, "/etc/cufile.json");
    std::ifstream user_config_file(config_file_path);
    // Modified config file is stored in a temporary directory
    auto const cudf_config_path = tmp_config_dir.path() + "/cufile.json";
    std::ofstream cudf_config_file(cudf_config_path);

    std::string line;
    while (std::getline(user_config_file, line)) {
      std::string const tag = "\"allow_compat_mode\"";
      if (line.find(tag) != std::string::npos) {
        // TODO: only replace the true/false value
        // Enable compatiblity mode when cuDF does not fall back to host path
        cudf_config_file << tag << ": " << (is_required() ? "true" : "false") << ",\n";
      } else {
        cudf_config_file << line << '\n';
      }

      // Point libcufile to the modified config file
      CUDF_EXPECTS(setenv(json_path_env_var.c_str(), cudf_config_path.c_str(), 0) == 0,
                   "Failed to set the cuFile config file environment variable.");
    }
  }
}
cufile_config const* cufile_config::instance()
{
  static cufile_config _instance;
  return &_instance;
}

/**
 * @brief Class that dynamically loads the cuFile library and manages the cuFile driver.
 */
class cufile_shim {
 private:
  cufile_shim();

  void* cf_lib                              = nullptr;
  decltype(cuFileDriverOpen)* driver_open   = nullptr;
  decltype(cuFileDriverClose)* driver_close = nullptr;

  std::unique_ptr<cudf::logic_error> init_error;
  auto is_valid() const noexcept { return init_error == nullptr; }

 public:
  cufile_shim(cufile_shim const&) = delete;
  cufile_shim& operator=(cufile_shim const&) = delete;

  static cufile_shim const* instance();

  ~cufile_shim()
  {
    driver_close();
    dlclose(cf_lib);
  }

  decltype(cuFileHandleRegister)* handle_register     = nullptr;
  decltype(cuFileHandleDeregister)* handle_deregister = nullptr;
  decltype(cuFileRead)* read                          = nullptr;
  decltype(cuFileWrite)* write                        = nullptr;
};

cufile_shim::cufile_shim()
{
  try {
    cf_lib      = dlopen("libcufile.so", RTLD_NOW);
    driver_open = reinterpret_cast<decltype(driver_open)>(dlsym(cf_lib, "cuFileDriverOpen"));
    CUDF_EXPECTS(driver_open != nullptr, "could not find cuFile cuFileDriverOpen symbol");
    driver_close = reinterpret_cast<decltype(driver_close)>(dlsym(cf_lib, "cuFileDriverClose"));
    CUDF_EXPECTS(driver_close != nullptr, "could not find cuFile cuFileDriverClose symbol");
    handle_register =
      reinterpret_cast<decltype(handle_register)>(dlsym(cf_lib, "cuFileHandleRegister"));
    CUDF_EXPECTS(handle_register != nullptr, "could not find cuFile cuFileHandleRegister symbol");
    handle_deregister =
      reinterpret_cast<decltype(handle_deregister)>(dlsym(cf_lib, "cuFileHandleDeregister"));
    CUDF_EXPECTS(handle_deregister != nullptr,
                 "could not find cuFile cuFileHandleDeregister symbol");
    read = reinterpret_cast<decltype(read)>(dlsym(cf_lib, "cuFileRead"));
    CUDF_EXPECTS(read != nullptr, "could not find cuFile cuFileRead symbol");
    write = reinterpret_cast<decltype(write)>(dlsym(cf_lib, "cuFileWrite"));
    CUDF_EXPECTS(write != nullptr, "could not find cuFile cuFileWrite symbol");

    CUDF_EXPECTS(driver_open().err == CU_FILE_SUCCESS, "Failed to initialize cuFile driver");
  } catch (cudf::logic_error const& err) {
    init_error = std::make_unique<cudf::logic_error>(err);
  }
}

cufile_shim const* cufile_shim::instance()
{
  static cufile_shim _instance;
  // Defer throwing to avoid repeated attempts to load the library
  if (!_instance.is_valid()) CUDF_FAIL("" + std::string(_instance.init_error->what()));

  return &_instance;
}

void cufile_registered_file::register_handle()
{
  CUfileDescr_t cufile_desc{};
  cufile_desc.handle.fd = _file.desc();
  cufile_desc.type      = CU_FILE_HANDLE_TYPE_OPAQUE_FD;
  CUDF_EXPECTS(shim->handle_register(&cf_handle, &cufile_desc).err == CU_FILE_SUCCESS,
               "Cannot register file handle with cuFile");
}

cufile_registered_file::~cufile_registered_file() { shim->handle_deregister(cf_handle); }

cufile_input_impl::cufile_input_impl(std::string const& filepath)
  : shim{cufile_shim::instance()},
    cf_file(shim, filepath, O_RDONLY | O_DIRECT),
    pool(16)  // The benefit from multithreaded read plateaus around 16 threads
{
  pool.sleep_duration = 10;
}

std::unique_ptr<datasource::buffer> cufile_input_impl::read(size_t offset,
                                                            size_t size,
                                                            rmm::cuda_stream_view stream)
{
  rmm::device_buffer out_data(size, stream);
  auto read_size = read(offset, size, reinterpret_cast<uint8_t*>(out_data.data()), stream);
  out_data.resize(read_size, stream);
  return datasource::buffer::create(std::move(out_data));
}

namespace {

template <typename DataT,
          typename F,
          typename ResultT = std::invoke_result_t<F, DataT*, size_t, size_t>>
std::vector<std::future<ResultT>> make_sliced_tasks(
  F function, DataT* ptr, size_t offset, size_t size, cudf::detail::thread_pool& pool)
{
  std::vector<std::future<ResultT>> slice_tasks;
  constexpr size_t max_slice_bytes = 4 * 1024 * 1024;
  size_t const n_slices            = util::div_rounding_up_safe(size, max_slice_bytes);
  size_t slice_offset              = 0;
  for (size_t t = 0; t < n_slices; ++t) {
    DataT* ptr_slice = ptr + slice_offset;

    size_t const slice_size = (t == n_slices - 1) ? size % max_slice_bytes : max_slice_bytes;
    slice_tasks.push_back(pool.submit(function, ptr_slice, slice_size, offset + slice_offset));

    slice_offset += slice_size;
  }
  return slice_tasks;
}

}  // namespace

std::future<size_t> cufile_input_impl::read_async(size_t offset,
                                                  size_t size,
                                                  uint8_t* dst,
                                                  rmm::cuda_stream_view stream)
{
  int device;
  cudaGetDevice(&device);

  auto read_slice = [device, gds_read = shim->read, file_handle = cf_file.handle()](
                      void* dst, size_t size, size_t offset) -> ssize_t {
    cudaSetDevice(device);
    auto read_size = gds_read(file_handle, dst, size, offset, 0);
    CUDF_EXPECTS(read_size != -1, "cuFile error reading from a file");
    return read_size;
  };

  auto slice_tasks = make_sliced_tasks(read_slice, dst, offset, size, pool);

  auto waiter = [](auto slice_tasks) -> size_t {
    return std::accumulate(slice_tasks.begin(), slice_tasks.end(), 0, [](auto sum, auto& task) {
      return sum + task.get();
    });
  };
  // The future returned from this function is deferred, not async because we want to avoid creating
  // threads for each read_async call. This overhead is significant in case of multiple small reads.
  return std::async(std::launch::deferred, waiter, std::move(slice_tasks));
}

size_t cufile_input_impl::read(size_t offset,
                               size_t size,
                               uint8_t* dst,
                               rmm::cuda_stream_view stream)
{
  auto result = read_async(offset, size, dst, stream);
  return result.get();
}

cufile_output_impl::cufile_output_impl(std::string const& filepath)
  : shim{cufile_shim::instance()},
    cf_file(shim, filepath, O_CREAT | O_RDWR | O_DIRECT, 0664),
    pool(16)
{
}

void cufile_output_impl::write(void const* data, size_t offset, size_t size)
{
  write_async(data, offset, size).wait();
}

std::future<void> cufile_output_impl::write_async(void const* data, size_t offset, size_t size)
{
  int device;
  cudaGetDevice(&device);

  auto write_slice = [device, gds_write = shim->write, file_handle = cf_file.handle()](
                       void const* src, size_t size, size_t offset) -> void {
    cudaSetDevice(device);
    auto write_size = gds_write(file_handle, src, size, offset, 0);
    CUDF_EXPECTS(write_size != -1 and write_size == static_cast<decltype(write_size)>(size),
                 "cuFile error writing to a file");
  };

  auto source      = static_cast<uint8_t const*>(data);
  auto slice_tasks = make_sliced_tasks(write_slice, source, offset, size, pool);

  auto waiter = [](auto slice_tasks) -> void {
    for (auto const& task : slice_tasks) {
      task.wait();
    }
  };
  // The future returned from this function is deferred, not async because we want to avoid creating
  // threads for each write_async call. This overhead is significant in case of multiple small
  // writes.
  return std::async(std::launch::deferred, waiter, std::move(slice_tasks));
}
#endif

std::unique_ptr<cufile_input_impl> make_cufile_input(std::string const& filepath)
{
#ifdef CUFILE_FOUND
  if (cufile_config::instance()->is_enabled()) {
    try {
      return std::make_unique<cufile_input_impl>(filepath);
    } catch (...) {
      if (cufile_config::instance()->is_required()) throw;
    }
  }
#endif
  return nullptr;
}

std::unique_ptr<cufile_output_impl> make_cufile_output(std::string const& filepath)
{
#ifdef CUFILE_FOUND
  if (cufile_config::instance()->is_enabled()) {
    try {
      return std::make_unique<cufile_output_impl>(filepath);
    } catch (...) {
      if (cufile_config::instance()->is_required()) throw;
    }
  }
#endif
  return nullptr;
}

}  // namespace detail
}  // namespace io
}  // namespace cudf
