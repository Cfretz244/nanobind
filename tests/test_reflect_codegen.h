/*
    Shared definitions for the codegen (tier 2) reflection test. Included by both
    the generator program (test_reflect_codegen_gen.cpp) and the bindings module
    (test_reflect_codegen.cpp). Note: there is NO hand-written trampoline here --
    the trampoline is produced by the codegen fallback and #included by the module.
*/

#pragma once

#include <map>
#include <string>
#include <vector>

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

// Uses std::vector and std::map in its signatures. The module TU deliberately does
// NOT #include <nanobind/stl/vector.h> / <nanobind/stl/map.h>; the generated header
// emits those caster includes automatically (roadmap #5, codegen route).
struct Bag {
    std::vector<int> items;
    Bag() = default;
    std::map<std::string, int> counts() const {
        std::map<std::string, int> m;
        for (int v : items)
            m[std::to_string(v)] = v;
        return m;
    }
};

} // namespace reflect_codegen_test
