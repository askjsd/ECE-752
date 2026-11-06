{"run": " > tmp", "files": ["../utilities/polybench.c", "gesummv.c"],
"lib": "-DPOLYBENCH_DUMP_ARRAYS -DLARGE_DATASET -I../utilities", "link_lib": "-lm", "exe_file": "a.out", "check": ["tmp"]}
