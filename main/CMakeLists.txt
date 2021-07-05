set(COMPONENT_SRCS "weasel.c fifostream.c pdmain.c \
../pd/src/g_canvas.c \
../pd/src/g_graph.c \
../pd/src/g_text.c \
../pd/src/g_rtext.c \
../pd/src/g_array.c \
../pd/src/g_template.c \
../pd/src/g_io.c \
../pd/src/g_scalar.c \
../pd/src/g_guiconnect.c \
../pd/src/g_readwrite.c \
../pd/src/g_clone.c \
../pd/src/m_pd.c \
../pd/src/m_class.c \
../pd/src/m_obj.c \
../pd/src/m_atom.c \
../pd/src/m_memory.c \
../pd/src/m_binbuf.c \
../pd/src/s_print.c \
../pd/src/s_utf8.c \
../pd/src/d_ugen.c \
../pd/src/d_resample.c \
../pd/src/d_soundfile.c \
../pd/src/d_global.c \
../pd/src/x_arithmetic.c \
../pd/src/x_connective.c \
../pd/src/x_interface.c \
../pd/src/x_misc.c \
../pd/src/x_time.c \
../pd/src/x_acoustics.c \
../pd/src/x_gui.c \
../pd/src/x_array.c \
../pd/src/x_scalar.c \
")

set(COMPONENT_ADD_INCLUDEDIRS ".")

set(COMPONENT_ADD_CFLAGS "-DPD -DHAVE_UNISTD_H -DHAVE_ALLOCA_H \
    -Wno-unused-variable")

register_component()
