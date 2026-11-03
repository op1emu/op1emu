include(FetchContent)

# The official R0.16 archive, rather than an unofficial Git mirror. The page's
# ffconf.h and diskio adapter live in web/src/storage.
function(op1_link_fatfs target)
    if(NOT TARGET op1-fatfs)
        FetchContent_Declare(fatfs
            URL https://elm-chan.org/fsw/ff/arc/ff16.zip
            URL_HASH SHA256=99f7dc1f7e095356e4a9e3dbe29959090d8b948afe2bbc5441e52fdf4b85449e
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE
        )
        FetchContent_MakeAvailable(fatfs)
        add_library(op1-fatfs STATIC
            ${fatfs_SOURCE_DIR}/source/ff.c
            ${fatfs_SOURCE_DIR}/source/ffunicode.c
            ${fatfs_SOURCE_DIR}/source/ffsystem.c
        )
        target_include_directories(op1-fatfs PUBLIC
            ${CMAKE_SOURCE_DIR}/web/src/storage
            ${fatfs_SOURCE_DIR}/source
        )
        # ff.h uses a quoted include and would otherwise always select the
        # archive's own default ffconf.h before any include search path.
        target_compile_options(op1-fatfs PUBLIC -include ${CMAKE_SOURCE_DIR}/web/src/storage/ffconf.h)
    endif()
    target_link_libraries(${target} PRIVATE op1-fatfs)
endfunction()
