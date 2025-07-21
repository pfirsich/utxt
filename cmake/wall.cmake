function(utxt_set_wall target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -pedantic -Wconversion)
    # According to the docs, gcc should not warn about designated initializers, but sadly it does:
    # https://gcc.gnu.org/bugzilla/show_bug.cgi?id=96868
    target_compile_options(${target} PRIVATE -Wno-missing-field-initializers)
  endif()
endfunction()
