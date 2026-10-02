# Tests link libvulkan (unlike the layer, which only uses the headers).
add_executable(x4vr_test_multiview_enable multiview_enable.cpp)
target_link_libraries(x4vr_test_multiview_enable PRIVATE Vulkan::Vulkan)

add_executable(x4vr_test_multiview_double multiview_double.cpp)
target_link_libraries(x4vr_test_multiview_double PRIVATE Vulkan::Vulkan)

add_executable(x4vr_test_multiview_render multiview_render.cpp)
target_link_libraries(x4vr_test_multiview_render PRIVATE Vulkan::Vulkan)

# The shader patches, exercised without a GPU. Needs no Vulkan library at all --
# it only ever rewrites words in a buffer, which is the point.
add_executable(x4vr_test_spirv_patch spirv_patch.cpp)
add_executable(x4vr_test_wgate_patch wgate_patch.cpp)   # task #82

# The projection math. Self-checking: it takes no arguments and returns
# non-zero on the first bad case, so it needs no runner script.
add_executable(x4vr_test_view_math view_math.cpp)

# The injector-to-layer channel. Header-only and process-local, so this needs
# neither Vulkan nor either component -- it links only a thread library, for the
# concurrent-writer case. Self-checking like view_math: no runner script.
add_executable(x4vr_test_share share.cpp)
find_package(Threads REQUIRED)
target_link_libraries(x4vr_test_share PRIVATE Threads::Threads)

# The dump format itself, round-tripped through an independent decoder. Needs
# no Vulkan and no GPU: the encoder is pure, which is why it was worth moving
# out of the layer's translation unit where nothing could reach it. Self-
# checking like view_math and share -- no runner script.
find_package(ZLIB REQUIRED)
add_executable(x4vr_test_png_writer png_writer.cpp)
target_link_libraries(x4vr_test_png_writer PRIVATE ZLIB::ZLIB)

# The injector's `env: run =` line must round-trip through a shell, because the
# documented way to build the next take's command is to diff it against the
# previous take's. Take 258's command shipped an unquoted value with spaces and
# would not start; Patola repaired it by hand. Round-tripped through sh, zsh
# and bash themselves rather than against an expected string, so a quoting bug
# and a wrong expectation cannot cancel. Self-checking -- no runner script.
add_executable(x4vr_test_shquote shquote.cpp)

# X4VR_UI_DUMP's grammar, PERIOD[:BURST[:GAPMS]]. Take 258 ran `600:6`, whose
# missing third field meant six consecutive presents 8.5 ms apart -- below the
# offline search's floor at any head speed, and nothing said so. Pure string
# work: no Vulkan, no GPU. Self-checking -- no runner script.
add_executable(x4vr_test_dumpspec dumpspec.cpp)

# The dump writers' pixel transforms, rewritten because take 259 measured them
# at 91% of a 2378 ms/frame flush -- both read the write-combined readback
# buffer one byte at a time. A performance change that alters output is a
# correctness bug wearing a stopwatch, so every case is compared byte for byte
# against the ORIGINAL per-pixel code, kept in the test verbatim.
add_executable(x4vr_test_dumpio dumpio.cpp)

# Is the readback memory type what makes a dump expensive? Take 258 spent
# 2451 ms per frame in the flush while the PNG encode measured 57-125 ms per
# image. Both readback buffers take the first HOST_VISIBLE|HOST_COHERENT type,
# which on AMD is uncached write-combined, and CPU reads from write-combined
# memory are slow in a way writes are not. Needs a real GPU -- it allocates
# both types, copies one source into each, and times the read. Refuses to
# report a ratio if both requests chose the same type.
add_executable(x4vr_test_readback_speed readback_speed.cpp)
target_link_libraries(x4vr_test_readback_speed PRIVATE Vulkan::Vulkan ZLIB::ZLIB)

# Where the cursor lands and what its bytes mean (task #17). Needs no Vulkan
# library -- it only uses the header's enums and two pure functions -- and no
# GPU, which is the whole point of pulling them out of the overlay class.
add_executable(x4vr_test_cursor_place cursor_place.cpp)
target_include_directories(x4vr_test_cursor_place PRIVATE ${Vulkan_INCLUDE_DIRS})

# The same overlay driven on a real GPU, under the validation layer. This is
# the one that would have caught a bad barrier or a cursor in one eye only,
# and it needs no X4.
add_executable(x4vr_test_cursor_render cursor_render.cpp)
target_link_libraries(x4vr_test_cursor_render PRIVATE Vulkan::Vulkan)

