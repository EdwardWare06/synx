// Discover source files, declare all modules, then bind imports before types.
#include "synx/compile.hpp"

#include <algorithm>
#include <filesystem>
#include <map>
#include <optional>
#include <set>

#include "synx/build.hpp"
#include "synx/lexer.hpp"
#include "synx/parser.hpp"

namespace synx {
namespace {
namespace fs = std::filesystem;
static fs::path utf8_path(const std::string& text) {
  return fs::path(std::u8string(reinterpret_cast<const char8_t*>(text.data()), text.size()));
}
static std::string path_text(const fs::path& path) {
  auto value = path.generic_u8string();
  return std::string(reinterpret_cast<const char*>(value.data()), value.size());
}
using sema::ModuleDef;
using sema::Symbol;
using sema::SymbolKind;

struct Import {
  ModuleDef* from;
  ModuleDef* target;
  const ast::ImportItem* ast;
  size_t consumed;
  bool done = false;
};

class ModuleLoader {
 public:
  explicit ModuleLoader(Compilation& c) : c(c), root(fs::absolute(utf8_path(c.user_file.path())).parent_path()), std_root(utf8_path(find_standard_library())) {}

  bool run() {
    auto* prelude = scope("<prelude>");
    prelude->is_prelude = true;
    c.resolver->declare_module(c.prelude_ast, prelude, true);
    // The prelude namespace is an explicit route to compiler-provided items.
    auto* exported = scope("std.prelude");
    exported->symbols = c.program.globals;
    for (auto& [name, symbol] : exported->symbols) { (void)name; symbol.is_pub = true; }
    auto* main = scope("");
    c.program.root_module = main;
    main->file = c.user_file.id();
    files[path_text(fs::weakly_canonical(utf8_path(c.user_file.path())))] = main;
    visit(main, c.user_ast);
    // Cyclic modules work because declarations already exist. Re-exports
    // may need several rounds before every imported name is visible.
    bool progress = true;
    while (progress) {
      progress = false;
      for (auto& item : imports) if (!item.done && bind(item, false)) { item.done = true; progress = true; }
    }
    for (auto& item : imports) if (!item.done) bind(item, true);
    return !c.sink.has_errors();
  }

 private:
  ModuleDef* scope(const std::string& name) {
    auto found = names.find(name);
    if (found != names.end()) return found->second;
    auto module = std::make_unique<ModuleDef>();
    module->name = name;
    auto* out = module.get();
    c.program.modules.push_back(std::move(module));
    names[name] = out;
    return out;
  }

  ModuleDef* view(ModuleDef* owner, const std::string& name, ModuleDef* base = nullptr) {
    auto key = std::make_pair(owner, name);
    auto found = views.find(key);
    if (found != views.end()) {
      if (base) {
        for (auto& [child, route] : found->second->symbols) {
          auto* existing = base->find(child);
          if (existing && existing->kind != SymbolKind::Module)
            c.sink.error(route.span, "module namespace conflicts with '" + child + "'", "SNX0302");
        }
        found->second->base_module = base;
      }
      return found->second;
    }
    auto module = std::make_unique<ModuleDef>();
    module->name = name;
    module->namespace_owner = owner;
    module->base_module = base;
    auto* out = module.get();
    c.program.modules.push_back(std::move(module));
    views[key] = out;
    return out;
  }

  static Symbol module_symbol(ModuleDef* module) {
    Symbol symbol{SymbolKind::Module};
    symbol.module_def = module;
    return symbol;
  }

  std::string prefix(const ast::Path& path, size_t count) {
    std::string name;
    for (size_t i = 0; i < count; ++i) { if (i) name += '.'; name += path.segments[i].name; }
    return name;
  }

  ModuleDef* load(const std::string& name, const fs::path& path, Span span) {
    std::string canonical = path_text(fs::weakly_canonical(path));
    auto alias = [&](ModuleDef* module) { names[name] = module; return module; };
    auto found = files.find(canonical);
    if (found != files.end()) return alias(found->second);
    for (auto& [existing, module] : files) {
      std::error_code error;
      if (fs::equivalent(path, utf8_path(existing), error) && !error) return alias(module);
    }
    auto source = SourceFile::load_utf8(path_text(path));
    if (!source) { c.sink.error(span, "cannot read module '" + name + "'", "SNX0303"); return nullptr; }
    source->set_id(static_cast<uint32_t>(c.imported_sources.size() + 2));
    auto unit = std::make_unique<ModuleSource>(std::move(*source));
    auto* ptr = unit.get();
    c.imported_sources.push_back(std::move(unit));
    c.sink.add_source(ptr->file);
    Lexer lexer(ptr->file, c.sink);
    Parser parser(ptr->file, lexer.tokenize(), c.sink);
    ptr->ast = parser.parse_module();
    auto* module = scope(name);
    module->file = ptr->file.id();
    files[canonical] = module;  // record before following a cycle
    visit(module, ptr->ast);
    return module;
  }

