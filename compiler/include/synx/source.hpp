#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace synx {

// A half open byte range [start, end) inside one source file.
struct Span {
  uint32_t start = 0;
  uint32_t end = 0;
  uint32_t file = 0;

  uint32_t length() const { return end - start; }
  static Span merge(Span a, Span b) {
    return Span{a.start < b.start ? a.start : b.start,
                a.end > b.end ? a.end : b.end, a.file};
  }
};

// One based line and column. Column counts bytes, not characters.
struct LineCol {
  uint32_t line = 1;
  uint32_t col = 1;
};

// The text of one source file plus a line table for diagnostics.
class SourceFile {
 public:
  SourceFile(std::string path, std::string text);

  // Reads a native narrow file path. Returns nullopt if it cannot be read.
  static std::optional<SourceFile> load(const std::string& path);

  // Reads a UTF-8 file path on every platform. Retains the UTF-8 display path.
  static std::optional<SourceFile> load_utf8(const std::string& path);

  uint32_t id() const { return id_; }
  void set_id(uint32_t id) { id_ = id; }

  const std::string& path() const { return path_; }
  std::string_view text() const { return text_; }

  LineCol line_col(uint32_t offset) const;

  // Text of the given one based line without the line ending.
  std::string_view line_text(uint32_t line) const;
  uint32_t line_count() const {
    return static_cast<uint32_t>(line_starts_.size());
  }

 private:
  uint32_t id_ = 0;
  std::string path_;
  std::string text_;
  std::vector<uint32_t> line_starts_;
};

}  // namespace synx
