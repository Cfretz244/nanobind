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

// A class TEMPLATE with virtual functions (roadmap #6, codegen route). Its
// specialization Processor<int> is discovered via UsesProcessor's signature below,
// and the generator must emit a trampoline for that specialization (spelling the
// template-id Processor<int> as a fully-qualified C++ type) so a Python subclass can
// override its virtuals.
template <class T>
struct Processor {
    Processor() = default;
    virtual ~Processor() = default;
    virtual T process(T x) const = 0;            // pure virtual
    virtual int kind() const { return 0; }       // non-pure virtual
};

inline int run_processor_int(const Processor<int>& p, int x) { return p.process(x); }

// Non-template class that references Processor<int> in a signature, so the spec is
// auto-discovered (and thus bound + given a generated trampoline).
struct UsesProcessor {
    Processor<int>* p = nullptr;
};

} // namespace reflect_codegen_test
