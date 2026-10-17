// The C++ runtime Half-Life's client and server need, in place of libstdc++ (the toolchain's
// is built for glibc): allocation and what a pure virtual call or a failure ends in.
#include <cstdio>
#include <cstdlib>
#include <new>

void* operator new(std::size_t size)
{
    void* p = malloc(size ? size : 1);
    if (!p) {
        fputs("halflife: out of memory\n", stderr);
        abort();
    }
    return p;
}
void* operator new[](std::size_t size) { return operator new(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }
void operator delete(void* p) noexcept { free(p); }
void operator delete[](void* p) noexcept { free(p); }
void operator delete(void* p, std::size_t) noexcept { free(p); }
void operator delete[](void* p, std::size_t) noexcept { free(p); }

extern "C" void __cxa_pure_virtual()
{
    fputs("halflife: pure virtual call\n", stderr);
    abort();
}

namespace std
{
    static void fail(const char* what)
    {
        fprintf(stderr, "halflife: %s\n", what);
        abort();
    }
    void __throw_length_error(const char*) { fail("length error"); }
    void __throw_logic_error(const char*) { fail("logic error"); }
    void __throw_out_of_range(const char*) { fail("out of range"); }
    void __throw_out_of_range_fmt(const char*, ...) { fail("out of range"); }
    void __throw_invalid_argument(const char*) { fail("invalid argument"); }
    void __throw_bad_alloc() { fail("bad alloc"); }
    void __throw_bad_array_new_length() { fail("bad array length"); }
    void __throw_bad_function_call() { fail("bad function call"); }
}
