themedir = $(datadir)/plymouth/themes/jolicloud-logo
nodist_theme_DATA = jolicloud-logo.plymouth
dist_theme_DATA =   jolicloud-logo.script                                \
                    jolicloud_logo.png                                   \
                    jolicloud_logo-stretch.png                           \
                    cloud.png                                            \
                    cloud-stretch.png                                    \
                    blue_background.png                                  \
                    box.png                                              \
                    bullet.png                                           \
                    entry.png                                            \
                    lock.png                                             \
                    progress_bar.png                                     \
                    progress_box.png                                     \
                    progress_left.png                                    \
                    progress_right.png



MAINTAINERCLEANFILES = Makefile.in jolicloud-logo.plymouth
CLEANFILES = jolicloud-logo.plymouth

jolicloud-logo.plymouth: $(srcdir)/jolicloud-logo.plymouth.in
	sed -e 's,[@]PLYMOUTH_THEME_PATH[@],$(PLYMOUTH_THEME_PATH),g' \
	       $(srcdir)/jolicloud-logo.plymouth.in > jolicloud-logo.plymouth

EXTRA_DIST = jolicloud-logo.plymouth.in
