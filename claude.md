---
alwaysApply: true
---

# C++ Best Practices Guide

## Purpose

This document is a practical set of C++ best practices for writing code that is readable, safe, maintainable, and performant.
It focuses on coding conventions, patterns to prefer, common mistakes to avoid, and habits that scale well in real projects.

---

## 1. General Principles

- Prefer clarity over cleverness.
- Write code for the next engineer, not just for the compiler.
- Optimize after measuring, not before.
- Keep interfaces small and easy to understand.
- Make invalid states hard or impossible to represent.
- Prefer compile-time guarantees over runtime surprises.
- Use the type system to express intent.
- Keep functions and classes focused on one responsibility.
- Minimize global state.
- Favor consistency across the codebase.

---

## 2. Style and Naming Conventions

- Pick one naming convention and use it everywhere.
- Use descriptive names instead of short cryptic abbreviations.
- Prefer `snake_case` or `camelCase` consistently for variables and functions.
- Use `PascalCase` consistently for types if that is the team convention.
- Name booleans so they read naturally, like `is_ready`, `has_value`, or `should_retry`.
- Avoid names like `tmp`, `data2`, `obj`, or `stuff` unless the scope is tiny and obvious.
- Use singular names for single objects and plural names for collections.
- Make constants visually distinct from mutable variables if your style guide supports that.
- Avoid misleading names that hide ownership or lifetime expectations.
- Keep namespaces meaningful and not overly deep.

---

## 3. Headers and Source Files

- Put declarations in headers and definitions in source files when appropriate.
- Keep headers minimal and focused.
- Use `#pragma once` or include guards consistently.
- Do not put `using namespace ...` in headers.
- Include only what you need.
- Prefer forward declarations where they reduce coupling safely.
- Avoid exposing implementation details in public headers.
- Keep header dependencies small to improve build times.
- Order includes consistently, such as standard library, third-party, then project headers.
- Make each translation unit easy to understand on its own.

---

## 4. Modern C++ Practices

- Prefer modern C++ features over legacy C-style patterns.
- Use `nullptr` instead of `NULL` or `0`.
- Use `using` instead of `typedef`.
- Prefer range-based `for` loops when appropriate.
- Use `auto` when it improves readability, not when it hides important types.
- Use `constexpr` for values and functions known at compile time.
- Prefer `enum class` over plain `enum`.
- Use `override` on overridden virtual functions.
- Use `final` when a type or method is not intended to be extended.
- Prefer structured bindings when they make code clearer.
- Use `[[nodiscard]]` on functions whose return value should not be ignored.
- Use `std::optional`, `std::variant`, and `std::expected`-style patterns where they model intent clearly.
- Prefer standard library facilities before inventing your own utilities.

---

## 5. Memory and Resource Management

- Prefer RAII for all resource management.
- Avoid raw `new` and `delete` in application code.
- Use `std::unique_ptr` for exclusive ownership.
- Use `std::shared_ptr` only when shared ownership is actually required.
- Avoid `std::shared_ptr` by default because it adds cost and complexity.
- Use `std::weak_ptr` to break ownership cycles.
- Prefer stack allocation when practical.
- Keep ownership rules obvious from the interface.
- Do not return pointers or references to local variables.
- Be explicit about whether a function takes ownership or borrows.
- Wrap non-memory resources too, such as file handles, sockets, locks, and threads.
- Make cleanup automatic and exception-safe.

---

## 6. Const Correctness

- Use `const` wherever it expresses real immutability.
- Mark member functions `const` when they do not modify object state.
- Pass read-only objects by `const&` when copying would be unnecessary.
- Prefer `const` local variables when values should not change.
- Do not overuse `const` in ways that hurt readability.
- Treat const correctness as part of API design, not decoration.

---

## 7. Function Design

- Keep functions short and focused.
- A function should do one thing well.
- Prefer clear inputs and outputs over hidden side effects.
- Pass small cheap-to-copy types by value.
- Pass large objects by reference or const reference as appropriate.
- Return values instead of using output parameters when possible.
- Avoid long parameter lists.
- Group related parameters into structs or types.
- Prefer overloads or well-designed parameter objects over boolean flag arguments.
- Document preconditions and postconditions when they are not obvious.
- Make failure modes clear.
- Avoid functions with surprising performance costs.
- If a function is hard to name, it may be doing too much.

---

## 8. Classes and Object Design

