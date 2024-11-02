android_headers = dependency(
	'android-headers',
	version: '>=9.0.0',
	required: 'hwcomposer' in backends,
)

libdroid = dependency(
	'libdroid-0',
	required: 'hwcomposer' in backends,
)

if not (android_headers.found()) or not (libdroid.found())
	subdir_done()
endif

wlr_files += files(
	'backend.c',
	'hwcomposer2.c',
	'output.c',
)

features += { 'hwcomposer-backend': true }
wlr_deps += [android_headers, libdroid]