# The OpenXR bring-up, on a real GPU and a real runtime. Links libvulkan but
# NOT libopenxr_loader: common/x4vr_xr.hpp dlopen()s the loader, because the
# layer is injected into someone else's process and a missing DT_NEEDED there
# is X4 refusing to start rather than a graceful fall back to flatscreen. The
# headers are a build dependency; the loader is not.
if(TARGET OpenXR::headers OR OPENXR_HEADER_DIR)
    add_executable(x4vr_test_xr_probe xr_probe.cpp)
    target_link_libraries(x4vr_test_xr_probe PRIVATE Vulkan::Vulkan ${CMAKE_DL_LIBS})
    if(TARGET OpenXR::headers)
        target_link_libraries(x4vr_test_xr_probe PRIVATE OpenXR::headers)
    else()
        target_include_directories(x4vr_test_xr_probe PRIVATE ${OPENXR_HEADER_DIR})
    endif()
else()
    message(STATUS "OpenXR headers not found — skipping x4vr_test_xr_probe")
endif()

# Shaders are committed as SPIR-V so the suite does not need glslc to run.
configure_file(fullscreen.vert.spv fullscreen.vert.spv COPYONLY)
configure_file(fullscreen_camera.vert.spv fullscreen_camera.vert.spv COPYONLY)
configure_file(sample_fullscreen_uncam.vert.spv sample_fullscreen_uncam.vert.spv COPYONLY)
configure_file(solid.frag.spv solid.frag.spv COPYONLY)
configure_file(sample.frag.spv sample.frag.spv COPYONLY)
configure_file(sample_two.frag.spv sample_two.frag.spv COPYONLY)
configure_file(sample_shadow.frag.spv sample_shadow.frag.spv COPYONLY)
configure_file(sample_size.frag.spv sample_size.frag.spv COPYONLY)
configure_file(sample_bindless.frag.spv sample_bindless.frag.spv COPYONLY)
configure_file(sample_twin.frag.spv sample_twin.frag.spv COPYONLY)
configure_file(sample_twin_base.frag.spv sample_twin_base.frag.spv COPYONLY)
configure_file(sample_alias_binding.frag.spv sample_alias_binding.frag.spv COPYONLY)
configure_file(sample_camera_block.vert.spv sample_camera_block.vert.spv COPYONLY)
configure_file(sample_light_volume.vert.spv sample_light_volume.vert.spv COPYONLY)
configure_file(sample_invproj.frag.spv sample_invproj.frag.spv COPYONLY)
configure_file(sample_combined.spv sample_combined.spv COPYONLY)
# Task #76's pair: the copy_gamma shape, and the collider that must be refused.
configure_file(sample_uiswitch.frag.spv sample_uiswitch.frag.spv COPYONLY)
configure_file(sample_uiswitch_float.frag.spv sample_uiswitch_float.frag.spv COPYONLY)
configure_file(skybox_cube.comp.spv skybox_cube.comp.spv COPYONLY)
configure_file(noop.comp.spv noop.comp.spv COPYONLY)

# The inputmap rewrite that gives head-look a binding of its own. Pure string
# work on a fixture shaped like the real file, so it needs neither X4 nor a
# GPU. Self-checking like view_math: no runner script.
add_executable(x4vr_test_inputmap inputmap.cpp)

# The head-look arithmetic: pose decomposition, the open-loop integrator, its
# clamp and its recentre. Pure and dependency-free, so no X4 and no GPU.
# Self-checking like view_math: no runner script.
add_executable(x4vr_test_headlook headlook.cpp)

# The SHAPE of #36's queue lock, against the ABBA deadlock takes 298-301 hit
# under SteamVR. Threads only -- no Vulkan, no runtime -- so it runs anywhere
# and its negative control (the old nesting must deadlock) is deterministic.
add_executable(x4vr_test_queue_lock queue_lock.cpp)
find_package(Threads REQUIRED)
target_link_libraries(x4vr_test_queue_lock PRIVATE Threads::Threads)

# Task #75: the staleness matcher, driven against series whose lag is known by
# construction. Uses common/x4vr_camring.hpp itself, not a copy, so a change to
# the matcher cannot pass here and fail in the injector.
add_executable(x4vr_test_camring camring.cpp)

