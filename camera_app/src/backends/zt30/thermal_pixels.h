#ifndef CAMERA_APP_ZT30_THERMAL_PIXELS_H
#define CAMERA_APP_ZT30_THERMAL_PIXELS_H
#include <cstddef>
#include <cstdint>
namespace ca_zt30 {
static constexpr size_t ThermalPixelWidth=640, ThermalPixelHeight=512;
static constexpr size_t ThermalPixelCount=ThermalPixelWidth*ThermalPixelHeight;
static constexpr size_t ThermalPixelFrameBytes=ThermalPixelCount*4;
enum class ThermalPixelResult {
    Ok, InvalidInput, InvalidOutput, ShortOutput, InvalidStride,
    AddressOverflow, Overlap, UnalignedOutput
};
// Explicit fixed-layout candidates, never auto-detected. All NV12 capacities
// and strides are bytes; footprint is (rows-1)*stride+640. No range scaling,
// colour matrix, rotation, units or scene freshness is implied.
ThermalPixelResult thermal_uyvy_nv12(const uint8_t *frame,size_t bytes,
    uint8_t *y,size_t y_capacity,size_t y_stride,
    uint8_t *uv,size_t uv_capacity,size_t uv_stride);
// Capacity is uint16_t elements, aligned to alignof(uint16_t). Values are
// (hi<<8)|lo from the second half, with no temperature conversion.
ThermalPixelResult thermal_be16(const uint8_t *frame,size_t bytes,
    uint16_t *values,size_t capacity_elements);
} // namespace ca_zt30
#endif
