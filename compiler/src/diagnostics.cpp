#include "synx/diagnostics.hpp"

#include <sstream>

namespace synx {

void DiagnosticSink::error(Span span, std::string message, const char* code) {
  items_.push_back({Severity::Error, span, std::move(message), code});
  ++error_count_;
}

void DiagnosticSink::warning(Span span, std::string message) {
  items_.push_back({Severity::Warning, span, std::move(message), ""});
}

void DiagnosticSink::note(Span span, std::string message) {
  items_.push_back({Severity::Note, span, std::move(message), ""});
}

static const char* severity_name(Severity s) {
  switch (s) {
    case Severity::Error: return "error";
    case Severity::Warning: return "warning";
    case Severity::Note: return "note";
  }
  return "?";
}

std::string DiagnosticSink::render_one(const Diagnostic& d,
                                       const SourceFile& file) {
  LineCol lc = file.line_col(d.span.start);
  std::ostringstream out;
  out << file.path() << ':' << lc.line << ':' << lc.col << ": " << severity_name(d.severity);
  if (!d.code.empty()) out << '[' << d.code << ']';
  out << ": " << d.message << '\n';

  std::string_view line = file.line_text(lc.line);
  std::string number = std::to_string(lc.line);
  out << number << " | " << line << '\n';
  out << std::string(number.size(), ' ') << " | ";
  for (uint32_t i = 1; i < lc.col; ++i) {
    out << (i - 1 < line.size() && line[i - 1] == '\t' ? '\t' : ' ');
  }
  uint32_t width = d.span.length();
  LineCol end_lc = file.line_col(d.span.end);
  if (end_lc.line != lc.line) width = static_cast<uint32_t>(line.size()) - (lc.col - 1);
  if (width == 0) width = 1;
  out << std::string(width, '^') << '\n';
  return out.str();
}

std::string DiagnosticSink::render(const SourceFile& file) const {
  std::string out;
  for (const auto& d : items_) {
    auto source = sources_.find(d.span.file);
    out += render_one(d, source == sources_.end() ? file : *source->second);
  }
  return out;
}

}  // namespace synx
