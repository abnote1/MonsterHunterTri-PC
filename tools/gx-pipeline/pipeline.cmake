# Inyectado con -DCMAKE_PROJECT_MonsterHunterTri_INCLUDE=<este archivo>. No modifica export/.
# Sustituye fuentes del runtime por las copias de tools/gx-pipeline/ (generadas por make_patch.py):
#   canalizacion GX, cache de display lists, VI unico, copias EFB sin consultas GL, comparaciones TEV,
#   menu del port (P / Select+Start), texturas HD, filtrado, y guardado/cargado rapido.
# Anade port_savestate.cpp y antepone include/ (cabeceras con cambios: presentation, event_scheduler,
# ios_kernel). Reaplica /Ob3 a main.cpp e ios.cpp como el ejecutable instalado.
set(_NWII_PIPE_SWAPS
  "src/hle/gx\\.cpp$|gx.cpp"
  "src/hle/gx/fifo_parser\\.cpp$|fifo_parser.cpp"
  "src/hle/gx/renderer_gl\\.cpp$|renderer_gl.cpp"
  "src/hle/gx/tev_shader_gen\\.cpp$|tev_shader_gen.cpp"
  "src/hle/ios\\.cpp$|ios.cpp"
  "src/hle/ios_kernel\\.cpp$|ios_kernel.cpp"
  "src/hle/texture_cache_gl\\.cpp$|texture_cache_gl.cpp"
  "src/hle/pad\\.cpp$|pad.cpp"
  "src/hle/devices/usb_device\\.cpp$|usb_device.cpp"
  "src/hle/devices/fs_device\\.cpp$|fs_device.cpp"
  "src/input/input_manager\\.cpp$|input_manager.cpp"
  "src/core/os_hle_stubs\\.cpp$|os_hle_stubs.cpp"
  "src/hw/hw_ai\\.cpp$|hw_ai.cpp"
  "src/hw/hw_cp\\.cpp$|hw_cp.cpp"
  "src/hw/hw_di\\.cpp$|hw_di.cpp"
  "src/hw/hw_dsp\\.cpp$|hw_dsp.cpp"
  "src/hw/hw_ipc\\.cpp$|hw_ipc.cpp"
  "src/hw/hw_pe\\.cpp$|hw_pe.cpp"
  "src/hw/hw_pi\\.cpp$|hw_pi.cpp"
  "src/hw/hw_vi\\.cpp$|hw_vi.cpp"
  "src/core/cache_loop\\.cpp$|cache_loop.cpp")
