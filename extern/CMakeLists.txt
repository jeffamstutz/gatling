include(FetchContent)

# volk
FetchContent_Declare(
  volk
  GIT_REPOSITORY https://github.com/zeux/volk.git
  GIT_TAG 1.2.140
)
FetchContent_MakeAvailable(volk)

# shaderc
# TODO: this does not pull in dependencies, need to execute _deps/shaderc-src/utils/git-sync-deps manually
set(SHADERC_SKIP_INSTALL ON CACHE INTERNAL "")
set(SHADERC_SKIP_TESTS ON CACHE INTERNAL "")
set(SHADERC_SKIP_EXAMPLES ON CACHE INTERNAL "")
set(SHADERC_ENABLE_WERROR_COMPILE OFF CACHE INTERNAL "")
set(SHADERC_SKIP_INSTALL OFF CACHE INTERNAL "")
set(SHADERC_SKIP_INSTALL OFF CACHE INTERNAL "")
set(SHADERC_ENABLE_SHARED_CRT ON CACHE INTERNAL "")
FetchContent_Declare(
  shaderc
  GIT_REPOSITORY https://github.com/google/shaderc
  GIT_TAG v2021.1
)
FetchContent_MakeAvailable(shaderc)
