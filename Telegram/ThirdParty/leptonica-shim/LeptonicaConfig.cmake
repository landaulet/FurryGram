# FurryGram shim: make Tesseract's find_package(Leptonica CONFIG) resolve to the
# in-tree add_subdirectory() target `leptonica` instead of an installed copy.
# The real Leptonica does not export INTERFACE include dirs and scatters its
# generated headers (endianness.h, config_auto.h) across build dirs, so we list
# every location explicitly here.
set(Leptonica_FOUND TRUE)
set(Leptonica_VERSION "${FURRY_LEPTONICA_VERSION}")
set(Leptonica_INCLUDE_DIRS
	"${FURRY_LEPTONICA_PREFIX_DIR}"
	"${FURRY_LEPTONICA_SOURCE_DIR}/src"
	"${FURRY_LEPTONICA_BINARY_DIR}/src"
	"${CMAKE_BINARY_DIR}/src")
set(Leptonica_LIBRARIES leptonica)
set(Leptonica_LIBRARY_DIRS "")
