#include "cyber/gpu/gpu_reader.h"
#include "cyber/gpu/gpu_writer.h"

namespace apollo {
namespace cyber {

void CompilePublicGpuHeaders(const std::shared_ptr<Node>& node) {
  transport::GpuWriterOptions writer_options;
  auto writer =
      transport::CreateGpuWriter<uint64_t>(node, "/gpu", writer_options);
  transport::GpuReaderOptions reader_options;
  auto reader = transport::CreateGpuReader<uint64_t>(
      node, "/gpu", reader_options,
      [](const transport::GpuMsgView<uint64_t>&) {});
  (void)writer;
  (void)reader;
}

}  // namespace cyber
}  // namespace apollo
