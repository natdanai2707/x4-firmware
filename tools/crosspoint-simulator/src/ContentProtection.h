#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace freeink {
namespace content {

using ContentChunkSink = bool (*)(void* context, const uint8_t* data, size_t size);

class ContentDecryptor {
 public:
  virtual ~ContentDecryptor() = default;
  virtual bool isEncrypted(const std::string& itemPath) const = 0;
  virtual size_t decryptedSize(const std::string& itemPath) const = 0;
  virtual bool decryptToSink(const std::string& itemPath, ContentChunkSink sink, void* context) = 0;
};

std::unique_ptr<ContentDecryptor> openProtectedBook(const std::string& epubPath, std::string& err);

}  // namespace content
}  // namespace freeink
