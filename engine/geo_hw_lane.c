/* A second execution lane for the shared geometry implementation. Each lane
 * owns its camera, lighting cursor and statistics. ROM/point RAM remain shared
 * read-only while emulation is paused. Compile the same source with private
 * exported names: there is one geometry algorithm, with no copied fork. */
#define geo_hw_native_meshes geo_lane_native_meshes
#define geo_hw_clear_meshes geo_lane_clear_meshes
#define geo_hw_set_view geo_lane_set_view
#define geo_hw_object geo_lane_object
#define geo_hw_zoom_from_dspfloat geo_lane_zoom_from_dspfloat
#define g_eng_frame g_geo_lane_frame
#define g_geo_stats g_geo_lane_stats
#define g_bbox_cur g_geo_lane_bbox
#define g_zord_ap g_geo_lane_zord_ap
#define g_zord_os g_geo_lane_zord_os
#include "geo_hw.c"
