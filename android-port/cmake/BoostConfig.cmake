# Minimal Boost package for Dynarmic's find_package(Boost): header-only Boost::boost from the fetched release
# (DCP_BOOST_INCLUDE, set by the top-level CMakeLists before Dynarmic is added).
if(NOT TARGET Boost::boost)
  add_library(Boost::boost INTERFACE IMPORTED)
  set_target_properties(Boost::boost PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${DCP_BOOST_INCLUDE}")
endif()
if(NOT TARGET Boost::headers)
  add_library(Boost::headers ALIAS Boost::boost)
endif()
set(Boost_FOUND TRUE)
set(Boost_INCLUDE_DIRS "${DCP_BOOST_INCLUDE}")
