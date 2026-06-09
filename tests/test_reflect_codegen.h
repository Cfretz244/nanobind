/*
    Shared definitions for the codegen (tier 2) reflection test. Included by both
    the generator program (test_reflect_codegen_gen.cpp) and the bindings module
    (test_reflect_codegen.cpp). Note: there is NO hand-written trampoline here --
    the trampoline is produced by the codegen fallback and #included by the module.
*/

#pragma once

#include <string>

namespace reflect_codegen_test {

struct Worker {
    Worker() = default;
    virtual ~Worker() = default;
    virtual int work(int x) const = 0;                       // pure virtual
    virtual std::string label() const { return "worker"; }   // non-pure virtual
};

// C++ drivers that call the virtuals through a base reference, so a test can show
// that a Python override is dispatched into from C++.
inline int run_work(const Worker& w, int x) { return w.work(x); }
inline std::string run_label(const Worker& w) { return w.label(); }

// A class with no virtuals: the generator must emit no trampoline for it.
struct Pod {
    int a;
    Pod() : a(0) {}
};

} // namespace reflect_codegen_test
