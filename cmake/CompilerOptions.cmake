# Warning, hardening and sanitizer flags, applied through an INTERFACE target so
# that they reach FluxGate's own code but never third-party sources.
add_library(fluxgate_compile_options INTERFACE)

target_compile_options(fluxgate_compile_options INTERFACE
  -Wall
  -Wextra
  -Wpedantic
  -Wshadow
  -Wconversion
  -Wsign-conversion
  -Wnon-virtual-dtor
  -Wold-style-cast
  -Woverloaded-virtual
  -Wcast-align
  -Wformat=2
  -Wimplicit-fallthrough
  -Wnull-dereference
  -fstack-protector-strong
)

if(FLUXGATE_WERROR)
  target_compile_options(fluxgate_compile_options INTERFACE -Werror)
endif()

target_compile_definitions(fluxgate_compile_options INTERFACE
  $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
)

if(FLUXGATE_SANITIZE)
  string(FIND "${FLUXGATE_SANITIZE}" "undefined" undefined_position)
  if(undefined_position GREATER_EQUAL 0)
    set(FLUXGATE_SANITIZE_UNDEFINED ON)
  endif()
  target_compile_options(fluxgate_compile_options INTERFACE
    -fsanitize=${FLUXGATE_SANITIZE}
    -fno-sanitize-recover=all
    -fno-omit-frame-pointer
    # The vptr check probes memory with a pipe(), which fails once descriptors run
    # out, so it reports false positives in the resource-exhaustion tests. ASan still
    # catches a dangling Stream, which is the failure the check would have found.
    $<$<BOOL:${FLUXGATE_SANITIZE_UNDEFINED}>:-fno-sanitize=vptr>
  )
  target_link_options(fluxgate_compile_options INTERFACE
    -fsanitize=${FLUXGATE_SANITIZE}
    $<$<BOOL:${FLUXGATE_SANITIZE_UNDEFINED}>:-fno-sanitize=vptr>
  )
endif()
