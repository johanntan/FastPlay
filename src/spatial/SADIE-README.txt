SADIE II HRTF database (subject D1, Neumann KU100)

The built in HRTF used by 3D Audio (hrtf_data.cpp) is derived from the SADIE II database,
University of York: subject D1 (Neumann KU100 dummy head), D1_48K_24bit_256tap_FIR_SOFA.sofa,
8802 measured directions over the full sphere at 48 kHz.

Source: https://www.york.ac.uk/sadie-project/database.html, archived at
https://zenodo.org/records/12092466 (D1_HRIR_SOFA.zip).
SHA-256 of the SOFA file used: 9af7cb19531e52fb7ae8ec92621e6ab62b1d5fe584b3742be36699a0ddb0ccd4

License: Apache License, Version 2.0 (SADIE-LICENSE.txt, as distributed with the database). The
authors ask that the database and the paper be cited:

Armstrong, C., Thresh, L., Murphy, D., and Kearney, G., "A Perceptual Evaluation of Individual and
Non-Individual HRTFs: A Case Study of the SADIE II Database", Applied Sciences 8(11), 2029 (2018),
doi:10.3390/app8112029.

The table is a derived work: diffuse-field equalised, onset aligned and trimmed to 128 taps on a
5 x 15 degree grid.

The FFT used by the renderer is pffft (pffft/), by Julien Pommier, under the FFTPACK licence
reproduced at the top of pffft/pffft.c.
