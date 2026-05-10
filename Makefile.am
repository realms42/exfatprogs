## Makefile.am

ACLOCAL_AMFLAGS = -I m4

SUBDIRS = lib mkfs fsck tune label dump exfat2img resize

# make check — run the resize regression suite
check-local:
	MKFS="$(abs_builddir)/mkfs/mkfs.exfat" \
	RESIZE="$(abs_builddir)/resize/resize.exfat" \
	FSCK="$(abs_builddir)/fsck/fsck.exfat" \
	bash "$(srcdir)/tests/test_resize.sh"

# manpages
dist_man8_MANS =		\
	manpages/fsck.exfat.8	\
	manpages/tune.exfat.8	\
	manpages/mkfs.exfat.8	\
	manpages/exfatlabel.8	\
	manpages/dump.exfat.8	\
	manpages/exfat2img.8

# other stuff
EXTRA_DIST =			\
	include			\
	tests			\
	Android.bp		\
	lib/Android.bp		\
	mkfs/Android.bp		\
	tune/Android.bp		\
	fsck/Android.bp		\
	label/Android.bp	\
	dump/Android.bp		\
	exfat2img/Android.bp	\
	README.md
