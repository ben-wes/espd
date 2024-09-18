static const char patchfile[] = "\
canvas 0 0 450 300 12;\n\
#X obj 190 104 loadbang;\n\
#X msg 190 129 \; pd dsp 1;\n\
#X obj 119 158 dac~ 1;\n\
#X obj 118 98 osc~ 440;\n\
#X obj 119 126 *~ 0.1;\n\
#X connect 0 0 1 0;\n\
#X connect 3 0 4 0;\n\
#X connect 4 0 2 0;\n\
";