  void visit(ModuleDef* module, const ast::Module& ast) {
    c.resolver->declare_module(ast, module, false);
    for (auto& item : ast.items) {
      if (item->kind != ast::ItemKind::Import) continue;
      auto& request = static_cast<const ast::ImportItem&>(*item);
      ModuleDef* target = nullptr;
      size_t consumed = 0;
      for (size_t n = request.path.segments.size(); n > 0; --n) {
        auto name = prefix(request.path, n);
        if (name == "std.prelude") { target = scope(name); consumed = n; break; }
        bool standard = request.path.segments[0].name == "std";
        if (standard && std_root.empty()) {
          c.sink.error(request.span, "standard library not found", "SNX0303");
          break;
        }
        fs::path relative;
        for (size_t i = standard ? 1 : 0; i < n; ++i) relative /= request.path.segments[i].name;
        if (relative.empty()) continue;
        auto base = (standard ? std_root : root) / relative;
        fs::path file = base; file += ".snx";
        if (!fs::is_regular_file(file)) file = base / "mod.snx";
        if (!fs::is_regular_file(file)) continue;
        target = load(name, file, request.span);
        consumed = n;
        break;
      }
      if (!target) { c.sink.error(request.span, "cannot find imported module '" + request.path.to_string() + "'", "SNX0303"); continue; }
      imports.push_back({module, target, &request, consumed});
    }
  }

  bool put(ModuleDef* module, const std::string& name, Symbol symbol, Span span, bool exported) {
    symbol.is_pub = exported;
    auto found = module->symbols.find(name);
    if (found != module->symbols.end()) {
      auto& old = found->second;
      bool same = old.kind == symbol.kind && old.module_def == symbol.module_def && old.fn_def == symbol.fn_def &&
                  old.struct_def == symbol.struct_def && old.enum_def == symbol.enum_def && old.trait_def == symbol.trait_def &&
                  old.const_def == symbol.const_def && old.variant == symbol.variant;
      if (same) { old.is_pub = old.is_pub || exported; return true; }
      c.sink.error(span, "imported name '" + name + "' conflicts with an existing definition", "SNX0302");
      return false;
    }
    module->symbols[name] = symbol;
    return true;
  }

  bool bind(Import& request, bool report) {
    auto& ast = *request.ast;
    Symbol symbol = module_symbol(request.target);
    auto member = [&](const ast::PathSegment& segment) {
      if (symbol.kind == SymbolKind::Module) {
        auto* module = symbol.module_def;
        const ModuleDef* declared = nullptr;
        auto* found = module->find(segment.name, &declared);
        if (!found) return false;
        if (declared != request.from && declared->namespace_owner != request.from && !found->is_pub) {
          if (report) c.sink.error(segment.span, "'" + segment.name + "' is private to module '" + module->name + "'", "SNX0304");
          return false;
        }
        symbol = *found;
        return true;
      }
      if (symbol.kind == SymbolKind::Enum) {
        auto* variant = symbol.enum_def->find_variant(segment.name);
        if (!variant) return false;
        symbol.kind = SymbolKind::Variant;
        symbol.variant = variant;
        return true;
      }
      return false;
    };
    bool found = true;
    for (size_t i = request.consumed; found && i < ast.path.segments.size(); ++i) found = member(ast.path.segments[i]);
    std::vector<std::pair<std::string, Symbol>> bindings;
    if (found && !ast.names.empty()) {
      Symbol base = symbol;
      for (auto& name : ast.names) {
        symbol = base;
        if (!member(name)) { found = false; break; }
        bindings.push_back({name.name, symbol});
      }
    } else if (found) bindings.push_back({ast.path.segments.back().name, symbol});
    if (!found) {
      if (report) c.sink.error(ast.span, "cannot resolve import '" + ast.path.to_string() + "' (missing or private item, or cyclic re-export)", "SNX0303");
      return false;
    }
    // Qualified namespace routes are private to this importer. Only the
    // explicitly named binding is exported by pub import.
    ModuleDef* previous = nullptr;
    for (size_t i = 1; i <= request.consumed; ++i) {
      auto name = prefix(ast.path, i);
      auto* node = view(request.from, name, i == request.consumed ? request.target : nullptr);
      if (previous) {
        auto child = ast.path.segments[i - 1].name;
        auto* existing = previous->find(child);
        if (existing && existing->kind != SymbolKind::Module) {
          c.sink.error(ast.span, "module '" + name + "' conflicts with an existing definition", "SNX0302");
        } else { auto route = module_symbol(node); route.is_pub = false; route.span = ast.span; previous->symbols[child] = route; }
      } else if (request.consumed > 1) {
        put(request.from, ast.path.segments[0].name, module_symbol(node), ast.span, false);
      }
      previous = node;
    }
    for (auto& [name, value] : bindings) {
      if (value.kind == SymbolKind::Module && !value.module_def->namespace_owner)
        value = module_symbol(view(request.from, prefix(ast.path, ast.path.segments.size()), value.module_def));
      put(request.from, name, value, ast.span, ast.is_pub);
    }
    return true;
  }

  Compilation& c;
  fs::path root, std_root;
  std::map<std::string, ModuleDef*> names, files;
  std::map<std::pair<ModuleDef*, std::string>, ModuleDef*> views;
  std::vector<Import> imports;
};
}  // namespace

bool Compilation::load_modules() {
  try { return ModuleLoader(*this).run(); }
  catch (const fs::filesystem_error&) { sink.error(user_ast.items.empty() ? Span{0, 0, user_file.id()} : user_ast.items.front()->span, "module filesystem operation failed", "SNX0303"); return false; }
}
}  // namespace synx
