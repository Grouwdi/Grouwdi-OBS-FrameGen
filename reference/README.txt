CPU reference model of the Motion Smoother pipeline (Python 3, numpy, opencv-python).
Used while designing the filter: it runs the same algorithm on synthetic footage with a
known camera position, so smoothness and ghosting can be measured exactly.
gen_vectors.py writes the test vectors in ../test/vectors that the C retiming engine
(src/sc-timing.c) is checked against. Not needed to build or use the plugin.
