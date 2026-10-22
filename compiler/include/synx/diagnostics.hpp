#pragma once

#include <map>
#include <string>
#include <vector>

#include "synx/source.hpp"

namespace synx {

enum class Severity { Error, Warning, Note };

struct Diagnostic {
  Severity severity;
  Span span;
  std::string message;
  // Stable diagnostic code such as "SNX0401". Empty when not yet assigned.
  // Codes are assigned where the diagnostic is created.
  std::string code;
};

// Collects diagnostics for one compilation. Rendering is separate so tests
// can inspect messages without parsing text.
class DiagnosticSink {
 public:
  void add_source(const SourceFile& file) { sources_[file.id()] = &file; }
  void error(Span span, std::string message, const char* code = "");
  void warning(Span span, std::string message);
  void note(Span span, std::string message);

  bool has_errors() const { return error_count_ > 0; }
  size_t error_count() const { return error_count_; }
  const std::vector<Diagnostic>& all() const { return items_; }

  // Renders every diagnostic with the source line and a caret.
  std::string render(const SourceFile& file) const;
  static std::string render_one(const Diagnostic& d, const SourceFile& file);

 private:
  std::map<uint32_t, const SourceFile*> sources_;
  std::vector<Diagnostic> items_;
  size_t error_count_ = 0;
};

}  // namespace synx
