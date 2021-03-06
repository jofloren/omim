project(storage)

include_directories(${OMIM_ROOT}/3party/jansson/src)

set(
  SRC
  app_store.hpp
  country.cpp
  country.hpp
  country_decl.cpp
  country_decl.hpp
  country_info_getter.cpp
  country_info_getter.hpp
  country_name_getter.cpp
  country_name_getter.hpp
  country_polygon.hpp
  country_tree.hpp
  downloader_search_params.hpp
  downloading_policy.cpp
  downloading_policy.hpp
  http_map_files_downloader.cpp
  http_map_files_downloader.hpp
  index.cpp
  index.hpp
  map_files_downloader.hpp
  queued_country.cpp
  queued_country.hpp
  storage.cpp
  storage.hpp
  storage_defines.cpp
  storage_defines.hpp
  storage_helpers.cpp
  storage_helpers.hpp
)

add_library(${PROJECT_NAME} ${SRC})

omim_add_test_subdirectory(storage_tests)
omim_add_test_subdirectory(storage_integration_tests)