# The injector's hand-written SDL_MouseMotionEvent, asserted against the real
# SDL3 headers. The injector cannot include them (SDL2 and SDL3 share X4's
# process tree); this standalone binary can, so a future SDL that moves a field
# fails the build instead of steering the camera with a garbage float.
find_package(SDL3 QUIET)
if(SDL3_FOUND OR EXISTS /usr/include/SDL3/SDL_events.h)
  add_executable(x4vr_test_sdl_event_layout sdl_event_layout.cpp)
  # Task #75: the injector's queue-time measurement rests entirely on SDL
  # keeping a timestamp we set ourselves. A version that overwrote it would
  # leave `evflow` reporting a latency near zero with no other symptom -- a
  # plausible number, silently wrong. Needs the LIBRARY, not just the headers.
  if(SDL3_FOUND)
    add_executable(x4vr_test_sdl_push_timestamp sdl_push_timestamp.cpp)
    target_link_libraries(x4vr_test_sdl_push_timestamp PRIVATE SDL3::SDL3)
  else()
    message(STATUS "SDL3 library not found — skipping x4vr_test_sdl_push_timestamp")
  endif()
else()
  message(STATUS "SDL3 headers not found — skipping x4vr_test_sdl_event_layout")
endif()

# The Lua chunk scan. Pure string work on buffers that are NOT NUL-terminated,
# which is the hazard it exists to pin. Self-checking: no runner script.
add_executable(x4vr_test_lua_scan lua_scan.cpp)

add_executable(x4vr_test_guard_budget guard_budget.cpp)

# The IsHUDActive teardown guard's decoder (X4's exit crash was ours). Checks
# the refusals and, when X4 is installed, the real prologue. Self-checking.
add_executable(x4vr_test_riprel riprel.cpp)

# The camera-state scan (where X4 keeps its free-look angle). Planted state,
# decoys, a still camera and a state outside the scan. Self-checking.
add_executable(x4vr_test_camscan camscan.cpp)

# The camera write-watch: record layout, anchors against the real binary, the
# entry lookup, and a LIVE hardware watchpoint with decoy threads. Needs
# perf_event_paranoid <= 2; says SKIP otherwise. Self-checking.
add_executable(x4vr_test_camwatch camwatch.cpp)
# X4VR_HEADLOOK_WRITE: X4's matrix convention against take 319's own stored
# matrices, the parameter and base refusals, the target, and the code sites
# against the real binary. Self-checking.
add_executable(x4vr_test_camwrite camwrite.cpp)
# X4VR_SHADOWFOV_AB: the field sites and X4's default-field constant against
# the real binary, the layout check, and what each phase writes. Self-checking.
add_executable(x4vr_test_shadowfov shadowfov.cpp)
# The content.xml fork that enables our extensions for the modded session.
add_executable(x4vr_test_content_fork content_fork.cpp)
target_link_libraries(x4vr_test_camwatch PRIVATE Threads::Threads)

# Task #73's lag, and the lead that removes it. Drives the real head_look_step
# against a model of X4's camera. Its claim to being more than a story is that
# it independently reproduces take 136 -- kp 1.0 spinning too fast to read --
# from arithmetic alone. Self-checking: no runner script.
add_executable(x4vr_test_servo_lag servo_lag.cpp)

# Task #75: servo_lag's plant steps once per servo step. X4's camera reports at
# 30-60 Hz while X4VR_TICK_HZ drives the servo at 90, so that 1:1 assumption is
# exactly what cannot be assumed -- this drives the same real head_look_step()
# against a plant whose REPORT rate is decoupled from the step rate, and reports
# the same slam statistic the poselag stream measures. Self-checking: no runner
# script.
add_executable(x4vr_test_servo_rate servo_rate.cpp)

# Task #73's open-loop identification sequence, checked before a take is spent
# on it: that it never drives X4's camera into the clamp during a measured
# phase, and that the trace it produces can actually tell the candidate plants
# apart. Self-checking: no runner script.
add_executable(x4vr_test_plantid plantid.cpp)

# Task #75: X4's latency band, measured with the servo RUNNING. plantid needs
# the servo out of the loop for four minutes; this recovers the same band from
# an exogenous dither the servo does not know it is sending, which is what makes
# closed-loop identification valid. Proves the recovery on planted bands, proves
# it refuses when the dither is off, and measures the amplitude needed rather
# than guessing one into a player's headset. Self-checking: no runner script.
add_executable(x4vr_test_dither_id dither_id.cpp)
