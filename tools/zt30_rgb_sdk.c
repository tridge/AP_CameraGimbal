/* Link-only SDK dependency group. No constructors or hardware calls here.
 * DT_NEEDED puts the circular ISP/CUS3A/algorithm dependencies in one dlopen
 * scope before RTLD_NOW resolves their relocations or runs constructors.
 * Vendor SDK libraries are retained on the camera, not copied into this DSO.
 */
const unsigned apcg_zt30_rgb_sdk_abi = 1;
