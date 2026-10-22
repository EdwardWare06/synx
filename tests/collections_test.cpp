#include <string>

#include "synx/compile.hpp"
#include "test_support.hpp"

namespace {

void check(const char* label, const std::string& source, const std::string& expected = "") {
  auto c = synx::check_source("collections.snx", "import std.collections.{HashMap, sort}; import std.hash.Hash; " + source);
  if (!c->sink.has_errors()) c->run_lowering();
  const std::string errors = c->sink.render(c->user_file);
  ++synx_test::g_checks;
  if ((expected.empty() && c->sink.has_errors()) ||
      (!expected.empty() && errors.find(expected) == std::string::npos)) {
    ++synx_test::g_failures;
    std::printf("FAIL %s: expected '%s'\n%s", label, expected.c_str(), errors.c_str());
  }
}

}  // namespace

int main() {
  check("move key", "main() { var m = HashMap<String,int>.new(); var k = String.from(\"key\"); m.insert(k,1); print(k); }", "use of moved value");
  check("move value", "main() { var m = HashMap<int,String>.new(); var v = String.from(\"value\"); m.insert(1,v); print(v); }", "use of moved value");
  check("map is move only", "main() { var a = HashMap<int,int>.new(); var b = a; print(\"{a.len()}\"); }", "use of moved value");
  check("borrow across insert", "main() { var m=HashMap<int,int>.new(); m.insert(1,1); var r=m.get(1); m.insert(2,2); r.is_some(); }", "while it is already borrowed");
  check("borrow across remove", "main() { var m=HashMap<int,int>.new(); m.insert(1,1); var r=m.get(1); m.remove(1); r.is_some(); }", "while it is already borrowed");
  check("borrow across clear", "main() { var m=HashMap<int,int>.new(); m.insert(1,1); var r=m.get(1); m.clear(); r.is_some(); }", "while it is already borrowed");
  check("writable and shared result", "main() { var m=HashMap<int,int>.new(); m.insert(1,1); var r=m.get_mut(1); var s=m.get(1); r.is_some(); s.is_some(); }", "while it is borrowed as writable");
  check("write shared result", "main() { var m=HashMap<int,int>.new(); m.insert(1,1); match m.get(1) { Some(r)=>{ *r=2; } None=>{} } }", "read only");
  check("borrow ends", "main() { var m=HashMap<int,int>.new(); m.insert(1,1); { var r=m.get(1); r.is_some(); } m.insert(2,2); }");
  check("stored value scope", "main() { var m=HashMap<int,&const int>.new(); { int x=1; m.insert(1,&const x); } m.len(); }", "does not live past this scope");
  check("stored key scope", "main() { var m=HashMap<&const int,int>.new(); { int x=1; m.insert(&const x,1); } m.len(); }", "does not live past this scope");
  check("stored reference mutation", "main() { int x=1; var m=HashMap<int,&const int>.new(); m.insert(1,&const x); x=2; m.len(); }", "while it is borrowed");
  check("stored reference return", "HashMap<int,&const int> f() { int x=1; var m=HashMap<int,&const int>.new(); m.insert(1,&const x); return m; }", "returns a reference");
  check("writable parameter escape", "void f(&HashMap<int,&const int> m) { int x=1; m.insert(1,&const x); }", "returns a reference");
  check("nested stored references", "main() { var outer=Vec<HashMap<int,&const int>>.new(); { int x=1; var m=HashMap<int,&const int>.new(); m.insert(1,&const x); outer.push(m); } outer.len(); }", "does not live past this scope");
  check("missing Hash", "struct K {} impl Eq for K { bool eq(&const self,&const Self other) { return true; } } main() { var m=HashMap<K,int>.new(); }", "does not implement");
  check("missing Eq", "struct K {} impl Hash for K { ulong hash(&const self) { return 0; } } main() { var m=HashMap<K,int>.new(); }", "does not implement");
  check("no floating Hash", "main() { var m=HashMap<double,int>.new(); }", "does not implement");
  check("private raw storage", "main() { var m=HashMap<int,int>.new(); var p=m.values; }", "private to module");
  check("private key pointer", "main() { var m=HashMap<int,int>.new(); var p=m.key_ptr(0); }", "private to module");
  check("private capacity field", "main() { var m=HashMap<int,int>.new(); var n=m.cap; }", "private to module");
  check("private length field", "main() { var m=HashMap<int,int>.new(); var n=m.len; }", "private to module");
  check("private keys field", "main() { var m=HashMap<int,int>.new(); var p=m.keys; }", "private to module");
  check("private rehash", "main() { var m=HashMap<int,int>.new(); m.rehash(1); }", "private to module");
  check("private find", "main() { var m=HashMap<int,int>.new(); m.find(1); }", "private to module");
  check("private state pointer", "main() { var m=HashMap<int,int>.new(); m.state_ptr(0); }", "private to module");
  check("private literal", "main() { HashMap<int,int> m=HashMap { states:0 as *byte, keys:0 as *int, values:0 as *int, len:0, used:0, cap:0 }; }", "private to module");
  check("map passed by move", "void take(HashMap<int,int> m) {} main() { var m=HashMap<int,int>.new(); take(m); m.len(); }", "use of moved value");
  check("inferred Hash bound", "main() { var m=HashMap.new(); m.insert(1.5,1); }", "does not implement");
  check("annotated inferred Hash bound", "main() { HashMap<double,int> m=HashMap.new(); }", "does not implement");
  check("unbounded map parameter", "void f<K,V>(HashMap<K,V> m) {}", "does not implement");
  check("bounded map parameter", "void f<K:Hash+Eq,V>(HashMap<K,V> m) {}");
  check("recursive generic bounds", "struct Node<T:Hash> { Vec<Node<T>> children; } main() {} ");
  check("unbounded impl target", "trait Marker {} impl<K,V> Marker for HashMap<K,V> {}", "does not implement");
  check("constant local bound", "const VALUE={ HashMap<double,int> m; 1 }; main() {}", "does not implement");
  check("sort requires Ord", "struct Unordered {} main() { var v=Vec<Unordered>.new(); sort(&v); }", "does not implement");
  check("sort requires writable reference", "main() { var v=Vec<int>.new(); sort(&const v); }", "expected a writable reference");
  check("sort requires explicit borrow", "main() { var v=Vec<int>.new(); sort(v); }", "Write &");
  check("sort preserves active borrow", "main() { var v=Vec<int>.new(); v.push(1); var r=v.get(0); sort(&v); r.is_some(); }", "while it is already borrowed");
  check("swap preserves active borrow", "main() { var v=Vec<int>.new(); v.push(1); var r=v.get(0); v.swap(0,0); r.is_some(); }", "while it is already borrowed");
  check("sort preserves stored loans", "main() { var v=Vec<&const int>.new(); { int x=1; v.push(&const x); } sort(&v); }", "does not live past this scope");
  check("sort helper is private", "main() { var v=Vec<int>.new(); std.collections.sift_down(&v,0,1); }", "private to module");
  return synx_test::finish("collections_test");
}
