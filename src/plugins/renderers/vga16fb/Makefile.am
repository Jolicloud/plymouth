INCLUDES = -I$(top_srcdir)                                                    \
           -I$(srcdir)/../../../libply                                        \
           -I$(srcdir)/../../../libply-splash-core                            \
           -I$(srcdir)/../../..                                               \
           -I$(srcdir)/../..                                                  \
           -I$(srcdir)/..                                                     \
           -I$(srcdir)

plugindir = $(libdir)/plymouth/renderers
plugin_LTLIBRARIES = vga16fb.la

vga16fb_la_CFLAGS = $(PLYMOUTH_CFLAGS)

vga16fb_la_LDFLAGS = -module -avoid-version -export-dynamic
vga16fb_la_LIBADD = $(PLYMOUTH_LIBS)                                     \
                    ../../../libply/libply.la                            \
                    ../../../libply-splash-core/libply-splash-core.la
vga16fb_la_SOURCES = $(srcdir)/plugin.c

MAINTAINERCLEANFILES = Makefile.in