function(_nwii_pipeline_apply)
  set(_dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}")
  get_target_property(_srcs nwiiruntime SOURCES)
  set(_out "")
  set(_hits "")
  foreach(s IN LISTS _srcs)
    set(_rep "")
    foreach(pair IN LISTS _NWII_PIPE_SWAPS)
      string(REPLACE "|" ";" _p "${pair}")
      list(GET _p 0 _re)
      list(GET _p 1 _file)
      if(s MATCHES "${_re}")
        set(_rep "${_dir}/${_file}")
        list(APPEND _hits "${_file}")
      endif()
    endforeach()
    if(_rep)
      list(APPEND _out "${_rep}")
    else()
      list(APPEND _out "${s}")
    endif()
  endforeach()
  foreach(pair IN LISTS _NWII_PIPE_SWAPS)
    string(REPLACE "|" ";" _p "${pair}")
    list(GET _p 1 _file)
    if(NOT _file IN_LIST _hits)
      message(FATAL_ERROR "[gx-pipeline] no se encontro ${_file} en nwiiruntime")
    endif()
  endforeach()
  list(APPEND _out "${_dir}/port_savestate.cpp" "${_dir}/port_cloud.cpp" "${_dir}/port_perf.cpp" "${_dir}/port_net.cpp" "${_dir}/port_libc.cpp")
  set_property(TARGET nwiiruntime PROPERTY SOURCES "${_out}")
  target_include_directories(nwiiruntime BEFORE PUBLIC "${_dir}/include")
  target_include_directories(nwiiruntime PRIVATE "${_dir}")
  target_include_directories(MonsterHunterTri BEFORE PRIVATE "${_dir}/include")
  # main.cpp del ejecutable -> copia con el menu del port
  get_target_property(_exe_srcs MonsterHunterTri SOURCES)
  set(_exe_out "")
  set(_hit_main 0)
  set(_hit_170 0)
  set(_hit_1 0)
  foreach(s IN LISTS _exe_srcs)
    if(s MATCHES "src/core/main\\.cpp$")
      list(APPEND _exe_out "${_dir}/main.cpp")
      set(_hit_main 1)
    elseif(s MATCHES "(^|/)output_170\\.cpp$")
      list(APPEND _exe_out "${_dir}/output_170.cpp")
      set(_hit_170 1)
    elseif(s MATCHES "(^|/)output_1\\.cpp$")
      list(APPEND _exe_out "${_dir}/output_1.cpp")
      set(_hit_1 1)
    else()
      list(APPEND _exe_out "${s}")
    endif()
  endforeach()
  if(NOT _hit_main)
    message(FATAL_ERROR "[gx-pipeline] no se encontro main.cpp en MonsterHunterTri")
  endif()
  if(NOT _hit_170)
    message(FATAL_ERROR "[gx-pipeline] no se encontro output_170.cpp en MonsterHunterTri")
  endif()
  if(NOT _hit_1)
    message(FATAL_ERROR "[gx-pipeline] no se encontro output_1.cpp en MonsterHunterTri")
  endif()
  list(APPEND _exe_out "${_dir}/port_thp.cpp" "${_dir}/port_mem.cpp")
  set_property(TARGET MonsterHunterTri PROPERTY SOURCES "${_exe_out}")
  # la copia de output_170.cpp con las mismas opciones que la original (/O2, AVX2 si procede)
  get_source_file_property(_o170 output_170.cpp DIRECTORY "${CMAKE_SOURCE_DIR}" COMPILE_OPTIONS)
  if(NOT _o170)
    set(_o170 "/O2")
  endif()
  set_source_files_properties("${_dir}/output_170.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "${_o170}")
  set_source_files_properties("${_dir}/port_thp.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "/O2")
  set_source_files_properties("${_dir}/port_mem.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "/O2")
  get_source_file_property(_o1 output_1.cpp DIRECTORY "${CMAKE_SOURCE_DIR}" COMPILE_OPTIONS)
  if(NOT _o1)
    set(_o1 "/O2")
  endif()
  set_source_files_properties("${_dir}/output_1.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "${_o1}")
  # bucles del juego: comprobacion de interrupciones barata (port_fastpoll.h) en todas las output_N.cpp,
  # y la version compactada de tools/gx-pipeline/compacto/ si existe (compactar.py; NWII_COMPACT=OFF la evita)
  option(NWII_COMPACT "Usar las unidades generadas compactadas (tools/gx-pipeline/compacto)" ON)
  set(_nfi 0)
  set(_ncomp 0)
  set(_exe_final "")
  foreach(s IN LISTS _exe_out)
    if(s MATCHES "(^|/)(output_[0-9]+\\.cpp)$")
      set(_name "${CMAKE_MATCH_2}")
      get_source_file_property(_opts "${s}" DIRECTORY "${CMAKE_SOURCE_DIR}" COMPILE_OPTIONS)
      if(NOT _opts)
        set(_opts "")
      endif()
      list(APPEND _opts "/FI${_dir}/port_fastpoll.h")
      set(_src "${s}")
      if(NWII_COMPACT AND EXISTS "${_dir}/compacto/${_name}")
        set(_src "${_dir}/compacto/${_name}")
        math(EXPR _ncomp "${_ncomp}+1")
      endif()
      set_source_files_properties("${_src}" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "${_opts}")
      list(APPEND _exe_final "${_src}")
      math(EXPR _nfi "${_nfi}+1")
    else()
      list(APPEND _exe_final "${s}")
    endif()
  endforeach()
  set_property(TARGET MonsterHunterTri PROPERTY SOURCES "${_exe_final}")
  # pila de 64 MB (solo reservada) para los hilos: las llamadas indirectas ya no vuelven al despachador
  # (compactar.py, port_dispatch) y las cadenas de llamadas del juego quedan anidadas en la pila del host
  target_link_options(MonsterHunterTri PRIVATE "/STACK:67108864")
  message(STATUS "[gx-pipeline] unidades generadas compactadas: ${_ncomp} de ${_nfi}")
  message(STATUS "[gx-pipeline] port_fastpoll.h en ${_nfi} unidades generadas; memcpy/__fill_mem nativos (output_1.cpp)")
  # clang-cl no deja usar intrinsecos FMA sin activar la extension (MSVC si): thp_idct_simd.cpp usa _mm_fmadd_pd
  if(CMAKE_CXX_COMPILER_ID STREQUAL "Clang")
    set_source_files_properties(thp_idct_simd.cpp DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "/O2;/arch:AVX2;/clang:-mfma")
    message(STATUS "[gx-pipeline] clang-cl: thp_idct_simd.cpp con FMA")
  endif()
  target_include_directories(MonsterHunterTri PRIVATE "${CMAKE_SOURCE_DIR}")
  message(STATUS "[gx-pipeline] video THP nativo: output_170.cpp sustituida (opciones ${_o170})")
  set_source_files_properties("${_dir}/main.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}" PROPERTIES COMPILE_OPTIONS "/O2;/Ob3")
  set_source_files_properties("${_dir}/ios.cpp" DIRECTORY "${CMAKE_SOURCE_DIR}/nWiiRuntime" PROPERTIES COMPILE_OPTIONS "/Ob3")
  message(STATUS "[gx-pipeline] menu del port, texturas HD, guardado rapido; gx.cpp canalizado, cache DL ampliada, VI unico, copias EFB sin consultas GL, comparaciones TEV; main.cpp/ios.cpp /Ob3")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _nwii_pipeline_apply)
