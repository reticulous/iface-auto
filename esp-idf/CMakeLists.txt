file(GLOB AUTO_SRCS "src/*.cpp")

idf_component_register(
    SRCS ${AUTO_SRCS}
    INCLUDE_DIRS "include"
    REQUIRES
        spangap-core
        spangap-net
        reticulous-core
        lwip
)
