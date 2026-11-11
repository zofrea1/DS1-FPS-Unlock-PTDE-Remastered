# make_image.py EXE : writes ptde_image.bin, the exe's memory image (sections at their addresses), for test_rmath.
import sys, pefile
pe = pefile.PE(sys.argv[1])
open('ptde_image.bin', 'wb').write(pe.get_memory_mapped_image())
