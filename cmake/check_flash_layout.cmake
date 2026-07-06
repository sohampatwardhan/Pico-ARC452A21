if (EXISTS "${BIN_PATH}")
    file(SIZE "${BIN_PATH}" BIN_SIZE)
    if (BIN_SIZE GREATER FIRMWARE_LIMIT)
        message(FATAL_ERROR
            "Firmware image is ${BIN_SIZE} bytes, exceeding the reserved firmware "
            "partition of ${FIRMWARE_LIMIT} bytes.")
    endif()
endif()
