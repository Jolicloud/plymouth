INCLUDES = -I$(top_srcdir)                                                    \
           -I$(srcdir)/../../../libply                                        \
           -I$(srcdir)/../../../libply-splash-core                            \
           -I$(srcdir)/../../..                                               \
           -I$(srcdir)/../..                                                  \
           -I$(srcdir)/..                                                     \
           -I$(srcdir)

plugindir = $(libdir)/plymouth/renderers
plugin_LTLIBRARIES = drm.la

drm_la_CFLAGS = $(PLYMOUTH_CFLAGS) $(DRM_CFLAGS)

drm_la_LDFLAGS = -module -avoid-version -export-dynamic
drm_la_LIBADD = $(PLYMOUTH_LIBS) $(DRM_LIBS)                                  \
                         ../../../libply/libply.la                            \
                         ../../../libply-splash-core/libply-splash-core.la
drm_la_SOURCES = $(srcdir)/plugin.c                                           \
		 $(srcdir)/ply-renderer-driver.h                              \
		 $(srcdir)/ply-renderer-i915-driver.h                         \
		 $(srcdir)/ply-renderer-i915-driver.c                         \
		 $(srcdir)/ply-renderer-radeon-driver.h                       \
		 $(srcdir)/ply-renderer-radeon-driver.c                       \
		 $(srcdir)/ply-renderer-nouveau-driver.h                      \
		 $(srcdir)/ply-renderer-nouveau-driver.c

MAINTAINERCLEANFILES = Makefile.in
