savedcmd_hid-astroc40.mod := printf '%s\n'   hid-astroc40.o | awk '!x[$$0]++ { print("./"$$0) }' > hid-astroc40.mod
