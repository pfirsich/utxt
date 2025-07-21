function(utxt_set_no_rtti target)
  if(MSVC)
    # There is a way to do this for a single target, but it's very annoying, so I just don't
    # string(REGEX REPLACE "/GR" "/GR-" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  else()
    target_compile_options(${target} PRIVATE -fno-rtti)
  endif()
endfunction()