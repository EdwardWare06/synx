#include "synx/source.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace synx {

SourceFile::SourceFile(std::string path, std::string text)
    : path_(std::move(path)), text_(std::move(text)) {
  line_starts_.push_back(0);
  for (uint32_t i = 0; i < text_.size(); ++i) {
    if (text_[i] == '\n') line_starts_.push_back(i + 1);
  }
}

std::optional<SourceFile> SourceFile::load(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buf;
  buf << in.rdbuf();
  return SourceFile(path, buf.str());
}

std::optional<SourceFile> SourceFile::load_utf8(const std::string& path) {
  std::ifstream in(std::filesystem::path(std::u8string(path.begin(), path.end())),
                   std::ios::binary);
  if (!in) return std::nullopt;
  std::ostringstream buf;
  buf << in.rdbuf();
  return SourceFile(path, buf.str());
}

LineCol SourceFile::line_col(uint32_t offset) const {
  if (offset > text_.size()) offset = static_cast<uint32_t>(text_.size());
  auto it = std::upper_bound(line_starts_.begin(), line_starts_.end(), offset);
  uint32_t line = static_cast<uint32_t>(it - line_starts_.begin());  // 1 based
  uint32_t col = offset - line_starts_[line - 1] + 1;
  return LineCol{line, col};
}

std::string_view SourceFile::line_text(uint32_t line) const {
  if (line == 0 || line > line_starts_.size()) return {};
  uint32_t start = line_starts_[line - 1];
  uint32_t end = line < line_starts_.size()
                     ? line_starts_[line]
                     : static_cast<uint32_t>(text_.size());
  std::string_view view(text_);
  view = view.substr(start, end - start);
  while (!view.empty() && (view.back() == '\n' || view.back() == '\r')) {
    view.remove_suffix(1);
  }
  return view;
}

}  // namespace synx
