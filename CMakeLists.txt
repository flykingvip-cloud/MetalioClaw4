# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "2.0.53")

# Add this line to disable the specific warning
add_compile_options(-Wno-missing-field-initializers)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(xiaozhi)

# Custom wake word replaces the WakeNet entry inside the ESP-SR pack.
# Do not copy wakeword/srmodels.bin over the whole image: that drops vadnet/nsnet.
if(EXISTS "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin")
    if(TARGET srmodels_bin)
        idf_component_get_property(esp_sr_dir espressif__esp-sr COMPONENT_DIR)
        partition_table_get_partition_info(model_part_size "--partition-name model" "size")
        if(NOT model_part_size)
            set(model_part_size 978944)
        endif()
        add_custom_target(custom_wakeword_srmodels ALL
            COMMAND python3 "${CMAKE_SOURCE_DIR}/scripts/merge_wakeword_srmodels.py"
                --srmodels-dir "${CMAKE_BINARY_DIR}/srmodels"
                --wakeword "${CMAKE_SOURCE_DIR}/wakeword/srmodels.bin"
                --pack-script "${esp_sr_dir}/model/pack_model.py"
                --max-bytes "${model_part_size}"
            COMMENT "Merge custom wake word with ESP-SR vadnet/nsnet"
            VERBATIM
        )
        add_dependencies(custom_wakeword_srmodels srmodels_bin)
        if(TARGET flash)
            add_dependencies(flash custom_wakeword_srmodels)
        endif()
        message(STATUS "Custom wakeword merge: wakeword/srmodels.bin + ESP-SR vadnet/nsnet")
    else()
        message(WARNING "wakeword/srmodels.bin exists but srmodels_bin target not found; custom wakeword merge skipped")
    endif()
endif()