- Prefer simple classes with clear invariants.
- Keep data members private unless there is a strong reason not to.
- Make constructors establish valid objects.
- Use member initializer lists.
- Initialize every member.
- Prefer composition over inheritance.
- Use inheritance mainly for true subtype polymorphism.
- Avoid deep inheritance hierarchies.
- Declare special member functions deliberately.
- Follow the Rule of Zero when possible.
- If you manage resources manually, understand the Rule of Five.
- Make destructive or expensive operations explicit.
- Keep public APIs small and stable.
- Avoid god objects that know too much or do too much.

---

## 9. Error Handling

- Use exceptions or error-return patterns consistently across the codebase.
- Do not mix error handling styles randomly.
- Throw exceptions for exceptional situations, not normal control flow.
- If exceptions are disabled in a project, use clear return types and checked error paths.
- Never ignore errors silently.
- Add context when propagating errors upward.
- Clean up resources automatically so error paths stay safe.
- Use assertions for programmer mistakes, not user-facing runtime failures.
- Validate external input aggressively.

---

## 10. Containers and Algorithms

- Prefer standard containers such as `std::vector`, `std::array`, `std::unordered_map`, and `std::string`.
- Default to `std::vector` unless another container is clearly better.
- Reserve capacity when the size is known or can be estimated.
- Prefer standard algorithms over handwritten loops when they improve clarity.
- Use iterators and ranges correctly and carefully.
- Be aware of iterator invalidation rules.
- Avoid copying large containers accidentally.
- Prefer `emplace_back` only when it is actually better than `push_back`.
- Do not use linked lists unless you have measured a real need.
- Choose data structures based on actual access patterns.

---

## 11. Performance Practices

- Measure before optimizing.
- Use profiling tools to find bottlenecks.
- Prefer algorithmic improvements over micro-optimizations.
- Avoid unnecessary heap allocations.
- Avoid repeated string concatenation in hot paths without planning capacity.
- Pass and return objects efficiently, but do not sacrifice readability for tiny gains.
- Be cautious with premature inlining and template overuse.
- Understand move semantics, but do not force moves everywhere.
- Avoid hidden work in constructors, destructors, and overloaded operators.
- Know the cost of copies, allocations, locks, and virtual dispatch in critical code.

---

## 12. Concurrency and Threading

- Minimize shared mutable state.
- Prefer message passing or task-based designs when possible.
- Protect shared data with the right synchronization primitives.
- Use RAII wrappers for locks, like `std::lock_guard` and `std::scoped_lock`.
- Keep lock scope as small as possible.
- Avoid holding locks across slow operations.
- Be extremely careful with condition variables and memory ordering.
- Design to avoid deadlocks rather than trying to patch them later.
- Document thread-safety guarantees clearly.
- Treat concurrency bugs as correctness bugs, not rare edge cases.

---

## 13. Things to Avoid

- Avoid raw owning pointers.
- Avoid manual memory management in business logic.
- Avoid macros when a language feature can do the job.
- Avoid C-style casts; prefer `static_cast`, `const_cast`, `reinterpret_cast`, or `dynamic_cast` used carefully.
- Avoid magic numbers; name important constants.
- Avoid hidden global dependencies.
- Avoid giant functions and giant classes.
- Avoid duplicated logic.
- Avoid undefined behavior even if “it works on your machine.”
- Avoid exposing mutable internal state unnecessarily.
- Avoid writing clever template metaprogramming unless it provides clear value.
- Avoid comments that merely repeat the code.
- Avoid stale comments by writing self-explanatory code first.

---

## 14. Testing and Maintainability

- Write code that is easy to test.
- Separate pure logic from I/O and side effects when possible.
- Add unit tests for important behavior and edge cases.
- Reproduce bugs with tests before fixing them when practical.
- Refactor mercilessly when code becomes hard to understand.
- Keep modules loosely coupled and highly cohesive.
- Prefer deterministic behavior in tests.
- Make logs useful, structured, and not noisy.
- Leave the codebase a little cleaner than you found it.

---

## 15. Final Rules of Thumb

- Prefer safety by default.
- Prefer simple code over impressive code.
- Prefer standard library tools over custom utilities.
- Prefer explicit ownership and lifetime semantics.
- Prefer clear contracts and invariants.
- Prefer consistency over personal style battles.
- When in doubt, write the version that will be easiest to debug six months from now.
