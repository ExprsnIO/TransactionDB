# CompilerWarnings.cmake — apply a strict warning set to first-party targets only.
# Usage: tdb_set_warnings(<target>)
function(tdb_set_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4)
  else()
    target_compile_options(${target} PRIVATE
      -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
      -Wno-unused-parameter)
  endif()
endfunction()
