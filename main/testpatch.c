static const char patchfile[] = "\\
canvas 208 66 1338 642 12;\
#X obj 696 339 print;\
#X obj 381 596 dac~;\
#X obj 1057 187 adc~;\
#X obj 985 242 print~;\
#X obj 97 41 r key;\
#X msg 758 321 \; pd dsp 1;\
#X obj 382 494 *~ 0;\
#X obj 389 361 -~ 0.5;\
#X obj 283 411 *~ 1e+20;\
#X obj 283 437 clip~ 0 1;\
#X obj 382 520 -~ 0;\
#X obj 381 557 *~ 0;\
#X obj 449 553 *~ 0;\
#X obj 521 536 t b f;\
#X obj 521 562 1;\
#X obj 521 588 -;\
#X obj 1046 245 print~;\
#X msg 116 120 0;\
#X msg 161 120 1;\
#X msg 196 120 2;\
#X msg 233 121 4;\
#X msg 276 121 8;\
#X msg 351 120 16;\
#X msg 423 120 32;\
#X msg 482 119 64;\
#X msg 736 133 440;\
#X msg 788 136 0;\
#X msg 828 138 1;\
#X obj 452 285 t f f;\
#X obj 555 154 t b f;\
#X obj 695 164 t b f;\
#X obj 787 175 t b f;\
#X obj 281 469 -~;\
#X obj 281 495 *~ 0;\
#X obj 516 349 unpack 0 0 0 4 0, f 20;\
#X obj 520 315 pack 0 0 0 0 0;\
#X msg 895 143 0;\
#X msg 935 145 1;\
#X obj 894 182 t b f;\
#X obj 383 468 +~;\
#X obj 388 336 phasor~ 2;\
#X obj 105 75 sel 97 98 99 100 101 102 103 104 105 106 107 108 109 110 111 112 113 114 115 116, f 157;\
#X msg 1089 109 foo 1;\
#X msg 1146 107 foo 0;\
#X obj 1100 151 s pd;\
#X msg 696 131 0.1;\
#X obj 452 259 / 32, f 8;\
#X msg 558 121 32;\
#X msg 598 123 4096;\
#X connect 2 0 3 0;\
#X connect 2 1 16 0;\
#X connect 4 0 41 0;\
#X connect 6 0 10 0;\
#X connect 7 0 8 0;\
#X connect 7 0 32 1;\
#X connect 7 0 39 1;\
#X connect 8 0 9 0;\
#X connect 9 0 32 0;\
#X connect 10 0 11 0;\
#X connect 10 0 12 0;\
#X connect 11 0 1 0;\
#X connect 12 0 1 1;\
#X connect 13 0 14 0;\
#X connect 13 1 15 1;\
#X connect 14 0 15 0;\
#X connect 15 0 11 1;\
#X connect 17 0 46 0;\
#X connect 18 0 46 0;\
#X connect 19 0 46 0;\
#X connect 20 0 46 0;\
#X connect 21 0 46 0;\
#X connect 22 0 46 0;\
#X connect 23 0 46 0;\
#X connect 24 0 46 0;\
#X connect 25 0 30 0;\
#X connect 26 0 31 0;\
#X connect 27 0 31 0;\
#X connect 28 0 35 0;\
#X connect 29 0 46 0;\
#X connect 29 1 46 1;\
#X connect 30 0 46 0;\
#X connect 30 1 35 3;\
#X connect 31 0 46 0;\
#X connect 31 1 35 2;\
#X connect 32 0 33 0;\
#X connect 33 0 39 0;\
#X connect 34 0 6 1;\
#X connect 34 2 13 0;\
#X connect 34 2 12 1;\
#X connect 34 3 40 0;\
#X connect 34 4 33 1;\
#X connect 35 0 0 0;\
#X connect 35 0 34 0;\
#X connect 35 0 5 0;\
#X connect 36 0 38 0;\
#X connect 37 0 38 0;\
#X connect 38 0 46 0;\
#X connect 38 1 35 4;\
#X connect 39 0 6 0;\
#X connect 40 0 7 0;\
#X connect 41 0 17 0;\
#X connect 41 1 18 0;\
#X connect 41 2 19 0;\
#X connect 41 3 20 0;\
#X connect 41 4 21 0;\
#X connect 41 5 22 0;\
#X connect 41 6 23 0;\
#X connect 41 7 24 0;\
#X connect 41 8 47 0;\
#X connect 41 9 48 0;\
#X connect 41 10 45 0;\
#X connect 41 11 25 0;\
#X connect 41 12 26 0;\
#X connect 41 13 27 0;\
#X connect 41 14 36 0;\
#X connect 41 15 37 0;\
#X connect 41 16 3 0;\
#X connect 41 17 16 0;\
#X connect 41 18 42 0;\
#X connect 41 19 43 0;\
#X connect 42 0 44 0;\
#X connect 43 0 44 0;\
#X connect 45 0 30 0;\
#X connect 46 0 28 0;\
#X connect 47 0 29 0;\
#X connect 48 0 29 0;\
";
