# Inyectado con -DCMAKE_PROJECT_MonsterHunterTri_INCLUDE=<este archivo>.
# No modifica export/CMakeLists.txt ni ninguna fuente generada.
# Compila con /O2 las unidades output_N.cpp que el CMakeLists deja en /Od,
# salvo las de NWII_OD_KEEP (funciones gigantes, por defecto las de >10.000 lineas).
# El CMakeLists original no fija COMPILE_OPTIONS para estas unidades, asi que
# estas propiedades se conservan; el /Od del target va antes y /O2 prevalece.
if(NOT MSVC)
  return()
endif()
set(NWII_OD_KEEP "22;23;24;130;151;161;162" CACHE STRING "Unidades output_N que siguen en /Od")
set(_already_o2 0 1 2 3 4 5 6 7 8 9 10 11 13 16 17 156 158 159 160 166 167 168 169 170 174 175)
if(NWII_HOST_AVX2)
  set(_opts "/O2;/arch:AVX2;/fp:precise")
else()
  set(_opts "/O2")
endif()
set(_n 0)
foreach(i RANGE 0 184)
  list(FIND _already_o2 ${i} _a)
  list(FIND NWII_OD_KEEP ${i} _k)
  if(_a EQUAL -1 AND _k EQUAL -1)
    set_source_files_properties(output_${i}.cpp PROPERTIES COMPILE_OPTIONS "${_opts}")
    math(EXPR _n "${_n}+1")
  endif()
endforeach()
message(STATUS "[o2-generados] ${_n} unidades /Od pasan a ${_opts}; siguen /Od: ${NWII_OD_KEEP}")

# El ejecutable instalado (iteraciones 31 y 34) se enlazo a mano con main.cpp e
# ios.cpp en /O2 /Ob3; el CMakeLists no lo refleja. Se reaplica al final del
# procesado (DEFER) para que el CMakeLists no lo sobrescriba.
function(_nwii_o2_reapply_ob3)
  set_source_files_properties(nWiiRuntime/src/core/main.cpp PROPERTIES COMPILE_OPTIONS "/O2;/Ob3")
  set_source_files_properties(nWiiRuntime/src/hle/ios.cpp DIRECTORY "${CMAKE_SOURCE_DIR}/nWiiRuntime"
    PROPERTIES COMPILE_OPTIONS "/Ob3")
  message(STATUS "[o2-generados] main.cpp y ios.cpp con /Ob3 (como el ejecutable instalado)")
endfunction()
cmake_language(DEFER DIRECTORY "${CMAKE_SOURCE_DIR}" CALL _nwii_o2_reapply_ob3)
