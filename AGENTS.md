# Coding Guidelines

- Write simple, efficient, minimal C/C++ code.
- Avoid adding named variables for trivial expressions, especially when the value is used only once.
- Avoid `auto` for local variables. Do not use it for ordinary value or structure types.
- Disable C++ exceptions and RTTI across the entire codebase. Do not use either.
- Perform error checks as early as possible. Check application initialization, resource loading, and Vulkan object creation immediately. Avoid error checking after initialization; normal code should not fail.
- Treat the library as a low-level, thin wrapper, not as a validation layer. Validate only inputs and state whose misuse could make the wrapper itself crash.
  Document those preconditions and enforce programmer errors with asserts.
- Data and parameters passed directly from the user to Vulkan are the user's responsibility. Do not duplicate Vulkan validation or maintain shadow state
  solely to validate them; users should enable the Vulkan validation layer during development.
- Do not abort for programming errors. Enforce their documented preconditions with asserts and leave release builds free of those checks.
- Use error codes and error messages only for invalid external input data and initialization failures.
- Do not check for memory allocation failures. We cannot recover from running out of memory; managing memory usage properly is the user's responsibility.
- Avoid lambdas and complex templates.
- Avoid trivial single-line wrapper functions.
- Avoid trivial single-element wrapper structs.
- Avoid memory allocations, including short-lived local vectors.
- Do not use `std::shared_ptr`.
- Avoid `std::unique_ptr`, especially when the value can be stored directly as a data member.
- Do not use class inheritance or virtual functions.
- Do not use PIMPL interfaces.
- Avoid standard-library algorithms; prefer straightforward loops.
- Do not use hash maps or ordered maps.
- Do not use mutexes or atomics. The API is intentionally single-threaded and is not thread-safe yet.
- Avoid copying large user data structures. Prefer references to structures, and use spans for array data in structures and function parameters.
- Use a custom span type represented by a pointer and size. It must support construction from an initializer list so variable-length arguments remain concise. An initializer list passed as a function argument remains alive through that function call; do not retain a span backed by it after the call returns.
- Always pass `Span`, `ByteSpan`, and `GpuRange` function parameters by value. This allows the compiler to pass their pointer-and-size fields in registers instead of forcing a memory store/load round trip. Review all code against this rule after every change.
- Use C++20 designated initializers with named fields for structures.
- Give public API structure fields useful default values. At call sites, initialize only the non-default fields and name every initialized field.
- Vertex and pixel shaders and their variants share a shader source file. Put entirely different shaders in separate files. Do not combine unrelated shaders
  behind preprocessor conditionals.
- Keep each shader source file's CPU-shared declarations in its own matching shared header. Keep shader-specific root data and constants in that header;
  put types used by multiple shader files in a neutral common header.
- Always review code for performance issues before considering work complete.
- Line length is 160 characters. Please don't chop expressions to multiple lines if not needed.

# Documentation Guidelines

- Keep Markdown documentation short, precise, and focused on user-facing behavior and fidelity to the *No Graphics API* design. Emphasize GPU pointers,
  the root ABI, and relevant Vulkan extensions. Avoid obvious C/C++ conventions, internal plumbing, exhaustive `Desc` or API catalogs, and sample-specific
  asset or format details better left in source files. Keep example descriptions brief.
