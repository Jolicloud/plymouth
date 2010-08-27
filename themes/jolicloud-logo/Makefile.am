themedir = $(datadir)/plymouth/themes/jolicloud-logo
nodist_theme_DATA = jolicloud-logo.plymouth
dist_theme_DATA =   jolicloud-logo.script                                \
                    jolicloud_logo.png                                   \
                    jolicloud_logo16.png                                 \
                    password_field.png                                \
                    password_field16.png                              \
                    progress_dot_off.png                              \
                    progress_dot_off16.png                            \
                    progress_dot_on.png                               \
                    progress_dot_on16.png



MAINTAINERCLEANFILES = Makefile.in jolicloud-logo.plymouth
CLEANFILES = jolicloud-logo.plymouth

jolicloud-logo.plymouth: $(srcdir)/jolicloud-logo.plymouth.in
	sed -e 's,[@]PLYMOUTH_THEME_PATH[@],$(PLYMOUTH_THEME_PATH),g' \
	       $(srcdir)/jolicloud-logo.plymouth.in > jolicloud-logo.plymouth

EXTRA_DIST = jolicloud-logo.plymouth.in
