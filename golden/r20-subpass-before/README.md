# R20 baseline: invalid readback interpretation

PID 218 repeats the old v0-subpass result (4/16 reader, reported 16/16 writer).
The writer was never mapped; its reported values were the expected values, not
measurements. The reader indexed raw tiled storage as linear rows. These
records preserve the historical failure but do not establish a driver defect
or a quarter-width limitation. Corrected full-frame readback is the witness.
